/* SPDX-License-Identifier: BSD-2-Clause
 * B2L Bluetooth, step 1 (B1L-0210): does the BCM4375's Bluetooth half answer HCI on its UART?
 *
 * The S10's BCM4375 runs Wi-Fi over PCIe (B2LWifi.cpp) and Bluetooth over the dedicated UART_BT
 * (uart@10840000, DT alias uart1, "samsung,rts-gpio-control", USI v2).  Values come from the S10's
 * live DTB and the Samsung 4.14 kernel (drivers/bluetooth/bcm43xx.c, drivers/tty/serial/samsung.c,
 * cal-if/exynos9820/cmucal-sfr.c, pinctrl-exynos-arm64.c):
 *   /bluetooth gpios = <gpm8 0>, <gpg2 0>, <gpa2 3>: BT_EN (pinctrl@15C30000 +0x100),
 *   BT_WAKE (pinctrl@10430000 +0xC0), HOST_WAKE (pinctrl@15850000 +0x40, input, read only).
 *   UART pins gpq0-0..3 (pinctrl@15850000 +0xA0), function 2, pull-up.
 *   Clock: CMU_PERIC1 UART_BT_USER mux (bit 4: 0 = OSCCLK 26 MHz) -> DIV_CLK_PERIC1_UART_BT -> gates.
 *
 * What it does, in a thread, never in start():
 *   A (always, read only): DT node, pin banks, CMU_PERIC1 mux/div/gates/QCH of UART_BT.
 *   B (b2l_bt>=2, default): only if the UART_BT gates are open, after B2LWifi's firmware seed is
 *     ready + 30 s (BT_REG_ON is not raised while WLAN comes up): the UART_BT user mux moved to
 *     OSCCLK (boot value 0x10 = CLKCMU_PERIC1_IP, rate unknown; restored at the end, step 1b),
 *     UART registers read, then UART at 115200 8N1 with RTS/CTS, BT_WAKE=1, BT_EN=1, 100 ms
 *     (bcm43xx.c), HCI_Reset and HCI_Read_Local_Version, the answers logged, then BT_EN=0 and
 *     BT_WAKE=0 again: the chip ends powered off as Android leaves it at boot (rfkill blocked).
 *   Every register written is logged with its value before.  Nothing writes storage.
 *   Build 0211 observed: HCI_Reset OK, ROM hci=9 lmp=9 manufacturer=0xf lmp_subver=0x1111.
 *   C (b2l_bt>=3, default; step 2): between the ROM version and power-off, Read_Local_Name, then the
 *     patch RAM as Linux btbcm_patchram: Download_Minidriver (0xfc2e), 50 ms, every .hcd record
 *     (Write_RAM 0xfc4c ... Launch_RAM 0xfc4e) each waiting for its Command Complete, 250 ms,
 *     HCI_Reset, version and name again.  The .hcd is SYSTEM:/b2l-wifi/bt.hcd (bcm4375B1_semco.hcd,
 *     from the phone's vendor, kept out of git), copied at seed time by b2l_bt_fetch().
 *
 * Kill switch: b2l_bt=0 (nothing at all), b2l_bt=1 (A only), b2l_bt=2 (A+B, no firmware).
 * Log prefix "B2L BLUETOOTH" ("B2L BT" is the kernel backtrace marker).
 */
#include <pexpert/arm64/board_config.h>
#if defined(B2L)
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <pexpert/pexpert.h>
#include <pexpert/device_tree.h>
#include <arm64/b2l_diag.h>
#include <kern/thread.h>
#include <kern/clock.h>
#include <sys/errno.h>
#include <sys/kern_control.h>
#include <sys/kpi_mbuf.h>

extern "C" vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
extern "C" {
extern volatile int b2l_wifi_fw_state;
int b2l_system_file_copy(const char *, int (*)(void *, uint64_t, const void *, size_t), void *, uint64_t *);
void b2l_bt_fetch(void);
}

/* Step 2: patch RAM file, copied once by the seed (b2l_system_file_copy only works there, before
 * launchd), freed by the thread.  State: 0 pending, 1 ready, <0 -errno. */
#define BT_HCD_CAP (256u << 10)   /* bcm4375B1_semco.hcd is 176 645 bytes */
static uint8_t *b2l_bt_hcd;
static size_t b2l_bt_hcd_len;
static volatile int b2l_bt_fw_state;

static int
bt_hcd_sink(void *, uint64_t off, const void *buf, size_t n)
{
	if (off > BT_HCD_CAP || n > BT_HCD_CAP - off) {
		return EFBIG;
	}
	memcpy(b2l_bt_hcd + off, buf, n);
	return 0;
}

