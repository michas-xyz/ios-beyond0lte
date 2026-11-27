/* SPDX-License-Identifier: BSD-2-Clause
 * Minimal Exynos 9820 HS PHY + DWC3 USB device ADB responder for the XNU loader boot menu.
 * Hardware setup/teardown sequence follows beyond2lte-uefi B2LDwc3Probe:
 *   HS PHY base  0x10B00000, DWC3 base 0x10C00000, PMU USB PHY ctrl 0x1586072C.
 */
#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/IoLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include "BootMenuAdb.h"

#define B2L_DWC3_BASE                0x10C00000ULL
#define B2L_USB_PHY_BASE             0x10B00000ULL
#define B2L_PMU_BASE                 0x15860000ULL

#define B2L_PMU_USB_PHY_CTRL         0x072C
#define B2L_PMU_USB_PHY_ENABLE       BIT0

#define EXYNOS_USBCON_CTRL_VER       0x00
#define EXYNOS_USBCON_LINK_CTRL      0x04
#define EXYNOS_USBCON_LINK_PORT      0x08
#define EXYNOS_USBCON_CLKRST         0x20
#define EXYNOS_USBCON_UTMI           0x50
#define EXYNOS_USBCON_HSP            0x54
#define EXYNOS_USBCON_HSP_TUNE       0x58
#define EXYNOS_USBCON_HSP_TEST       0x5C

#define EXYNOS_USBCON_CTRL_VER_03    0x00030000U

#define LINKCTRL_DIS_QACT_LINKGATE   BIT12
#define LINKCTRL_DIS_QACT_ID0        BIT11
#define LINKCTRL_DIS_QACT_VBUS_VALID BIT10
#define LINKCTRL_DIS_QACT_BVALID     BIT9
#define LINKCTRL_FORCE_QACT          BIT8
#define LINKCTRL_BUS_FILTER_BYPASS   (0xFU << 4)
#define LINKCTRL_QACT_DISABLE_MASK   (LINKCTRL_DIS_QACT_LINKGATE | \
                                      LINKCTRL_DIS_QACT_ID0 | \
                                      LINKCTRL_DIS_QACT_VBUS_VALID | \
                                      LINKCTRL_DIS_QACT_BVALID)

#define LINKPORT_HUB_PORT_SEL_OCD_U3 BIT3
#define LINKPORT_HUB_PORT_SEL_OCD_U2 BIT2

#define CLKRST_PHY20_SW_RST          BIT13
#define CLKRST_PHY20_RST_SEL         BIT12
#define CLKRST_PHY30_SW_RST          BIT3
#define CLKRST_PHY30_RST_SEL         BIT2
#define CLKRST_PORT_RST              BIT1

#define UTMI_FORCE_VBUSVALID         BIT5
#define UTMI_FORCE_BVALID            BIT4
#define UTMI_DP_PULLDOWN             BIT3
#define UTMI_DM_PULLDOWN             BIT2
#define UTMI_FORCE_SUSPEND           BIT1
#define UTMI_FORCE_SLEEP             BIT0

#define HSP_VBUSVLDEXTSEL            BIT13
#define HSP_VBUSVLDEXT               BIT12
#define HSP_EN_UTMISUSPEND           BIT9
#define HSP_COMMONONN                BIT8

#define HSP_TUNE_TXVREF_MASK         (0xFU << 28)
#define HSP_TUNE_TXRES_MASK          (0x3U << 21)
#define HSP_TUNE_TXPREEMPA_MASK      (0x3U << 18)
#define HSP_TUNE_SQRX_MASK           (0x7U << 8)
#define HSP_TUNE_COMPDIS_MASK        (0x7U << 0)
#define HSP_TUNE_DEVICE_MASK         (HSP_TUNE_TXVREF_MASK | \
                                      HSP_TUNE_TXRES_MASK | \
                                      HSP_TUNE_TXPREEMPA_MASK | \
                                      HSP_TUNE_SQRX_MASK | \
                                      HSP_TUNE_COMPDIS_MASK)
#define HSP_TUNE_DEVICE_VALUE        ((0xCU << 28) | (0x3U << 21) | \
                                      (0x3U << 18) | (0x3U << 8) | 0x7U)
#define HSP_TEST_SIDDQ               BIT24

#define DWC3_GCTL                    0xC110
#define DWC3_GSTS                    0xC118
#define DWC3_GSNPSID                 0xC120
#define DWC3_GHWPARAMS0              0xC140
#define DWC3_GUSB2PHYCFG0            0xC200
#define DWC3_GUSB3PIPECTL0           0xC2C0
#define DWC3_GEVNTADRLO0             0xC400
#define DWC3_GEVNTADRHI0             0xC404
#define DWC3_GEVNTSIZ0               0xC408
#define DWC3_GEVNTCOUNT0             0xC40C
#define DWC3_DCFG                    0xC700
#define DWC3_DCTL                    0xC704
#define DWC3_DEVTEN                  0xC708
#define DWC3_DSTS                    0xC70C
#define DWC3_DALEPENA                0xC720
#define DWC3_DEP_BASE(Ep)            (0xC800U + ((Ep) * 0x10U))
#define DWC3_DEPCMDPAR2(Ep)          (DWC3_DEP_BASE(Ep) + 0x00U)
#define DWC3_DEPCMDPAR1(Ep)          (DWC3_DEP_BASE(Ep) + 0x04U)
#define DWC3_DEPCMDPAR0(Ep)          (DWC3_DEP_BASE(Ep) + 0x08U)
#define DWC3_DEPCMD(Ep)              (DWC3_DEP_BASE(Ep) + 0x0CU)

#define DWC3_GSNPSID_MASK            0xFFFF0000U
#define DWC3_IP_DWC31                0x33310000U
#define DWC3_GCTL_PRTCAP_SHIFT       12
#define DWC3_GCTL_PRTCAP_MASK        3U
#define DWC3_GCTL_PRTCAP_DEVICE      2U
#define DWC3_GCTL_PRTCAP_OTG         3U
#define DWC3_GCTL_PRTCAP_FIELD_MASK  (3U << DWC3_GCTL_PRTCAP_SHIFT)
#define DWC3_GCTL_CORESOFTRESET      BIT11
#define DWC3_GCTL_SCALEDOWN_MASK     (3U << 4)
#define DWC3_GCTL_DISSCRAMBLE        BIT3
#define DWC3_GCTL_GBLHIBERNATIONEN   BIT1
#define DWC3_GCTL_DSBLCLKGTNG        BIT0
#define DWC3_GSTS_CURMOD_MASK        3U
#define DWC3_GSTS_CURMOD_DEVICE      0U
#define DWC3_GHWPARAMS0_AWIDTH_SHIFT 24
#define DWC3_GUSB2PHYCFG_PHYSOFTRST  BIT31
#define DWC3_GUSB2PHYCFG_U2_FREECLK  BIT30
#define DWC3_GUSB2PHYCFG_USBTRD_MASK (0xFU << 10)
#define DWC3_GUSB2PHYCFG_USBTRD_16   (5U << 10)
#define DWC3_GUSB2PHYCFG_ENBLSLPM    BIT8
#define DWC3_GUSB2PHYCFG_SOFTRST     BIT31
#define DWC3_GUSB2PHYCFG_SUSPHY      BIT6
#define DWC3_GUSB2PHYCFG_ULPI_UTMI   BIT4
#define DWC3_GUSB2PHYCFG_PHYIF_MASK  BIT3
#define DWC3_GUSB2PHYCFG_PHYIF_16    BIT3
#define DWC3_GUSB3PIPECTL_PHYSOFTRST BIT31
#define DWC3_GUSB3PIPECTL_SUSPHY     BIT17

#define DWC3_GEVNTSIZ_INTMASK        BIT31
#define DWC3_GEVNTSIZ_SIZE_MASK      0xFFFFU
#define DWC3_GEVNTCOUNT_MASK         0xFFFCU
#define DWC3_GEVNTCOUNT_EHB          BIT31

#define DWC3_DCTL_RUN_STOP           BIT31
#define DWC3_DCTL_CSFTRST            BIT30
#define DWC3_DCTL_ULSTCHNGREQ_SHIFT  5
#define DWC3_DCTL_ULSTCHNGREQ_MASK   (0xFU << DWC3_DCTL_ULSTCHNGREQ_SHIFT)
#define DWC3_DCTL_ULSTCHNGREQ(Value) (((Value) << DWC3_DCTL_ULSTCHNGREQ_SHIFT) & DWC3_DCTL_ULSTCHNGREQ_MASK)
#define DWC3_DCTL_ULSTCHNG_RX_DETECT DWC3_DCTL_ULSTCHNGREQ(0x5U)

#define DWC3_DCFG_DEVSPD_MASK        7U
#define DWC3_DCFG_DEVSPD_HS          0U
#define DWC3_DCFG_DEVADDR_SHIFT      3
#define DWC3_DCFG_DEVADDR_MASK       (0x7FU << DWC3_DCFG_DEVADDR_SHIFT)
#define DWC3_DCFG_DEVADDR(Addr)      (((Addr) << DWC3_DCFG_DEVADDR_SHIFT) & DWC3_DCFG_DEVADDR_MASK)
#define DWC3_DSTS_DEVCTRLHLT         BIT22
#define DWC3_DSTS_CONNECTSPD_MASK    7U

#define DWC3_DEPCMD_CMDACT           BIT10
#define DWC3_DEPCMD_HIPRI_FORCERM    BIT11
#define DWC3_DEPCMD_STATUS_MASK      (0xFU << 12)
#define DWC3_DEPCMD_PARAM_SHIFT      16
#define DWC3_DEPCMD_PARAM(Value)     ((Value) << DWC3_DEPCMD_PARAM_SHIFT)
#define DWC3_DEPCMD_GET_RSC_IDX(Cmd) (((Cmd) >> 16) & 0x7FU)
#define DWC3_DEPCMD_SETEPCONFIG      0x01U
#define DWC3_DEPCMD_SETTRANSFRESOURCE 0x02U
#define DWC3_DEPCMD_SETSTALL         0x04U
#define DWC3_DEPCMD_CLEARSTALL       0x05U
#define DWC3_DEPCMD_STARTTRANSFER    0x06U
#define DWC3_DEPCMD_ENDTRANSFER      0x08U
#define DWC3_DEPCMD_DEPSTARTCFG      0x09U

