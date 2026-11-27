/* SPDX-License-Identifier: BSD-2-Clause
 * Boot menu logic, without any UEFI dependency so the host tests can drive it (tests/boot_menu_test.c).
 * The UEFI side (BootMenuEfi.c) turns key presses and the clock into events and carries out the returned action.
 */
#ifndef B2L_BOOT_MENU_H
#define B2L_BOOT_MENU_H

#include <stdint.h>

enum b2l_menu_item { B2L_MENU_BOOT, B2L_MENU_RECOVERY, B2L_MENU_REBOOT, B2L_MENU_COUNT };
enum b2l_menu_key { B2L_KEY_NONE, B2L_KEY_UP, B2L_KEY_DOWN, B2L_KEY_SELECT, B2L_KEY_BACK };
enum b2l_menu_action {
    B2L_ACT_NONE,       /* nothing changed */
    B2L_ACT_REDRAW,     /* selection or countdown changed */
    B2L_ACT_BOOT, B2L_ACT_RECOVERY, B2L_ACT_REBOOT
};
/* How the menu starts. FORCE_* come from a volume key held while the loader starts. */
enum b2l_menu_start { B2L_START_NORMAL, B2L_START_FORCE_MENU, B2L_START_FORCE_RECOVERY };

#define B2L_MENU_DEBOUNCE_MS 150u

struct b2l_menu {
    unsigned selected;
    int counting;               /* the auto-boot countdown runs until the first key */
    uint32_t timeout_ms;
    uint64_t start_ms;
    unsigned shown_seconds;     /* last countdown value drawn, to redraw once a second */
    enum b2l_menu_key last_key;
    uint64_t last_key_ms;
};

/* remembered: the item chosen last time (B2L_MENU_COUNT if none). It only places the cursor: when the countdown ends
 * the menu always boots normally, so a remembered "reboot" can never loop. timeout_ms 0: no countdown. */
void b2l_menu_init(struct b2l_menu *menu, unsigned remembered, uint32_t timeout_ms, enum b2l_menu_start start,
                   uint64_t now_ms);
/* One key (or B2L_KEY_NONE for a clock tick). Pure function of the state, the key and the time. */
enum b2l_menu_action b2l_menu_event(struct b2l_menu *menu, enum b2l_menu_key key, uint64_t now_ms);
/* Whole seconds left before the automatic boot, 0 when no countdown runs. */
unsigned b2l_menu_seconds_left(const struct b2l_menu *menu, uint64_t now_ms);

#endif