extern "C" void
b2l_bt_fetch(void)
{
	int level = 4;
	PE_parse_boot_argn("b2l_bt", &level, sizeof(level));
	if (level < 3) {
		b2l_bt_fw_state = -1;
		return;
	}
	uint64_t len = 0;
	b2l_bt_hcd = (uint8_t *)IOMallocData(BT_HCD_CAP);
	int e = b2l_bt_hcd ? b2l_system_file_copy("b2l-wifi/bt.hcd", bt_hcd_sink, nullptr, &len) : ENOMEM;
	if (e && b2l_bt_hcd) {
		IOFreeData(b2l_bt_hcd, BT_HCD_CAP);
		b2l_bt_hcd = nullptr;
	}
	b2l_bt_hcd_len = e ? 0 : (size_t)len;
	__asm__ volatile("dmb ish" ::: "memory");
	b2l_bt_fw_state = e ? -e : 1;
	b2l_diag_record("B2L BLUETOOTH FW: fetch bt.hcd %llu bytes, error %d\n", len, e);
}

#define BT_PIN_ALIVE_PHYS   0x15850000ULL   /* gpa0..4, gpq0 */
#define BT_PIN_PERI0_PHYS   0x10430000ULL   /* gpp0..3, gpg0, gpg1, gpg2, gpg4 */
#define BT_PIN_CMGP_PHYS    0x15C30000ULL   /* gpm0..31, one pin each */
#define BT_GPA2             0x040u
#define BT_GPQ0             0x0A0u
#define BT_GPG2             0x0C0u
#define BT_GPM8             0x100u
#define BT_HOSTWAKE_PIN     3u
#define PIN_CON 0x00u
#define PIN_DAT 0x04u
#define PIN_PUD 0x08u

#define BT_CMU_PHYS         0x10800000ULL   /* CMU_PERIC1 */
#define BT_CMU_MUX          0x01c0u         /* PLL_CON0_MUX_CLKCMU_PERIC1_UART_BT_USER: bit 4 sel, bit 7 busy */
#define BT_CMU_MUX2         0x01c8u
#define BT_CMU_DIV          0x1814u         /* DIV_CLK_PERIC1_UART_BT: ratio bits 0..3 */
#define BT_CMU_GATE_RST     0x2020u         /* gates: bit 20 manual, bit 21 value, bit 28 auto */
#define BT_CMU_GATE_IPCLK   0x2098u
#define BT_CMU_GATE_PCLK    0x209cu
#define BT_CMU_QCH          0x3038u         /* QCH_CON_UART_BT_QCH: bit 0 hw enable, bit 1 clock req */
#define BT_OSCCLK_HZ        26000000u       /* DT oscclk clock-frequency 0x18cba80 */

#define BT_UART_PHYS        0x10840000ULL
#define U_LCON    0x00u
#define U_CON     0x04u
#define U_FCON    0x08u
#define U_MCON    0x0Cu
#define U_TRSTAT  0x10u
#define U_ERSTAT  0x14u
#define U_FSTAT   0x18u
#define U_MSTAT   0x1Cu
#define U_TXH     0x20u
#define U_RXH     0x24u
#define U_BRDIV   0x28u
#define U_FRACVAL 0x2Cu
#define U_INTP    0x30u
#define U_INTM    0x38u
#define USI_CON   0xC4u
#define USI_OPT   0xC8u
#define UFSTAT_TXFULL (1u << 24)
#define UFSTAT_RXFULL (1u << 8)
#define UTRSTAT_TXE   (1u << 2)

/* samsung.c with has_fracval: div = rate/baud rounded, UBRDIV = div/16 - 1, UFRACVAL = div % 16. */
static constexpr uint32_t bt_div(uint32_t rate, uint32_t baud) { return (rate + baud / 2) / baud; }
static_assert(bt_div(BT_OSCCLK_HZ, 115200) / 16 - 1 == 13 && bt_div(BT_OSCCLK_HZ, 115200) % 16 == 2,
    "26 MHz / 115200 must give UBRDIV 13, UFRACVAL 2 as Linux");

class B2LBluetooth : public IOService {
	OSDeclareDefaultStructors(B2LBluetooth);
	volatile uint8_t *alive, *peri0, *cmgp, *cmu, *uart;
	int level;
	uint32_t muxBoot;

