/* SPDX-License-Identifier: BSD-2-Clause
 * Boot menu, UEFI side: keys from the firmware keypad (Silicium KeypadDxe on beyond1lte: volume up = SCAN_UP,
 * volume down = SCAN_DOWN, power = Enter on release, Bixby = Esc), the screen drawn by BootMenuDraw.c and blitted
 * to the GOP (text console as a fallback), actions through the runtime services. The decisions themselves are in BootMenu.c (host tested).
 *
 * Nothing here writes a partition. The remembered choice is a UEFI variable: on this firmware the variable store
 * is emulated in RAM (SiliciumPkg PcdEmuVariableNvModeEnable=TRUE), so it is kept across a return to the menu, not
 * across a reboot.
 */
#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/PrintLib.h>
#include <Library/IoLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiLib.h>
#include <Protocol/GraphicsOutput.h>
#include "BootMenu.h"
#include "BootMenuAdb.h"
#include "BootMenuEfi.h"
#include "BootMenuDraw.h"

#ifndef B2L_MENU_TIMEOUT_S
#define B2L_MENU_TIMEOUT_S 5        /* build option: -D B2L_MENU_TIMEOUT_S=n, 0 = no automatic boot */
#endif
#define B2L_MENU_PROBE_MS 700u      /* a volume key held at start repeats after 500 ms (KeypadDeviceLib) */

/* Exynos 9820 PMU (firmware MemoryMapLib "PMU" 0x15860000, mapped as device memory). Samsung's kernel asks the
 * bootloader for recovery by writing INFORM3 = 0x12345674 (drivers/samsung/sec_reboot.c, SEC_RESET_REASON_RECOVERY)
 * and SYSIP_DAT0 = 0xFF (drivers/soc/samsung/exynos-reboot.c, REBOOT_MODE_RECOVERY) before the software reset.
 * NOT YET TESTED ON THE PHONE: which of the two the S10 bootloader reads has not been checked. */
#define B2L_PMU_BASE          0x15860000ULL
#define B2L_PMU_INFORM3       0x080C
#define B2L_PMU_SYSIP_DAT0    0x0810
/* exynos-reboot.c (non-9810 path): clear RST_STAT, then SWRESET = 1 << 1. Silicium's S5E9820 ResetSystemLib writes
 * 1 (the 9810 trigger bit), which does nothing on the S10: both reboot entries said "Reboot failed" in B1L-0216. */
#define B2L_PMU_RST_STAT      0x0404
#define B2L_PMU_SWRESET       0x3A00
#define B2L_SWRESET_TRIGGER   (1u << 1)
#define B2L_RESET_RECOVERY    0x12345674u
#define B2L_REBOOT_RECOVERY   0xFFu

STATIC EFI_GUID mB2lMenuGuid = { 0x78da37fb, 0x7929, 0x467d, { 0xbc, 0xd8, 0xd3, 0x44, 0x2c, 0x84, 0x3d, 0x39 } };
STATIC CONST CHAR16 *mItems[B2L_MENU_COUNT] = {
    L"Boot iOS", L"Reboot to recovery", L"Reboot",
};
STATIC EFI_GRAPHICS_OUTPUT_PROTOCOL *mGop;
STATIC UINT32 *mPixels;     /* back buffer, one GOP mode in size, kept for the loader's lifetime */

#define B2L_DECON_BASE        0x19030000ULL

#define B2L_DECON_UPDATE_REQ  0x0060
#define B2L_DECON_UPDATE_BITS ((1u << 31) | 0x3fu)
#define B2L_DECON_UPDATE      0x1410

struct stride_mode {
    UINT32 Stride;
    UINT32 RenderW;
    BOOLEAN UseBlt;
    CONST CHAR8 *Name;
};

STATIC CONST struct stride_mode mModes[] = {
    { 1080, 1080, FALSE, "Mode 0: Linear 1080 (Default S10e)" },
    {    0,    0, TRUE,  "Mode 1: GOP Blt (Firmware Blit)" },
    { 1440, 1080, FALSE, "Mode 2: Linear 1440 Stride" },
    { 1080, 1080, FALSE, "Mode 3: DECON HW Win 5/0 Fullscreen" },
};
#define MODE_COUNT (sizeof(mModes) / sizeof(mModes[0]))
STATIC UINTN mModeIndex = 0;

STATIC CHAR8 mDiagBuf[128] = "S10e Display Ready";

