# Walkthrough: Porting & Customization of kaeru - Redmi 13C (gale)

This guide provides a step-by-step process of porting and customizing the `kaeru` payload for the Redmi 13C (gale) bootloader, directly referencing the official wiki resources:
1. 📖 [Porting kaeru to a new device](https://github.com/R0rt1z2/kaeru/wiki/Porting-kaeru-to-a-new-device)
2. 📖 [Customization and kaeru APIs](https://github.com/R0rt1z2/kaeru/wiki/Customization-and-kaeru-APIs)

---

## 1. Memory Address Comparison (Before vs After)

Here is the comparison of memory addresses in the `configs/xiaomi/gale_defconfig` file after performing static analysis on `lk.img`:

| Config Parameter | Before Analysis | After Analysis | Status / Technical Explanation |
| :--- | :--- | :--- | :--- |
| **`CONFIG_APP_ADDRESS`** | *(Empty)* | **`0x4C42A3CC`** | **[NEW]** The entry point address of the main boot application (`app()`). Obtained by tracing the jump at the end of the `fastboot continue` command. |
| **`CONFIG_BOOTMODE_ADDRESS`** | *(Empty)* | **`0x4C5765A4`** | **[NEW]** The address of the bootmode variable in RAM/BSS. Located via relative pointer calculation inside the `fastboot continue` function. |
| **`CONFIG_FASTBOOT_FAIL_ADDRESS`** | *(Empty)* | **`0x4C42B820`** | **[CORRECTION]** Address `0x4C42B820`, initially detected as OKAY, is corrected to FAIL as it loads the `"FAIL"` string. |
| **`CONFIG_FASTBOOT_OKAY_ADDRESS`** | `0x4C42B820` | **`0x4C42BA00`** | **[CORRECTION]** Updated to the actual `fastboot_okay()` address which loads the `"OKAY"` string. |
| **`CONFIG_PLATFORM_INIT_CALLER`** | *(Empty)* | **`0x4C425E0C`** | **[NEW]** The address of the instruction calling `platform_init` in the `bootstrap2` thread. |

---

## 2. Analysis Steps (Porting)

### Step 1: Disassembling the Bootloader (`lk.img`)
Since MediaTek's LK is compiled in the Thumb-2 instruction architecture (mixed 16/32-bit), the disassembly of the machine code must be set specifically to avoid misinterpretation of the instruction codes.
I installed `binutils-arm-none-eabi` and ran:
```bash
arm-none-eabi-objdump -m arm -M force-thumb -b binary --adjust-vma=0x4C3FFE00 -D lk.img > lk.asm
```
*Note: VMA is adjusted to `0x4C3FFE00` to account for the 512-byte MediaTek binary header (0x200).*

### Step 2: Locating Fastboot Responses
Both `fastboot_okay` and `fastboot_fail` jump to the common sender function at `0x4C42B5F0`. I traced the two callers of this function:
* The function at address `0x4C42B820` loads a PC-relative offset that points to address `0x4C5182D4`. In memory, the binary data there is the string `"FAIL"`.
* The function at address `0x4C42BA00` loads a PC-relative offset that points to address `0x4C49C964`. In memory, the binary data there is the string `"OKAY"`.

### Step 3: Finding `bootmode` (`CONFIG_BOOTMODE_ADDRESS`)
Inside the `fastboot continue` function (`0x4C42E1CC`), there is an instruction to load the bootmode variable into a register to reset it to `BOOTMODE_NORMAL (0)`.
I traced the address pointer register loaded from the literal address `0x4C42E228` which points to RAM `0x4C573654`. At that RAM location, there is a pointer pointing to the actual bootmode variable at **`0x4C5765A4`**.

### Step 4: Finding the Main Application Pointer (`CONFIG_APP_ADDRESS`)
After the `fastboot continue` command is executed, the system jumps to enter the main application at address:
```assembly
4c42e200:  b.w  0x4c42a3cc
```
Thus, the address `0x4C42A3CC` is confirmed as the address of `app()`.

### Step 5: Finding the Platform Init Caller
I searched for all branch instructions `bl` that lead to the platform initialization address (`0x4C4039DC`) and found its caller in the bootstrap thread at address **`0x4C425E0C`**:
```assembly
4c425e0c:  bl  0x4c4039dc
```

---

## 3. Customization & Spoofing (board-gale.c)

All customization logic is placed in `board/xiaomi/board-gale.c`:

* **Disable Image Authentication (`get_vfy_policy`)**:
   Forces pattern (`0xB508, 0xF7FF, 0xFF63, 0xF3C0`) at `0x4C417B58` to return `0`.
* **Allow Flashing when Locked (`get_dl_policy`)**:
   Forces pattern (`0xB508, 0xF7FF, 0xFF5D, 0xF000`) at `0x4C417B64` to return `0`.
* **Allow AVB Errors (`avb_slot_verify`)**:
   Patches pattern (`0xF005, 0x0301, 0xF083, 0x0A01, 0x930D, 0x9B70`) at `0x4C465E5A` with `0xF04F, 0x0301`.
* **Bootloader Lock Spoofing (`seccfg_get_lock_state` & AVB Cmdline)**:
  1. Patches `seccfg_get_lock_state` at `0x4C471120` to return `2` (Unlocked) so the internal bootloader runs without flashing constraints.
  2. Patches the AVB cmdline function at `0x4C462260` by inserting a 16-bit NOP at offset `+ 0x9C` (`0x4C4622FC`). This tricks the Android OS into reporting the lock state as `"locked"`.

---

## 4. Rebuilding
```bash
./build.sh gale lk.img
```