#define DWC3_DEPCFG_XFER_COMPLETE_EN    BIT8
#define DWC3_DEPCFG_XFER_IN_PROGRESS_EN BIT9
#define DWC3_DEPCFG_XFER_NOT_READY_EN   BIT10
#define DWC3_DEPCFG_EP_NUMBER(Ep)       (((Ep) & 0x1FU) << 25)
#define DWC3_DEPCFG_EP_TYPE(Type)       (((Type) & 0x3U) << 1)
#define DWC3_DEPCFG_MAX_PACKET_SIZE(S)  (((S) & 0x7FFU) << 3)
#define DWC3_DEPCFG_FIFO_NUMBER(N)      (((N) & 0x1FU) << 17)
#define DWC3_DEPCFG_ACTION_INIT         (0U << 30)
#define DWC3_DEPCFG_ACTION_MODIFY       (2U << 30)
#define DWC3_DEPXFERCFG_NUM_XFER_RES(N) ((N) & 0xFFFFU)

#define DWC3_TRB_SIZE_MASK           0x00FFFFFFU
#define DWC3_TRB_CTRL_HWO            BIT0
#define DWC3_TRB_CTRL_LST            BIT1
#define DWC3_TRB_CTRL_TRBCTL(Type)   (((Type) & 0x3FU) << 4)
#define DWC3_TRB_CTRL_ISP_IMI        BIT10
#define DWC3_TRB_CTRL_IOC            BIT11
#define DWC3_TRBCTL_NORMAL           DWC3_TRB_CTRL_TRBCTL(1)
#define DWC3_TRBCTL_CONTROL_SETUP    DWC3_TRB_CTRL_TRBCTL(2)
#define DWC3_TRBCTL_CONTROL_STATUS2  DWC3_TRB_CTRL_TRBCTL(3)
#define DWC3_TRBCTL_CONTROL_STATUS3  DWC3_TRB_CTRL_TRBCTL(4)
#define DWC3_TRBCTL_CONTROL_DATA     DWC3_TRB_CTRL_TRBCTL(5)

#define DWC3_DEPEVT_XFERCOMPLETE         0x01U
#define DWC3_DEPEVT_XFERNOTREADY         0x03U
#define DWC3_DEPEVT_STATUS_CONTROL_STATUS 2U

#define DWC3_DEVTEN_EVNTOVERFLOWEN   BIT11
#define DWC3_DEVTEN_CMDCMPLTEN       BIT10
#define DWC3_DEVTEN_ERRTICERREN      BIT9
#define DWC3_DEVTEN_WKUPEVTEN        BIT4
#define DWC3_DEVTEN_CONNECTDONEEN    BIT2
#define DWC3_DEVTEN_USBRSTEN         BIT1
#define DWC3_DEVTEN_DISCONNEVTEN     BIT0
#define DWC3_ADB_DEVICE_EVENTS       (DWC3_DEVTEN_EVNTOVERFLOWEN | \
                                      DWC3_DEVTEN_ERRTICERREN | \
                                      DWC3_DEVTEN_WKUPEVTEN | \
                                      DWC3_DEVTEN_CONNECTDONEEN | \
                                      DWC3_DEVTEN_USBRSTEN | \
                                      DWC3_DEVTEN_DISCONNEVTEN)

#define USB_REQ_GET_STATUS           0x00U
#define USB_REQ_CLEAR_FEATURE        0x01U
#define USB_REQ_SET_FEATURE          0x03U
#define USB_REQ_SET_ADDRESS          0x05U
#define USB_REQ_GET_DESCRIPTOR       0x06U
#define USB_REQ_GET_CONFIGURATION    0x08U
#define USB_REQ_SET_CONFIGURATION    0x09U
#define USB_REQ_GET_INTERFACE        0x0AU
#define USB_REQ_SET_INTERFACE        0x0BU
#define USB_REQ_TYPE_DIRECTION_IN    BIT7
#define USB_REQ_TYPE_TYPE_MASK       0x60U
#define USB_REQ_TYPE_STANDARD        0x00U
#define USB_REQ_TYPE_RECIPIENT_MASK  0x1FU
#define USB_REQ_RECIPIENT_ENDPOINT   0x02U
#define USB_FEATURE_ENDPOINT_HALT    0x00U
#define USB_DESC_TYPE_DEVICE         0x01U
#define USB_DESC_TYPE_CONFIGURATION  0x02U
#define USB_DESC_TYPE_STRING         0x03U
#define USB_DESC_TYPE_DEVICE_QUALIFIER 0x06U
#define USB_DESC_TYPE_OTHER_SPEED    0x07U

/* ADB wire protocol constants */
#define ADB_CMD_CNXN                 0x4E584E43U
#define ADB_CMD_OPEN                 0x4E45504FU
#define ADB_CMD_OKAY                 0x59414B4FU
#define ADB_CMD_CLSE                 0x45534C43U
#define ADB_CMD_WRTE                 0x45545257U
#define ADB_VERSION_MAX              0x01000001U
#define ADB_MAXDATA                  4096U

/* DMA layout (< 4 GiB, 60 KiB total) */
#define DWC3_EVENT_BUFFER_SIZE       EFI_PAGE_SIZE
#define DWC3_EVENT_WORD_COUNT        (DWC3_EVENT_BUFFER_SIZE / sizeof(UINT32))
#define B2L_ADB_EVENT_OFFSET         0U
#define B2L_ADB_EP0_TRB_OFFSET       EFI_PAGE_SIZE
#define B2L_ADB_EP1_TRB_OFFSET       (EFI_PAGE_SIZE + 0x40U)
#define B2L_ADB_EP2_TRB_OFFSET       (EFI_PAGE_SIZE + 0x80U)
#define B2L_ADB_EP3_TRB_OFFSET       (EFI_PAGE_SIZE + 0xC0U)
#define B2L_ADB_CONTROL_OFFSET       (EFI_PAGE_SIZE * 2U)
#define B2L_ADB_RX_OFFSET            (EFI_PAGE_SIZE * 3U)
#define B2L_ADB_RX_BYTES             (EFI_PAGE_SIZE * 4U)
#define B2L_ADB_TX_OFFSET            (EFI_PAGE_SIZE * 7U)
#define B2L_ADB_TX_BYTES             (EFI_PAGE_SIZE * 2U)
#define B2L_ADB_DMA_BYTES            (B2L_ADB_TX_OFFSET + B2L_ADB_TX_BYTES)

#define B2L_ADB_EP0_MAX_PACKET       64U
#define B2L_ADB_CMD_TIMEOUT_US       100000U
#define B2L_ADB_STOP_TIMEOUT_MS      500U
#define B2L_ADB_TX_TIMEOUT_MS        500U

#pragma pack(1)
typedef struct {
    UINT32 BufferPointerLow;
    UINT32 BufferPointerHigh;
    UINT32 Size;
    UINT32 Control;
} B2L_DWC3_TRB;

typedef struct {
    UINT8  RequestType;
    UINT8  Request;
    UINT16 Value;
    UINT16 Index;
    UINT16 Length;
} B2L_USB_SETUP;

typedef struct {
    UINT32 Command;
    UINT32 Arg0;
    UINT32 Arg1;
    UINT32 DataLength;
    UINT32 DataCrc32;
    UINT32 Magic;
} B2L_ADB_MSG;
#pragma pack()

typedef struct {
    UINT32 Gctl;
    UINT32 Usb2PhyCfg;
    UINT32 Usb3PipeCtl;
    UINT32 EventAddressLow;
    UINT32 EventAddressHigh;
    UINT32 EventSize;
    UINT32 DeviceEventEnable;
    UINT32 DeviceConfig;
    UINT32 DeviceControl;
} B2L_DWC3_SAVED;

typedef struct {
    UINT32 PmuUsbPhy;
    UINT32 LinkControl;
    UINT32 LinkPort;
    UINT32 ClockReset;
    UINT32 Utmi;
    UINT32 HsPhy;
    UINT32 HsTune;
    UINT32 HsTest;
} B2L_PHY_SAVED;

typedef enum {
    EP0_SETUP_ARMED = 0,
    EP0_DATA_IN,
    EP0_WAIT_STATUS,
    EP0_STATUS,
} B2L_EP0_STATE;

typedef enum {
    RX_WAIT_HEADER = 0,
    RX_WAIT_PAYLOAD,
} B2L_ADB_RX_STATE;

typedef struct {
    BOOLEAN              Active;
    BOOLEAN              ControllerRunning;
    BOOLEAN              PhySavedValid;
    BOOLEAN              BulkInitialized;
    BOOLEAN              Configured;
    BOOLEAN              AdbConnected;
    BOOLEAN              StatusDirty;
    EFI_PHYSICAL_ADDRESS DmaAddress;
    VOID                *Dma;
    volatile UINT32     *EventWords;
    UINTN                EventReadPos;
    B2L_DWC3_TRB        *EpTrb[4];
    UINT8               *ControlBuffer;
    UINT8               *RxBuffer;
    UINT8               *TxBuffer;
    BOOLEAN              EpActive[4];
    UINT32               EpResource[4];
    UINT32               BulkMaxPacket;
    UINT8                ConfigValue;
    B2L_EP0_STATE        Ep0State;
    BOOLEAN              Ep0ThreeStage;
    B2L_ADB_RX_STATE     RxState;
    B2L_ADB_MSG          RxMsg;
    UINT8                PayloadBuf[ADB_MAXDATA + 1];
    UINT32               PayloadHave;
    UINT32               NextLocalId;
    B2L_ADB_ACTION       PendingAction;
    B2L_DWC3_SAVED       SavedDwc3;
    B2L_PHY_SAVED        SavedPhy;
    CHAR8                StatusLine[64];
} B2L_ADB_CTX;

STATIC B2L_ADB_CTX mAdb;

