# Walkthrough: Redmi 13C (gale) 的 kaeru 移植与定制

本指南提供了为 Redmi 13C (gale) 引导加载程序移植和定制 `kaeru` 载荷的逐步说明，直接引用了官方 Wiki 资源：
1. 📖 [Porting kaeru to a new device](https://github.com/R0rt1z2/kaeru/wiki/Porting-kaeru-to-a-new-device)
2. 📖 [Customization and kaeru APIs](https://github.com/R0rt1z2/kaeru/wiki/Customization-and-kaeru-APIs)

---

## 1. 内存地址对比（移植前 vs 移植后）

对 `lk.img` 进行静态分析后，`configs/xiaomi/gale_defconfig` 配置文件中关键地址的对比结果如下：

| 配置参数 | 移植分析前 | 移植分析后 | 状态 / 技术说明 |
| :--- | :--- | :--- | :--- |
| **`CONFIG_APP_ADDRESS`** | *(空)* | **`0x4C42A3CC`** | **[新增]** 主引导应用程序的入口地址 (`app()`)。通过追踪 `fastboot continue` 命令结尾处的跳转获得。 |
| **`CONFIG_BOOTMODE_ADDRESS`** | *(空)* | **`0x4C5765A4`** | **[新增]** RAM/BSS 中 bootmode 引导模式变量的地址。通过计算 `fastboot continue` 函数内部的相对指针获得。 |
| **`CONFIG_FASTBOOT_FAIL_ADDRESS`** | *(空)* | **`0x4C42B820`** | **[修正]** 地址 `0x4C42B820` 最初被错误识别为 OKAY 响应，现修正为 FAIL，因为它加载了 `"FAIL"` 字符串。 |
| **`CONFIG_FASTBOOT_OKAY_ADDRESS`** | `0x4C42B820` | **`0x4C42BA00`** | **[修正]** 更新为真实的 `fastboot_okay()` 响应函数地址（加载了 `"OKAY"` 字符串）。 |
| **`CONFIG_PLATFORM_INIT_CALLER`** | *(空)* | **`0x4C425E0C`** | **[新增]** 在 `bootstrap2` 线程中调用硬件初始化函数 `platform_init` 的指令地址。 |

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

* **免除映像文件签名校验 (`get_vfy_policy`)**:
   强制将 `0x4C417B58` 处的特征码（`0xB508, 0xF7FF, 0xFF63, 0xF3C0`）的返回值修改为 `0`（通过校验）。
* **允许加锁状态下烧录分区 (`get_dl_policy`)**:
   强制将 `0x4C417B64` 处的特征码（`0xB508, 0xF7FF, 0xFF5D, 0xF000`）的返回值修改为 `0`。
* **绕过 AVB 报错防线 (`avb_slot_verify`)**:
   将 `0x4C465E5A` 处的特征码（`0xF005, 0x0301, 0xF083, 0x0A01, 0x930D, 0x9B70`）修改替换为 `0xF04F, 0x0301`。
* **伪装引导加载程序锁定状态 (`seccfg_get_lock_state` & AVB 命令行补丁)**:
  1. 拦截 `0x4C471120` 处的安全锁获取函数，强制返回 `2`（未锁定），保证 Bootloader 内部功能正常闪存和引导。
  2. 拦截 `0x4C462260` 处的 AVB 内核命令行构造函数，在其偏移量 `+ 0x9C`（地址 `0x4C4622FC`）处写入一个 16 位的 NOP，迫使其将 `"locked"` 安全状态传达给 Android 操作系统。

---

## 4. 重新编译与构建
```bash
./build.sh gale lk.img
```