STATIC VOID
TriggerDeconUpdate(VOID)
{
    MmioWrite32(B2L_DECON_BASE + B2L_DECON_UPDATE_REQ,
                MmioRead32(B2L_DECON_BASE + B2L_DECON_UPDATE_REQ) | B2L_DECON_UPDATE_BITS);
    MmioWrite32(B2L_DECON_BASE + B2L_DECON_UPDATE, 1);
}

STATIC VOID
ApplyDisplayMode(UINTN ModeIndex)
{
    if (ModeIndex == 3) {
        /* Mode 3: Hardware Window 5 and Window 0 fullscreen activation strictly via mapped DECON at 0x19030000.
         * BG image size 1080x2280 (0x08E80438), window bounds (0,0)-(1079,2279) (0x08E70437).
         */
        MmioWrite32(B2L_DECON_BASE + 0x0200, (2280 << 16) | 1080);
        MmioWrite32(B2L_DECON_BASE + 0x0214, MmioRead32(B2L_DECON_BASE + 0x0214) | (1u << 20) | 1u);
        MmioWrite32(B2L_DECON_BASE + 0x1008, 0);
        MmioWrite32(B2L_DECON_BASE + 0x100C, (2279 << 16) | 1079);
        MmioWrite32(B2L_DECON_BASE + 0x10F8, 0);
        MmioWrite32(B2L_DECON_BASE + 0x10FC, (2279 << 16) | 1079);
        TriggerDeconUpdate();
        AsciiStrCpyS(mDiagBuf, sizeof(mDiagBuf), "DECON HW Win 5/0 Fullscreen applied");
    } else {
        AsciiSPrint(mDiagBuf, sizeof(mDiagBuf), "%a", mModes[ModeIndex].Name);
    }
}

STATIC UINT64
NowMs(VOID)
{
    UINT64 Count, Frequency;
    __asm__ volatile("mrs %0, cntvct_el0" : "=r"(Count));
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(Frequency));
    return Frequency ? DivU64x64Remainder(Count, Frequency / 1000 ? Frequency / 1000 : 1, NULL) : 0;
}

#define B2L_GPA_BASE          0x15850000ULL
#define B2L_GPA0_DAT          0x0004
#define B2L_GPA2_DAT          0x0044

STATIC enum b2l_menu_key
ReadKey(VOID)
{
    EFI_INPUT_KEY Key;
    if (gST->ConIn != NULL && !EFI_ERROR(gST->ConIn->ReadKeyStroke(gST->ConIn, &Key))) {
        if (Key.ScanCode == SCAN_UP) return B2L_KEY_UP;
        if (Key.ScanCode == SCAN_DOWN) return B2L_KEY_DOWN;
        if (Key.UnicodeChar == CHAR_CARRIAGE_RETURN) return B2L_KEY_SELECT;
        if (Key.ScanCode == SCAN_ESC) return B2L_KEY_BACK;
    }

    /* Direct MMIO polling on Exynos 9820 GPA Alive registers */
    UINT32 Gpa0 = MmioRead32(B2L_GPA_BASE + B2L_GPA0_DAT);
    UINT32 Gpa2 = MmioRead32(B2L_GPA_BASE + B2L_GPA2_DAT);

    STATIC BOOLEAN PrevVolUp = FALSE;
    STATIC BOOLEAN PrevVolDown = FALSE;
    STATIC BOOLEAN PrevPower = FALSE;
    STATIC BOOLEAN PrevBixby = FALSE;

    BOOLEAN VolUp   = !(Gpa0 & (1u << 3));
    BOOLEAN VolDown = !(Gpa0 & (1u << 4));
    BOOLEAN Bixby   = !(Gpa0 & (1u << 6));
    BOOLEAN Power   = !(Gpa2 & (1u << 4));

    enum b2l_menu_key Result = B2L_KEY_NONE;
    if (VolUp && !PrevVolUp) Result = B2L_KEY_UP;
    else if (VolDown && !PrevVolDown) Result = B2L_KEY_DOWN;
    else if (Power && !PrevPower) Result = B2L_KEY_SELECT;
    else if (Bixby && !PrevBixby) Result = B2L_KEY_BACK;

    PrevVolUp   = VolUp;
    PrevVolDown = VolDown;
    PrevPower   = Power;
    PrevBixby   = Bixby;

    return Result;
}

