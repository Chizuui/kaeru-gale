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

static uint32_t seccfg_addr = 0;
static uint32_t avb_cmdline_addr = 0;
static uint32_t load_and_verify_vbmeta_addr = 0;
static uint32_t fastboot_processor_addr = 0;

__attribute__((used)) void spoof_lock_state(void) {
    int spoofing = is_spoofing_enabled();

    fastboot_publish("is-spoofing", spoofing ? "1" : "0");

    if (spoofing) {
        printf("[KAERU] Bootloader lock status spoofing: ENABLED\n");

        // 1. Force seccfg_get_lock_state to return LKS_LOCK (1) to Android OS
        if (seccfg_addr) {
            PATCH_MEM(seccfg_addr + 6,
                      0x2301,  // movs r3, #1 (LKS_LOCK)
                      0x6023,  // str r3, [r4, #0]
                      0x2000,  // movs r0, #0 (Success status)
                      0xbd10   // pop {r4, pc}
            );
        }

        // 2. Force AVB cmdline to always report "locked" to Android OS
        if (avb_cmdline_addr) {
            NOP(avb_cmdline_addr + 0x9C, 2); // NOP the beq.n branch
        }

        // 3. Remove fastboot lock/security constraints (from 0x4C42B830 base)
        if (fastboot_processor_addr) {
            NOP(fastboot_processor_addr + 0xFE, 1);  // NOP cbz r0, 0x4c42b978
            NOP(fastboot_processor_addr + 0x12C, 1); // NOP cbz r0, 0x4c42b98a
            NOP(fastboot_processor_addr + 0x16E, 2); // NOP bl fastboot_fail for "not support"
            NOP(fastboot_processor_addr + 0x17A, 2); // NOP bl fastboot_fail for "not allowed"
        }

        // 4. Bypass load_and_verify_vbmeta key verification
        if (load_and_verify_vbmeta_addr) {
            NOP(load_and_verify_vbmeta_addr, 2); // NOP bne.w error (0x4C464CF8)
            PATCH_MEM(load_and_verify_vbmeta_addr + 0x72, 0x2301); // cmp r3, #0 -> movs r3, #1 (0x4C464D6A)
        }
    } else {
        printf("[KAERU] Bootloader lock status spoofing: DISABLED\n");

        // 1. Force seccfg_get_lock_state to return LKS_UNLOCK (2) (standard unlocked)
        if (seccfg_addr) {
            PATCH_MEM(seccfg_addr + 6,
                      0x2302,  // movs r3, #2 (LKS_UNLOCK)
                      0x6023,  // str r3, [r4, #0]
                      0x2000,  // movs r0, #0 (Success status)
                      0xbd10   // pop {r4, pc}
            );
        }
    }
}

static void __attribute__((naked)) spoof_lock_state_hook(void) {
    asm volatile(
        "push {r0-r3, lr}\n"
        "bl spoof_lock_state\n"
        "pop {r0-r3, lr}\n"
        "movw ip, #0x0D41\n"
        "movt ip, #0x4C40\n"
        "bx ip\n"
    );
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

    // Find VMAs of seccfg, AVB cmdline, fastboot, and vbmeta verification functions
    seccfg_addr = SEARCH_PATTERN(LK_START, LK_END, 0xB1D0, 0xB510, 0x4604, 0xF7FF, 0xFFDD);
    if (seccfg_addr) {
        printf("Found seccfg_get_lock_state at 0x%08X\n", seccfg_addr);
    }

    avb_cmdline_addr = SEARCH_PATTERN(LK_START, LK_END, 0xE92D, 0x4FF0, 0x4691, 0xF102);
    if (avb_cmdline_addr) {
        printf("Found AVB cmdline function at 0x%08X\n", avb_cmdline_addr);
    }

    uint32_t fastboot_gate_addr = SEARCH_PATTERN(LK_START, LK_END, 0x493A, 0x9804, 0x4479, 0xF015);
    if (fastboot_gate_addr) {
        fastboot_processor_addr = fastboot_gate_addr - 0xC4;
        printf("Found fastboot command processor at 0x%08X\n", fastboot_processor_addr);
    }

    load_and_verify_vbmeta_addr = SEARCH_PATTERN(LK_START, LK_END, 0xF47F, 0xAE6B, 0xE688, 0xF8DD);
    if (load_and_verify_vbmeta_addr) {
        printf("Found load_and_verify_vbmeta at 0x%08X\n", load_and_verify_vbmeta_addr);
        
        // Always bypass chained key length mismatch so it goes to memcmp path
        PATCH_MEM(load_and_verify_vbmeta_addr - 0x32C, 0x429B); // cmp r2, r3 -> cmp r3, r3
    }

    // Hook env_init_done (VMA 0x4C4057FA) to run our dynamic spoofing logic
    uint32_t storage_init_done_caller = SEARCH_PATTERN(LK_START, LK_END, 0xB00C, 0xE8BD, 0x41F0, 0xF7FB, 0xBAA1);
    if (storage_init_done_caller) {
        uint32_t hook_addr = storage_init_done_caller + 6;
        printf("Found storage_init_done tail-call at 0x%08X, hooking...\n", hook_addr);
        PATCH_BRANCH(hook_addr, (void *)spoof_lock_state_hook);
    }

    // Hook cmdline_pre_process to restore unlocked state when booting recovery
    addr = SEARCH_PATTERN(LK_START, LK_END, 0xE92D, 0x47F0, 0xF7FF, 0xFFA6);
    if (addr) {
        printf("Found cmdline_pre_process at 0x%08X\n", addr);
        PATCH_CALL(addr, (void *)handle_recovery_boot, TARGET_THUMB);
    }

    // Register custom fastboot command to toggle cmdline spoofing:
    //   fastboot oem bldr_spoof on   -> enable bootloader lock spoofing
    //   fastboot oem bldr_spoof off  -> disable bootloader lock spoofing
    fastboot_register("oem bldr_spoof", cmd_spoof_bootloader_lock, 0);
}

void board_late_init(void) {
    printf("Entering late init for Redmi 13C (gale)\n");

    uint32_t addr = 0;

    // Disable dm-verity corruption warning ("Your device is corrupt")
    addr = SEARCH_PATTERN(LK_START, LK_END, 0xB530, 0xB083, 0xAB02, 0x2200);
    if (addr) {
        printf("Found dm_verity_corruption at 0x%08X\n", addr);
        FORCE_RETURN(addr, 0);
    }

    video_printf("\n[KAERU] Loaded on Redmi 13C (gale)!\n");
    if (is_spoofing_enabled()) {
        video_printf("[KAERU] Bootloader lock status spoofed!\n");
    } else {
        video_printf("[KAERU] Bootloader spoofing is disabled.\n");
    }
}

