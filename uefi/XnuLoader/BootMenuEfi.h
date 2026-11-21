/* SPDX-License-Identifier: BSD-2-Clause */
#ifndef B2L_BOOT_MENU_EFI_H
#define B2L_BOOT_MENU_EFI_H

typedef enum { B2L_CHOICE_BOOT } B2L_BOOT_CHOICE;

/* Shows the menu and returns only to boot: hold, recovery and reboot are carried out inside. Note (may be NULL) is
 * shown under the menu; NoCountdown after a failed boot so a broken kernel is never retried in a loop.
 * Without a console input or output it returns at once (the loader then boots as it did before the menu). */
B2L_BOOT_CHOICE B2lBootMenu(CONST CHAR8 *Note, BOOLEAN NoCountdown);
/* Leaves the loader's own error messages on screen until a key is pressed. */
VOID B2lBootFailed(EFI_STATUS Status);
VOID B2lSetErrorStage(CONST CHAR8 *Stage);

#endif
