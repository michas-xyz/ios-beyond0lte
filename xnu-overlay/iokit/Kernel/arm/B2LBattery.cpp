/* SPDX-License-Identifier: BSD-2-Clause
 * Samsung Galaxy S10e (beyond0lte / Exynos 9820) battery & fuel gauge driver.
 *
 * Reads the Maxim MAX77705 fuel gauge (I2C address 0x36 on CMGP pins
 * gpm12-0 SCL / gpm13-0 SDA in pinctrl@15C30000 via open-drain bit-bang I2C,
 * restoring original CON mode after each transaction so CMU_CMGP gating of
 * 0x15D60000 can never stall the bus) and publishes live battery percentage,
 * voltage, current, and charging state through XNU's IOPMPowerSource.
 *
 * Kill switch: b2l_battery=0.
 */
#include <pexpert/arm64/board_config.h>
#if defined(B2L)
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/pwr_mgt/IOPMPowerSource.h>
#include <IOKit/pwr_mgt/IOPM.h>
#include <pexpert/pexpert.h>
#include <arm64/b2l_diag.h>

extern "C" vm_offset_t ml_io_map(vm_offset_t, vm_size_t);

#define B2L_BAT_CMGP_PIN_PHYS   0x15C30000ULL
#define B2L_BAT_GPM12_OFF       0x180u  /* gpm12-0 = SCL */
#define B2L_BAT_GPM13_OFF       0x1A0u  /* gpm13-0 = SDA */
#define B2L_BAT_PIN_CON         0x00u
#define B2L_BAT_PIN_DAT         0x04u
#define B2L_BAT_PIN_PUD         0x08u

#define B2L_BAT_FG_ADDR         0x36u   /* max77705-fuelgauge */
#define B2L_FG_REG_REPCAP       0x05u   /* RepCap (1 mAh/LSB on beyond0lte) */
#define B2L_FG_REG_REPSOC       0x06u   /* RepSOC (high byte %, low byte /256) */
#define B2L_FG_REG_VCELL        0x09u   /* VCELL (78.125 uV/LSB) */
#define B2L_FG_REG_AVGCURR      0x0Bu   /* AvgCurrent (signed, 0.3125 mA/LSB for 2 mOhm) */
#define B2L_FG_REG_FULLCAP      0x10u   /* FullCapRep (1 mAh/LSB) */
#define B2L_FG_REG_CYCLES       0x17u   /* Cycles */

#define B2L_BAT_POLL_MS         5000u

static inline void b2l_fg_decode(
    uint16_t repSocRaw, uint16_t vcellRaw, int16_t avgCurrRaw,
    uint16_t repCapRaw, uint16_t fullCapRaw, uint16_t cyclesRaw,
    unsigned int *socPctOut, unsigned int *vcellMvOut, int *avgCurrMaOut,
    unsigned int *repCapMahOut, unsigned int *fullCapMahOut,
    unsigned int *cyclesOut, bool *chargingOut, bool *extConnOut)
{
    unsigned int soc = (unsigned int)(repSocRaw >> 8);
    if (soc > 100u) {
        soc = 100u;
    }
    unsigned int vcellMv = (unsigned int)(((uint64_t)vcellRaw * 78125ULL) / 1000000ULL);
    if (soc == 0u && repSocRaw != 0u && vcellMv >= 3000u) {
        soc = 1u;
    }
    int currMa = (int)(((int64_t)avgCurrRaw * 3125LL) / 10000LL);
    *socPctOut = soc;
    *vcellMvOut = vcellMv;
    *avgCurrMaOut = currMa;
    *repCapMahOut = (unsigned int)repCapRaw;
    *fullCapMahOut = fullCapRaw ? (unsigned int)fullCapRaw : 3000u;
    *cyclesOut = (unsigned int)cyclesRaw;
    *extConnOut = (currMa >= 0);
    *chargingOut = (currMa > 0) && (soc < 100u);
}

class B2LBattery : public IOPMPowerSource {
    OSDeclareDefaultStructors(B2LBattery);

    volatile uint8_t   *pins;
    IOWorkLoop         *workLoop;
    IOTimerEventSource *timer;

    const OSSymbol     *symDesignCap;
    const OSSymbol     *symRawCur;
    const OSSymbol     *symRawMax;
    const OSSymbol     *symNomCap;
    const OSSymbol     *symFullyCharged;
    const OSSymbol     *symRawExtConn;

    bool                firstSampleLogged;
    bool                busStuck;