/* Waits at most Ms for a key or ADB action; returns at once when one arrives. */
STATIC enum b2l_menu_key
WaitKey(EFI_EVENT Timer, UINTN Ms, B2L_ADB_ACTION *AdbAct, BOOLEAN *AdbRedraw)
{
    (VOID)Timer;
    UINT64 Until = NowMs() + Ms;
    do {
        if (AdbAct) {
            BOOLEAN Dirty = FALSE;
            B2L_ADB_ACTION Act = B2lAdbPoll(&Dirty);
            if (Dirty && AdbRedraw) *AdbRedraw = TRUE;
            if (Act != B2L_ADB_ACT_NONE) {
                *AdbAct = Act;
                return B2L_KEY_NONE;
            }
            if (Dirty) return B2L_KEY_NONE;
        }
        enum b2l_menu_key Key = ReadKey();
        if (Key != B2L_KEY_NONE) return Key;
        gBS->Stall(1000);  /* 1 ms polling slice keeps ADB USB transfers fast */
    } while (NowMs() < Until);
    return B2L_KEY_NONE;
}

/* Graphics when the GOP is there and the back buffer fits in memory; otherwise the firmware text console. */
STATIC VOID
Draw(CONST struct b2l_menu *Menu, UINT64 Now, CONST CHAR8 *Note, CONST CHAR8 *Status)
{
    unsigned Left = b2l_menu_seconds_left(Menu, Now);
    if (mGop == NULL) {
        gBS->LocateProtocol(&gEfiGraphicsOutputProtocolGuid, NULL, (VOID **)&mGop);
    }
    if (mPixels == NULL && mGop != NULL && mGop->Mode != NULL && mGop->Mode->Info != NULL &&
        mGop->Mode->Info->PixelFormat == PixelBlueGreenRedReserved8BitPerColor) {
        UINT32 MaxW = 1440;
        UINT32 MaxH = 3040;
        if (mGop->Mode->Info->HorizontalResolution > MaxW) MaxW = mGop->Mode->Info->HorizontalResolution;
        if (mGop->Mode->Info->VerticalResolution > MaxH) MaxH = mGop->Mode->Info->VerticalResolution;
        mPixels = AllocatePool((UINTN)MaxW * MaxH * 4);
    }
    if (mPixels && mGop && mGop->Mode && mGop->Mode->Info) {
        CONST struct stride_mode *Mode = &mModes[mModeIndex];
        UINT32 RenderW = Mode->RenderW ? Mode->RenderW : (mGop->Mode->Info->HorizontalResolution > 1080 ? 1080 : mGop->Mode->Info->HorizontalResolution);
        UINT32 H = mGop->Mode->Info->VerticalResolution;
        if (H == 0) H = 2280;
        UINT32 Stride = Mode->Stride ? Mode->Stride : mGop->Mode->Info->PixelsPerScanLine;
        if (Stride == 0) Stride = 1440;

        CONST CHAR8 *StatusText = Status ? Status : B2lAdbStatus();
        CONST CHAR8 *ModeNote = Note ? Note : (mModeIndex ? mDiagBuf : NULL);

        struct b2l_menu_view View = { Menu->selected, Left, B2L_MENU_TIMEOUT_S, StatusText, ModeNote };
        b2l_menu_render(mPixels, RenderW, RenderW, H, &View);

        if (Mode->UseBlt) {
            if (!EFI_ERROR(mGop->Blt(mGop, (EFI_GRAPHICS_OUTPUT_BLT_PIXEL *)mPixels, EfiBltBufferToVideo, 0, 0, 0, 0, RenderW, H, 0)))
                return;
        } else if (mGop->Mode->FrameBufferBase) {
            volatile UINT32 *Fb = (volatile UINT32 *)(UINTN)mGop->Mode->FrameBufferBase;
            for (UINT32 y = 0; y < H; y++) {
                CopyMem((VOID *)(Fb + y * Stride), mPixels + y * RenderW, RenderW * 4);
                if (Stride > RenderW) {
                    ZeroMem((VOID *)(Fb + y * Stride + RenderW), (Stride - RenderW) * 4);
                }
            }
            return;
        }
    }
    gST->ConOut->ClearScreen(gST->ConOut);
    Print(L"iOS 26 - Galaxy S10\r\n\r\n");
    for (UINTN I = 0; I < B2L_MENU_COUNT; I++)
        Print(L" %s %s\r\n", I == Menu->selected ? L">" : L" ", mItems[I]);
    Print(L"\r\nVolume +/-: move    Power: select\r\n");
    Print(L"%a\r\n", Status ? Status : B2lAdbStatus());
    if (Left) Print(L"Booting iOS in %u s\r\n", Left);
    if (Note) Print(L"\r\n%a\r\n", Note);
}