STATIC UINT32 Dwc3Read(UINTN Off) { return MmioRead32((UINTN)(B2L_DWC3_BASE + Off)); }
STATIC UINT32 PhyRead(UINTN Off)  { return MmioRead32((UINTN)(B2L_USB_PHY_BASE + Off)); }
STATIC UINT32 PmuRead(UINTN Off)  { return MmioRead32((UINTN)(B2L_PMU_BASE + Off)); }

STATIC VOID Dwc3Write(UINTN Off, UINT32 Val) { MmioWrite32((UINTN)(B2L_DWC3_BASE + Off), Val); }
STATIC VOID PhyWrite(UINTN Off, UINT32 Val)  { MmioWrite32((UINTN)(B2L_USB_PHY_BASE + Off), Val); }
STATIC VOID PmuWrite(UINTN Off, UINT32 Val)  { MmioWrite32((UINTN)(B2L_PMU_BASE + Off), Val); }

STATIC VOID
CleanInvalidateDcache(IN VOID *Buffer, IN UINTN Length)
{
    UINT64 Ctr;
    __asm__ volatile("mrs %0, ctr_el0" : "=r"(Ctr));
    UINT64 Line = 4ULL << ((Ctr >> 16) & 15);
    if (Line < 16) Line = 64;
    UINTN Start = ((UINTN)Buffer) & ~(UINTN)(Line - 1);
    UINTN End = ((UINTN)Buffer) + Length;
    for (UINTN At = Start; At < End; At += (UINTN)Line) {
        __asm__ volatile("dc civac, %0" :: "r"(At) : "memory");
    }
    __asm__ volatile("dsb sy" ::: "memory");
}

STATIC VOID
SetStatus(CONST CHAR8 *Text)
{
    AsciiStrCpyS(mAdb.StatusLine, sizeof(mAdb.StatusLine), Text);
    mAdb.StatusDirty = TRUE;
}

STATIC VOID
SavePhyState(B2L_PHY_SAVED *Saved)
{
    Saved->PmuUsbPhy   = PmuRead(B2L_PMU_USB_PHY_CTRL);
    Saved->LinkControl = PhyRead(EXYNOS_USBCON_LINK_CTRL);
    Saved->LinkPort    = PhyRead(EXYNOS_USBCON_LINK_PORT);
    Saved->ClockReset  = PhyRead(EXYNOS_USBCON_CLKRST);
    Saved->Utmi        = PhyRead(EXYNOS_USBCON_UTMI);
    Saved->HsPhy       = PhyRead(EXYNOS_USBCON_HSP);
    Saved->HsTune      = PhyRead(EXYNOS_USBCON_HSP_TUNE);
    Saved->HsTest      = PhyRead(EXYNOS_USBCON_HSP_TEST);
}

STATIC VOID
IsolatePhyForRetry(VOID)
{
    UINT32 V;
    V = PhyRead(EXYNOS_USBCON_CLKRST) | CLKRST_PHY20_SW_RST | CLKRST_PHY20_RST_SEL |
        CLKRST_PHY30_SW_RST | CLKRST_PHY30_RST_SEL;
    PhyWrite(EXYNOS_USBCON_CLKRST, V);
    V = PhyRead(EXYNOS_USBCON_UTMI) | UTMI_FORCE_SUSPEND | UTMI_FORCE_SLEEP;
    PhyWrite(EXYNOS_USBCON_UTMI, V);
    V = PhyRead(EXYNOS_USBCON_HSP_TEST) | HSP_TEST_SIDDQ;
    PhyWrite(EXYNOS_USBCON_HSP_TEST, V);
    gBS->Stall(1000);
}

STATIC BOOLEAN
EnableHsPhyAttempt(CONST B2L_PHY_SAVED *Saved)
{
    UINTN  Poll;
    UINT32 Value;

    PmuWrite(B2L_PMU_USB_PHY_CTRL, Saved->PmuUsbPhy | B2L_PMU_USB_PHY_ENABLE);
    for (Poll = 0; Poll < 20; Poll++) {
        if ((PmuRead(B2L_PMU_USB_PHY_CTRL) & B2L_PMU_USB_PHY_ENABLE) != 0) break;
        gBS->Stall(100);
    }
    if ((PmuRead(B2L_PMU_USB_PHY_CTRL) & B2L_PMU_USB_PHY_ENABLE) == 0) return FALSE;
    gBS->Stall(1000);

    Value  = PhyRead(EXYNOS_USBCON_LINK_CTRL);
    Value |= LINKCTRL_QACT_DISABLE_MASK;
    Value &= ~LINKCTRL_FORCE_QACT;
    gBS->Stall(500);
    PhyWrite(EXYNOS_USBCON_LINK_CTRL, Value);
    gBS->Stall(500);
    PhyWrite(EXYNOS_USBCON_LINK_CTRL, PhyRead(EXYNOS_USBCON_LINK_CTRL) | LINKCTRL_FORCE_QACT);

    Value = PhyRead(EXYNOS_USBCON_CLKRST) | CLKRST_PHY20_SW_RST | CLKRST_PHY20_RST_SEL |
            CLKRST_PHY30_SW_RST | CLKRST_PHY30_RST_SEL;
    PhyWrite(EXYNOS_USBCON_CLKRST, Value);

    Value = PhyRead(EXYNOS_USBCON_UTMI) &
            ~(UTMI_FORCE_SUSPEND | UTMI_FORCE_SLEEP | UTMI_DP_PULLDOWN | UTMI_DM_PULLDOWN);
    PhyWrite(EXYNOS_USBCON_UTMI, Value);

    PhyWrite(EXYNOS_USBCON_HSP, PhyRead(EXYNOS_USBCON_HSP) | HSP_EN_UTMISUSPEND | HSP_COMMONONN);
    gBS->Stall(100);

    PhyWrite(EXYNOS_USBCON_LINK_CTRL, PhyRead(EXYNOS_USBCON_LINK_CTRL) | LINKCTRL_BUS_FILTER_BYPASS);
    PhyWrite(EXYNOS_USBCON_UTMI, PhyRead(EXYNOS_USBCON_UTMI) | UTMI_FORCE_BVALID | UTMI_FORCE_VBUSVALID);
    PhyWrite(EXYNOS_USBCON_HSP, PhyRead(EXYNOS_USBCON_HSP) | HSP_VBUSVLDEXTSEL | HSP_VBUSVLDEXT);

    Value  = PhyRead(EXYNOS_USBCON_HSP_TUNE) & ~HSP_TUNE_DEVICE_MASK;
    Value |= HSP_TUNE_DEVICE_VALUE;
    PhyWrite(EXYNOS_USBCON_HSP_TUNE, Value);

    PhyWrite(EXYNOS_USBCON_HSP_TEST, PhyRead(EXYNOS_USBCON_HSP_TEST) & ~HSP_TEST_SIDDQ);
    gBS->Stall(10);

    Value  = PhyRead(EXYNOS_USBCON_CLKRST) | CLKRST_PHY20_RST_SEL;
    Value &= ~(CLKRST_PHY20_SW_RST | CLKRST_PHY30_SW_RST | CLKRST_PORT_RST);
    PhyWrite(EXYNOS_USBCON_CLKRST, Value);
    gBS->Stall(75);

    PhyWrite(EXYNOS_USBCON_LINK_PORT,
             PhyRead(EXYNOS_USBCON_LINK_PORT) | LINKPORT_HUB_PORT_SEL_OCD_U3 | LINKPORT_HUB_PORT_SEL_OCD_U2);

    for (Poll = 0; Poll < 20; Poll++) {
        if (((PmuRead(B2L_PMU_USB_PHY_CTRL) & B2L_PMU_USB_PHY_ENABLE) != 0) &&
            ((PhyRead(EXYNOS_USBCON_HSP_TEST) & HSP_TEST_SIDDQ) == 0)) {
            return TRUE;
        }
        gBS->Stall(100);
    }
    return FALSE;
}

STATIC BOOLEAN
EnableHsPhy(CONST B2L_PHY_SAVED *Saved)
{
    if ((Saved->PmuUsbPhy & B2L_PMU_USB_PHY_ENABLE) != 0) {
        IsolatePhyForRetry();
    }
    for (UINTN Attempt = 0; Attempt < 3; Attempt++) {
        if (EnableHsPhyAttempt(Saved)) return TRUE;
        if ((PmuRead(B2L_PMU_USB_PHY_CTRL) & B2L_PMU_USB_PHY_ENABLE) != 0) {
            IsolatePhyForRetry();
        } else {
            gBS->Stall(1000);
        }
    }
    return FALSE;
}

STATIC VOID
RestoreHsPhy(CONST B2L_PHY_SAVED *Saved)
{
    PhyWrite(EXYNOS_USBCON_UTMI, PhyRead(EXYNOS_USBCON_UTMI) | UTMI_FORCE_SUSPEND | UTMI_FORCE_SLEEP);
    PhyWrite(EXYNOS_USBCON_HSP_TEST, PhyRead(EXYNOS_USBCON_HSP_TEST) | HSP_TEST_SIDDQ);
    PhyWrite(EXYNOS_USBCON_LINK_CTRL, PhyRead(EXYNOS_USBCON_LINK_CTRL) & ~LINKCTRL_FORCE_QACT);
    PmuWrite(B2L_PMU_USB_PHY_CTRL, Saved->PmuUsbPhy);
    gBS->Stall(10);
    PhyWrite(EXYNOS_USBCON_HSP_TEST, Saved->HsTest);
    PhyWrite(EXYNOS_USBCON_HSP_TUNE, Saved->HsTune);
    PhyWrite(EXYNOS_USBCON_HSP, Saved->HsPhy);
    PhyWrite(EXYNOS_USBCON_UTMI, Saved->Utmi);
    PhyWrite(EXYNOS_USBCON_CLKRST, Saved->ClockReset);
    PhyWrite(EXYNOS_USBCON_LINK_PORT, Saved->LinkPort);
    PhyWrite(EXYNOS_USBCON_LINK_CTRL, Saved->LinkControl);
}

