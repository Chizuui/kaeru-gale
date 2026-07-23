# Walkthrough: Redmi 13C (gale) 的 kaeru 移植与定制

本指南提供了为 Redmi 13C (gale) 引导加载程序移植和定制 `kaeru` 载荷的逐步说明，直接引用了官方 Wiki 资源：
1. 📖 [Porting kaeru to a new device](https://github.com/R0rt1z2/kaeru/wiki/Porting-kaeru-to-a-new-device)
2. 📖 [Customization and kaeru APIs](https://github.com/R0rt1z2/kaeru/wiki/Customization-and-kaeru-APIs)

---

## 1. 内存地址对比（移植前 vs 移植后）

对 `lk.img` 进行静态分析后，`configs/xiaomi/gale_defconfig` 配置文件中关键地址的对比结果如下：

| 配置参数 | 地址值 | 状态 / 技术说明 |
| :--- | :--- | :--- |
| **`CONFIG_APP_ADDRESS`** | **`0x4C42A3CC`** | **[新增]** 主引导应用程序（`app()`）的入口点。通过 `fastboot continue` 末尾的 tail-jump 追踪。 |
| **`CONFIG_BOOTMODE_ADDRESS`** | **`0x4C5765A4`** | **[新增]** BSS 中的启动模式变量，通过 `fastboot continue` 函数内指针链定位。 |
| **`CONFIG_FASTBOOT_FAIL_ADDRESS`** | **`0x4C42B820`** | 加载 `"FAIL"` 格式字符串（字符串搜索确认）。 |
| **`CONFIG_FASTBOOT_OKAY_ADDRESS`** | **`0x4C42BA00`** | 加载 `"OKAY"` 格式字符串。 |
| **`CONFIG_PLATFORM_INIT_CALLER`** | **`0x4C425E0C`** | `bootstrap2` 线程中的 `bl platform_init` 指令地址。 |
| **`CONFIG_GET_ENV_ADDRESS`** | **`0x4C45C4E8`** | **[新增]** 公开的 `get_env(char *key)` 包装函数 — 被调用 25 次，tail-call 到 `env_lookup_with_area()`。 |
| **`CONFIG_SET_ENV_ADDRESS`** | **`0x4C45C700`** | **[新增]** 公开的 `set_env(char *key, char *val)` 包装函数 — tail-call 到 `set_env_with_area(key, val, 0)`。 |

---

## 2. 分析步骤 (移植过程)

### 步骤 1: 反汇编引导加载程序 (`lk.img`)
由于 MediaTek 的 LK 是在 Thumb-2 指令架构（混合 16/32 位）下编译的，机器码的反汇编必须进行专门设置，以避免错误解析指令代码。
我安装了 `binutils-arm-none-eabi` 并运行了：
```bash
arm-none-eabi-objdump -m arm -M force-thumb -b binary --adjust-vma=0x4C3FFE00 -D lk.img > lk.asm
```
*注意：调整 VMA 至 `0x4C3FFE00` 以抵消 512 字节的 MediaTek 二进制头（0x200）。*

### 步骤 2: 定位 Fastboot 响应函数
`fastboot_okay` 和 `fastboot_fail` 都会跳转到 `0x4C42B5F0` 处的通用发送函数。我追踪了该函数的两个调用者：
* 位于地址 `0x4C42B820` 的函数加载了一个 PC 相对偏移，指向地址 `0x4C5182D4`。在内存中，该处的二进制数据是字符串 `"FAIL"`。
* 位于地址 `0x4C42BA00` 的函数加载了一个 PC 相对偏移，指向地址 `0x4C49C964`。在内存中，该处的二进制数据是字符串 `"OKAY"`。

### 步骤 3: 寻找 `bootmode` 变量地址 (`CONFIG_BOOTMODE_ADDRESS`)
在 `fastboot continue` 函数（`0x4C42E1CC`）内部，有一条指令将 bootmode 变量加载到寄存器中以将其重置为 `BOOTMODE_NORMAL (0)`。
我追踪了从文字地址 `0x4C42E228` 加载的地址指针寄存器，该地址指向 RAM 中的 `0x4C573654`。在此 RAM 位置处，有一个指针指向真正的 bootmode 变量，位于 **`0x4C5765A4`**。

### 步骤 4: 寻找主引导应用入口地址 (`CONFIG_APP_ADDRESS`)
执行 `fastboot continue` 命令后，系统跳转以进入主应用程序，地址为：
```assembly
4c42e200:  b.w  0x4c42a3cc
```
因此，确认地址 `0x4C42A3CC` 为 `app()` 的地址。

### 步骤 5: 寻找 Platform Init 调用点
我搜索了所有指向平台初始化地址（`0x4C4039DC`）的跳转指令 `bl`，并在引导线程的地址 **`0x4C425E0C`** 处找到了其调用者：
```assembly
4c425e0c:  bl  0x4c4039dc
```

---

## 3. 定制与锁定状态伪装 (board-gale.c)

所有的定制逻辑均写入 `board/xiaomi/board-gale.c` 源码文件中。与此前无条件的早期修改不同，我们通过挂钩环境初始化完成时（`env_init_done`）的代码，根据环境变量 `is_spoofing_enabled()` 的状态（`1` 或 `0`）动态应用各种补丁。

### 挂钩环境初始化完成 (Env Init Done)
由于环境变量在 `board_early_init` 运行期间尚未就绪，我们通过 `PATCH_BRANCH` 挂钩存储初始化函数的 tail-call 调用（`0x4C4057FA`），使其跳转至 `spoof_lock_state_hook()`，从而在此处运行我们的动态配置。

### 当启用伪装时 (`bldr_spoof` 设为 "1")
* **伪装锁定状态 (`seccfg_get_lock_state`)**: 在 `0x4C471120 + 6` 处修改函数体，向指针写入 `1`（已锁定），从而欺骗 TEE 和 Android OS。
* **AVB 命令行伪装**: 在 `0x4C462260 + 0x9C` 写入 NOP，强制在所有正常启动时向内核命令行传入 `verifiedbootstate=green`。
* **绕过 Fastboot 安全门槛**: 在 fastboot 命令行处理器 (`0x4C42B830`) 的 `cbz` 门槛和 `bl fastboot_fail` 指令处写入 NOP，使得设备在报告“已锁定”状态时，仍可正常执行 fastboot 刷写和擦除操作。
* **绕过 AVB 链式公钥验证 (`load_and_verify_vbmeta`)**: NOP 掉 `0x4C464CF8` 处的 `bne.w` 报错跳转，修补 `0x4C4649CC` 处的 `cmp r2, r3` 为 `cmp r3, r3`（强制跳过公钥长度检查），并修补 `0x4C464D6A` 处的 `cmp r3, #0` 为 `movs r3, #1`（强制将该公钥标记为信任）。这允许加载和启动未签名的 modem 和其他固件镜像。

### 当禁用伪装时 (`bldr_spoof` 设为 "0")
* **伪装锁定状态 (`seccfg_get_lock_state`)**: 修改函数体以返回 `2`（未锁定），使 TEE、Android OS 和 fastboot 将设备识别为完全解锁状态。
* **其他补丁**: 被跳过，因此设备将以其真实的未锁定状态引导启动。

---

## 4. Recovery 引导的命令行修复

为使 TWRP / fastbootd 正确检测设备为**已解锁**状态（在启用伪装时），在进入 Recovery 模式时必须将内核命令行还原为真实的解锁状态。

### 策略
通过 `PATCH_CALL` 挂钩 `cmdline_pre_process`。如果满足 `get_bootmode() == BOOTMODE_RECOVERY` 且已启用伪装，`handle_recovery_boot()` 将替换以下内容：
- `androidboot.verifiedbootstate=green` → `orange`
- `androidboot.secureboot=1` → `0`
- `androidboot.vbmeta.device_state=locked` → `unlocked`

### 关键地址（静态分析获得）

| 符号 | 地址 | 定位方式 |
| :--- | :--- | :--- |
| `cmdline_pre_process` | `0x4C42F544` | lk.img 中的特征码 `0xE92D, 0x47F0, 0xF7FF, 0xFFA6` |
| `g_cmdline` (CMDLINE1) | `0x4C579626` | 解码 cmdline 构造函数中的 PC 相对字面量池 |
| 第二命令行缓冲区 (CMDLINE2) | `0x4C5795D8` | 同函数相邻字面量池 |

---

## 5. 重新编译与构建
```bash
./build.sh gale lk.img
```