	static uint32_t rd(volatile uint8_t *b, unsigned o) {
		uint32_t v = *(volatile uint32_t *)(b + o);
		__asm__ volatile("dmb oshld" ::: "memory");
		return v;
	}
	static void wr(volatile uint8_t *b, unsigned o, uint32_t v) {
		*(volatile uint32_t *)(b + o) = v;
		__asm__ volatile("dsb oshst" ::: "memory");
	}
	/* Logged write: the value before is in the log (owner's rule for unknown blocks). */
	static void wrl(const char *what, volatile uint8_t *b, unsigned o, uint32_t v) {
		uint32_t before = rd(b, o);
		wr(b, o, v);
		b2l_diag_record("B2L BLUETOOTH: write %s +%#x %#x -> %#x (read back %#x)\n", what, o, before, v, rd(b, o));
	}
	static void setPinFunc(const char *what, volatile uint8_t *b, unsigned bank, unsigned pin, uint32_t func) {
		uint32_t con = rd(b, bank + PIN_CON);
		wrl(what, b, bank + PIN_CON, (con & ~(0xfu << (pin * 4))) | (func << (pin * 4)));
	}
	static void setPinOut(const char *what, volatile uint8_t *b, unsigned bank, unsigned pin, bool high) {
		uint32_t dat = rd(b, bank + PIN_DAT);
		wrl(what, b, bank + PIN_DAT, high ? (dat | (1u << pin)) : (dat & ~(1u << pin)));
		setPinFunc(what, b, bank, pin, 1);   /* function 1 = output */
	}
	void auditPins(const char *when);
	bool clockUsable(uint32_t *rateOut);
	unsigned otherEvents;
	bool sendCmd(uint16_t op, const uint8_t *p, uint8_t n);
	int readEvent(uint8_t *buf, int max, unsigned ms);
	int cmd(uint16_t op, const uint8_t *p, uint8_t n, unsigned ms, uint8_t *ev, int *evLen);
	int logVersion(const char *when);
	void logName(const char *when);
	bool patchram(void);
	void relayLoop(void);
	void hciTest(uint32_t rate);
	static void btThread(void *arg, wait_result_t);
public:
	bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
};
OSDefineMetaClassAndStructors(B2LBluetooth, IOService);

bool
B2LBluetooth::start(IOService *provider)
{
	if (!IOService::start(provider)) {
		return false;
	}
	level = 4;
	otherEvents = 0;
	PE_parse_boot_argn("b2l_bt", &level, sizeof(level));
	if (level <= 0) {
		b2l_diag_record("B2L BLUETOOTH: off (b2l_bt=0)\n");
		return true;
	}
	thread_t th;
	retain();
	if (kernel_thread_start(&B2LBluetooth::btThread, this, &th) == KERN_SUCCESS) {
		thread_deallocate(th);
	} else {
		release();
		b2l_diag_record("B2L BLUETOOTH: thread start failed\n");
	}
	return true;   /* never on the boot path */
}

void
B2LBluetooth::auditPins(const char *when)
{
	b2l_diag_record("B2L BLUETOOTH PINS[%s]: gpm8 con=%#x dat=%#x pud=%#x | gpg2 con=%#x dat=%#x pud=%#x | "
	    "gpa2 con=%#x dat=%#x pud=%#x hostwake=%u | gpq0 con=%#x dat=%#x pud=%#x\n", when,
	    rd(cmgp, BT_GPM8 + PIN_CON), rd(cmgp, BT_GPM8 + PIN_DAT), rd(cmgp, BT_GPM8 + PIN_PUD),
	    rd(peri0, BT_GPG2 + PIN_CON), rd(peri0, BT_GPG2 + PIN_DAT), rd(peri0, BT_GPG2 + PIN_PUD),
	    rd(alive, BT_GPA2 + PIN_CON), rd(alive, BT_GPA2 + PIN_DAT), rd(alive, BT_GPA2 + PIN_PUD),
	    (rd(alive, BT_GPA2 + PIN_DAT) >> BT_HOSTWAKE_PIN) & 1,
	    rd(alive, BT_GPQ0 + PIN_CON), rd(alive, BT_GPQ0 + PIN_DAT), rd(alive, BT_GPQ0 + PIN_PUD));
}

/* Read only.  A gate is off when manual (bit 20) with value 0 (bit 21); the QCH is off when
 * neither hardware-controlled (bit 0) nor requested (bit 1).  Touching UART_BT then would hang the bus. */