STATIC BOOLEAN
PreflightSafe(EFI_PHYSICAL_ADDRESS DmaAddr)
{
    UINT32 CoreId = Dwc3Read(DWC3_GSNPSID);
    UINT32 Gctl   = Dwc3Read(DWC3_GCTL);
    UINT32 Gsts   = Dwc3Read(DWC3_GSTS);
    UINT32 Hwp0   = Dwc3Read(DWC3_GHWPARAMS0);
    UINT32 Dctl   = Dwc3Read(DWC3_DCTL);
    UINT32 Dsts   = Dwc3Read(DWC3_DSTS);
    UINT32 Cnt    = Dwc3Read(DWC3_GEVNTCOUNT0);
    UINT32 PrtCap = (Gctl >> DWC3_GCTL_PRTCAP_SHIFT) & DWC3_GCTL_PRTCAP_MASK;

    if ((CoreId & DWC3_GSNPSID_MASK) != DWC3_IP_DWC31) return FALSE;
    if ((Gctl & DWC3_GCTL_CORESOFTRESET) != 0) return FALSE;
    if (PrtCap != DWC3_GCTL_PRTCAP_DEVICE && PrtCap != DWC3_GCTL_PRTCAP_OTG) return FALSE;
    if ((Gsts & DWC3_GSTS_CURMOD_MASK) != DWC3_GSTS_CURMOD_DEVICE) return FALSE;
    if ((Dctl & (DWC3_DCTL_RUN_STOP | DWC3_DCTL_CSFTRST)) != 0 || (Dsts & DWC3_DSTS_DEVCTRLHLT) == 0) return FALSE;
    if (Dwc3Read(DWC3_DALEPENA) != 0 || Dwc3Read(DWC3_DEVTEN) != 0) return FALSE;
    if ((Dwc3Read(DWC3_DEPCMD(0)) & DWC3_DEPCMD_CMDACT) != 0 ||
        (Dwc3Read(DWC3_DEPCMD(1)) & DWC3_DEPCMD_CMDACT) != 0) return FALSE;
    if ((Cnt & (DWC3_GEVNTCOUNT_MASK | DWC3_GEVNTCOUNT_EHB)) != 0) return FALSE;
    if ((DmaAddr >> 32) != 0 && (Hwp0 >> DWC3_GHWPARAMS0_AWIDTH_SHIFT) < 64) return FALSE;
    if (PhyRead(EXYNOS_USBCON_CTRL_VER) != EXYNOS_USBCON_CTRL_VER_03) return FALSE;
    if ((Dwc3Read(DWC3_GUSB2PHYCFG0) & (DWC3_GUSB2PHYCFG_SOFTRST | DWC3_GUSB2PHYCFG_SUSPHY)) != 0) return FALSE;
    return TRUE;
}

STATIC EFI_STATUS
SendEpCmd(UINTN Ep, UINT32 Cmd, UINT32 P0, UINT32 P1, UINT32 P2, UINT32 *OutCmd)
{
    Dwc3Write(DWC3_DEPCMDPAR0(Ep), P0);
    Dwc3Write(DWC3_DEPCMDPAR1(Ep), P1);
    Dwc3Write(DWC3_DEPCMDPAR2(Ep), P2);
    Dwc3Write(DWC3_DEPCMD(Ep), Cmd | DWC3_DEPCMD_CMDACT);
    UINT32 Val = 0;
    for (UINTN Us = 0; Us < B2L_ADB_CMD_TIMEOUT_US; Us++) {
        Val = Dwc3Read(DWC3_DEPCMD(Ep));
        if ((Val & DWC3_DEPCMD_CMDACT) == 0) break;
        gBS->Stall(1);
    }
    if (OutCmd) *OutCmd = Val;
    if ((Val & DWC3_DEPCMD_CMDACT) != 0) return EFI_TIMEOUT;
    if ((Val & DWC3_DEPCMD_STATUS_MASK) != 0) return EFI_DEVICE_ERROR;
    return EFI_SUCCESS;
}

STATIC EFI_STATUS
StartXfer(UINTN Ep, EFI_PHYSICAL_ADDRESS BufAddr, UINT32 Len, UINT32 Type)
{
    if (Ep >= 4 || mAdb.EpActive[Ep]) return EFI_ALREADY_STARTED;
    B2L_DWC3_TRB *Trb = mAdb.EpTrb[Ep];
    EFI_PHYSICAL_ADDRESS TrbAddr = mAdb.DmaAddress + B2L_ADB_EP0_TRB_OFFSET + (Ep * 0x40U);
    ZeroMem(Trb, sizeof(*Trb));
    Trb->BufferPointerLow  = (UINT32)BufAddr;
    Trb->BufferPointerHigh = (UINT32)(BufAddr >> 32);
    Trb->Size              = Len & DWC3_TRB_SIZE_MASK;
    Trb->Control           = Type | DWC3_TRB_CTRL_HWO | DWC3_TRB_CTRL_ISP_IMI | DWC3_TRB_CTRL_IOC | DWC3_TRB_CTRL_LST;
    CleanInvalidateDcache(Trb, sizeof(*Trb));
    UINT32 Cmd = 0;
    EFI_STATUS St = SendEpCmd(Ep, DWC3_DEPCMD_STARTTRANSFER, (UINT32)(TrbAddr >> 32), (UINT32)TrbAddr, 0, &Cmd);
    if (!EFI_ERROR(St)) {
        mAdb.EpResource[Ep] = DWC3_DEPCMD_GET_RSC_IDX(Cmd);
        mAdb.EpActive[Ep]   = TRUE;
    }
    return St;
}

STATIC VOID
EndXfer(UINTN Ep)
{
    if (Ep >= 4 || !mAdb.EpActive[Ep]) return;
    SendEpCmd(Ep, DWC3_DEPCMD_ENDTRANSFER | DWC3_DEPCMD_HIPRI_FORCERM | DWC3_DEPCMD_PARAM(mAdb.EpResource[Ep]),
              0, 0, 0, NULL);
    gBS->Stall(100);
    mAdb.EpActive[Ep]   = FALSE;
    mAdb.EpResource[Ep] = 0;
}

STATIC BOOLEAN
NextEvent(UINT32 *Event)
{
    UINT32 Cnt   = Dwc3Read(DWC3_GEVNTCOUNT0);
    UINT32 Bytes = Cnt & DWC3_GEVNTCOUNT_MASK;
    if (Bytes == 0) return FALSE;
    if (Bytes > DWC3_EVENT_BUFFER_SIZE || (Bytes & 3U) != 0) return FALSE;
    CleanInvalidateDcache(mAdb.Dma, DWC3_EVENT_BUFFER_SIZE);
    *Event = mAdb.EventWords[mAdb.EventReadPos];
    mAdb.EventReadPos = (mAdb.EventReadPos + 1) % DWC3_EVENT_WORD_COUNT;
    Dwc3Write(DWC3_GEVNTCOUNT0, sizeof(UINT32));
    return TRUE;
}

/* Google/Android VID 0x18D1, ADB Interface Class 0xFF/0x42/0x01 (matched natively by Windows WinUsb.inf). */
STATIC CONST UINT8 mDevDesc[] = {
    18, USB_DESC_TYPE_DEVICE, 0x00, 0x02,
    0x00, 0x00, 0x00, B2L_ADB_EP0_MAX_PACKET,
    0xD1, 0x18, 0xE7, 0x4E, 0x00, 0x01,
    1, 2, 3, 1
};

STATIC CONST UINT8 mCfgDesc[] = {
    9, USB_DESC_TYPE_CONFIGURATION, 32, 0, 1, 1, 0, 0xC0, 1,
    9, 4, 0, 0, 2, 0xFF, 0x42, 0x01, 4,
    7, 5, 0x01, 0x02, 0x00, 0x02, 0,
    7, 5, 0x81, 0x02, 0x00, 0x02, 0
};

STATIC CONST UINT8 mDevQualDesc[] = {
    10, USB_DESC_TYPE_DEVICE_QUALIFIER, 0x00, 0x02,
    0x00, 0x00, 0x00, B2L_ADB_EP0_MAX_PACKET, 1, 0
};

STATIC UINTN
BuildStrDesc(UINT8 Index, UINT8 *Buf, UINTN Cap)
{
    CONST CHAR8 *Text;
    if (Cap < 4) return 0;
    if (Index == 0) { Buf[0] = 4; Buf[1] = USB_DESC_TYPE_STRING; Buf[2] = 0x09; Buf[3] = 0x04; return 4; }
    switch (Index) {
    case 1: Text = "Samsung"; break;
    case 2: Text = "Galaxy S10e XnuLoader"; break;
    case 3: Text = "beyond0lte"; break;
    case 4: Text = "ADB Interface"; break;
    default: return 0;
    }
    UINTN Len = AsciiStrLen(Text);
    if (2 + Len * 2 > Cap) return 0;
    Buf[0] = (UINT8)(2 + Len * 2);
    Buf[1] = USB_DESC_TYPE_STRING;
    for (UINTN i = 0; i < Len; i++) {
        Buf[2 + i * 2] = (UINT8)Text[i];
        Buf[3 + i * 2] = 0;
    }
    return Buf[0];
}

STATIC EFI_STATUS
ConfigEp(UINTN Ep, UINT32 Type, UINT32 MaxPkt, BOOLEAN Modify)
{
    UINT32 P0 = DWC3_DEPCFG_EP_TYPE(Type) | DWC3_DEPCFG_MAX_PACKET_SIZE(MaxPkt) |
                (Modify ? DWC3_DEPCFG_ACTION_MODIFY : DWC3_DEPCFG_ACTION_INIT);
    if (Ep & 1U) P0 |= DWC3_DEPCFG_FIFO_NUMBER(Ep >> 1);
    UINT32 P1 = DWC3_DEPCFG_EP_NUMBER(Ep) | DWC3_DEPCFG_XFER_COMPLETE_EN;
    P1 |= (Ep <= 1) ? DWC3_DEPCFG_XFER_NOT_READY_EN : DWC3_DEPCFG_XFER_IN_PROGRESS_EN;
    return SendEpCmd(Ep, DWC3_DEPCMD_SETEPCONFIG, P0, P1, 0, NULL);
}

