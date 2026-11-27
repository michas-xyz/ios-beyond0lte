/* SPDX-License-Identifier: BSD-2-Clause
 * Minimal Exynos 9820 DWC3 ADB responder for the XNU loader boot menu.
 */
#ifndef B2L_BOOT_MENU_ADB_H
#define B2L_BOOT_MENU_ADB_H

#include <Uefi.h>

typedef enum {
    B2L_ADB_ACT_NONE = 0,
    B2L_ADB_ACT_BOOT,
    B2L_ADB_ACT_RECOVERY,
    B2L_ADB_ACT_REBOOT,
} B2L_ADB_ACTION;

/* Bring up HS PHY and DWC3 in device mode for ADB. Safe no-op if hardware preflight fails. */
VOID B2lAdbInit(VOID);

/* Service pending DWC3 events and ADB messages without blocking. Sets *StatusChanged when UI text changes. */
B2L_ADB_ACTION B2lAdbPoll(BOOLEAN *StatusChanged);

/* Short ASCII status line suitable for the boot menu screen. */
CONST CHAR8 *B2lAdbStatus(VOID);

/* Cleanly halt DWC3 and restore PHY/PMU registers before booting XNU or resetting. */
VOID B2lAdbStop(VOID);

#endif
