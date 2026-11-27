/* Host test of the boot menu logic (uefi/XnuLoader/BootMenu.c). */
#include "../uefi/XnuLoader/BootMenu.h"
#include <assert.h>
#include <stdio.h>

static struct b2l_menu m;

int
main(void)
{
    /* No key: countdown 5,4,...,1 then a normal boot at exactly 5 s, whatever item was remembered. */
    b2l_menu_init(&m, B2L_MENU_REBOOT, 5000, B2L_START_NORMAL, 1000);
    assert(m.selected == B2L_MENU_REBOOT && b2l_menu_seconds_left(&m, 1000) == 5);
    assert(b2l_menu_event(&m, B2L_KEY_NONE, 1500) == B2L_ACT_NONE);
    assert(b2l_menu_event(&m, B2L_KEY_NONE, 2001) == B2L_ACT_REDRAW && b2l_menu_seconds_left(&m, 2001) == 4);
    assert(b2l_menu_event(&m, B2L_KEY_NONE, 5999) == B2L_ACT_REDRAW);
    assert(b2l_menu_event(&m, B2L_KEY_NONE, 6000) == B2L_ACT_BOOT);
    assert(b2l_menu_event(&m, B2L_KEY_NONE, 9000) == B2L_ACT_NONE);       /* fires once */

    /* Any key stops the countdown; navigation wraps both ways; select returns the item's action. */
    b2l_menu_init(&m, B2L_MENU_COUNT, 5000, B2L_START_NORMAL, 0);
    assert(m.selected == B2L_MENU_BOOT);
    assert(b2l_menu_event(&m, B2L_KEY_UP, 100) == B2L_ACT_REDRAW && m.selected == B2L_MENU_REBOOT && !m.counting);
    assert(b2l_menu_event(&m, B2L_KEY_NONE, 60000) == B2L_ACT_NONE);      /* no boot after a key */
    assert(b2l_menu_event(&m, B2L_KEY_DOWN, 200) == B2L_ACT_REDRAW && m.selected == B2L_MENU_BOOT);
    assert(b2l_menu_event(&m, B2L_KEY_DOWN, 400) == B2L_ACT_REDRAW && m.selected == B2L_MENU_RECOVERY);
    assert(b2l_menu_event(&m, B2L_KEY_SELECT, 500) == B2L_ACT_RECOVERY);
    assert(b2l_menu_event(&m, B2L_KEY_DOWN, 700) == B2L_ACT_REDRAW && m.selected == B2L_MENU_REBOOT);
    assert(b2l_menu_event(&m, B2L_KEY_SELECT, 800) == B2L_ACT_REBOOT);
    assert(b2l_menu_event(&m, B2L_KEY_DOWN, 1000) == B2L_ACT_REDRAW && m.selected == B2L_MENU_BOOT);
    assert(b2l_menu_event(&m, B2L_KEY_SELECT, 1100) == B2L_ACT_BOOT);

    /* Debounce: the same key within 150 ms is dropped, another key is not, the same key later is taken. */
    b2l_menu_init(&m, B2L_MENU_BOOT, 0, B2L_START_NORMAL, 0);
    assert(!m.counting && b2l_menu_seconds_left(&m, 0) == 0);
    assert(b2l_menu_event(&m, B2L_KEY_DOWN, 1000) == B2L_ACT_REDRAW && m.selected == 1);
    assert(b2l_menu_event(&m, B2L_KEY_DOWN, 1149) == B2L_ACT_NONE && m.selected == 1);
    assert(b2l_menu_event(&m, B2L_KEY_UP, 1150) == B2L_ACT_REDRAW && m.selected == 0);
    assert(b2l_menu_event(&m, B2L_KEY_UP, 1301) == B2L_ACT_REDRAW && m.selected == 2);
    /* A clock that goes backwards is not a bounce: the key is taken rather than blocked until the clock catches up. */
    assert(b2l_menu_event(&m, B2L_KEY_UP, 10) == B2L_ACT_REDRAW && m.selected == 1);

    /* BACK only stops the countdown. */
    b2l_menu_init(&m, B2L_MENU_RECOVERY, 5000, B2L_START_NORMAL, 0);
    assert(b2l_menu_event(&m, B2L_KEY_BACK, 10) == B2L_ACT_REDRAW && m.selected == B2L_MENU_RECOVERY && !m.counting);
    assert(b2l_menu_event(&m, B2L_KEY_BACK, 500) == B2L_ACT_NONE);

    /* Forced starts: no countdown; volume-up start puts the cursor on recovery but does not reboot by itself. */
    b2l_menu_init(&m, B2L_MENU_BOOT, 5000, B2L_START_FORCE_MENU, 0);
    assert(!m.counting && b2l_menu_event(&m, B2L_KEY_NONE, 99999) == B2L_ACT_NONE);
    b2l_menu_init(&m, B2L_MENU_BOOT, 5000, B2L_START_FORCE_RECOVERY, 0);
    assert(!m.counting && m.selected == B2L_MENU_RECOVERY && b2l_menu_event(&m, B2L_KEY_NONE, 99999) == B2L_ACT_NONE);
    assert(b2l_menu_event(&m, B2L_KEY_SELECT, 100000) == B2L_ACT_RECOVERY);

    /* An out-of-range remembered value falls back to boot. */
    b2l_menu_init(&m, 77, 5000, B2L_START_NORMAL, 0);
    assert(m.selected == B2L_MENU_BOOT);
    puts("boot menu: countdown, wrap, select, debounce, back, forced starts PASS");
    return 0;
}