STATIC EFI_STATUS
ArmEp0(VOID)
{
    if (mAdb.EpActive[0]) return EFI_ALREADY_STARTED;
    ZeroMem(mAdb.ControlBuffer, B2L_ADB_EP0_MAX_PACKET);
    CleanInvalidateDcache(mAdb.ControlBuffer, B2L_ADB_EP0_MAX_PACKET);
    EFI_STATUS St = StartXfer(0, mAdb.DmaAddress + B2L_ADB_CONTROL_OFFSET,
                              sizeof(B2L_USB_SETUP), DWC3_TRBCTL_CONTROL_SETUP);
    if (!EFI_ERROR(St)) mAdb.Ep0State = EP0_SETUP_ARMED;
    return St;
}

STATIC EFI_STATUS
ArmBulkOut(VOID)
{
    if (!mAdb.Configured || mAdb.EpActive[2]) return EFI_SUCCESS;
    CleanInvalidateDcache(mAdb.RxBuffer, B2L_ADB_RX_BYTES);
    return StartXfer(2, mAdb.DmaAddress + B2L_ADB_RX_OFFSET, B2L_ADB_RX_BYTES, DWC3_TRBCTL_NORMAL);
}

STATIC UINTN  mFailStep;
STATIC UINT32 mFailReg;
STATIC UINT32 mFailDsts;

STATIC EFI_STATUS
InitCtrlEps(VOID)
{
    UINT32 CmdReg = 0;
    EFI_STATUS St = SendEpCmd(0, DWC3_DEPCMD_DEPSTARTCFG, 0, 0, 0, &CmdReg);
    if (EFI_ERROR(St)) { mFailStep = 1; mFailReg = CmdReg; mFailDsts = Dwc3Read(DWC3_DSTS); return St; }
    for (UINTN Ep = 0; Ep < 4; Ep++) {
        St = SendEpCmd(Ep, DWC3_DEPCMD_SETTRANSFRESOURCE, DWC3_DEPXFERCFG_NUM_XFER_RES(1), 0, 0, &CmdReg);
        if (EFI_ERROR(St)) { mFailStep = 2 + Ep; mFailReg = CmdReg; mFailDsts = Dwc3Read(DWC3_DSTS); return St; }
    }
    St = SendEpCmd(0, DWC3_DEPCMD_SETEPCONFIG,
                   DWC3_DEPCFG_EP_TYPE(0) | DWC3_DEPCFG_MAX_PACKET_SIZE(B2L_ADB_EP0_MAX_PACKET) | DWC3_DEPCFG_ACTION_INIT,
                   DWC3_DEPCFG_EP_NUMBER(0) | DWC3_DEPCFG_XFER_COMPLETE_EN | DWC3_DEPCFG_XFER_NOT_READY_EN,
                   0, &CmdReg);
    if (EFI_ERROR(St)) { mFailStep = 6; mFailReg = CmdReg; mFailDsts = Dwc3Read(DWC3_DSTS); return St; }
    St = SendEpCmd(1, DWC3_DEPCMD_SETEPCONFIG,
                   DWC3_DEPCFG_EP_TYPE(0) | DWC3_DEPCFG_MAX_PACKET_SIZE(B2L_ADB_EP0_MAX_PACKET) | DWC3_DEPCFG_FIFO_NUMBER(0) | DWC3_DEPCFG_ACTION_INIT,
                   DWC3_DEPCFG_EP_NUMBER(1) | DWC3_DEPCFG_XFER_COMPLETE_EN | DWC3_DEPCFG_XFER_NOT_READY_EN,
                   0, &CmdReg);
    if (EFI_ERROR(St)) { mFailStep = 7; mFailReg = CmdReg; mFailDsts = Dwc3Read(DWC3_DSTS); return St; }
    Dwc3Write(DWC3_DALEPENA, BIT0 | BIT1);
    St = ArmEp0();
    if (EFI_ERROR(St)) { mFailStep = 8; mFailReg = Dwc3Read(DWC3_DEPCMD(0)); mFailDsts = Dwc3Read(DWC3_DSTS); }
    return St;
}

STATIC EFI_STATUS
InitBulkEps(VOID)
{
    UINT32 Pkt = mAdb.BulkMaxPacket ? mAdb.BulkMaxPacket : 512U;
    EFI_STATUS St = ConfigEp(2, 2, Pkt, mAdb.BulkInitialized);
    if (EFI_ERROR(St)) return St;
    St = ConfigEp(3, 2, Pkt, mAdb.BulkInitialized);
    if (EFI_ERROR(St)) return St;
    mAdb.BulkInitialized = TRUE;
    Dwc3Write(DWC3_DALEPENA, BIT0 | BIT1 | BIT2 | BIT3);
    return EFI_SUCCESS;
}

STATIC EFI_STATUS
StartCtrlIn(CONST VOID *Data, UINTN DataLen, UINT16 ReqLen)
{
    UINTN Len = MIN(DataLen, (UINTN)ReqLen);
    if (Len > EFI_PAGE_SIZE) return EFI_BAD_BUFFER_SIZE;
    if (Len && Data != mAdb.ControlBuffer) CopyMem(mAdb.ControlBuffer, Data, Len);
    CleanInvalidateDcache(mAdb.ControlBuffer, Len ? Len : 1);
    EFI_STATUS St = StartXfer(1, mAdb.DmaAddress + B2L_ADB_CONTROL_OFFSET, (UINT32)Len, DWC3_TRBCTL_CONTROL_DATA);
    if (!EFI_ERROR(St)) {
        mAdb.Ep0ThreeStage = TRUE;
        mAdb.Ep0State      = EP0_DATA_IN;
    }
    return St;
}

STATIC VOID
StallEp0(VOID)
{
    EndXfer(0);
    EndXfer(1);
    SendEpCmd(0, DWC3_DEPCMD_SETSTALL, 0, 0, 0, NULL);
    mAdb.Ep0State = EP0_SETUP_ARMED;
    ArmEp0();
}

STATIC EFI_STATUS
StartCtrlStatus(UINTN Ep)
{
    UINT32 Type = mAdb.Ep0ThreeStage ? DWC3_TRBCTL_CONTROL_STATUS3 : DWC3_TRBCTL_CONTROL_STATUS2;
    EFI_STATUS St = StartXfer(Ep, mAdb.DmaAddress + B2L_ADB_CONTROL_OFFSET, 0, Type);
    if (!EFI_ERROR(St)) mAdb.Ep0State = EP0_STATUS;
    return St;
}

STATIC EFI_STATUS
HandleSetup(VOID)
{
    B2L_USB_SETUP S;
    CleanInvalidateDcache(mAdb.ControlBuffer, B2L_ADB_EP0_MAX_PACKET);
    CopyMem(&S, mAdb.ControlBuffer, sizeof(S));
    mAdb.Ep0ThreeStage = (S.Length != 0);

    if ((S.RequestType & USB_REQ_TYPE_TYPE_MASK) == USB_REQ_TYPE_STANDARD) {
        UINT8 Reply[2] = { 0, 0 };
        switch (S.Request) {
        case USB_REQ_GET_DESCRIPTOR: {
            if ((S.RequestType & USB_REQ_TYPE_DIRECTION_IN) == 0) break;
            UINT8 Dt = (UINT8)(S.Value >> 8), Di = (UINT8)S.Value;
            CONST VOID *Desc = NULL;
            UINTN Len = 0;
            if (Dt == USB_DESC_TYPE_DEVICE) {
                Desc = mDevDesc; Len = sizeof(mDevDesc);
            } else if (Dt == USB_DESC_TYPE_CONFIGURATION || Dt == USB_DESC_TYPE_OTHER_SPEED) {
                CopyMem(mAdb.ControlBuffer, mCfgDesc, sizeof(mCfgDesc));
                if (Dt == USB_DESC_TYPE_OTHER_SPEED || mAdb.BulkMaxPacket == 64) {
                    if (Dt == USB_DESC_TYPE_OTHER_SPEED) mAdb.ControlBuffer[1] = USB_DESC_TYPE_OTHER_SPEED;
                    UINT16 P = (Dt == USB_DESC_TYPE_OTHER_SPEED) ? 64 : (UINT16)mAdb.BulkMaxPacket;
                    mAdb.ControlBuffer[22] = (UINT8)P; mAdb.ControlBuffer[23] = (UINT8)(P >> 8);
                    mAdb.ControlBuffer[29] = (UINT8)P; mAdb.ControlBuffer[30] = (UINT8)(P >> 8);
                }
                Desc = mAdb.ControlBuffer; Len = sizeof(mCfgDesc);
            } else if (Dt == USB_DESC_TYPE_STRING) {
                Len = BuildStrDesc(Di, mAdb.ControlBuffer, EFI_PAGE_SIZE);
                Desc = mAdb.ControlBuffer;
            } else if (Dt == USB_DESC_TYPE_DEVICE_QUALIFIER) {
                Desc = mDevQualDesc; Len = sizeof(mDevQualDesc);
            }
            if (Desc && Len) return StartCtrlIn(Desc, Len, S.Length);
            break;
        }
        case USB_REQ_SET_ADDRESS:
            if (S.RequestType != 0 || S.Length != 0 || S.Value > 127) break;
            Dwc3Write(DWC3_DCFG, (Dwc3Read(DWC3_DCFG) & ~DWC3_DCFG_DEVADDR_MASK) | DWC3_DCFG_DEVADDR(S.Value));
            mAdb.Ep0ThreeStage = FALSE;
            mAdb.Ep0State      = EP0_WAIT_STATUS;
            return EFI_SUCCESS;
        case USB_REQ_GET_CONFIGURATION:
            if ((S.RequestType & USB_REQ_TYPE_DIRECTION_IN) == 0) break;
            Reply[0] = mAdb.ConfigValue;
            return StartCtrlIn(Reply, 1, S.Length);
        case USB_REQ_SET_CONFIGURATION:
            if (S.RequestType != 0 || S.Length != 0 || S.Value > 1) break;
            EndXfer(2);
            EndXfer(3);
            mAdb.RxState     = RX_WAIT_HEADER;
            mAdb.PayloadHave = 0;
            if (S.Value == 1) {
                EFI_STATUS St = InitBulkEps();
                if (EFI_ERROR(St)) return St;
                mAdb.Configured  = TRUE;
                mAdb.ConfigValue = 1;
                SetStatus("ADB: USB configured, ready");
                ArmBulkOut();
            } else {
                Dwc3Write(DWC3_DALEPENA, BIT0 | BIT1);
                mAdb.Configured   = FALSE;
                mAdb.AdbConnected = FALSE;
                mAdb.ConfigValue  = 0;
                SetStatus("ADB: waiting for USB host");
            }
            mAdb.Ep0ThreeStage = FALSE;
            mAdb.Ep0State      = EP0_WAIT_STATUS;
            return EFI_SUCCESS;
        case USB_REQ_GET_STATUS:
            if ((S.RequestType & USB_REQ_TYPE_DIRECTION_IN) == 0) break;
            return StartCtrlIn(Reply, 2, S.Length);
        case USB_REQ_CLEAR_FEATURE:
            if ((S.RequestType & USB_REQ_TYPE_RECIPIENT_MASK) == USB_REQ_RECIPIENT_ENDPOINT &&
                S.Value == USB_FEATURE_ENDPOINT_HALT && S.Length == 0) {
                UINTN Ep = (S.Index & 0x0FU) << 1;
                if (S.Index & 0x80U) Ep++;
                if (Ep < 4) {
                    SendEpCmd(Ep, DWC3_DEPCMD_CLEARSTALL, 0, 0, 0, NULL);
                    mAdb.Ep0ThreeStage = FALSE;
                    mAdb.Ep0State      = EP0_WAIT_STATUS;
                    return EFI_SUCCESS;
                }
            }
            break;
        case USB_REQ_SET_FEATURE:
            if (S.Length == 0 && (S.RequestType & USB_REQ_TYPE_RECIPIENT_MASK) != USB_REQ_RECIPIENT_ENDPOINT) {
                mAdb.Ep0ThreeStage = FALSE;
                mAdb.Ep0State      = EP0_WAIT_STATUS;
                return EFI_SUCCESS;
            }
            break;
        case USB_REQ_GET_INTERFACE:
            if ((S.RequestType & USB_REQ_TYPE_DIRECTION_IN) != 0) return StartCtrlIn(Reply, 1, S.Length);
            break;
        case USB_REQ_SET_INTERFACE:
            if (S.Value == 0 && S.Index == 0 && S.Length == 0) {
                mAdb.Ep0ThreeStage = FALSE;
                mAdb.Ep0State      = EP0_WAIT_STATUS;
                return EFI_SUCCESS;
            }
            break;
        default:
            break;
        }
    }
    StallEp0();
    return EFI_UNSUPPORTED;
}

