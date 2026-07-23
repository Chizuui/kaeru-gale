//
// SPDX-FileCopyrightText: 2026 Chizuui <desckun@gmail.com>
// SPDX-License-Identifier: AGPL-3.0-or-later
//

#include <board_ops.h>

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

    // 4. Force seccfg_get_lock_state to return 2 (unlocked state)
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

    // 5. Force AVB cmdline to always report locked
    addr = SEARCH_PATTERN(LK_START, LK_END, 0xE92D, 0x4FF0, 0x4691, 0xF102);
    if (addr) {
        printf("Found AVB cmdline function at 0x%08X\n", addr);
        // NOP out the 16-bit branch (beq.n 0x4c4622e8) at offset 0x9C
        NOP(addr + 0x9C, 1);
    }
}

void board_late_init(void) {
    printf("Entering late init for Redmi 13C (gale)\n");
    video_printf("\n[KAERU] Loaded on Redmi 13C (gale)!\n");
    video_printf("[KAERU] Bootloader lock status spoofed!\n");
}
