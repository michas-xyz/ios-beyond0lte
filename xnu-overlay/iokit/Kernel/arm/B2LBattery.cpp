/* SPDX-License-Identifier: BSD-2-Clause
 * Samsung Galaxy S10e (beyond0lte / Exynos 9820) battery & fuel gauge driver.
 *
 * Reads the Maxim MAX77705 fuel gauge (I2C address 0x36 on hsi2c@15D60000 /
 * USI_CMGP3, pins gpm12-0 SCL / gpm13-0 SDA in pinctrl@15C30000) and publishes
 * live battery percentage, voltage, current, and charging state through XNU's
 * IOPMPowerSource so iOS powerd / SpringBoard reflect actual hardware state.
 *
 * Kill switch: b2l_battery=0.
 */
#include <pexpert/arm64/board_config.h>
#if defined(B2L)
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/pwr_mgt/IOPMPowerSource.h>
#include <IOKit/pwr_mgt/IOPM.h>
#include <kern/clock.h>
#include <pexpert/pexpert.h>
#include <arm64/b2l_diag.h>

#define B2L_BAT_I2C_PHYS        0x15D60000ULL
#define B2L_BAT_CMGP_PIN_PHYS   0x15C30000ULL
#define B2L_BAT_GPM12_OFF       0x180u  /* gpm12-0 = SCL */
#define B2L_BAT_GPM13_OFF       0x1A0u  /* gpm13-0 = SDA */
#define B2L_BAT_PIN_CON         0x00u
#define B2L_BAT_PIN_DAT         0x04u

#define B2L_BAT_FG_ADDR         0x36u   /* max77705-fuelgauge */
#define B2L_FG_REG_REPCAP       0x05u   /* RepCap (1 mAh/LSB on beyond0lte) */
#define B2L_FG_REG_REPSOC       0x06u   /* RepSOC (high byte %, low byte /256) */
#define B2L_FG_REG_TEMP         0x08u   /* Temp (signed 1/256 degC) */
#define B2L_FG_REG_VCELL        0x09u   /* VCELL (78.125 uV/LSB) */
#define B2L_FG_REG_AVGCURR      0x0Bu   /* AvgCurrent (signed, 0.3125 mA/LSB for 2 mOhm) */
#define B2L_FG_REG_FULLCAP      0x10u   /* FullCapRep (1 mAh/LSB) */
#define B2L_FG_REG_CYCLES       0x17u   /* Cycles */

/* Exynos5 HSI2C registers (0x15D60000, samsung,usi-i2c-v2) */
#define HSI2C_CTL               0x00u
#define HSI2C_FIFO_CTL          0x04u
#define HSI2C_TRAILING_CTL      0x08u
#define HSI2C_INT_ENABLE        0x20u
#define HSI2C_INT_STATUS        0x24u
#define HSI2C_FIFO_STATUS       0x30u
#define HSI2C_TX_DATA           0x34u
#define HSI2C_RX_DATA           0x38u
#define HSI2C_CONF              0x40u
#define HSI2C_AUTO_CONF         0x44u
#define HSI2C_TIMEOUT           0x48u
#define HSI2C_TRANS_STATUS      0x50u
#define HSI2C_TIMING_FS1        0x60u
#define HSI2C_TIMING_FS2        0x64u
#define HSI2C_TIMING_FS3        0x68u
#define HSI2C_TIMING_SLA        0x6Cu
#define HSI2C_ADDR              0x70u
#define HSI2C_USI_CON           0xC4u
#define HSI2C_USI_OPT           0xC8u

