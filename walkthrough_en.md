# Walkthrough: Porting & Customization of kaeru - Redmi 13C (gale)

This guide provides a step-by-step process of porting and customizing the `kaeru` payload for the Redmi 13C (gale) bootloader, directly referencing the official wiki resources:
1. 📖 [Porting kaeru to a new device](https://github.com/R0rt1z2/kaeru/wiki/Porting-kaeru-to-a-new-device)
2. 📖 [Customization and kaeru APIs](https://github.com/R0rt1z2/kaeru/wiki/Customization-and-kaeru-APIs)

---

## 1. Memory Address Comparison (Before vs After)

Here is the comparison of memory addresses in the `configs/xiaomi/gale_defconfig` file after performing static analysis on `lk.img`:

| Config Parameter | Value | Status / Technical Explanation |
| :--- | :--- | :--- |
| **`CONFIG_APP_ADDRESS`** | **`0x4C42A3CC`** | **[NEW]** Entry point of the main boot application (`app()`). Traced from the tail-jump in `fastboot continue`. |
| **`CONFIG_BOOTMODE_ADDRESS`** | **`0x4C5765A4`** | **[NEW]** Bootmode variable in BSS. Located via pointer chain in the `fastboot continue` function. |
| **`CONFIG_FASTBOOT_FAIL_ADDRESS`** | **`0x4C42B820`** | Loads the `"FAIL"` format string (confirmed via string search). |
| **`CONFIG_FASTBOOT_OKAY_ADDRESS`** | **`0x4C42BA00`** | Loads the `"OKAY"` format string. |
| **`CONFIG_PLATFORM_INIT_CALLER`** | **`0x4C425E0C`** | The `bl platform_init` instruction in the `bootstrap2` thread. |
| **`CONFIG_GET_ENV_ADDRESS`** | **`0x4C45C4E8`** | **[NEW]** Public `get_env(char *key)` wrapper — called 25× across LK, tail-calls the internal `env_lookup_with_area()`. |
| **`CONFIG_SET_ENV_ADDRESS`** | **`0x4C45C700`** | **[NEW]** Public `set_env(char *key, char *val)` wrapper — tail-calls `set_env_with_area(key, val, 0)`. |

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

All customization logic is placed in `board/xiaomi/board-gale.c`. Instead of unconditional early patches, we hook right after environment initialization (`env_init_done`) to apply patches dynamically depending on whether `is_spoofing_enabled()` is set to `1` or `0`.

### Hooking Env Init done
Since the env variables are not initialized during `board_early_init`, we hook the tail-call in the storage initialization function (`0x4C4057FA`) using `PATCH_BRANCH` to jump to `spoof_lock_state_hook()`, which runs our dynamic configuration.

### When Spoofing is ENABLED (`bldr_spoof` is "1")
* **Lock Spoofing (`seccfg_get_lock_state`)**: Patches function body at `0x4C471120 + 6` to write `1` (Locked) to the pointer, spoofing lock status for TEE and Android OS.
* **AVB Cmdline Spoofing**: NOP-patches the `beq.n` at `0x4C462260 + 0x9C` — forces `verifiedbootstate=green` into the kernel cmdline on all normal boots.
* **Fastboot Constraints Bypass**: NOP-patches `cbz` gates and `bl fastboot_fail` instructions in the fastboot command processor (`0x4C42B830`) so that fastboot commands work normally even while the lock state reports "locked".
* **AVB Chained Key Verification Bypass (`load_and_verify_vbmeta`)**: NOP-patches the `bne.w` error path branch at `0x4C464CF8` and patches `cmp r2, r3` to `cmp r3, r3` at `0x4C4649CC` (forcing length check to always pass) and `cmp r3, #0` to `movs r3, #1` at `0x4C464D6A` (forcing key trusted state). This allows booting unsigned modems/images.

### When Spoofing is DISABLED (`bldr_spoof` is "0")
* **Lock Spoofing (`seccfg_get_lock_state`)**: Patches function body to return `2` (Unlocked) so TEE, Android OS, and fastboot detect the device as fully unlocked.
* **Other patches**: Bypassed so the device boots in its natural unlocked state.

---

## 4. Recovery Cmdline Spoofing

For TWRP / fastbootd to correctly detect the device as **unlocked** when spoofing is enabled, the kernel cmdline must be restored to the real unlocked state when booting into recovery.

### Strategy
Hook `cmdline_pre_process` via `PATCH_CALL`. If `get_bootmode() == BOOTMODE_RECOVERY` and spoofing is enabled, `handle_recovery_boot()` replaces:
- `androidboot.verifiedbootstate=green` → `orange`
- `androidboot.secureboot=1` → `0`
- `androidboot.vbmeta.device_state=locked` → `unlocked`

### Key Addresses (found via static analysis)

| Symbol | Address | How Found |
| :--- | :--- | :--- |
| `cmdline_pre_process` | `0x4C42F544` | Pattern `0xE92D, 0x47F0, 0xF7FF, 0xFFA6` in lk.img |
| `g_cmdline` (CMDLINE1) | `0x4C579626` | Decoded PC-relative literal pool in cmdline builder |
| Secondary cmdline (CMDLINE2) | `0x4C5795D8` | Adjacent literal pool in same function |

---

## 5. Rebuilding
```bash
./build.sh gale lk.img
```