STATIC VOID
HandleEp0Event(UINT32 Ev)
{
    UINTN Ep   = (Ev >> 1) & 0x1FU;
    UINTN Type = (Ev >> 6) & 0xFU;
    UINTN Stat = (Ev >> 12) & 0xFU;
    if (Ep > 1) return;

    if (Type == DWC3_DEPEVT_XFERCOMPLETE) {
        mAdb.EpActive[Ep]   = FALSE;
        mAdb.EpResource[Ep] = 0;
        CleanInvalidateDcache(mAdb.EpTrb[Ep], sizeof(B2L_DWC3_TRB));
        if (Ep == 0 && mAdb.Ep0State == EP0_SETUP_ARMED) {
            HandleSetup();
        } else if (Ep == 1 && mAdb.Ep0State == EP0_DATA_IN) {
            mAdb.Ep0State = EP0_WAIT_STATUS;
        } else if (mAdb.Ep0State == EP0_STATUS) {
            mAdb.Ep0ThreeStage = FALSE;
            ArmEp0();
        }
    } else if (Type == DWC3_DEPEVT_XFERNOTREADY && Stat == DWC3_DEPEVT_STATUS_CONTROL_STATUS &&
               mAdb.Ep0State == EP0_WAIT_STATUS) {
        if (EFI_ERROR(StartCtrlStatus(Ep))) StallEp0();
    }
}

STATIC VOID
HandleDeviceEvent(UINT32 Ev)
{
    if (((Ev >> 1) & 0x7FU) != 0) return;
    UINTN Type = (Ev >> 8) & 0xFU;
    if (Type == 1) { /* USB_RESET */
        EndXfer(1);
        EndXfer(2);
        EndXfer(3);
        mAdb.Configured    = FALSE;
        mAdb.AdbConnected  = FALSE;
        mAdb.ConfigValue   = 0;
        mAdb.Ep0ThreeStage = FALSE;
        mAdb.Ep0State      = EP0_SETUP_ARMED;
        mAdb.RxState       = RX_WAIT_HEADER;
        mAdb.PayloadHave   = 0;
        Dwc3Write(DWC3_DCFG, Dwc3Read(DWC3_DCFG) & ~DWC3_DCFG_DEVADDR_MASK);
        Dwc3Write(DWC3_DALEPENA, BIT0 | BIT1);
        SetStatus("ADB: USB bus reset");
        if (!mAdb.EpActive[0]) ArmEp0();
    } else if (Type == 2) { /* CONNECT_DONE */
        UINT32 Spd = Dwc3Read(DWC3_DSTS) & DWC3_DSTS_CONNECTSPD_MASK;
        mAdb.BulkMaxPacket = (Spd == 0) ? 512U : 64U;
        ConfigEp(0, 0, B2L_ADB_EP0_MAX_PACKET, TRUE);
        ConfigEp(1, 0, B2L_ADB_EP0_MAX_PACKET, TRUE);
        if (mAdb.BulkInitialized) {
            ConfigEp(2, 2, mAdb.BulkMaxPacket, TRUE);
            ConfigEp(3, 2, mAdb.BulkMaxPacket, TRUE);
        }
        SetStatus("ADB: enumerating USB");
    } else if (Type == 0) { /* DISCONNECT */
        EndXfer(2);
        EndXfer(3);
        mAdb.Configured   = FALSE;
        mAdb.AdbConnected = FALSE;
        mAdb.ConfigValue  = 0;
        mAdb.RxState      = RX_WAIT_HEADER;
        mAdb.PayloadHave  = 0;
        SetStatus("ADB: waiting for USB cable");
    }
}

STATIC UINT32
AdbSum(CONST VOID *Data, UINT32 Len)
{
    CONST UINT8 *P = (CONST UINT8 *)Data;
    UINT32 Sum = 0;
    for (UINT32 i = 0; i < Len; i++) Sum += P[i];
    return Sum;
}

STATIC BOOLEAN
SendBulkInSync(CONST VOID *Data, UINT32 Len)
{
    if (!mAdb.Configured || Len > B2L_ADB_TX_BYTES) return FALSE;
    if (Len > 0) CopyMem(mAdb.TxBuffer, Data, Len);
    CleanInvalidateDcache(mAdb.TxBuffer, Len ? Len : 1);
    if (EFI_ERROR(StartXfer(3, mAdb.DmaAddress + B2L_ADB_TX_OFFSET, Len, DWC3_TRBCTL_NORMAL))) {
        return FALSE;
    }
    for (UINTN Ms = 0; Ms < B2L_ADB_TX_TIMEOUT_MS; Ms++) {
        UINT32 Ev;
        while (NextEvent(&Ev)) {
            if ((Ev & BIT0) == 0) {
                UINTN Ep   = (Ev >> 1) & 0x1FU;
                UINTN Type = (Ev >> 6) & 0xFU;
                if (Ep == 3 && Type == DWC3_DEPEVT_XFERCOMPLETE) {
                    mAdb.EpActive[3]   = FALSE;
                    mAdb.EpResource[3] = 0;
                    return TRUE;
                }
                if (Ep <= 1) HandleEp0Event(Ev);
            } else {
                HandleDeviceEvent(Ev);
                if (!mAdb.Configured) {
                    EndXfer(3);
                    return FALSE;
                }
            }
        }
        gBS->Stall(1000);
    }
    EndXfer(3);
    return FALSE;
}

STATIC VOID
SendAdbPacket(UINT32 Cmd, UINT32 Arg0, UINT32 Arg1, CONST VOID *Data, UINT32 Len)
{
    B2L_ADB_MSG Msg;
    Msg.Command    = Cmd;
    Msg.Arg0       = Arg0;
    Msg.Arg1       = Arg1;
    Msg.DataLength = Len;
    Msg.DataCrc32  = (Data && Len) ? AdbSum(Data, Len) : 0;
    Msg.Magic      = Cmd ^ 0xFFFFFFFFU;
    if (!SendBulkInSync(&Msg, sizeof(Msg))) return;
    if (Len > 0 && Data != NULL) {
        if (!SendBulkInSync(Data, Len)) return;
        UINT32 Pkt = mAdb.BulkMaxPacket ? mAdb.BulkMaxPacket : 512U;
        if ((Len % Pkt) == 0) SendBulkInSync(mAdb.TxBuffer, 0);
    }
}

STATIC BOOLEAN
StrStartsWith(CONST CHAR8 *S, CONST CHAR8 *Prefix)
{
    while (*Prefix) {
        if (*S++ != *Prefix++) return FALSE;
    }
    return TRUE;
}