bool
B2LBluetooth::clockUsable(uint32_t *rateOut)
{
	uint32_t mux = rd(cmu, BT_CMU_MUX), mux2 = rd(cmu, BT_CMU_MUX2), div = rd(cmu, BT_CMU_DIV);
	uint32_t gr = rd(cmu, BT_CMU_GATE_RST), gi = rd(cmu, BT_CMU_GATE_IPCLK), gp = rd(cmu, BT_CMU_GATE_PCLK);
	uint32_t q = rd(cmu, BT_CMU_QCH);
	b2l_diag_record("B2L BLUETOOTH CMU: mux=%#x mux2=%#x div=%#x gate rst=%#x ipclk=%#x pclk=%#x qch=%#x\n",
	    mux, mux2, div, gr, gi, gp, q);
	auto gateOff = [](uint32_t g) { return (g & (1u << 20)) && !(g & (1u << 21)); };
	if (gateOff(gr) || gateOff(gi) || gateOff(gp) || !(q & 3u)) {
		b2l_diag_record("B2L BLUETOOTH: UART_BT clock off, UART left alone (no CMU write in this step)\n");
		return false;
	}
	/* Build 0210: boot value mux=0x10 (CLKCMU_PERIC1_IP, rate unknown).  hciTest() moves this user
	 * mux, which feeds UART_BT alone, to OSCCLK and puts it back at the end. */
	muxBoot = mux;
	*rateOut = BT_OSCCLK_HZ / ((div & 0xfu) + 1);
	b2l_diag_record("B2L BLUETOOTH: UART_BT gates open, %u Hz once on OSCCLK (mux now %s)\n", *rateOut,
	    (mux & (1u << 4)) ? "CLKCMU_PERIC1_IP" : "OSCCLK");
	return true;
}

bool
B2LBluetooth::sendCmd(uint16_t op, const uint8_t *p, uint8_t n)
{
	uint8_t pkt[4 + 255] = { 0x01, (uint8_t)op, (uint8_t)(op >> 8), n };
	if (n) {
		memcpy(pkt + 4, p, n);
	}
	for (unsigned i = 0; i < 4u + n; i++) {
		unsigned spins = 0;
		while ((rd(uart, U_FSTAT) & UFSTAT_TXFULL) && spins++ < 2000) {
			IODelay(10);
		}
		wr(uart, U_TXH, pkt[i]);
	}
	for (unsigned t = 0; t < 200; t++) {   /* a full 259-byte packet takes 22.5 ms at 115200 */
		if (rd(uart, U_TRSTAT) & UTRSTAT_TXE) {
			return true;
		}
		IOSleep(1);
	}
	b2l_diag_record("B2L BLUETOOTH: HCI %#06x not sent (CTS from the chip? umstat=%#x ufstat=%#x)\n",
	    op, rd(uart, U_MSTAT), rd(uart, U_FSTAT));
	return false;
}

/* Collects bytes until one complete H4 event (0x04 code len ...) or the timeout; returns the count.
 * Bytes before an 0x04 are dropped (resync). */
int
B2LBluetooth::readEvent(uint8_t *buf, int max, unsigned ms)
{
	int n = 0;
	for (unsigned t = 0; t <= ms; t++) {
		uint32_t fs = rd(uart, U_FSTAT);
		unsigned avail = (fs & UFSTAT_RXFULL) ? 256u : (fs & 0xffu);
		while (avail-- && n < max) {
			buf[n] = (uint8_t)rd(uart, U_RXH);
			if (n > 0 || buf[0] == 0x04) {
				n++;
			}
			if (n >= 3 && n >= 3 + buf[2]) {
				return n;   /* never read past one event: the next stays in the FIFO */
			}
		}
		if (n >= max) {
			return n;
		}
		IOSleep(1);   /* 256-byte FIFO = 22 ms at 115200: 1 ms polls cannot overrun */
	}
	return n;
}

static void
bt_hex(char *out, size_t cap, const uint8_t *b, int n)
{
	size_t at = 0;
	out[0] = 0;
	for (int i = 0; i < n && at + 4 < cap; i++) {
		at += snprintf(out + at, cap - at, "%02x ", b[i]);
	}
}

/* Sends one command and waits for its Command Complete (status = ev[6]) or Command Status
 * (status = ev[3]); other events are counted and skipped.  -1 = no answer, -2 = not sent. */
int
B2LBluetooth::cmd(uint16_t op, const uint8_t *p, uint8_t n, unsigned ms, uint8_t *ev, int *evLen)
{
	if (!sendCmd(op, p, n)) {
		return -2;
	}
	uint64_t deadline, now;
	clock_interval_to_deadline(ms, kMillisecondScale, &deadline);
	while ((now = mach_absolute_time()) < deadline) {
		uint64_t ns;
		absolutetime_to_nanoseconds(deadline - now, &ns);
		int got = readEvent(ev, 3 + 255, (unsigned)(ns / 1000000));
		if (got < 3 || got < 3 + ev[2]) {
			break;
		}
		*evLen = got;
		if (ev[1] == 0x0e && ev[2] >= 4 && (ev[4] | (ev[5] << 8)) == op) {
			return ev[6];
		}
		if (ev[1] == 0x0f && ev[2] >= 4 && (ev[5] | (ev[6] << 8)) == op) {
			return ev[3];
		}
		otherEvents++;
	}
	return -1;
}