#define HSI2C_FUNC_I2C          (1u << 0)
#define HSI2C_MASTER            (1u << 3)
#define HSI2C_RXCHON            (1u << 6)
#define HSI2C_TXCHON            (1u << 7)
#define HSI2C_SW_RST            (1u << 31)
#define HSI2C_RXFIFO_EN         (1u << 0)
#define HSI2C_TXFIFO_EN         (1u << 1)
#define HSI2C_INT_TRANS_DONE    (1u << 7)
#define HSI2C_INT_TRANS_ERRS    ((1u << 8) | (1u << 9) | (1u << 10) | (1u << 11))
#define HSI2C_RX_FIFO_EMPTY     (1u << 24)
#define HSI2C_TX_FIFO_FULL      (1u << 7)
#define HSI2C_AUTO_MODE         (1u << 31)
#define HSI2C_HS_MODE           (1u << 29)
#define HSI2C_READ_WRITE        (1u << 16)
#define HSI2C_STOP_AFTER_TRANS  (1u << 17)
#define HSI2C_MASTER_RUN        (1u << 31)
#define HSI2C_TIMEOUT_EN        (1u << 31)
#define HSI2C_MASTER_BUSY       (1u << 17)
#define HSI2C_MASTER_ST_MASK    0xFu
#define HSI2C_MASTER_ST_LOSE    0xCu

