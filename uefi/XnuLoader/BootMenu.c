/* SPDX-License-Identifier: BSD-2-Clause */
#include "BootMenu.h"

void
b2l_menu_init(struct b2l_menu *menu, unsigned remembered, uint32_t timeout_ms, enum b2l_menu_start start,
              uint64_t now_ms)
{
    menu->selected = remembered < B2L_MENU_COUNT ? remembered : B2L_MENU_BOOT;
    if (start == B2L_START_FORCE_RECOVERY) menu->selected = B2L_MENU_RECOVERY;
    menu->counting = start == B2L_START_NORMAL && timeout_ms != 0;
    menu->timeout_ms = timeout_ms;
    menu->start_ms = now_ms;
    menu->shown_seconds = b2l_menu_seconds_left(menu, now_ms);
    menu->last_key = B2L_KEY_NONE;
    menu->last_key_ms = 0;
}

unsigned
b2l_menu_seconds_left(const struct b2l_menu *menu, uint64_t now_ms)
{
    if (!menu->counting) return 0;
    uint64_t elapsed = now_ms >= menu->start_ms ? now_ms - menu->start_ms : 0;
    if (elapsed >= menu->timeout_ms) return 0;
    return (unsigned)((menu->timeout_ms - elapsed + 999u) / 1000u);
}

enum b2l_menu_action
b2l_menu_event(struct b2l_menu *menu, enum b2l_menu_key key, uint64_t now_ms)
{
    if (key == B2L_KEY_NONE) {
        if (!menu->counting) return B2L_ACT_NONE;
        if (now_ms >= menu->start_ms && now_ms - menu->start_ms >= menu->timeout_ms) {
            menu->counting = 0;
            return B2L_ACT_BOOT;    /* never the remembered item: see BootMenu.h */
        }
        unsigned left = b2l_menu_seconds_left(menu, now_ms);
        if (left == menu->shown_seconds) return B2L_ACT_NONE;
        menu->shown_seconds = left;
        return B2L_ACT_REDRAW;
    }
    /* A bouncing contact or the keypad's own repeat: the same key again too soon is dropped. */
    if (key == menu->last_key && now_ms >= menu->last_key_ms && now_ms - menu->last_key_ms < B2L_MENU_DEBOUNCE_MS)
        return B2L_ACT_NONE;
    menu->last_key = key;
    menu->last_key_ms = now_ms;
    int was_counting = menu->counting;
    menu->counting = 0;             /* any key stops the automatic boot */
    switch (key) {
    case B2L_KEY_UP:
        menu->selected = (menu->selected + B2L_MENU_COUNT - 1u) % B2L_MENU_COUNT;
        return B2L_ACT_REDRAW;
    case B2L_KEY_DOWN:
        menu->selected = (menu->selected + 1u) % B2L_MENU_COUNT;
        return B2L_ACT_REDRAW;
    case B2L_KEY_SELECT:
        switch (menu->selected) {
        case B2L_MENU_BOOT: return B2L_ACT_BOOT;
        case B2L_MENU_RECOVERY: return B2L_ACT_RECOVERY;
        default: return B2L_ACT_REBOOT;
        }
    default:                        /* BACK: only stops the countdown */
        return was_counting ? B2L_ACT_REDRAW : B2L_ACT_NONE;
    }
}