    uint32_t rdPin(uint32_t off) {
        uint32_t v = *(volatile uint32_t *)(pins + off);
        __asm__ volatile("dmb oshld" ::: "memory");
        return v;
    }

    void wrPin(uint32_t off, uint32_t val) {
        *(volatile uint32_t *)(pins + off) = val;
        __asm__ volatile("dsb oshst" ::: "memory");
    }

    void pinMode0(uint32_t bankOff, uint32_t fn) {
        uint32_t con = rdPin(bankOff + B2L_BAT_PIN_CON);
        wrPin(bankOff + B2L_BAT_PIN_CON, (con & ~0xFu) | (fn & 0xFu));
    }

    uint32_t pinGet0(uint32_t bankOff) {
        return rdPin(bankOff + B2L_BAT_PIN_DAT) & 1u;
    }

    void bbLow(uint32_t bankOff) {
        uint32_t dat = rdPin(bankOff + B2L_BAT_PIN_DAT);
        wrPin(bankOff + B2L_BAT_PIN_DAT, dat & ~1u);
        pinMode0(bankOff, 1u);
        IODelay(5);
    }

    void bbSdaRel(void) {
        pinMode0(B2L_BAT_GPM13_OFF, 0u);
        IODelay(5);
    }

    void bbSclRel(void) {
        pinMode0(B2L_BAT_GPM12_OFF, 0u);
        for (unsigned i = 0; i < 1000 && !pinGet0(B2L_BAT_GPM12_OFF); ++i) {
            IODelay(1);
        }
        if (!pinGet0(B2L_BAT_GPM12_OFF)) {
            busStuck = true;
        }
        IODelay(5);
    }

    void bbStart(void) {
        bbSdaRel();
        bbSclRel();
        bbLow(B2L_BAT_GPM13_OFF);
        bbLow(B2L_BAT_GPM12_OFF);
    }

    void bbStop(void) {
        bbLow(B2L_BAT_GPM13_OFF);
        bbSclRel();
        bbSdaRel();
    }

    bool bbPut(uint8_t v) {
        for (int i = 7; i >= 0; --i) {
            if ((v >> i) & 1u) {
                bbSdaRel();
            } else {
                bbLow(B2L_BAT_GPM13_OFF);
            }
            bbSclRel();
            bbLow(B2L_BAT_GPM12_OFF);
        }
        bbSdaRel();
        bbSclRel();
        bool ack = !pinGet0(B2L_BAT_GPM13_OFF);
        bbLow(B2L_BAT_GPM12_OFF);
        return ack;
    }

    uint8_t bbGet(bool ack) {
        uint8_t v = 0;
        bbSdaRel();
        for (int i = 0; i < 8; ++i) {
            bbSclRel();
            v = (uint8_t)((v << 1) | pinGet0(B2L_BAT_GPM13_OFF));
            bbLow(B2L_BAT_GPM12_OFF);
        }
        if (ack) {
            bbLow(B2L_BAT_GPM13_OFF);
        }
        bbSclRel();
        bbLow(B2L_BAT_GPM12_OFF);
        bbSdaRel();
        return v;
    }

    int bitBangRead16(uint8_t reg, uint16_t *out) {
        uint32_t origCon12 = rdPin(B2L_BAT_GPM12_OFF + B2L_BAT_PIN_CON);
        uint32_t origCon13 = rdPin(B2L_BAT_GPM13_OFF + B2L_BAT_PIN_CON);
        busStuck = false;
        bbSdaRel();
        bbSclRel();
        int step = 0;
        if (!pinGet0(B2L_BAT_GPM12_OFF) || !pinGet0(B2L_BAT_GPM13_OFF)) {
            step = 10;
            goto restore;
        }
        bbStart();
        if (!bbPut((uint8_t)(B2L_BAT_FG_ADDR << 1))) { step = 1; }
        if (!step && !bbPut(reg)) { step = 2; }
        if (!step) {
            bbStart();
            if (!bbPut((uint8_t)((B2L_BAT_FG_ADDR << 1) | 1u))) { step = 3; }
        }
        if (!step) {
            uint8_t lo = bbGet(true);
            uint8_t hi = bbGet(false);
            *out = (uint16_t)((uint16_t)lo | ((uint16_t)hi << 8));
        }
        bbStop();
        if (!step && busStuck) {
            step = 11;
        }
    restore:
        wrPin(B2L_BAT_GPM12_OFF + B2L_BAT_PIN_CON, origCon12);
        wrPin(B2L_BAT_GPM13_OFF + B2L_BAT_PIN_CON, origCon13);
        return step;
    }