STATIC VOID
Remember(UINT8 Item)
{
    /* Best effort: a failure only means the cursor starts on "Boot iOS" next time. */
    (VOID)gRT->SetVariable(L"B2LBootMenuItem", &mB2lMenuGuid,
                           EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS, sizeof(Item), &Item);
}

STATIC unsigned
Remembered(VOID)
{
    UINT8 Item = B2L_MENU_COUNT;
    UINTN Size = sizeof(Item);
    if (EFI_ERROR(gRT->GetVariable(L"B2LBootMenuItem", &mB2lMenuGuid, NULL, &Size, &Item)) || Size != sizeof(Item))
        return B2L_MENU_COUNT;
    return Item;
}

STATIC VOID
Reset(BOOLEAN Recovery)
{
    if (Recovery) {
        MmioWrite32(B2L_PMU_BASE + B2L_PMU_INFORM3, B2L_RESET_RECOVERY);
        MmioWrite32(B2L_PMU_BASE + B2L_PMU_SYSIP_DAT0, B2L_REBOOT_RECOVERY);
    }
    MmioWrite32(B2L_PMU_BASE + B2L_PMU_RST_STAT, 0);
    MmioWrite32(B2L_PMU_BASE + B2L_PMU_SWRESET, B2L_SWRESET_TRIGGER);
    gBS->Stall(500 * 1000);
    gRT->ResetSystem(EfiResetCold, EFI_SUCCESS, 0, NULL);   /* fallback: the firmware's own reset */
    /* Still here: neither reset took. Say so instead of hanging silently. */
    Print(L"\r\nReboot failed. Hold Power to switch the phone off.\r\n");
}

/* Volume up held at start: menu with recovery preselected. Volume down held: menu without countdown. */
STATIC enum b2l_menu_start
ProbeStart(EFI_EVENT Timer)
{
    UINT64 Until = NowMs() + B2L_MENU_PROBE_MS;
    while (NowMs() < Until) {
        enum b2l_menu_key Key = WaitKey(Timer, 50, NULL, NULL);
        if (Key == B2L_KEY_UP) return B2L_START_FORCE_RECOVERY;
        if (Key == B2L_KEY_DOWN) return B2L_START_FORCE_MENU;
    }
    return B2L_START_NORMAL;
}

B2L_BOOT_CHOICE
B2lBootMenu(CONST CHAR8 *Note, BOOLEAN NoCountdown)
{
    EFI_EVENT Timer = NULL;
    struct b2l_menu Menu;
    B2L_BOOT_CHOICE Choice = B2L_CHOICE_BOOT;

    if (gST->ConIn == NULL || gST->ConOut == NULL) return B2L_CHOICE_BOOT;   /* no keypad: old behaviour */
    /* BDS arms a 5-minute watchdog for applications; "Stay here" must not end in a firmware reset. */
    (VOID)gBS->SetWatchdogTimer(0, 0, 0, NULL);
    if (EFI_ERROR(gBS->CreateEvent(EVT_TIMER, 0, NULL, NULL, &Timer))) Timer = NULL;

    enum b2l_menu_start Start = NoCountdown ? B2L_START_FORCE_MENU : ProbeStart(Timer);
    b2l_menu_init(&Menu, Remembered(), B2L_MENU_TIMEOUT_S * 1000u, Start, NowMs());
    ApplyDisplayMode(mModeIndex);
    B2lAdbInit();
    Draw(&Menu, NowMs(), Note, NULL);
    for (;;) {
        B2L_ADB_ACTION AdbAct = B2L_ADB_ACT_NONE;
        BOOLEAN AdbDirty = FALSE;
        enum b2l_menu_key Key = WaitKey(Timer, 100, &AdbAct, &AdbDirty);
        UINT64 Now = NowMs();
        if (AdbAct == B2L_ADB_ACT_BOOT) {
            Choice = B2L_CHOICE_BOOT;
            break;
        }
        if (AdbAct == B2L_ADB_ACT_RECOVERY || AdbAct == B2L_ADB_ACT_REBOOT) {
            BOOLEAN Rec = (AdbAct == B2L_ADB_ACT_RECOVERY);
            Draw(&Menu, Now, Note, Rec ? "Rebooting to recovery..." : "Rebooting...");
            B2lAdbStop();
            Reset(Rec);
            B2lAdbInit();
            b2l_menu_init(&Menu, Menu.selected, 0, B2L_START_FORCE_MENU, NowMs());
            Draw(&Menu, NowMs(), Note, NULL);
            continue;
        }
        if (Key == B2L_KEY_BACK) {
            continue;
        }
        enum b2l_menu_action Action = b2l_menu_event(&Menu, Key, Now);
        if (Action == B2L_ACT_NONE) {
            if (AdbDirty) Draw(&Menu, Now, Note, NULL);
            continue;
        }
        if (Action == B2L_ACT_REDRAW) { Draw(&Menu, Now, Note, NULL); continue; }
        Remember((UINT8)Menu.selected);
        if (Action == B2L_ACT_BOOT) { Choice = B2L_CHOICE_BOOT; break; }
        Draw(&Menu, Now, Note, Action == B2L_ACT_RECOVERY ? "Rebooting to recovery..." : "Rebooting...");
        B2lAdbStop();
        Reset(Action == B2L_ACT_RECOVERY);
        B2lAdbInit();
        b2l_menu_init(&Menu, Menu.selected, 0, B2L_START_FORCE_MENU, NowMs());
        Draw(&Menu, NowMs(), Note, NULL);
    }
    B2lAdbStop();
    if (Timer) gBS->CloseEvent(Timer);
    gST->ConOut->ClearScreen(gST->ConOut);
    return Choice;
}