/* Read_Local_Version, logged; returns lmp_subver or -1. */
int
B2LBluetooth::logVersion(const char *when)
{
	uint8_t ev[3 + 255];
	int n = 0;
	int st = cmd(0x1001, nullptr, 0, 1000, ev, &n);
	if (st != 0 || n != 15) {
		b2l_diag_record("B2L BLUETOOTH[%s]: Read_Local_Version status %d (%d bytes)\n", when, st, n);
		return -1;
	}
	b2l_diag_record("B2L BLUETOOTH[%s]: hci=%u rev=%#x lmp=%u manufacturer=%#x lmp_subver=%#x\n", when,
	    ev[7], ev[8] | (ev[9] << 8), ev[10], ev[11] | (ev[12] << 8), ev[13] | (ev[14] << 8));
	return (int)(((uint32_t)(ev[13] | (ev[14] << 8)) << 16) | (uint32_t)(ev[8] | (ev[9] << 8)));
}

/* Read_Local_Name: the ROM gives the chip name (BCM4375B1 expected), the patch its config name. */
void
B2LBluetooth::logName(const char *when)
{
	uint8_t ev[3 + 255];
	int n = 0;
	int st = cmd(0x0c14, nullptr, 0, 1000, ev, &n);
	char name[49] = { 0 };
	for (int i = 0; st == 0 && i < 48 && 7 + i < n && ev[7 + i]; i++) {
		name[i] = (ev[7 + i] >= 0x20 && ev[7 + i] < 0x7f) ? (char)ev[7 + i] : '?';
	}
	b2l_diag_record("B2L BLUETOOTH[%s]: Read_Local_Name status %d \"%s\"\n", when, st, name);
}

/* Linux btbcm_patchram: minidriver, every .hcd record answered, launch, 250 ms. */
bool
B2LBluetooth::patchram(void)
{
	uint8_t ev[3 + 255];
	int n = 0;
	uint64_t t0 = mach_absolute_time(), ns;
	int st = cmd(0xfc2e, nullptr, 0, 1000, ev, &n);
	b2l_diag_record("B2L BLUETOOTH FW: Download_Minidriver status %d\n", st);
	if (st != 0) {
		return false;
	}
	IOSleep(50);
	const uint8_t *f = b2l_bt_hcd;
	size_t len = b2l_bt_hcd_len, off = 0;
	unsigned recs = 0;
	bool launched = false;
	while (off + 3 <= len && !launched) {
		uint16_t op = (uint16_t)(f[off] | (f[off + 1] << 8));
		uint8_t pl = f[off + 2];
		if (off + 3 + pl > len) {
			b2l_diag_record("B2L BLUETOOTH FW: record %u truncated at %zu/%zu\n", recs, off, len);
			return false;
		}
		launched = op == 0xfc4e;
		st = cmd(op, f + off + 3, pl, 1000, ev, &n);
		/* tolerated: the chip may restart before it answers Launch_RAM */
		if (st != 0 && !(launched && st == -1)) {
			b2l_diag_record("B2L BLUETOOTH FW: record %u op %#06x (%u bytes) status %d\n", recs, op, pl, st);
			return false;
		}
		recs++;
		off += 3u + pl;
	}
	absolutetime_to_nanoseconds(mach_absolute_time() - t0, &ns);
	b2l_diag_record("B2L BLUETOOTH FW: %u records, %zu/%zu bytes, launch %s, %llu ms, %u other events\n",
	    recs, off, len, launched ? "sent" : "MISSING", ns / 1000000, otherEvents);
	IOSleep(250);
	return launched;
}

#define BT_TX_RING_SZ 16384u
static kern_ctl_ref g_bt_ctl;
static IOLock *g_bt_tx_lock;
static uint8_t g_bt_tx_ring[BT_TX_RING_SZ];
static unsigned g_bt_tx_head, g_bt_tx_tail;
static volatile uint32_t g_bt_data_unit;
static unsigned g_bt_ios_tx_cnt;

static errno_t
bt_ctl_connect(kern_ctl_ref kctlref __unused, struct sockaddr_ctl *sac __unused, void **unitinfo __unused)
{
	return 0;
}

static errno_t
bt_ctl_disconnect(kern_ctl_ref kctlref __unused, u_int32_t unit, void *unitinfo __unused)
{
	if (g_bt_data_unit == unit) {
		g_bt_data_unit = 0;
	}
	return 0;
}

