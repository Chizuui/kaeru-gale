//
// SPDX-FileCopyrightText: 2026 Chizuui <desckun@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//

#include <board_ops.h>

// Kernel cmdline buffers in BSS (found via static analysis of lk.img)
#define CMDLINE1_ADDR 0x4C579626
#define CMDLINE2_ADDR 0x4C5795D8

// Swap verifiedbootstate and secureboot params back to "unlocked" state
// so fastbootd / recovery tools know the device is actually unlocked.
static void patch_cmdline(char *cmdline) {
    cmdline_replace(cmdline, "androidboot.verifiedbootstate=",
                    "green", "orange");
    cmdline_replace(cmdline, "androidboot.secureboot=",
                    "1", "0");
    cmdline_replace(cmdline, "androidboot.vbmeta.device_state=",
                    "locked", "unlocked");
}

// Called from the cmdline_pre_process hook. Only restores unlocked state
// when booting into recovery — normal boots stay spoofed as "locked".
static void handle_recovery_boot(void) {
    if (get_bootmode() != BOOTMODE_RECOVERY || !is_spoofing_enabled())
        return;

    printf("Recovery boot detected, patching cmdline for unlocked state.\n");

    static const uint32_t cmdline_addrs[] = { CMDLINE1_ADDR, CMDLINE2_ADDR };
    for (int i = 0; i < ARRAY_SIZE(cmdline_addrs); i++) {
        printf("Patching cmdline at 0x%08X\n", cmdline_addrs[i]);
        patch_cmdline((char *)cmdline_addrs[i]);
    }
}

void board_early_init(void) {
    printf("Entering early init for Redmi 13C (gale)\n");

    uint32_t addr = 0;

    // 1. Disable image authentication (get_vfy_policy -> return 0)
    addr = SEARCH_PATTERN(LK_START, LK_END, 0xB508, 0xF7FF, 0xFF63, 0xF3C0);
    if (addr) {
        printf("Found get_vfy_policy at 0x%08X\n", addr);
        FORCE_RETURN(addr, 0);
    }

    // 2. Allow flashing (get_dl_policy -> return 0)
    addr = SEARCH_PATTERN(LK_START, LK_END, 0xB508, 0xF7FF, 0xFF5D, 0xF000);
    if (addr) {
        printf("Found get_dl_policy at 0x%08X\n", addr);
        FORCE_RETURN(addr, 0);
    }

    // 3. Allow AVB verification errors (avb_slot_verify allow-error gate)
    addr = SEARCH_PATTERN(LK_START, LK_END, 0xF005, 0x0301, 0xF083, 0x0A01, 0x930D, 0x9B70);
    if (addr) {
        printf("Found avb_slot_verify allow-error gate at 0x%08X\n", addr);
        PATCH_MEM(addr, 0xF04F, 0x0301);
    }

    // 4. Force seccfg_get_lock_state to return 2 (unlocked state) so
    //    the internal bootloader allows flashing via fastboot.
    addr = SEARCH_PATTERN(LK_START, LK_END, 0xB1D0, 0xB510, 0x4604, 0xF7FF, 0xFFDD);
    if (addr) {
        printf("Found seccfg_get_lock_state at 0x%08X\n", addr);
        PATCH_MEM(addr + 6,
                  0x2301,  // movs r3, #1
                  0x6023,  // str r3, [r4, #0]
                  0x2002,  // movs r0, #2
                  0xbd10   // pop {r4, pc}
        );
    }

    // 5. Force AVB cmdline to always report "locked" to Android OS so
    //    Play Integrity / SafetyNet passes on normal boots.
    addr = SEARCH_PATTERN(LK_START, LK_END, 0xE92D, 0x4FF0, 0x4691, 0xF102);
    if (addr) {
        printf("Found AVB cmdline function at 0x%08X\n", addr);
        // NOP the beq.n branch that picks the actual device state,
        // forcing libavb to always use the "locked" string.
        NOP(addr + 0x9C, 1);
    }

    // 6. Hook cmdline_pre_process to restore unlocked state when booting
    //    into recovery, so fastbootd and recovery tools allow flashing.
    addr = SEARCH_PATTERN(LK_START, LK_END, 0xE92D, 0x47F0, 0xF7FF, 0xFFA6);
    if (addr) {
        printf("Found cmdline_pre_process at 0x%08X\n", addr);
        PATCH_CALL(addr, (void *)handle_recovery_boot, TARGET_THUMB);
    }

    // Publish spoofing status so users can query it via:
    //   fastboot getvar is-spoofing
    //
    // NOTE: board_early_init runs before env is initialized, so
    // is_spoofing_enabled() would always return 0 here (env not ready).
    // Since gale patches seccfg_get_lock_state unconditionally, we just
    // report "1" always. The oem bldr_spoof command can still toggle the
    // env var for the cmdline hook (handle_recovery_boot) which runs later.
    fastboot_publish("is-spoofing", "1");

    // Register custom fastboot command to toggle cmdline spoofing:
    //   fastboot oem bldr_spoof enable   -> keep verifiedbootstate=green on OS
    //   fastboot oem bldr_spoof disable  -> expose orange/unlocked to OS
    fastboot_register("oem bldr_spoof", cmd_spoof_bootloader_lock, 0);
}

void board_late_init(void) {
    printf("Entering late init for Redmi 13C (gale)\n");

    uint32_t addr = 0;

    // Disable dm-verity corruption warning ("Your device is corrupt") that
    // appears on boot when the bootloader is unlocked, to avoid the
    // 5-second power-off delay and the scary red warning screen.
    addr = SEARCH_PATTERN(LK_START, LK_END, 0xB530, 0xB083, 0xAB02, 0x2200);
    if (addr) {
        printf("Found dm_verity_corruption at 0x%08X\n", addr);
        FORCE_RETURN(addr, 0);
    }

    video_printf("\n[KAERU] Loaded on Redmi 13C (gale)!\n");
    video_printf("[KAERU] Bootloader lock status spoofed!\n");
}