STATIC CHAR8 mErrorStage[128] = "Init";

VOID
B2lSetErrorStage(CONST CHAR8 *Stage)
{
    if (Stage != NULL) {
        AsciiStrCpyS(mErrorStage, sizeof(mErrorStage), Stage);
    }
}

VOID
B2lBootFailed(EFI_STATUS Status)
{
    if (mPixels && mGop && mGop->Mode && mGop->Mode->FrameBufferBase) {
        UINT32 RenderW = 1080;
        UINT32 H = mGop->Mode->Info->VerticalResolution ? mGop->Mode->Info->VerticalResolution : 2280;
        UINT32 Stride = 1080;
        CHAR8 Msg[128];
        CHAR8 SubMsg[128];
        AsciiSPrint(Msg, sizeof(Msg), "Boot failed: %r", Status);
        AsciiSPrint(SubMsg, sizeof(SubMsg), "At: %a. Press key.", mErrorStage);
        struct b2l_menu_view View = { 0, 0, 0, Msg, SubMsg };
        b2l_menu_render(mPixels, RenderW, RenderW, H, &View);
        volatile UINT32 *Fb = (volatile UINT32 *)(UINTN)mGop->Mode->FrameBufferBase;
        for (UINT32 y = 0; y < H; y++) {
            CopyMem((VOID *)(Fb + y * Stride), mPixels + y * RenderW, RenderW * 4);
        }
    }
    Print(L"\r\nBoot failed: %r (Stage: %a)\r\n", Status, mErrorStage);
    if (mGop && mGop->Mode && mGop->Mode->Info) {
        Print(L"GOP: %ux%u Scan=%u FB=0x%lx\r\n",
            mGop->Mode->Info->HorizontalResolution,
            mGop->Mode->Info->VerticalResolution,
            mGop->Mode->Info->PixelsPerScanLine,
            mGop->Mode->FrameBufferBase);
    }
    Print(L"DECON[0..3]: 0x%08x 0x%08x 0x%08x 0x%08x\r\n",
        MmioRead32(0x19030000), MmioRead32(0x19030004),
        MmioRead32(0x19030010), MmioRead32(0x19030020));
    if (gST->ConIn == NULL) return;
    // Flush pending keystrokes from the menu selection so the error screen does not flash and vanish
    EFI_INPUT_KEY FlushKey;
    while (!EFI_ERROR(gST->ConIn->ReadKeyStroke(gST->ConIn, &FlushKey))) {}
    // Stall 2.5 seconds to ensure the error message is clearly readable on display
    gBS->Stall(2500000);
    while (!EFI_ERROR(gST->ConIn->ReadKeyStroke(gST->ConIn, &FlushKey))) {}
    Print(L"Press any key to return to the menu.\r\n");
    while (WaitKey(NULL, 1000, NULL, NULL) == B2L_KEY_NONE) {}
}