static errno_t
bt_ctl_send(kern_ctl_ref kctlref __unused, u_int32_t unit, void *unitinfo __unused, mbuf_t m, int flags __unused)
{
	if (g_bt_data_unit != unit) {
		g_bt_data_unit = unit;
		b2l_diag_record("B2L BLUETOOTH H4: client connected (unit %u)\n", unit);
	}
	if (g_bt_tx_lock) {
		IOLockLock(g_bt_tx_lock);
		for (mbuf_t cur = m; cur; cur = mbuf_next(cur)) {
			const uint8_t *p = (const uint8_t *)mbuf_data(cur);
			size_t n = mbuf_len(cur);
			for (size_t i = 0; i < n; i++) {
				unsigned nxt = (g_bt_tx_head + 1u) % BT_TX_RING_SZ;
				if (nxt != g_bt_tx_tail) {
					g_bt_tx_ring[g_bt_tx_head] = p[i];
					g_bt_tx_head = nxt;
				}
			}
		}
		if (g_bt_ios_tx_cnt < 8) {
			g_bt_ios_tx_cnt++;
			b2l_diag_record("B2L BLUETOOTH: iOS sends HCI (%u)\n", g_bt_ios_tx_cnt);
		}
		IOLockUnlock(g_bt_tx_lock);
	}
	mbuf_freem(m);
	return 0;
}

static errno_t
bt_ctl_setopt(kern_ctl_ref kctlref __unused, u_int32_t unit __unused, void *unitinfo __unused, int opt, void *data, size_t len)
{
	if (opt == 0xb2 && data && len > 0) {
		char msg[161];
		size_t n = len < 160 ? len : 160;
		memcpy(msg, data, n);
		msg[n] = '\0';
		b2l_diag_record("B2L BLUETOOTH TOOL: %s\n", msg);
	}
	return 0;
}

void
B2LBluetooth::relayLoop(void)
{
	uint8_t rxbuf[256];
	for (;;) {
		bool did = false;
		if (g_bt_tx_lock) {
			IOLockLock(g_bt_tx_lock);
			while (g_bt_tx_tail != g_bt_tx_head) {
				if (rd(uart, U_FSTAT) & UFSTAT_TXFULL) {
					break;
				}
				wr(uart, U_TXH, g_bt_tx_ring[g_bt_tx_tail]);
				g_bt_tx_tail = (g_bt_tx_tail + 1u) % BT_TX_RING_SZ;
				did = true;
			}
			IOLockUnlock(g_bt_tx_lock);
		}
		uint32_t fs = rd(uart, U_FSTAT);
		unsigned avail = (fs & UFSTAT_RXFULL) ? 256u : (fs & 0xffu);
		unsigned rxn = 0;
		while (avail-- && rxn < sizeof(rxbuf)) {
			rxbuf[rxn++] = (uint8_t)rd(uart, U_RXH);
			did = true;
		}
		uint32_t unit = g_bt_data_unit;
		if (rxn > 0 && g_bt_ctl && unit) {
			ctl_enqueuedata(g_bt_ctl, unit, rxbuf, rxn, 0);
		}
		if (!did) {
			IOSleep(1);
		}
	}
}