    void pollOnce(void) {
        if (!pins) return;
        uint16_t repSocRaw = 0, vcellRaw = 0, avgCurrRaw = 0, repCapRaw = 0, fullCapRaw = 0, cyclesRaw = 0;
        int s1 = bitBangRead16(B2L_FG_REG_REPSOC, &repSocRaw);
        int s2 = s1 ? s1 : bitBangRead16(B2L_FG_REG_VCELL, &vcellRaw);
        if (s1 || s2) {
            if (!firstSampleLogged) {
                firstSampleLogged = true;
                b2l_diag_record("B2L BATTERY: read failed step=%d,%d gpm12=(%x,%x) gpm13=(%x,%x)\n",
                    s1, s2,
                    rdPin(B2L_BAT_GPM12_OFF + B2L_BAT_PIN_CON), rdPin(B2L_BAT_GPM12_OFF + B2L_BAT_PIN_DAT),
                    rdPin(B2L_BAT_GPM13_OFF + B2L_BAT_PIN_CON), rdPin(B2L_BAT_GPM13_OFF + B2L_BAT_PIN_DAT));
            }
            return;
        }
        (void)bitBangRead16(B2L_FG_REG_AVGCURR, &avgCurrRaw);
        (void)bitBangRead16(B2L_FG_REG_REPCAP, &repCapRaw);
        (void)bitBangRead16(B2L_FG_REG_FULLCAP, &fullCapRaw);
        (void)bitBangRead16(B2L_FG_REG_CYCLES, &cyclesRaw);

        unsigned int socPct = 0, vcellMv = 0, repCapMah = 0, fullCapMah = 0, cycles = 0;
        int avgCurrMa = 0;
        bool charging = false, extConn = false;

        b2l_fg_decode(repSocRaw, vcellRaw, (int16_t)avgCurrRaw,
                      repCapRaw, fullCapRaw, cyclesRaw,
                      &socPct, &vcellMv, &avgCurrMa,
                      &repCapMah, &fullCapMah, &cycles,
                      &charging, &extConn);

        if (vcellMv < 2000u || vcellMv > 5500u) {
            if (!firstSampleLogged) {
                firstSampleLogged = true;
                b2l_diag_record("B2L BATTERY: implausible vcell=%umV (raw soc=%04x vcell=%04x)\n",
                    vcellMv, repSocRaw, vcellRaw);
            }
            return;
        }

        setBatteryInstalled(true);
        setMaxCapacity(100);
        setCurrentCapacity(socPct);
        setVoltage(vcellMv);
        setAmperage(avgCurrMa);
        setIsCharging(charging);
        setExternalConnected(extConn);
        setExternalChargeCapable(extConn);
        setCycleCount(cycles);
        setTimeRemaining(-1);

        if (symDesignCap) {
            OSNumber *n = OSNumber::withNumber(3000u, 32);
            if (n) { setPSProperty(symDesignCap, n); n->release(); }
        }
        if (symRawCur) {
            OSNumber *n = OSNumber::withNumber(repCapMah, 32);
            if (n) { setPSProperty(symRawCur, n); n->release(); }
        }
        if (symRawMax) {
            OSNumber *n = OSNumber::withNumber(fullCapMah, 32);
            if (n) { setPSProperty(symRawMax, n); n->release(); }
        }
        if (symNomCap) {
            OSNumber *n = OSNumber::withNumber(fullCapMah, 32);
            if (n) { setPSProperty(symNomCap, n); n->release(); }
        }
        if (symFullyCharged) {
            setPSProperty(symFullyCharged, (extConn && socPct >= 100u) ? kOSBooleanTrue : kOSBooleanFalse);
        }
        if (symRawExtConn) {
            setPSProperty(symRawExtConn, extConn ? kOSBooleanTrue : kOSBooleanFalse);
        }

        updateStatus();

        if (!firstSampleLogged) {
            firstSampleLogged = true;
            b2l_diag_record("B2L BATTERY: live soc=%u%% (raw %04x) vcell=%umV curr=%dmA chg=%d rep=%u full=%u\n",
                socPct, repSocRaw, vcellMv, avgCurrMa, charging ? 1 : 0, repCapMah, fullCapMah);
        }
    }

