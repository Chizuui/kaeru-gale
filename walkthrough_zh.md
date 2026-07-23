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

所有的定制补丁均写入 `board/xiaomi/board-gale.c` 源码文件中：

* **免除签名校验 (`get_vfy_policy`)**: 强制特征码 `0xB508, 0xF7FF, 0xFF63, 0xF3C0` 返回 `0`。
* **允许刷写分区 (`get_dl_policy`)**: 强制特征码 `0xB508, 0xF7FF, 0xFF5D, 0xF000` 返回 `0`。
* **绕过 AVB 报错 (`avb_slot_verify`)**: 修补特征码以强制走允许路径。
* **伪装锁定状态 (`seccfg_get_lock_state`)**: 在 `0x4C471120` 强制返回 `2`（未锁定），bootloader 允许刷写。
* **AVB 命令行伪装**: 在 `0x4C462260 + 0x9C` 写 NOP，强制 `verifiedbootstate=green` 传入内核（正常启动时 Play Integrity 正常工作）。

---

## 4. Recovery 引导的命令行修复

为使 TWRP / fastbootd 正确检测设备为**已解锁**状态，在进入 Recovery 模式时必须将内核命令行还原为真实的解锁状态。

### 策略
参照 `board-earth.c`：通过 `PATCH_CALL` 挂钩 `cmdline_pre_process`，在钩子函数中检测 Recovery 模式并覆写命令行缓冲区。

### 关键地址（静态分析获得）

| 符号 | 地址 | 定位方式 |
| :--- | :--- | :--- |
| `cmdline_pre_process` | `0x4C42F544` | lk.img 中的特征码 `0xE92D, 0x47F0, 0xF7FF, 0xFFA6` |
| `g_cmdline` (CMDLINE1) | `0x4C579626` | 解码 cmdline 构造函数中的 PC 相对字面量池 |
| 第二命令行缓冲区 (CMDLINE2) | `0x4C5795D8` | 同函数相邻字面量池 |

### `handle_recovery_boot()` 的行为
1. 检查 `get_bootmode() == BOOTMODE_RECOVERY` 且 `is_spoofing_enabled()` — 正常启动时提前返回。
2. 对两个 BSS 缓冲区调用 `patch_cmdline()`，替换以下内容：
   - `androidboot.verifiedbootstate=green` → `orange`
   - `androidboot.secureboot=1` → `0`
   - `androidboot.vbmeta.device_state=locked` → `unlocked`

---

## 4. 重新编译与构建
```bash
./build.sh gale lk.img
```