void
B2LBluetooth::hciTest(uint32_t rate)
{
	if (muxBoot & (1u << 4)) {
		wrl("cmu uart_bt mux", cmu, BT_CMU_MUX, muxBoot & ~(1u << 4));
		for (unsigned i = 0; i < 100 && (rd(cmu, BT_CMU_MUX) & (1u << 7)); i++) {   /* BUSY */
			IODelay(10);
		}
	}
	b2l_diag_record("B2L BLUETOOTH UART[before]: ulcon=%#x ucon=%#x ufcon=%#x umcon=%#x utrstat=%#x uerstat=%#x "
	    "ufstat=%#x umstat=%#x ubrdiv=%#x ufrac=%#x usi_con=%#x usi_opt=%#x\n",
	    rd(uart, U_LCON), rd(uart, U_CON), rd(uart, U_FCON), rd(uart, U_MCON), rd(uart, U_TRSTAT),
	    rd(uart, U_ERSTAT), rd(uart, U_FSTAT), rd(uart, U_MSTAT), rd(uart, U_BRDIV), rd(uart, U_FRACVAL),
	    rd(uart, USI_CON), rd(uart, USI_OPT));

	/* Pins: UART function 2 with pull-up on gpq0-0..3 (DT uart1-bus). */
	uint32_t con = rd(alive, BT_GPQ0 + PIN_CON), pud = rd(alive, BT_GPQ0 + PIN_PUD);
	wrl("gpq0 con", alive, BT_GPQ0 + PIN_CON, (con & ~0xffffu) | 0x2222u);
	wrl("gpq0 pud", alive, BT_GPQ0 + PIN_PUD, (pud & ~0xffffu) | 0x3333u);

	/* USI v2 (exynos_usi_init), then the port as samsung.c leaves it for 115200 8N1 CRTSCTS, polled. */
	wrl("usi_con", uart, USI_CON, 1);
	IODelay(5);
	wrl("usi_con", uart, USI_CON, 0);
	wrl("usi_opt", uart, USI_OPT, 2);       /* HWACG CLKREQ_ON */
	wrl("uintm", uart, U_INTM, 0xf);        /* no interrupt: we poll */
	wr(uart, U_INTP, 0xf);
	uint32_t div = bt_div(rate, 115200);
	wrl("ulcon", uart, U_LCON, 3);
	wrl("ubrdiv", uart, U_BRDIV, div / 16 - 1);
	wrl("ufrac", uart, U_FRACVAL, div % 16);
	wrl("ucon", uart, U_CON, 0x305);        /* TX/RX level, TX/RX polling mode */
	wrl("ufcon", uart, U_FCON, 0x7);        /* FIFO on, both reset */
	IODelay(10);
	wrl("umcon", uart, U_MCON, 1u << 4);    /* AFC: RTS/CTS by hardware */

	/* Power on as bcm43xx_bt_rfkill_set_power: BT_WAKE high, BT_EN high, 100 ms. */
	setPinOut("bt_wake", peri0, BT_GPG2, 0, true);
	setPinOut("bt_en", cmgp, BT_GPM8, 0, true);
	IOSleep(100);
	auditPins("on");
	uint8_t junk[32];
	int j = readEvent(junk, sizeof(junk), 20);
	b2l_diag_record("B2L BLUETOOTH: %d byte(s) waiting after power-on, uerstat=%#x umstat=%#x\n",
	    j, rd(uart, U_ERSTAT), rd(uart, U_MSTAT));

	uint8_t ev[3 + 255];
	char hex[3 * 32 + 1];
	int n = 0;
	int st = cmd(0x0c03, nullptr, 0, 1000, ev, &n);
	bt_hex(hex, sizeof(hex), ev, n < 32 ? n : 32);
	b2l_diag_record("B2L BLUETOOTH: HCI_Reset status %d -> %d byte(s): %s\n", st, n, hex);
	if (st == 0) {
		b2l_diag_record("B2L BLUETOOTH: THE CHIP ANSWERS (HCI_Reset OK at 115200)\n");
		int rom = logVersion("rom");
		logName("rom");
		if (level >= 3 && b2l_bt_fw_state == 1) {
			bool ok = patchram();
			int left = readEvent(junk, sizeof(junk), 20);   /* anything the restart printed */
			st = cmd(0x0c03, nullptr, 0, 1000, ev, &n);
			b2l_diag_record("B2L BLUETOOTH FW: HCI_Reset after launch status %d (%d leftover byte(s))\n", st, left);
			int now = st == 0 ? logVersion("patched") : -1;
			if (st == 0) {
				logName("patched");
			}
			if (ok && now >= 0 && now != rom) {
				b2l_diag_record("B2L BLUETOOTH FW: THE FIRMWARE RUNS (%#x -> %#x)\n", rom, now);
				if (level >= 4) {
					uint8_t mac[6] = { 0x02, 0x42, 0x32, 0x4c, 0x10, 0x02 };
					DTEntry chosen;
					const void *mp = nullptr;
					unsigned msz = 0;
					if (SecureDTLookupEntry(nullptr, "/chosen", &chosen) == kSuccess &&
					    SecureDTGetProperty(chosen, "mac-address-bluetooth0", &mp, &msz) == kSuccess &&
					    msz == 6) {
						memcpy(mac, mp, 6);
					}
					uint8_t revmac[6] = { mac[5], mac[4], mac[3], mac[2], mac[1], mac[0] };
					int wst = cmd(0xfc01, revmac, 6, 1000, ev, &n);
					int rst = cmd(0x1009, nullptr, 0, 1000, ev, &n);
					b2l_diag_record("B2L BLUETOOTH: synthetic address in place (write %d read %d)\n", wst, rst);

					g_bt_tx_lock = IOLockAlloc();
					struct kern_ctl_reg reg;
					bzero(&reg, sizeof(reg));
					strlcpy(reg.ctl_name, "com.apple.uart.sk.bluetooth", sizeof(reg.ctl_name));
					reg.ctl_sendsize = 32768;
					reg.ctl_recvsize = 32768;
					reg.ctl_connect = bt_ctl_connect;
					reg.ctl_disconnect = bt_ctl_disconnect;
					reg.ctl_send = bt_ctl_send;
					reg.ctl_setopt = bt_ctl_setopt;
					errno_t ce = ctl_register(&reg, &g_bt_ctl);

					setName("bluetooth");
					setProperty("compatible", "brcm,bcm4375");
					setProperty("local-mac-address", mac, 6);
					setProperty("transport-speed", 115200, 32);
					setProperty("B2LBluetoothReady", kOSBooleanTrue);
					registerService();
					b2l_diag_record("B2L BLUETOOTH: THE RADIO IS UP for iOS (ctl errno %d, registry \"bluetooth\" published)\n", ce);
					if (ce == 0) {
						if (b2l_bt_hcd) {
							IOFreeData(b2l_bt_hcd, BT_HCD_CAP);
							b2l_bt_hcd = nullptr;
						}
						relayLoop();
					}
				}
			} else {
				b2l_diag_record("B2L BLUETOOTH FW: firmware not confirmed (ok %d, subver %#x -> %#x)\n", ok, rom, now);
			}
		} else {
			b2l_diag_record("B2L BLUETOOTH FW: no download (level %d, fetch state %d)\n", level, b2l_bt_fw_state);
		}
	} else {
		b2l_diag_record("B2L BLUETOOTH: no HCI answer, uerstat=%#x ufstat=%#x umstat=%#x\n",
		    rd(uart, U_ERSTAT), rd(uart, U_FSTAT), rd(uart, U_MSTAT));
	}

	/* Back to rfkill-blocked: chip off, as Android leaves it at boot. */
	setPinOut("bt_en", cmgp, BT_GPM8, 0, false);
	setPinOut("bt_wake", peri0, BT_GPG2, 0, false);
	auditPins("off");
	if (muxBoot & (1u << 4)) {
		wrl("cmu uart_bt mux", cmu, BT_CMU_MUX, muxBoot);
	}
}