/* Verified beyond0lte 15d60000.hsi2c 400 kHz timings (IPCLK=200 MHz) */
#define B2L_BAT_DEFAULT_FS1     0x01F0FF00u
#define B2L_BAT_DEFAULT_FS2     0x030003E0u
#define B2L_BAT_DEFAULT_FS3     0x00210000u
#define B2L_BAT_DEFAULT_SLA     0x0000004Eu

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

    IOMemoryDescriptor *i2cDesc;
    IOMemoryMap        *i2cMap;
    volatile uint8_t   *i2c;

    IOMemoryDescriptor *pinDesc;
    IOMemoryMap        *pinMap;
    volatile uint8_t   *pins;

    IOWorkLoop         *workLoop;
    IOTimerEventSource *timer;

    const OSSymbol     *symDesignCap;
    const OSSymbol     *symRawCur;
    const OSSymbol     *symRawMax;
    const OSSymbol     *symNomCap;
    const OSSymbol     *symFullyCharged;
    const OSSymbol     *symRawExtConn;

    uint32_t            savedFs1, savedFs2, savedFs3, savedSla;
    bool                useBitBang;
    bool                firstSampleLogged;
    bool                busStuck;

    static uint64_t uptimeUS(void) {
        uint64_t abs = 0, ns = 0;
        clock_get_uptime(&abs);
        absolutetime_to_nanoseconds(abs, &ns);
        return ns / 1000ULL;
    }

    uint32_t rdI2c(uint32_t off) {
        uint32_t v = *(volatile uint32_t *)(i2c + off);
        __asm__ volatile("dmb oshld" ::: "memory");
        return v;
    }

    void wrI2c(uint32_t off, uint32_t val) {
        *(volatile uint32_t *)(i2c + off) = val;
        __asm__ volatile("dsb oshst" ::: "memory");
    }

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

    bool bitBangRead16(uint8_t reg, uint16_t *out) {
        busStuck = false;
        bbSdaRel();
        bbSclRel();
        if (!pinGet0(B2L_BAT_GPM12_OFF) || !pinGet0(B2L_BAT_GPM13_OFF)) {
            return false;
        }
        bool ok = true;
        bbStart();
        if (!bbPut((uint8_t)(B2L_BAT_FG_ADDR << 1))) { ok = false; }
        if (ok && !bbPut(reg)) { ok = false; }
        if (ok) {
            bbStart();
            if (!bbPut((uint8_t)((B2L_BAT_FG_ADDR << 1) | 1u))) { ok = false; }
        }
        uint8_t lo = 0, hi = 0;
        if (ok) {
            lo = bbGet(true);
            hi = bbGet(false);
        }
        bbStop();
        if (!ok || busStuck) {
            return false;
        }
        *out = (uint16_t)((uint16_t)lo | ((uint16_t)hi << 8));
        return true;
    }

    void hsi2cResetController(void) {
        if (!i2c) return;
        uint32_t usi = rdI2c(HSI2C_USI_CON);
        if (usi & 1u) {
            wrI2c(HSI2C_USI_CON, usi & ~1u);
            IODelay(10);
        }
        uint32_t ctl = rdI2c(HSI2C_CTL);
        wrI2c(HSI2C_CTL, ctl | HSI2C_SW_RST);
        IODelay(10);
        wrI2c(HSI2C_CTL, ctl & ~HSI2C_SW_RST);
        IODelay(10);

        wrI2c(HSI2C_TIMING_FS1, savedFs1);
        wrI2c(HSI2C_TIMING_FS2, savedFs2);
        wrI2c(HSI2C_TIMING_FS3, savedFs3);
        if (savedSla) {
            wrI2c(HSI2C_TIMING_SLA, savedSla);
        }
        uint32_t timeout = rdI2c(HSI2C_TIMEOUT) & ~HSI2C_TIMEOUT_EN;
        wrI2c(HSI2C_TIMEOUT, timeout);
        wrI2c(HSI2C_CTL, HSI2C_FUNC_I2C | HSI2C_MASTER);
        wrI2c(HSI2C_TRAILING_CTL, 0xFu);
        uint32_t conf = (rdI2c(HSI2C_CONF) & ~HSI2C_HS_MODE) | HSI2C_AUTO_MODE;
        wrI2c(HSI2C_CONF, conf);
        wrI2c(HSI2C_INT_ENABLE, 0u);
        wrI2c(HSI2C_INT_STATUS, rdI2c(HSI2C_INT_STATUS));
    }

    bool hsi2cWaitIdle(unsigned budgetUs) {
        uint64_t deadline = uptimeUS() + budgetUs;
        for (;;) {
            if (!(rdI2c(HSI2C_TRANS_STATUS) & HSI2C_MASTER_BUSY)) {
                return true;
            }
            if (uptimeUS() >= deadline) {
                return false;
            }
            IODelay(20);
        }
    }

    bool hsi2cXfer(uint8_t *buf, unsigned len, bool isRead) {
        if (!i2c || !buf || len == 0 || len > 16) {
            return false;
        }
        if (!hsi2cWaitIdle(10000) || (rdI2c(HSI2C_TRANS_STATUS) & HSI2C_MASTER_ST_MASK) == HSI2C_MASTER_ST_LOSE) {
            hsi2cResetController();
            if (!hsi2cWaitIdle(10000)) {
                return false;
            }
        }

        wrI2c(HSI2C_INT_ENABLE, 0u);
        wrI2c(HSI2C_INT_STATUS, rdI2c(HSI2C_INT_STATUS));
        wrI2c(HSI2C_ADDR, (B2L_BAT_FG_ADDR & 0x3FFu) << 10);

        uint32_t fifoCtl = HSI2C_RXFIFO_EN | HSI2C_TXFIFO_EN;
        fifoCtl |= isRead ? (len << 4) : (len << 16);
        wrI2c(HSI2C_FIFO_CTL, fifoCtl);

        uint32_t ctl = HSI2C_FUNC_I2C | HSI2C_MASTER | (isRead ? HSI2C_RXCHON : HSI2C_TXCHON);
        wrI2c(HSI2C_CTL, ctl);

        uint32_t conf = (rdI2c(HSI2C_CONF) & ~HSI2C_HS_MODE) | HSI2C_AUTO_MODE;
        wrI2c(HSI2C_CONF, conf);

        uint32_t autoConf = HSI2C_MASTER_RUN | HSI2C_STOP_AFTER_TRANS | len;
        if (isRead) {
            autoConf |= HSI2C_READ_WRITE;
        }
        wrI2c(HSI2C_AUTO_CONF, autoConf);

        uint64_t deadline = uptimeUS() + 20000ULL;
        unsigned done = 0;
        bool transDone = false;

        while (uptimeUS() < deadline) {
            uint32_t st = rdI2c(HSI2C_INT_STATUS);
            if (st & HSI2C_INT_TRANS_ERRS) {
                wrI2c(HSI2C_INT_STATUS, st);
                hsi2cResetController();
                return false;
            }
            if (st & HSI2C_INT_TRANS_DONE) {
                wrI2c(HSI2C_INT_STATUS, st);
                transDone = true;
            }
            if (isRead) {
                while (done < len && !(rdI2c(HSI2C_FIFO_STATUS) & HSI2C_RX_FIFO_EMPTY)) {
                    buf[done++] = (uint8_t)(rdI2c(HSI2C_RX_DATA) & 0xFFu);
                }
            } else {
                while (done < len && !(rdI2c(HSI2C_FIFO_STATUS) & HSI2C_TX_FIFO_FULL)) {
                    wrI2c(HSI2C_TX_DATA, buf[done++]);
                }
            }
            if (done == len && (transDone || !(rdI2c(HSI2C_TRANS_STATUS) & HSI2C_MASTER_BUSY))) {
                hsi2cWaitIdle(5000);
                wrI2c(HSI2C_CTL, HSI2C_FUNC_I2C | HSI2C_MASTER);
                return true;
            }
            IODelay(20);
        }

        hsi2cResetController();
        return false;
    }

    bool readReg16(uint8_t reg, uint16_t *out) {
        if (useBitBang) {
            return bitBangRead16(reg, out);
        }
        uint8_t cmd = reg;
        uint8_t rx[2] = { 0, 0 };
        if (!hsi2cXfer(&cmd, 1, false)) {
            return false;
        }
        if (!hsi2cXfer(rx, 2, true)) {
            return false;
        }
        *out = (uint16_t)((uint16_t)rx[0] | ((uint16_t)rx[1] << 8));
        return true;
    }

    void pollOnce(void) {
        uint16_t repSocRaw = 0, vcellRaw = 0, avgCurrRaw = 0, repCapRaw = 0, fullCapRaw = 0, cyclesRaw = 0;
        if (!readReg16(B2L_FG_REG_REPSOC, &repSocRaw) ||
            !readReg16(B2L_FG_REG_VCELL, &vcellRaw)) {
            if (!firstSampleLogged) {
                firstSampleLogged = true;
                b2l_diag_record("B2L BATTERY: fuelgauge read failed (bb=%d)\n", useBitBang ? 1 : 0);
            }
            return;
        }
        (void)readReg16(B2L_FG_REG_AVGCURR, &avgCurrRaw);
        (void)readReg16(B2L_FG_REG_REPCAP, &repCapRaw);
        (void)readReg16(B2L_FG_REG_FULLCAP, &fullCapRaw);
        (void)readReg16(B2L_FG_REG_CYCLES, &cyclesRaw);

        unsigned int socPct = 0, vcellMv = 0, repCapMah = 0, fullCapMah = 0, cycles = 0;
        int avgCurrMa = 0;
        bool charging = false, extConn = false;

        b2l_fg_decode(repSocRaw, vcellRaw, (int16_t)avgCurrRaw,
                      repCapRaw, fullCapRaw, cyclesRaw,
                      &socPct, &vcellMv, &avgCurrMa,
                      &repCapMah, &fullCapMah, &cycles,
                      &charging, &extConn);

        if (vcellMv < 2000u || vcellMv > 5500u) {
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
            b2l_diag_record("B2L BATTERY: live soc=%u%% vcell=%umV curr=%dmA chg=%d rep=%u full=%u bb=%d\n",
                socPct, vcellMv, avgCurrMa, charging ? 1 : 0, repCapMah, fullCapMah, useBitBang ? 1 : 0);
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
    if (!enabled) {
        return false;
    }
    if (!IOPMPowerSource::start(provider)) {
        return false;
    }

    symDesignCap    = OSSymbol::withCStringNoCopy(kIOPMPSDesignCapacityKey);
    symRawCur       = OSSymbol::withCStringNoCopy("AppleRawCurrentCapacity");
    symRawMax       = OSSymbol::withCStringNoCopy("AppleRawMaxCapacity");
    symNomCap       = OSSymbol::withCStringNoCopy("NominalChargeCapacity");
    symFullyCharged = OSSymbol::withCStringNoCopy(kIOPMFullyChargedKey);
    symRawExtConn   = OSSymbol::withCStringNoCopy("AppleRawExternalConnected");

    pinDesc = IOMemoryDescriptor::withPhysicalAddress(B2L_BAT_CMGP_PIN_PHYS, 4096, kIODirectionInOut);
    if (pinDesc) {
        pinMap = pinDesc->map();
        if (pinMap) {
            pins = (volatile uint8_t *)pinMap->getVirtualAddress();
        }
    }
    if (!pins) {
        b2l_diag_record("B2L BATTERY: pinctrl map failed\n");
        IOPMPowerSource::stop(provider);
        return false;
    }

    uint32_t gpm12Con = rdPin(B2L_BAT_GPM12_OFF + B2L_BAT_PIN_CON);
    uint32_t gpm13Con = rdPin(B2L_BAT_GPM13_OFF + B2L_BAT_PIN_CON);
    uint32_t sclFn = gpm12Con & 0xFu;
    uint32_t sdaFn = gpm13Con & 0xFu;

    if (sclFn == 2u && sdaFn == 2u) {
        i2cDesc = IOMemoryDescriptor::withPhysicalAddress(B2L_BAT_I2C_PHYS, 4096, kIODirectionInOut);
        if (i2cDesc) {
            i2cMap = i2cDesc->map();
            if (i2cMap) {
                i2c = (volatile uint8_t *)i2cMap->getVirtualAddress();
            }
        }
    }

    if (i2c) {
        uint32_t usiCon = rdI2c(HSI2C_USI_CON);
        if (usiCon & 1u) {
            wrI2c(HSI2C_USI_CON, usiCon & ~1u);
            IODelay(10);
        }
        uint32_t fs1 = rdI2c(HSI2C_TIMING_FS1);
        uint32_t fs2 = rdI2c(HSI2C_TIMING_FS2);
        uint32_t fs3 = rdI2c(HSI2C_TIMING_FS3);
        uint32_t sla = rdI2c(HSI2C_TIMING_SLA);
        b2l_diag_record("B2L BATTERY: hsi2c@15d60000 ctl=%08x conf=%08x trans=%08x fs1=%08x fs2=%08x usi=%08x scl=%u sda=%u\n",
            rdI2c(HSI2C_CTL), rdI2c(HSI2C_CONF), rdI2c(HSI2C_TRANS_STATUS), fs1, fs2, usiCon, sclFn, sdaFn);

        savedFs1 = (fs1 && fs2) ? fs1 : B2L_BAT_DEFAULT_FS1;
        savedFs2 = (fs1 && fs2) ? fs2 : B2L_BAT_DEFAULT_FS2;
        savedFs3 = (fs1 && fs2) ? fs3 : B2L_BAT_DEFAULT_FS3;
        savedSla = sla ? sla : B2L_BAT_DEFAULT_SLA;
        useBitBang = false;
    } else if (sclFn == 0u && sdaFn == 0u) {
        useBitBang = true;
        b2l_diag_record("B2L BATTERY: using gpm12/13 bitbang I2C (scl=%u sda=%u)\n", sclFn, sdaFn);
    } else {
        b2l_diag_record("B2L BATTERY: unexpected pin state scl=%u sda=%u\n", sclFn, sdaFn);
        IOPMPowerSource::stop(provider);
        return false;
    }

    pollOnce();

    /* Ensure battery properties exist even if initial read deferred to timer. */
    if (!firstSampleLogged || !batteryInstalled()) {
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
    if (i2cMap) { i2cMap->release(); i2cMap = nullptr; }
    if (i2cDesc) { i2cDesc->release(); i2cDesc = nullptr; }
    i2c = nullptr;
    if (pinMap) { pinMap->release(); pinMap = nullptr; }
    if (pinDesc) { pinDesc->release(); pinDesc = nullptr; }
    pins = nullptr;
    IOPMPowerSource::free();
}

#endif /* B2L */