STATIC VOID
ProcessAdbMessage(CONST B2L_ADB_MSG *Msg, CONST UINT8 *Payload, UINT32 Len)
{
    if (Msg->Command == ADB_CMD_CNXN) {
        STATIC CONST CHAR8 kBanner[] =
            "device::ro.product.name=beyond0lte;ro.product.model=Galaxy S10e;ro.product.device=beyond0lte;";
        UINT32 Ver = (Msg->Arg0 < ADB_VERSION_MAX) ? Msg->Arg0 : ADB_VERSION_MAX;
        mAdb.AdbConnected = TRUE;
        SetStatus("ADB: connected (beyond0lte)");
        SendAdbPacket(ADB_CMD_CNXN, Ver, ADB_MAXDATA, kBanner, (UINT32)AsciiStrLen(kBanner));
        return;
    }

    if (Msg->Command == ADB_CMD_OPEN) {
        UINT32 RemoteId = Msg->Arg0;
        UINT32 LocalId  = ++mAdb.NextLocalId;
        if (!LocalId) LocalId = ++mAdb.NextLocalId;

        CHAR8 Svc[256];
        UINTN N = MIN((UINTN)Len, sizeof(Svc) - 1);
        CopyMem(Svc, Payload, N);
        Svc[N] = '\0';

        if (StrStartsWith(Svc, "reboot:")) {
            CONST CHAR8 *Target = Svc + 7;
            SendAdbPacket(ADB_CMD_OKAY, LocalId, RemoteId, NULL, 0);
            SendAdbPacket(ADB_CMD_CLSE, LocalId, RemoteId, NULL, 0);
            if (StrStartsWith(Target, "recovery") || StrStartsWith(Target, "twrp")) {
                mAdb.PendingAction = B2L_ADB_ACT_RECOVERY;
            } else if (StrStartsWith(Target, "boot") || StrStartsWith(Target, "ios")) {
                mAdb.PendingAction = B2L_ADB_ACT_BOOT;
            } else {
                mAdb.PendingAction = B2L_ADB_ACT_REBOOT;
            }
            return;
        }

        if (StrStartsWith(Svc, "shell:") || StrStartsWith(Svc, "shell,")) {
            CONST CHAR8 *Cmd = Svc;
            while (*Cmd && *Cmd != ':') Cmd++;
            if (*Cmd == ':') Cmd++;
            while (*Cmd == ' ' || *Cmd == '\t' || *Cmd == '\r' || *Cmd == '\n') Cmd++;

            SendAdbPacket(ADB_CMD_OKAY, LocalId, RemoteId, NULL, 0);
            CONST CHAR8 *Out = "Galaxy S10e XnuLoader ADB (cmds: reboot, reboot recovery, boot)\n";
            if (StrStartsWith(Cmd, "reboot recovery") || StrStartsWith(Cmd, "reboot twrp") ||
                AsciiStrCmp(Cmd, "recovery") == 0) {
                Out = "Rebooting to recovery...\n";
                mAdb.PendingAction = B2L_ADB_ACT_RECOVERY;
            } else if (AsciiStrCmp(Cmd, "reboot") == 0 || StrStartsWith(Cmd, "reboot ")) {
                Out = "Rebooting...\n";
                mAdb.PendingAction = B2L_ADB_ACT_REBOOT;
            } else if (AsciiStrCmp(Cmd, "boot") == 0 || AsciiStrCmp(Cmd, "ios") == 0) {
                Out = "Booting iOS...\n";
                mAdb.PendingAction = B2L_ADB_ACT_BOOT;
            } else if (*Cmd != '\0' && AsciiStrCmp(Cmd, "help") != 0 && AsciiStrCmp(Cmd, "status") != 0) {
                Out = "Supported: reboot, reboot recovery, boot, status\n";
            }
            SendAdbPacket(ADB_CMD_WRTE, LocalId, RemoteId, Out, (UINT32)AsciiStrLen(Out));
            SendAdbPacket(ADB_CMD_CLSE, LocalId, RemoteId, NULL, 0);
            return;
        }

        SendAdbPacket(ADB_CMD_CLSE, 0, RemoteId, NULL, 0);
        return;
    }

    if (Msg->Command == ADB_CMD_WRTE) {
        SendAdbPacket(ADB_CMD_OKAY, Msg->Arg1, Msg->Arg0, NULL, 0);
    }
}

STATIC VOID
HandleBulkOutComplete(VOID)
{
    mAdb.EpActive[2]   = FALSE;
    mAdb.EpResource[2] = 0;
    CleanInvalidateDcache(mAdb.EpTrb[2], sizeof(B2L_DWC3_TRB));
    UINT32 Rem = mAdb.EpTrb[2]->Size & DWC3_TRB_SIZE_MASK;
    UINT32 Actual = (Rem <= B2L_ADB_RX_BYTES) ? (B2L_ADB_RX_BYTES - Rem) : 0;
    if (Actual == 0) {
        ArmBulkOut();
        return;
    }
    CleanInvalidateDcache(mAdb.RxBuffer, Actual);

    if (mAdb.RxState == RX_WAIT_HEADER) {
        if (Actual >= sizeof(B2L_ADB_MSG)) {
            CopyMem(&mAdb.RxMsg, mAdb.RxBuffer, sizeof(B2L_ADB_MSG));
            if ((mAdb.RxMsg.Command ^ mAdb.RxMsg.Magic) == 0xFFFFFFFFU && mAdb.RxMsg.DataLength <= ADB_MAXDATA) {
                UINT32 Tail = Actual - (UINT32)sizeof(B2L_ADB_MSG);
                if (mAdb.RxMsg.DataLength == 0) {
                    ProcessAdbMessage(&mAdb.RxMsg, mAdb.PayloadBuf, 0);
                } else if (Tail >= mAdb.RxMsg.DataLength) {
                    ProcessAdbMessage(&mAdb.RxMsg, mAdb.RxBuffer + sizeof(B2L_ADB_MSG), mAdb.RxMsg.DataLength);
                } else {
                    if (Tail > 0) CopyMem(mAdb.PayloadBuf, mAdb.RxBuffer + sizeof(B2L_ADB_MSG), Tail);
                    mAdb.PayloadHave = Tail;
                    mAdb.RxState = RX_WAIT_PAYLOAD;
                }
            }
        }
    } else {
        UINT32 Need = mAdb.RxMsg.DataLength - mAdb.PayloadHave;
        UINT32 Copy = MIN(Actual, Need);
        if (Copy > 0) {
            CopyMem(mAdb.PayloadBuf + mAdb.PayloadHave, mAdb.RxBuffer, Copy);
            mAdb.PayloadHave += Copy;
        }
        if (mAdb.PayloadHave >= mAdb.RxMsg.DataLength) {
            mAdb.RxState = RX_WAIT_HEADER;
            mAdb.PayloadHave = 0;
            ProcessAdbMessage(&mAdb.RxMsg, mAdb.PayloadBuf, mAdb.RxMsg.DataLength);
        }
    }

    if (mAdb.Configured && mAdb.PendingAction == B2L_ADB_ACT_NONE) {
        ArmBulkOut();
    }
}