void
B2LBluetooth::btThread(void *arg, wait_result_t)
{
	B2LBluetooth *self = (B2LBluetooth *)arg;
	DTEntry node;
	const void *prop = nullptr;
	unsigned size = 0;
	bool dt = SecureDTLookupEntry(nullptr, "/bluetooth", &node) == kSuccess;
	uint32_t g[9] = { 0 };
	if (dt && SecureDTGetProperty(node, "gpios", &prop, &size) == kSuccess && size == sizeof(g)) {
		memcpy(g, prop, sizeof(g));
	}
	b2l_diag_record("B2L BLUETOOTH: /bluetooth %s, gpios <%#x %u %#x %#x %u %#x %#x %u %#x> level %d\n",
	    dt ? "present" : "missing", g[0], g[1], g[2], g[3], g[4], g[5], g[6], g[7], g[8], self->level);

	self->alive = (volatile uint8_t *)ml_io_map(BT_PIN_ALIVE_PHYS, 0x1000);
	self->peri0 = (volatile uint8_t *)ml_io_map(BT_PIN_PERI0_PHYS, 0x1000);
	self->cmgp = (volatile uint8_t *)ml_io_map(BT_PIN_CMGP_PHYS, 0x1000);
	self->cmu = (volatile uint8_t *)ml_io_map(BT_CMU_PHYS, 0x4000);
	if (!self->alive || !self->peri0 || !self->cmgp || !self->cmu) {
		b2l_diag_record("B2L BLUETOOTH: map failed\n");
		self->release();
		return;
	}
	self->auditPins("boot");
	uint32_t rate = 0;
	bool clk = self->clockUsable(&rate);
	/* The iOS DT is built by XnuLoader (DeviceTree.c) and has no /bluetooth: addresses come from
	 * the live Android DTB (docs/bluetooth/plan.md), so the node is only reported. */
	if (self->level >= 2 && clk) {
		unsigned waited = 0;
		while (b2l_wifi_fw_state == 0 && waited < 600000) {
			IOSleep(250);
			waited += 250;
		}
		IOSleep(30000);
		while (self->level >= 3 && b2l_bt_fw_state == 0 && waited < 660000) {   /* fetched just after wifi */
			IOSleep(250);
			waited += 250;
		}
		b2l_diag_record("B2L BLUETOOTH: start after wifi seed %d (%u ms) + 30 s, bt.hcd state %d (%zu bytes)\n",
		    b2l_wifi_fw_state, waited, b2l_bt_fw_state, b2l_bt_hcd_len);
		self->uart = (volatile uint8_t *)ml_io_map(BT_UART_PHYS, 0x1000);
		if (self->uart) {
			self->hciTest(rate);
		}
	}
	if (b2l_bt_hcd) {
		IOFreeData(b2l_bt_hcd, BT_HCD_CAP);
		b2l_bt_hcd = nullptr;
	}
	b2l_diag_record("B2L BLUETOOTH: step 2 done\n");
	self->release();
}
#endif /* B2L */