    static void onTimer(OSObject *owner, IOTimerEventSource *sender) {
        B2LBattery *self = (B2LBattery *)owner;
        if (!self) return;
        self->pollOnce();
        if (sender) {
            sender->setTimeoutMS(B2L_BAT_POLL_MS);
        }
    }

public:
    bool init(void) APPLE_KEXT_OVERRIDE;
    bool init(OSDictionary *dict) APPLE_KEXT_OVERRIDE;
    bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
    void stop(IOService *provider) APPLE_KEXT_OVERRIDE;
    void free(void) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(B2LBattery, IOPMPowerSource);

bool B2LBattery::init(void)
{
    return IOPMPowerSource::init();
}

bool B2LBattery::init(OSDictionary *dict)
{
    if (!IOPMPowerSource::init()) {
        return false;
    }
    if (dict) {
        OSDictionary *pt = getPropertyTable();
        if (!pt) {
            setPropertyTable(dict);
        } else {
            pt->merge(dict);
        }
    }
    return true;
}

bool B2LBattery::start(IOService *provider)
{
    int enabled = 1;
    PE_parse_boot_argn("b2l_battery", &enabled, sizeof(enabled));
    b2l_diag_record("B2L BATTERY: start(enabled=%d)\n", enabled);
    if (!enabled) {
        return false;
    }
    if (!IOPMPowerSource::start(provider)) {
        b2l_diag_record("B2L BATTERY: IOPMPowerSource::start failed\n");
        return false;
    }

    symDesignCap    = OSSymbol::withCStringNoCopy(kIOPMPSDesignCapacityKey);
    symRawCur       = OSSymbol::withCStringNoCopy("AppleRawCurrentCapacity");
    symRawMax       = OSSymbol::withCStringNoCopy("AppleRawMaxCapacity");
    symNomCap       = OSSymbol::withCStringNoCopy("NominalChargeCapacity");
    symFullyCharged = OSSymbol::withCStringNoCopy(kIOPMFullyChargedKey);
    symRawExtConn   = OSSymbol::withCStringNoCopy("AppleRawExternalConnected");

    pins = (volatile uint8_t *)ml_io_map(B2L_BAT_CMGP_PIN_PHYS, 0x1000);
    if (!pins) {
        b2l_diag_record("B2L BATTERY: cmgp pinctrl map failed\n");
        IOPMPowerSource::stop(provider);
        return false;
    }

    b2l_diag_record("B2L BATTERY: pins before gpm12=(%x,%x,%x) gpm13=(%x,%x,%x)\n",
        rdPin(B2L_BAT_GPM12_OFF + B2L_BAT_PIN_CON), rdPin(B2L_BAT_GPM12_OFF + B2L_BAT_PIN_DAT), rdPin(B2L_BAT_GPM12_OFF + B2L_BAT_PIN_PUD),
        rdPin(B2L_BAT_GPM13_OFF + B2L_BAT_PIN_CON), rdPin(B2L_BAT_GPM13_OFF + B2L_BAT_PIN_DAT), rdPin(B2L_BAT_GPM13_OFF + B2L_BAT_PIN_PUD));

    pollOnce();

    if (!batteryInstalled()) {
        setBatteryInstalled(true);
        setMaxCapacity(100);
        setCurrentCapacity(50);
        setExternalConnected(false);
        setIsCharging(false);
        updateStatus();
    }

    workLoop = getWorkLoop();
    timer = IOTimerEventSource::timerEventSource(this, &B2LBattery::onTimer);
    if (workLoop && timer && workLoop->addEventSource(timer) == kIOReturnSuccess) {
        timer->setTimeoutMS(B2L_BAT_POLL_MS);
    }

    registerService();
    b2l_diag_record("B2L BATTERY: registered (cap=%u/%u)\n", currentCapacity(), maxCapacity());
    return true;
}

void B2LBattery::stop(IOService *provider)
{
    if (timer) {
        timer->cancelTimeout();
        if (workLoop) {
            workLoop->removeEventSource(timer);
        }
    }
    IOPMPowerSource::stop(provider);
}

void B2LBattery::free(void)
{
    OSSafeReleaseNULL(timer);
    OSSafeReleaseNULL(symDesignCap);
    OSSafeReleaseNULL(symRawCur);
    OSSafeReleaseNULL(symRawMax);
    OSSafeReleaseNULL(symNomCap);
    OSSafeReleaseNULL(symFullyCharged);
    OSSafeReleaseNULL(symRawExtConn);
    pins = nullptr;
    IOPMPowerSource::free();
}

#endif /* B2L */