VOID
B2lAdbInit(VOID)
{
    if (mAdb.Active) return;
    ZeroMem(&mAdb, sizeof(mAdb));
    mAdb.BulkMaxPacket = 512U;
    AsciiStrCpyS(mAdb.StatusLine, sizeof(mAdb.StatusLine), "ADB: initializing");

    EFI_PHYSICAL_ADDRESS DmaAddr = MAX_UINT32;
    EFI_STATUS St = gBS->AllocatePages(AllocateMaxAddress, EfiReservedMemoryType,
                                       EFI_SIZE_TO_PAGES(B2L_ADB_DMA_BYTES), &DmaAddr);
    if (EFI_ERROR(St)) {
        SetStatus("ADB: off (no low DMA)");
        return;
    }

    VOID *Dma = (VOID *)(UINTN)DmaAddr;
    ZeroMem(Dma, B2L_ADB_DMA_BYTES);
    CleanInvalidateDcache(Dma, B2L_ADB_DMA_BYTES);

    if (!PreflightSafe(DmaAddr)) {
        gBS->FreePages(DmaAddr, EFI_SIZE_TO_PAGES(B2L_ADB_DMA_BYTES));
        SetStatus("ADB: off (DWC3 busy)");
        return;
    }

    mAdb.DmaAddress    = DmaAddr;
    mAdb.Dma           = Dma;
    mAdb.EventWords    = (volatile UINT32 *)(UINTN)(DmaAddr + B2L_ADB_EVENT_OFFSET);
    mAdb.EpTrb[0]      = (B2L_DWC3_TRB *)(UINTN)(DmaAddr + B2L_ADB_EP0_TRB_OFFSET);
    mAdb.EpTrb[1]      = (B2L_DWC3_TRB *)(UINTN)(DmaAddr + B2L_ADB_EP1_TRB_OFFSET);
    mAdb.EpTrb[2]      = (B2L_DWC3_TRB *)(UINTN)(DmaAddr + B2L_ADB_EP2_TRB_OFFSET);
    mAdb.EpTrb[3]      = (B2L_DWC3_TRB *)(UINTN)(DmaAddr + B2L_ADB_EP3_TRB_OFFSET);
    mAdb.ControlBuffer = (UINT8 *)(UINTN)(DmaAddr + B2L_ADB_CONTROL_OFFSET);
    mAdb.RxBuffer      = (UINT8 *)(UINTN)(DmaAddr + B2L_ADB_RX_OFFSET);
    mAdb.TxBuffer      = (UINT8 *)(UINTN)(DmaAddr + B2L_ADB_TX_OFFSET);

    mAdb.SavedDwc3.Gctl              = Dwc3Read(DWC3_GCTL);
    mAdb.SavedDwc3.Usb2PhyCfg        = Dwc3Read(DWC3_GUSB2PHYCFG0);
    mAdb.SavedDwc3.Usb3PipeCtl       = Dwc3Read(DWC3_GUSB3PIPECTL0);
    mAdb.SavedDwc3.EventAddressLow   = Dwc3Read(DWC3_GEVNTADRLO0);
    mAdb.SavedDwc3.EventAddressHigh  = Dwc3Read(DWC3_GEVNTADRHI0);
    mAdb.SavedDwc3.EventSize         = Dwc3Read(DWC3_GEVNTSIZ0);
    mAdb.SavedDwc3.DeviceEventEnable = Dwc3Read(DWC3_DEVTEN);
    mAdb.SavedDwc3.DeviceConfig      = Dwc3Read(DWC3_DCFG);
    mAdb.SavedDwc3.DeviceControl     = Dwc3Read(DWC3_DCTL);
    SavePhyState(&mAdb.SavedPhy);
    mAdb.PhySavedValid = TRUE;
    mAdb.Active = TRUE;

    if (!EnableHsPhy(&mAdb.SavedPhy)) {
        B2lAdbStop();
        SetStatus("ADB: off (PHY failed)");
        return;
    }

    /* Reset DWC3 core in Device mode and park unused SuperSpeed PIPE PHY before endpoint commands. */
    UINT32 Gctl = (Dwc3Read(DWC3_GCTL) & ~DWC3_GCTL_PRTCAP_FIELD_MASK) |
                  (DWC3_GCTL_PRTCAP_DEVICE << DWC3_GCTL_PRTCAP_SHIFT);
    Dwc3Write(DWC3_GCTL, Gctl);
    gBS->Stall(100);

    Dwc3Write(DWC3_GUSB3PIPECTL0, Dwc3Read(DWC3_GUSB3PIPECTL0) | DWC3_GUSB3PIPECTL_PHYSOFTRST);
    Dwc3Write(DWC3_GUSB2PHYCFG0, Dwc3Read(DWC3_GUSB2PHYCFG0) | DWC3_GUSB2PHYCFG_PHYSOFTRST);
    gBS->Stall(100);
    Dwc3Write(DWC3_GUSB3PIPECTL0, Dwc3Read(DWC3_GUSB3PIPECTL0) & ~DWC3_GUSB3PIPECTL_PHYSOFTRST);
    Dwc3Write(DWC3_GUSB2PHYCFG0, Dwc3Read(DWC3_GUSB2PHYCFG0) & ~DWC3_GUSB2PHYCFG_PHYSOFTRST);
    gBS->Stall(100);

    Dwc3Write(DWC3_DCTL, Dwc3Read(DWC3_DCTL) | DWC3_DCTL_CSFTRST);
    for (UINTN Us = 0; Us < 100000; Us++) {
        if ((Dwc3Read(DWC3_DCTL) & DWC3_DCTL_CSFTRST) == 0) break;
        gBS->Stall(1);
    }
    if ((Dwc3Read(DWC3_DCTL) & DWC3_DCTL_CSFTRST) != 0) {
        B2lAdbStop();
        SetStatus("ADB: off (DWC3 reset timeout)");
        return;
    }
    gBS->Stall(50000);

    Dwc3Write(DWC3_GUSB3PIPECTL0, Dwc3Read(DWC3_GUSB3PIPECTL0) | DWC3_GUSB3PIPECTL_SUSPHY);
    UINT32 U2Cfg = Dwc3Read(DWC3_GUSB2PHYCFG0);
    U2Cfg &= ~(DWC3_GUSB2PHYCFG_PHYIF_MASK | DWC3_GUSB2PHYCFG_USBTRD_MASK |
               DWC3_GUSB2PHYCFG_ULPI_UTMI | DWC3_GUSB2PHYCFG_SUSPHY |
               DWC3_GUSB2PHYCFG_ENBLSLPM | DWC3_GUSB2PHYCFG_U2_FREECLK);
    U2Cfg |= DWC3_GUSB2PHYCFG_PHYIF_16 | DWC3_GUSB2PHYCFG_USBTRD_16;
    Dwc3Write(DWC3_GUSB2PHYCFG0, U2Cfg);

    Gctl  = Dwc3Read(DWC3_GCTL);
    Gctl &= ~(DWC3_GCTL_SCALEDOWN_MASK | DWC3_GCTL_DISSCRAMBLE |
              DWC3_GCTL_GBLHIBERNATIONEN | DWC3_GCTL_DSBLCLKGTNG | DWC3_GCTL_PRTCAP_FIELD_MASK);
    Gctl |= (DWC3_GCTL_PRTCAP_DEVICE << DWC3_GCTL_PRTCAP_SHIFT);
    Dwc3Write(DWC3_GCTL, Gctl);
    gBS->Stall(100);

    Dwc3Write(DWC3_DCFG, (Dwc3Read(DWC3_DCFG) & ~(DWC3_DCFG_DEVSPD_MASK | DWC3_DCFG_DEVADDR_MASK)) |
                         DWC3_DCFG_DEVSPD_HS);
    Dwc3Write(DWC3_GEVNTADRLO0, (UINT32)DmaAddr);
    Dwc3Write(DWC3_GEVNTADRHI0, (UINT32)(DmaAddr >> 32));
    Dwc3Write(DWC3_GEVNTSIZ0, DWC3_GEVNTSIZ_INTMASK | (DWC3_EVENT_BUFFER_SIZE & DWC3_GEVNTSIZ_SIZE_MASK));
    Dwc3Write(DWC3_GEVNTCOUNT0, Dwc3Read(DWC3_GEVNTCOUNT0) & DWC3_GEVNTCOUNT_MASK);
    Dwc3Write(DWC3_DEVTEN, DWC3_ADB_DEVICE_EVENTS);
    Dwc3Write(DWC3_DALEPENA, 0);

    mFailStep = 0;
    mFailReg  = 0;
    mFailDsts = 0;
    if (EFI_ERROR(InitCtrlEps())) {
        CHAR8 Err[64];
        AsciiSPrint(Err, sizeof(Err), "ADB: off (EP0 s=%u r=%08x d=%08x)", (UINT32)mFailStep, mFailReg, mFailDsts);
        B2lAdbStop();
        SetStatus(Err);
        return;
    }

    UINT32 Dctl = (Dwc3Read(DWC3_DCTL) & ~DWC3_DCTL_ULSTCHNGREQ_MASK) | DWC3_DCTL_ULSTCHNG_RX_DETECT;
    Dwc3Write(DWC3_DCTL, Dctl);
    Dwc3Write(DWC3_DCTL, Dctl | DWC3_DCTL_RUN_STOP);
    mAdb.ControllerRunning = TRUE;

    for (UINTN Ms = 0; Ms < 200; Ms++) {
        if ((Dwc3Read(DWC3_DSTS) & DWC3_DSTS_DEVCTRLHLT) == 0) break;
        gBS->Stall(1000);
    }
    SetStatus("ADB: ready (connect USB)");
}

B2L_ADB_ACTION
B2lAdbPoll(BOOLEAN *StatusChanged)
{
    if (!mAdb.Active) {
        if (StatusChanged) { *StatusChanged = mAdb.StatusDirty; mAdb.StatusDirty = FALSE; }
        return B2L_ADB_ACT_NONE;
    }
    UINT32 Ev;
    while (mAdb.PendingAction == B2L_ADB_ACT_NONE && NextEvent(&Ev)) {
        if ((Ev & BIT0) == 0) {
            UINTN Ep   = (Ev >> 1) & 0x1FU;
            UINTN Type = (Ev >> 6) & 0xFU;
            if (Ep <= 1) HandleEp0Event(Ev);
            else if (Ep == 2 && Type == DWC3_DEPEVT_XFERCOMPLETE) HandleBulkOutComplete();
        } else {
            HandleDeviceEvent(Ev);
        }
    }
    if (StatusChanged) {
        *StatusChanged = mAdb.StatusDirty;
        mAdb.StatusDirty = FALSE;
    }
    B2L_ADB_ACTION Act = mAdb.PendingAction;
    mAdb.PendingAction = B2L_ADB_ACT_NONE;
    return Act;
}

CONST CHAR8 *
B2lAdbStatus(VOID)
{
    return mAdb.StatusLine[0] ? mAdb.StatusLine : "ADB: off";
}

VOID
B2lAdbStop(VOID)
{
    if (!mAdb.Active) return;

    for (UINTN Ep = 0; Ep < 4; Ep++) EndXfer(Ep);
    Dwc3Write(DWC3_DALEPENA, 0);
    Dwc3Write(DWC3_DEVTEN, 0);

    if (mAdb.ControllerRunning) {
        UINT32 Dctl = (Dwc3Read(DWC3_DCTL) & ~DWC3_DCTL_ULSTCHNGREQ_MASK) | DWC3_DCTL_ULSTCHNG_RX_DETECT;
        Dwc3Write(DWC3_DCTL, Dctl);
        Dwc3Write(DWC3_DCTL, Dctl & ~DWC3_DCTL_RUN_STOP);
        gBS->Stall(20000);
        for (UINTN Ms = 0; Ms < B2L_ADB_STOP_TIMEOUT_MS; Ms++) {
            UINT32 Ev;
            while (NextEvent(&Ev)) {}
            if ((Dwc3Read(DWC3_DSTS) & DWC3_DSTS_DEVCTRLHLT) != 0) break;
            gBS->Stall(1000);
        }
    }

    BOOLEAN Halted = (Dwc3Read(DWC3_DSTS) & DWC3_DSTS_DEVCTRLHLT) != 0;
    if (Halted) {
        UINT32 Ev;
        while (NextEvent(&Ev)) {}
        Dwc3Write(DWC3_GEVNTSIZ0, DWC3_GEVNTSIZ_INTMASK);
        Dwc3Write(DWC3_GEVNTADRLO0, 0);
        Dwc3Write(DWC3_GEVNTADRHI0, 0);
        Dwc3Write(DWC3_GEVNTADRLO0, mAdb.SavedDwc3.EventAddressLow);
        Dwc3Write(DWC3_GEVNTADRHI0, mAdb.SavedDwc3.EventAddressHigh);
        Dwc3Write(DWC3_GEVNTSIZ0, mAdb.SavedDwc3.EventSize);
        Dwc3Write(DWC3_DEVTEN, mAdb.SavedDwc3.DeviceEventEnable);
        Dwc3Write(DWC3_DCFG, mAdb.SavedDwc3.DeviceConfig);
        Dwc3Write(DWC3_DCTL, mAdb.SavedDwc3.DeviceControl);
        Dwc3Write(DWC3_GUSB2PHYCFG0, mAdb.SavedDwc3.Usb2PhyCfg);
        Dwc3Write(DWC3_GUSB3PIPECTL0, mAdb.SavedDwc3.Usb3PipeCtl);
        Dwc3Write(DWC3_GCTL, mAdb.SavedDwc3.Gctl);
        if (mAdb.PhySavedValid) RestoreHsPhy(&mAdb.SavedPhy);
        if (mAdb.DmaAddress) {
            CleanInvalidateDcache(mAdb.Dma, B2L_ADB_DMA_BYTES);
            gBS->FreePages(mAdb.DmaAddress, EFI_SIZE_TO_PAGES(B2L_ADB_DMA_BYTES));
        }
    }

    mAdb.Active = FALSE;
    mAdb.ControllerRunning = FALSE;
    mAdb.Configured = FALSE;
    mAdb.AdbConnected = FALSE;
}
