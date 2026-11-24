/* SPDX-License-Identifier: BSD-2-Clause
 * Exynos 9820 eight-CPU A55/A75/M4 trial through the stock SMC conduit.
 * Firmware targets0..5 and0x100/0x101. No CPU_OFF, suspend or DVFS policy.
 */
#include <pexpert/arm64/board_config.h>
#if defined(B2L)
#include <IOKit/IOCPU.h>
#include <IOKit/IOLib.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSNumber.h>
#include <arm64/proc_reg.h>
extern "C" int get_preemption_level(void);
#include <kern/clock.h>
#include <kern/processor.h>
#include <kern/thread.h>
#include <mach/mach_time.h>
#include <pexpert/pexpert.h>
#include <pexpert/device_tree.h>
#include "b2l_secondary.h"
#include "B2LACPM.h"
#include "b2l_rtc.h"
#include "B2LMali.h"
#include "B2LThermal.h"
#include "B2LAudio.h"   /* B1L-0210: audio stage A0, read-only */
extern "C" void hakios_mali_init(void);   /* bsd/dev/b2l_mali_core.c (G3) */
#include <IOKit/IOBufferMemoryDescriptor.h>
extern "C" void FlushPoC_DcacheRegion(vm_offset_t va, size_t length);
#include "b2l_cpu_topology.h"
#include <arm64/b2l_cpu_affinity.h>

extern "C" void b2l_secondary_entry(void);
extern "C" uint64_t b2l_smp_progress(unsigned cpu);
extern "C" void b2l_early_trace(const char *, ...);
extern "C" void b2l_send_self_ipi(void);
extern "C" void b2l_send_ipi(unsigned cpu);
extern "C" bool b2l_cpu_ipi_ready(unsigned cpu);
extern "C" uint64_t b2l_cpu_irq_count(unsigned cpu, boolean_t timer);
extern "C" bool b2l_prepare_cpu_start(unsigned cpu);
extern "C" bool b2l_smp_rejected(void);
extern "C" processor_t b2l_bind_cpu(processor_t processor);

static_assert(MAX_CPUS == 8, "eight-CPU A55/A75/M4 experiment");
class B2LCPU;
static B2LCPU *b2l_cpus[MAX_CPUS];
static unsigned providers_ready, registered_cpus;
static uint64_t secondary_pa;
static volatile unsigned long long *secondary_page;
extern "C" vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
static void prepareSecondaryPage(void)
{
    DTEntry chosen; const void *property; unsigned size; uint64_t range[2];
    if (SecureDTLookupEntry(nullptr,"/chosen",&chosen)!=kSuccess ||
        SecureDTGetProperty(chosen,"b2l,secondary-trampoline",&property,&size)!=kSuccess || size!=16)
        panic("B2L SMP: missing firmware-owned low entry");
    memcpy(range,property,sizeof(range));
    if (!b2l_secondary_address(range[0]) || range[1]!=B2L_SECONDARY_BYTES)
        panic("B2L SMP: invalid low secondary allocation");
    secondary_pa=range[0];
    secondary_page=(volatile unsigned long long *)ml_io_map(secondary_pa,B2L_SECONDARY_BYTES);
    if (!secondary_page || !b2l_secondary_template(secondary_page) ||
        secondary_page[B2L_SECONDARY_TARGET_WORD] || secondary_page[B2L_SECONDARY_STAGE_WORD])
        panic("B2L SMP: low entry template is not pristine");
    b2l_early_trace("B2L SMP: low entry prepared pa=0x%llx size=16KiB\n",secondary_pa);
}
static uint32_t xcall_observed[MAX_CPUS];

/* Observe context without changing IRQ state, binding or preemption counts.
 * Never repair an imbalance by forcibly enabling preemption. */
static void startupContext(const char *stage, unsigned target, bool sleepable)
{
    int level = get_preemption_level();
    boolean_t irq = ml_get_interrupts_enabled();
    b2l_early_trace("B2L CTX: %s target=%u current=%u preempt=%d irq=%u thread=%p\n",
        stage, target, (unsigned)cpu_number(), level, (unsigned)irq, current_thread());
    if (sleepable && (level != 0 || !irq))
        panic("B2L SMP: non-sleepable context at %s target=%u current=%u preempt=%d irq=%u",
            stage, target, (unsigned)cpu_number(), level, (unsigned)irq);
}


/* SMCCC v1.0 permits x0..x17 to change. No version/feature probe is needed:
 * the stock DT explicitly supplies this legacy CPU_ON ID and SMC conduit. */
static int32_t b2l_cpu_on(uint64_t target, uint64_t entry, uint64_t context)
{
    register uint64_t x0 __asm__("x0") = 0xc4000003ULL;
    register uint64_t x1 __asm__("x1") = target;
    register uint64_t x2 __asm__("x2") = entry;
    register uint64_t x3 __asm__("x3") = context;
    register uint64_t x4 __asm__("x4") = 0;
    register uint64_t x5 __asm__("x5") = 0;
    register uint64_t x6 __asm__("x6") = 0;
    register uint64_t x7 __asm__("x7") = 0;
    __asm__ volatile("smc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3),
        "+r"(x4), "+r"(x5), "+r"(x6), "+r"(x7) : :
        "x8", "x9", "x10", "x11", "x12", "x13",
        "x14", "x15", "x16", "x17", "cc", "memory");
    return (int32_t)x0;
}
/* Read-only diagnostic: cycles per architected-timer interval on the current
 * CPU. Saves and restores the PMU state; no clock or voltage is changed. */
static uint64_t b2l_measure_cpu_khz(void)
{
    boolean_t ints = ml_set_interrupts_enabled(FALSE);
    uint64_t pmcr = __builtin_arm_rsr64("PMCR_EL0");
    uint64_t enabled = __builtin_arm_rsr64("PMCNTENSET_EL0");
    uint64_t filter = __builtin_arm_rsr64("PMCCFILTR_EL0");
    __builtin_arm_wsr64("PMCCFILTR_EL0", 0);
    __builtin_arm_wsr64("PMCR_EL0", (pmcr | 1ULL | 4ULL) & ~(2ULL | 8ULL));
    __builtin_arm_wsr64("PMCNTENSET_EL0", 1ULL << 31);
    __builtin_arm_isb(15);
    uint64_t frq = __builtin_arm_rsr64("CNTFRQ_EL0"), span = frq / 5000; /* 200 us: below the 461 us masked-IRQ limit */
    uint64_t t0 = __builtin_arm_rsr64("CNTVCT_EL0"), c0 = __builtin_arm_rsr64("PMCCNTR_EL0"), t1;
    do { __builtin_arm_isb(15); t1 = __builtin_arm_rsr64("CNTVCT_EL0"); } while (t1 - t0 < span);
    uint64_t c1 = __builtin_arm_rsr64("PMCCNTR_EL0");
    if (!(enabled & (1ULL << 31))) __builtin_arm_wsr64("PMCNTENCLR_EL0", 1ULL << 31);
    __builtin_arm_wsr64("PMCR_EL0", pmcr);
    __builtin_arm_wsr64("PMCCFILTR_EL0", filter);
    __builtin_arm_isb(15);
    ml_set_interrupts_enabled(ints);
    return (t1 > t0 && frq >= 1000) ? (c1 - c0) * (frq / 1000) / (t1 - t0) : 0;
}

static void xcall_proof(void *arg)
{
    /* XNU uses a non-NULL parameter as the xcall publication sentinel. */
    uintptr_t encoded = (uintptr_t)arg;
    if (!encoded || encoded > MAX_CPUS)
        panic("B2L CPU: invalid xcall proof parameter");
    unsigned expected = (unsigned)(encoded - 1);
    if ((unsigned)cpu_number() != expected)
        panic("B2L CPU: xcall ran on unexpected CPU");
    __atomic_store_n(&xcall_observed[expected], 1, __ATOMIC_RELEASE);
}

class B2LCPU : public IOCPU {
    OSDeclareDefaultStructors(B2LCPU);
    const OSSymbol *cpuName = nullptr;
    unsigned cpuInitialized = 0;
public:
    bool start(IOService *) APPLE_KEXT_OVERRIDE;
    void registerProcessor(void);
    void free(void) APPLE_KEXT_OVERRIDE;
    void initCPU(bool) APPLE_KEXT_OVERRIDE;
    kern_return_t startCPU(vm_offset_t, vm_offset_t) APPLE_KEXT_OVERRIDE;
    void signalCPU(IOCPU *) APPLE_KEXT_OVERRIDE;
    void quiesceCPU(void) APPLE_KEXT_OVERRIDE;
    void haltCPU(void) APPLE_KEXT_OVERRIDE;
    const OSSymbol *getCPUName(void) APPLE_KEXT_OVERRIDE;
    static void finishStartup(void *, wait_result_t);
    static void waitForStartup(unsigned);
};
OSDefineMetaClassAndStructors(B2LCPU, IOCPU);

bool B2LCPU::start(IOService *provider)
{
    const ml_topology_info_t *top = ml_get_topology_info();
    OSNumber *logical = OSDynamicCast(OSNumber, provider->getProperty("logical-cpu-id"));
    OSData *reg = OSDynamicCast(OSData, provider->getProperty("reg"));
    if (!logical || !reg || reg->getLength() != sizeof(uint32_t) ||
        top->num_cpus < 1 || top->num_cpus > MAX_CPUS || top->num_clusters != 3 || top->boot_cpu != &top->cpus[0] ||
        top->cpus[0].phys_id != 0 ||
        logical->unsigned32BitValue() >= MAX_CPUS) return false;
    unsigned id = logical->unsigned32BitValue();
    const ml_topology_cpu_t *cpu = &top->cpus[id];
    const cluster_type_t expectedType = id < 4 ? CLUSTER_TYPE_E : id < 6 ? CLUSTER_TYPE_M : CLUSTER_TYPE_P;
    if (cpu->phys_id != (unsigned)b2l_cpu_firmware_id(id) ||
        cpu->cluster_id != b2l_cpu_cluster(id) || cpu->cluster_type != expectedType ||
        cpu->cluster_core_id != b2l_cpu_cluster_core(id) ||
        top->clusters[0].cpu_mask != 0x0f || top->clusters[1].cpu_mask != 0x30 || top->clusters[2].cpu_mask != 0xc0 ||
        *(const uint32_t *)reg->getBytesNoCopy() != cpu->phys_id) return false;
    if (!IOCPU::start(provider)) return false;
    cpuName = OSSymbol::withCStringNoCopy(id < 4 ? "Exynos 9820 Cortex-A55" : id < 6 ? "Exynos 9820 Cortex-A75" : "Exynos 9820 Samsung M4");
    if (!cpuName) return false;
    setCPUNumber(id);
    /* IOService matching is asynchronous. Collect all providers before a
     * single coordinator registers CPU0 first, then the secondaries. */
    B2LCPU *expected = nullptr;
    if (!__atomic_compare_exchange_n(&b2l_cpus[id], &expected, this, false,
            __ATOMIC_RELEASE, __ATOMIC_RELAXED))
        panic("B2L SMP: duplicate CPU provider %u", id);
    unsigned count = __atomic_add_fetch(&providers_ready, 1, __ATOMIC_ACQ_REL);
    b2l_early_trace("B2L SMP: provider CPU%u ready (%u/8)\n", id, count);
    if (count == MAX_CPUS) {
        thread_t coordinator;
        kern_return_t kr = kernel_thread_start(&B2LCPU::finishStartup, nullptr, &coordinator);
        if (kr != KERN_SUCCESS) panic("B2L CPU: coordinator failed (%d)", kr);
        thread_deallocate(coordinator);
    }
    return true;
}

void B2LCPU::registerProcessor(void)
{
    unsigned id = getCPUNumber();
    const ml_topology_cpu_t *cpu = &ml_get_topology_info()->cpus[id];
    startupContext("register-enter", id, true);
    ml_processor_info_t info = {};
    info.cpu_id = reinterpret_cast<cpu_id_t>(this);
    info.phys_id = cpu->phys_id;
    info.log_id = id;
    info.cluster_id = cpu->cluster_id;
    info.cluster_type = cpu->cluster_type;
    info.l2_cache_id = cpu->l2_cache_id;
    info.l2_cache_size = cpu->l2_cache_size;
    info.l3_cache_id = cpu->l3_cache_id;
    info.l3_cache_size = cpu->l3_cache_size;
    perfmon_interrupt_handler_func pmi = nullptr;
    /* XNU 12377 invokes processor_boot inside registration. Never call
     * processor_start again: that would violate its boot state machine. */
    b2l_early_trace("B2L SMP: register CPU%u begin\n", id);
    kern_return_t kr = ml_processor_register(&info, &machProcessor, &ipi_handler, &pmi);
    startupContext("native-register-return", id, true);
    if (kr != KERN_SUCCESS || pmi)
        panic("B2L CPU: registration failed (%d), PMI=%p", kr, pmi);
    b2l_early_trace("B2L SMP: register CPU%u returned\n", id);
    __atomic_add_fetch(&registered_cpus, 1, __ATOMIC_ACQ_REL);
    startupContext("publish-begin", id, true);
    registerService();
    startupContext("publish-return", id, true);
}

void B2LCPU::initCPU(bool boot)
{
    unsigned id = getCPUNumber();
    if (!boot || id >= MAX_CPUS || (unsigned)cpu_number() != id)
        panic("B2L CPU: unsupported resume or wrong startup CPU");
    uint64_t raw=__builtin_arm_rsr64("MPIDR_EL1"),midr=__builtin_arm_rsr64("MIDR_EL1");
    if(b2l_cpu_from_identity(raw,midr)!=(int)id || !b2l_cpu_midr_matches(id,midr))
        panic("B2L CPU: identity mismatch CPU%u MPIDR=0x%llx MIDR=0x%llx",id,raw,midr);
    startupContext("initCPU-enter", id, false);
    ml_broadcast_cpu_event(CPU_BOOTED, id);
    /* Secondary init runs at splsched with IRQs masked. Queue self-SGI
     * and return; waiting here would deadlock before secondary_cpu_main. */
    b2l_send_self_ipi();
    setCPUState(kIOCPUStateRunning);
    __atomic_store_n(&cpuInitialized, 1, __ATOMIC_RELEASE);
    startupContext("initCPU-return", id, false);
    b2l_early_trace("B2L CPU: cpu=%u entered initCPU, MPIDR=0x%llx\n", id,
        __builtin_arm_rsr64("MPIDR_EL1") & 0xffffULL);
}

kern_return_t B2LCPU::startCPU(vm_offset_t start, vm_offset_t arg)
{
    if (start || arg || getCPUNumber() == 0 || getCPUNumber() >= MAX_CPUS ||
        __atomic_load_n(&cpuInitialized, __ATOMIC_ACQUIRE))
        panic("B2L CPU: unsupported CPU_ON request");
    vm_offset_t entry = ml_static_vtop((vm_offset_t)&b2l_secondary_entry);
    if (!entry || (entry & 3)) panic("B2L CPU: invalid secondary entry");
    /* Secondaries start with caches off. Publish every reset input explicitly to
     * PoC rather than relying on a coherent-IO flush wrapper being active. */
    unsigned target = getCPUNumber();
    if (!b2l_prepare_cpu_start(target))
        panic("B2L CPU: secondary reset data incomplete");
    if (!secondary_page || !b2l_secondary_template(secondary_page) ||
        (secondary_page[B2L_SECONDARY_TARGET_WORD] && secondary_page[B2L_SECONDARY_TARGET_WORD]!=entry))
        panic("B2L SMP: secondary trampoline changed");
    secondary_page[B2L_SECONDARY_TARGET_WORD]=entry;
    secondary_page[B2L_SECONDARY_STAGE_WORD]=0;
    for(unsigned i=0;i<3;++i)secondary_page[B2L_SECONDARY_IDENTITY_WORD+i]=0;
    __asm__ volatile("dsb sy":::"memory");
    if (secondary_page[B2L_SECONDARY_TARGET_WORD]!=entry)
        panic("B2L SMP: low entry target readback failed");
    b2l_early_trace("B2L SMP: CPU_ON begin target=%u firmware=0x%x entry=0x%llx destination=0x%llx\n",
        target,(unsigned)b2l_cpu_firmware_id(target),secondary_pa,(uint64_t)entry);
    startupContext("CPU_ON-enter", target, false);
    int32_t result = b2l_cpu_on((uint64_t)b2l_cpu_firmware_id(target), secondary_pa, 0);
    startupContext("CPU_ON-return", target, false);
    b2l_early_trace("B2L SMP: CPU_ON result=%d low-stage=%llu secondary-stage=%llu\n",
        result,secondary_page[B2L_SECONDARY_STAGE_WORD],b2l_smp_progress(target));
    /* XNU discards the startCPU return. Fail explicitly on real EL3 errors. */
    if (result != 0) panic("B2L CPU: CPU%u CPU_ON failed (%d)", target, result);
    return KERN_SUCCESS;
}

void B2LCPU::signalCPU(IOCPU *target)
{
    if (!target || target->getCPUNumber() >= MAX_CPUS)
        panic("B2L CPU: invalid IPI target");
    b2l_send_ipi(target->getCPUNumber());
}

void B2LCPU::waitForStartup(unsigned id)
{
    B2LCPU *c=__atomic_load_n(&b2l_cpus[id],__ATOMIC_ACQUIRE);
    uint64_t deadline,lastStage=UINT64_MAX,lastLow=UINT64_MAX,lastLog=0;
    nanoseconds_to_deadline(5*NSEC_PER_SEC,&deadline);
    for(;;){
        uint64_t low=secondary_page[B2L_SECONDARY_STAGE_WORD];
        uint64_t stage=b2l_smp_progress(id),now=mach_absolute_time();
        unsigned initialized=__atomic_load_n(&c->cpuInitialized,__ATOMIC_ACQUIRE);
        bool ipi=b2l_cpu_ipi_ready(id);
        uint64_t timer=b2l_cpu_irq_count(id,TRUE);
        if(stage!=lastStage || low!=lastLow || !lastLog || now-lastLog>=B2L_COUNTER_HZ){
            b2l_early_trace("B2L SMP WAIT: cpu=%u low=%llu secondary=%llu EL=%llu MPIDR=0x%llx MIDR=0x%llx init=%u ipi=%u timer=%llu\n",
                id,low,stage,secondary_page[B2L_SECONDARY_IDENTITY_WORD]>>2,
                secondary_page[B2L_SECONDARY_IDENTITY_WORD+1],secondary_page[B2L_SECONDARY_IDENTITY_WORD+2],
                initialized,(unsigned)ipi,timer);
            lastStage=stage;lastLow=low;lastLog=now;
        }
        if(b2l_smp_rejected())panic("B2L SMP: CPU%u rejected affinity before next CPU_ON",id);
        /* Idle CPUs need not have an armed timer yet. Timer proof runs
         * below while the coordinator is explicitly bound to each CPU. */
        if(initialized && ipi)break;
        if(now>deadline)panic("B2L SMP: CPU%u incomplete low=%llu secondary=%llu init=%u ipi=%u timer=%llu\n"
            "B2L LOW: EL=%llu MPIDR=0x%llx MIDR=0x%llx target=0x%llx before next CPU_ON",
            id,low,stage,initialized,(unsigned)ipi,timer,
            secondary_page[B2L_SECONDARY_IDENTITY_WORD]>>2,
            secondary_page[B2L_SECONDARY_IDENTITY_WORD+1],
            secondary_page[B2L_SECONDARY_IDENTITY_WORD+2],
            secondary_page[B2L_SECONDARY_TARGET_WORD]);
        IOSleep(1);
    }
    b2l_early_trace("B2L SMP: CPU%u startup acknowledged before next CPU_ON\n",id);
}

/* ---- ACPM DVFS (see B2LACPM.h). Runs once, in the coordinator thread. ---- */
static int b2l_acpm_xfer(struct b2l_acpm *a, uint32_t cmd[4])
{
    /* Same algorithm as Samsung __acpm_ipc_send_data(): full check, copy at
     * front, publish front, doorbell. A reply is polled only on ap_poll
     * channels; TYPE_BUFFER/interrupt channels (DVFS ch5) are fire-and-forget,
     * exactly as Linux returns without waiting there. */
    uint32_t front = b2l_acpm_rd(a, a->tx_front), rear = b2l_acpm_rd(a, a->tx_rear);
    if (front >= a->len) return -1;
    uint32_t next = front + 1;
    if (next >= a->len) next = 0;
    uint64_t deadline;
    nanoseconds_to_deadline(15 * NSEC_PER_MSEC, &deadline);
    while (next == b2l_acpm_rd(a, a->tx_rear)) {
        if (mach_absolute_time() > deadline) {
            b2l_early_trace("B2L DVFS: tx full front=%u rear=%u len=%u\n", front, rear, a->len);
            return -2;
        }
        IODelay(10);
    }
    if (++a->seq >= 64) a->seq = 1;
    cmd[0] = (cmd[0] & 0xffffu) | (a->seq << 16);
    uint32_t slot = a->tx_base + a->elem * front;
    for (uint32_t i = 0; i < a->elem / 4; i++) b2l_acpm_wr(a, slot + 4 * i, i < 4 ? cmd[i] : 0);
    __asm__ volatile("dsb sy" ::: "memory");
    b2l_acpm_wr(a, a->tx_front, next);
    __asm__ volatile("dsb sy" ::: "memory");
    *(volatile uint32_t *)(a->mbox + B2L_ACPM_INTGR0) = (1u << a->id) << 16;
    if (!a->poll && !a->reply) return 0;
    nanoseconds_to_deadline(50 * NSEC_PER_MSEC, &deadline);
    for (;;) {
        uint32_t f = b2l_acpm_rd(a, a->rx_front), r = b2l_acpm_rd(a, a->rx_rear);
        if (f >= a->len || r >= a->len) return -3;
        for (uint32_t i = r; i != f; i = (i + 1) % a->len) {
            uint32_t at = a->rx_base + a->elem * i;
            if (((b2l_acpm_rd(a, at) >> 16) & 0x3f) != a->seq) continue;
            for (uint32_t k = 0; k < 4; k++) cmd[k] = b2l_acpm_rd(a, at + 4 * k);
            r = (r + 1) % a->len;
            b2l_acpm_wr(a, a->rx_rear, r);
            if (r == b2l_acpm_rd(a, a->rx_front)) *(volatile uint32_t *)(a->mbox + B2L_ACPM_INTCR1) = 1u << a->id;
            return 0;
        }
        if (mach_absolute_time() > deadline) return -4;
        IODelay(10);
    }
}

/* B1L-0207: S2MPS19 RTC through ACPM MFD (Samsung acpm_mfd.c + rtc-s2mps19.c): DT s2mps19mfd
 * acpm-ipc-channel 2, acpm-speedy-channel 0. cmd[0] = type<<8 | reg | speedy<<12; cmd[1] = func | arg<<8;
 * reply cmd[1] bits 24-31 = APM status, 8-15 = read byte; bulk bytes in cmd[2], cmd[3]. The only PMIC write
 * is the RTC update register's RUDR latch, as the Linux driver does on every read. Seconds UTC, 0 on failure. */
static int b2l_mfd(struct b2l_acpm *a, uint32_t reg, uint32_t func, uint32_t arg, uint32_t out[4])
{
    out[0] = (B2L_RTC_TYPE & 0xf) << 8 | (reg & 0xff); out[1] = (func & 0xff) | (arg & 0xff) << 8; out[2] = out[3] = 0;
    if (b2l_acpm_xfer(a, out)) return -1;
    return (out[1] >> 24) & 0xff ? -2 : 0;
}

extern "C" int64_t b2l_pmic_rtc_seconds(void)
{
    struct b2l_acpm a = {};
    a.sram = (volatile uint8_t *)ml_io_map(B2L_ACPM_SRAM_PA, B2L_ACPM_SRAM_SIZE);
    a.mbox = (volatile uint8_t *)ml_io_map(B2L_ACPM_MBOX_PA, B2L_ACPM_MBOX_SIZE);
    if (!a.sram || !a.mbox) return 0;
    int e = b2l_acpm_parse_ch(&a, 2);
    if (e) { IOLog("B2L RTC: ACPM channel 2 parse=%d\n", e); return 0; }
    a.reply = 1;
    /* drop replies left by the bootloader so a stale one cannot match our sequence numbers */
    if (b2l_acpm_rd(&a, a.rx_front) < a.len) b2l_acpm_wr(&a, a.rx_rear, b2l_acpm_rd(&a, a.rx_front));
    uint32_t c[4];
    if ((e = b2l_mfd(&a, B2L_RTC_REG_UPDATE, 0 /* FUNC_READ */, 0, c))) { IOLog("B2L RTC: read update reg %d\n", e); return 0; }
    uint32_t upd = ((c[1] >> 8) & 0xff & ~B2L_RTC_UDR_BITS) | B2L_RTC_RUDR;
    if ((e = b2l_mfd(&a, B2L_RTC_REG_UPDATE, 1 /* FUNC_WRITE */, upd, c))) { IOLog("B2L RTC: latch %d\n", e); return 0; }
    IODelay(1000);
    if ((e = b2l_mfd(&a, B2L_RTC_REG_SEC, 3 /* FUNC_BULK_READ */, B2L_RTC_NREGS, c))) { IOLog("B2L RTC: bulk read %d\n", e); return 0; }
    uint8_t d[B2L_RTC_NREGS];
    for (uint32_t i = 0; i < B2L_RTC_NREGS; i++) d[i] = (uint8_t)(i < 4 ? c[2] >> (8 * i) : c[3] >> (8 * (i - 4)));
    int64_t t = b2l_rtc_to_epoch(d);
    IOLog("B2L RTC: 20%02u-%02u-%02u %02u:%02u:%02u UTC -> %lld\n", d[6] & 0x7f, d[5] & 0x0f, d[4] & 0x1f,
        d[2] & 0x1f, d[1] & 0x7f, d[0] & 0x7f, t);
    return t;
}

static void b2l_measure_all(const char *when, processor_t const procs[MAX_CPUS])
{
    for (unsigned cpu = 0; cpu < MAX_CPUS; ++cpu) {
        if (!procs[cpu]) continue;
        processor_t previous = b2l_bind_cpu(procs[cpu]);
        thread_block(THREAD_CONTINUE_NULL);
        uint64_t khz = 0;
        for (unsigned k = 0; k < 5; ++k) { uint64_t m = b2l_measure_cpu_khz(); if (m > khz) khz = m; IOSleep(1); }
        b2l_early_trace("B2L CPUFREQ %s: cpu=%u %s ~%llu MHz\n", when, cpu,
            cpu < 4 ? "A55" : cpu < 6 ? "A75" : "M4", khz / 1000);
        b2l_bind_cpu(previous);
    }
}

/* One DVFS frequency request on the ACPM channel (single-slot TYPE_BUFFER
 * write as B1L-0015, else the queued transfer). */
static int b2l_acpm_send_rate(struct b2l_acpm *a, uint32_t idx, uint32_t khz)
{
    uint32_t cmd[4] = { idx, khz, B2L_ACPM_FREQ_REQ, 0 };
    if (a->type == 2 && a->len == 1) {
        if (++a->seq >= 64) a->seq = 1;
        cmd[0] = (cmd[0] & 0xffffu) | (a->seq << 16);
        for (uint32_t i = 0; i < a->elem / 4; i++) b2l_acpm_wr(a, a->tx_base + 4 * i, i < 4 ? cmd[i] : 0);
        __asm__ volatile("dsb sy" ::: "memory");
        *(volatile uint32_t *)(a->mbox + B2L_ACPM_INTGR0) = (1u << a->id) << 16;
        IODelay(500);
        return 0;
    }
    return b2l_acpm_xfer(a, cmd);
}

/* ---- T1 (B1L-0028): CPU thermal throttle. See B2LThermal.h. ----
 * Each second: read the TMU zones (max of each zone's sensors), and per
 * cluster move one fvmap level slower at >= hi, back one level at <= lo (never
 * faster than the boot's DVFS target), straight to the floor at >= crit
 * (hottest zone). Boot-args: b2l_thermal=0 (off), b2l_thr_hi/lo/crit (C),
 * b2l_tmu_enable=0 (never start a stopped TMU block). The hardware trip,
 * thresholds and TMU interrupts are never written. */
static struct b2l_acpm b2l_thr_acpm;
static uint32_t b2l_thr_target[4];   /* cl0, cl1, cl2, G3D */
static volatile uint8_t *b2l_tmu_va[2];
static uint32_t b2l_tmu_rd(unsigned b, uint32_t off) { return *(volatile uint32_t *)(b2l_tmu_va[b] + off); }
static void b2l_tmu_wr(unsigned b, uint32_t off, uint32_t v) { *(volatile uint32_t *)(b2l_tmu_va[b] + off) = v; __asm__ volatile("dsb sy" ::: "memory"); }

static void b2l_tmu_start_block(unsigned b)
{
    /* B1L-0030: read only. On the 9820 the TMU belongs to ACPM's thermal firmware; CONTROL is at 0x50. */
    uint32_t ctrl = b2l_tmu_rd(b, B2L_TMU_CONTROL), t0 = b2l_tmu_rd(b, b2l_tmu_triminfo(0));
    b2l_early_trace("B2L THERMAL: tmu%u CONTROL=%08x CORE_EN=%u TRIMINFO0=%08x (%s-point)\n", b, ctrl,
        ctrl & B2L_TMU_CORE_EN, t0, ((t0 >> 23) & 1) ? "two" : "one");
    for (unsigned s = 0; s < 16; s++) {
        uint32_t code = (b2l_tmu_rd(b, b2l_tmu_temp_reg(s)) >> b2l_tmu_temp_shift(s)) & 0x1ff, tri = b2l_tmu_rd(b, b2l_tmu_triminfo(s));
        int32_t mc = b2l_tmu_code_to_mc(code, tri, t0);
        if (code) b2l_early_trace("B2L THERMAL: tmu%u s%u code=%u trim=%08x -> %d mC\n", b, s, code, tri, mc);
    }
}

/* Max of a zone's valid sensors in millidegrees, or INT32_MIN. */
static int32_t b2l_zone_mc(unsigned z)
{
    unsigned b = b2l_zones[z].sub;
    uint32_t t0 = b2l_tmu_rd(b, b2l_tmu_triminfo(0));
    int32_t best = INT32_MIN;
    for (unsigned s = 0; s < 16; s++) {
        if (!(b2l_zones[z].sensors & (1u << s))) continue;
        uint32_t code = (b2l_tmu_rd(b, b2l_tmu_temp_reg(s)) >> b2l_tmu_temp_shift(s)) & 0x1ff;
        int32_t mc = b2l_tmu_code_to_mc(code, b2l_tmu_rd(b, b2l_tmu_triminfo(s)), t0);
        if (b2l_tmu_reading_ok(code, mc) && mc > best) best = mc;
    }
    return best;
}

static void b2l_thermal_thread(void *, wait_result_t)
{
    static const uint32_t idx[4] = { 2, 3, 4, 10 };
    static const unsigned zone_of[4] = { B2L_ZONE_LITTLE, B2L_ZONE_MID, B2L_ZONE_BIG, B2L_ZONE_G3D };
    static const char *const name[4] = { "cl0 A55", "cl1 A75", "cl2 M4", "g3d" };
    static const uint32_t floor_khz[4] = { 1000000, 1000000, 1000000, 400000 };
    int hi = 65, lo = 60, crit = 72;
    PE_parse_boot_argn("b2l_thr_hi", &hi, sizeof(hi));
    PE_parse_boot_argn("b2l_thr_lo", &lo, sizeof(lo));
    PE_parse_boot_argn("b2l_thr_crit", &crit, sizeof(crit));
    b2l_tmu_va[0] = (volatile uint8_t *)ml_io_map(B2L_TMU_TOP_PA, B2L_TMU_SIZE);
    b2l_tmu_va[1] = (volatile uint8_t *)ml_io_map(B2L_TMU_SUB_PA, B2L_TMU_SIZE);
    if (!b2l_tmu_va[0] || !b2l_tmu_va[1]) { b2l_early_trace("B2L THERMAL: TMU map failed\n"); thread_terminate(current_thread()); return; }
    b2l_tmu_start_block(0);
    b2l_tmu_start_block(1);
    static struct b2l_acpm_table t[4];
    uint32_t base[4], floor[4], lv[4];
    bool usable[4];
    for (unsigned c = 0; c < 4; c++) {
        usable[c] = b2l_acpm_table(&b2l_thr_acpm, idx[c], &t[c]) == 0;
        if (!b2l_thr_target[c]) usable[c] = false;          /* (the GPU when G4 did not set it) */
        if (!usable[c] && b2l_thr_target[c]) {
            /* No readable fvmap: a ladder from the boot target; ACPM rounds each request to a firmware level. */
            static const uint32_t pct[6] = { 100, 88, 76, 64, 52, 45 };
            t[c].count = 6;
            for (unsigned k = 0; k < 6; k++) t[c].rate[k] = b2l_thr_target[c] / 100 * pct[k] / 1000 * 1000;
            usable[c] = true;
        }
        base[c] = 0;
        if (usable[c]) {
            while (base[c] + 1 < t[c].count && t[c].rate[base[c]] > b2l_thr_target[c]) base[c]++;
            floor[c] = base[c];
            while (floor[c] + 1 < t[c].count && t[c].rate[floor[c]] > floor_khz[c]) floor[c]++;
        } else floor[c] = 0;
        lv[c] = base[c];
        b2l_early_trace("B2L THERMAL: %s table=%d levels=%u base=%u kHz floor=%u kHz\n", name[c], usable[c] ? 0 : -1,
            usable[c] ? t[c].count : 0, usable[c] ? t[c].rate[base[c]] : 0, usable[c] ? t[c].rate[floor[c]] : 0);
    }
    for (unsigned tick = 0;; tick++) {
        int32_t mc[B2L_NZONES], hottest = INT32_MIN;
        for (unsigned z = 0; z < B2L_NZONES; z++) { mc[z] = b2l_zone_mc(z); if (z != B2L_ZONE_G3D && mc[z] > hottest) hottest = mc[z]; }
        for (unsigned c = 0; c < 4; c++) {
            int32_t m = mc[zone_of[c]];
            if (!usable[c] || m == INT32_MIN) continue;
            if (c < 3 && hottest >= crit * 1000) m = hottest;
            uint32_t n = b2l_thr_step(lv[c], base[c], floor[c], m, hi * 1000, lo * 1000, crit * 1000);
            if (n != lv[c]) {
                int e = b2l_acpm_send_rate(&b2l_thr_acpm, idx[c], t[c].rate[n]);
                b2l_early_trace("B2L THERMAL: %s %d.%d C -> %u kHz (level %u, ret %d)\n", name[c], m / 1000, (m % 1000) / 100,
                    t[c].rate[n], n, e);
                if (!e) lv[c] = n;
            }
        }
        if (tick % 10 == 0)
            b2l_early_trace("B2L THERMAL: big=%d mid=%d little=%d g3d=%d C; kHz %u/%u/%u gpu %u\n",
                mc[0] == INT32_MIN ? -1 : mc[0] / 1000, mc[1] == INT32_MIN ? -1 : mc[1] / 1000,
                mc[2] == INT32_MIN ? -1 : mc[2] / 1000, mc[3] == INT32_MIN ? -1 : mc[3] / 1000,
                usable[0] ? t[0].rate[lv[0]] : 0, usable[1] ? t[1].rate[lv[1]] : 0, usable[2] ? t[2].rate[lv[2]] : 0,
                usable[3] ? t[3].rate[lv[3]] : 0);
        IOSleep(1000);
    }
}

static void b2l_dvfs_start(processor_t const procs[MAX_CPUS])
{
    /* S10 DTB: cpufreq domain@0/1/2 cal-id 0x0b040002/3/4. Targets are levels the
     * DTB itself lists (dm-constraints / ufc tables), one step under each table top;
     * M4 uses its DT max-freq = pm_qos-booting (Samsung applies it at every boot). */
    static const uint32_t idx[3] = { 2, 3, 4 };
    static const uint32_t target[3] = { 1846000, 2210000, 2080000 };
    static const char *const name[3] = { "cl0 A55", "cl1 A75", "cl2 M4" };
    uint32_t enable = 0; /* Clocks already stable from S-Boot; b2l_dvfs=1 to test ACPM */
    PE_parse_boot_argn("b2l_dvfs", &enable, sizeof(enable));
    struct b2l_acpm a = {};
    a.sram = (volatile uint8_t *)ml_io_map(B2L_ACPM_SRAM_PA, B2L_ACPM_SRAM_SIZE);
    a.mbox = (volatile uint8_t *)ml_io_map(B2L_ACPM_MBOX_PA, B2L_ACPM_MBOX_SIZE);
    if (!a.sram || !a.mbox) { b2l_early_trace("B2L DVFS: map failed\n"); return; }
    int e = b2l_acpm_parse(&a);
    b2l_early_trace("B2L DVFS: parse=%d ch=%u id=%u type=%u poll=%u elem=%u len=%u tx=%x/%x/%x (front=%u rear=%u) fvmap=%x enable=%u\n",
        e, B2L_ACPM_DVFS_CH, a.id, a.type, a.poll, a.elem, a.len, a.tx_base, a.tx_front, a.tx_rear,
        e ? 0 : b2l_acpm_rd(&a, a.tx_front), e ? 0 : b2l_acpm_rd(&a, a.tx_rear), a.fvmap, enable);
    if (enable > 1) {
        uint32_t plugins = b2l_acpm_rd(&a, B2L_ACPM_INITDATA), np = b2l_acpm_rd(&a, B2L_ACPM_INITDATA + 4);
        if (np <= 16 && b2l_acpm_in(plugins, 48u * np))
            for (uint32_t o = 0; o < 48u * np; o += 24)
                b2l_early_trace("B2L DVFS DUMP plug+%03x: %08x %08x %08x %08x %08x %08x\n", o,
                    b2l_acpm_rd(&a, plugins + o), b2l_acpm_rd(&a, plugins + o + 4), b2l_acpm_rd(&a, plugins + o + 8),
                    b2l_acpm_rd(&a, plugins + o + 12), b2l_acpm_rd(&a, plugins + o + 16), b2l_acpm_rd(&a, plugins + o + 20));
    }
    if (e || !enable) return;
    a.seq = 40;
    bool sent = false;
    for (unsigned c = 0; c < 3; ++c) {
        uint32_t cmd[4] = { idx[c], target[c], B2L_ACPM_FREQ_REQ, 0 };
        int se;
        if (a.type == 2 && a.len == 1) {
            /* Experiment (B1L-0014): one-slot TYPE_BUFFER channel left at front=rear=0 by
             * S-Boot; the queue check can never pass. Write the single slot and ring the
             * doorbell, the way dequeue_policy() reads TYPE_BUFFER replies (base, no index). */
            if (++a.seq >= 64) a.seq = 1;
            cmd[0] = (cmd[0] & 0xffffu) | (a.seq << 16);
            for (uint32_t i = 0; i < a.elem / 4; i++) b2l_acpm_wr(&a, a.tx_base + 4 * i, i < 4 ? cmd[i] : 0);
            __asm__ volatile("dsb sy" ::: "memory");
            *(volatile uint32_t *)(a.mbox + B2L_ACPM_INTGR0) = (1u << a.id) << 16;
            IODelay(500);
            b2l_early_trace("B2L DVFS: %s buffer-write rx=%08x %08x %08x %08x tx=%08x\n", name[c],
                b2l_acpm_rd(&a, a.rx_base), b2l_acpm_rd(&a, a.rx_base + 4), b2l_acpm_rd(&a, a.rx_base + 8),
                b2l_acpm_rd(&a, a.rx_base + 12), b2l_acpm_rd(&a, a.tx_base));
            se = 0;
        } else {
            se = b2l_acpm_xfer(&a, cmd);
        }
        b2l_early_trace("B2L DVFS: %s request %u kHz -> ret=%d front=%u rear=%u\n", name[c], target[c], se,
            b2l_acpm_rd(&a, a.tx_front), b2l_acpm_rd(&a, a.tx_rear));
        IOLog("B2L DVFS: %s request %u kHz ret=%d\n", name[c], target[c], se);
        sent |= !se;
        IOSleep(20);
    }
    if (sent) { IOSleep(50); b2l_measure_all("after-dvfs", procs); }
    /* G4 (B1L-0030): the G3D through ACPM like the clusters: cal_id 0x0b04000a = index 10. The level just
     * under the DT gpu_max_clock 702000 ("near max, safer"), voltage chosen by the firmware with it.
     * Kill switch: b2l_gpu_dvfs=0. */
    uint32_t gpu_dvfs = 1, gpu_khz = 0;
    PE_parse_boot_argn("b2l_gpu_dvfs", &gpu_dvfs, sizeof(gpu_dvfs));
    {
        static struct b2l_acpm_table gt;
        int te = b2l_acpm_table(&a, 10, &gt);
        b2l_early_trace("B2L GPU DVFS: fvmap[10] parse=%d levels=%u top=%u kHz enable=%u\n", te, te ? 0 : gt.count,
            te ? 0 : gt.rate[0], gpu_dvfs);
        for (uint32_t i = 0; !te && i < gt.count; i++)
            b2l_early_trace("B2L GPU DVFS: level %u %u kHz %u uV\n", i, gt.rate[i], gt.volt[i]);
        if (!te && gpu_dvfs && gt.rate[0] <= 1000000) gpu_khz = b2l_acpm_pick(&gt, 650001);   /* B1L-0110: 650 MHz fixed (owner); the pick is strictly below */
        if (gpu_khz) {
            int ge = b2l_acpm_send_rate(&a, 10, gpu_khz);
            b2l_early_trace("B2L GPU DVFS: request %u kHz ret=%d\n", gpu_khz, ge);
            IOLog("B2L GPU DVFS: request %u kHz ret=%d\n", gpu_khz, ge);
        }
    }
    uint32_t thermal = 1;
    PE_parse_boot_argn("b2l_thermal", &thermal, sizeof(thermal));
    if (sent && thermal) {
        b2l_thr_acpm = a;
        for (unsigned c = 0; c < 3; ++c) b2l_thr_target[c] = target[c];
        b2l_thr_target[3] = gpu_khz;
        thread_t th = THREAD_NULL;
        if (kernel_thread_start(b2l_thermal_thread, nullptr, &th) == KERN_SUCCESS) thread_deallocate(th);
        else b2l_early_trace("B2L THERMAL: thread start failed\n");
    }
}

/* GPU G1 (read-only) + G2a (reset, power on/off by the GPU's own registers).
 * pd-embedded_g3d status: PMU 0x15862100 +4 bit0 (DTB gpu_pmu_status_reg_offset/mask).
 * Nothing is touched unless the domain is already on and GPU_ID is a G76. */
static bool b2l_mali_wait(volatile uint32_t *g, uint32_t reg, uint32_t mask, uint32_t want, unsigned ms)
{
    uint64_t deadline; nanoseconds_to_deadline((uint64_t)ms * NSEC_PER_MSEC, &deadline);
    while ((g[reg / 4] & mask) != want) {
        if (mach_absolute_time() > deadline) return false;
        IODelay(10);
    }
    return true;
}

/* G2b: first real GPU job. One physically contiguous 6-page buffer:
 * pages 0..3 = AArch64 4K page tables (L0..L3), 4 = job descriptor, 5 = result.
 * GPU VA 0x100000 + k*4K maps page k. WRITE_VALUE writes 0xB21CAFE5 to VA
 * 0x105000. CPU caches are cleaned/invalidated to PoC around every access. */
static void b2l_mali_as_disable(volatile uint32_t *g)
{
    volatile uint32_t *as = g + MALI_AS(0) / 4;
    as[MALI_AS_TRANSTAB_LO / 4] = 0; as[MALI_AS_TRANSTAB_HI / 4] = 0;
    as[MALI_AS_MEMATTR_LO / 4] = 0; as[MALI_AS_MEMATTR_HI / 4] = 0;
    as[MALI_AS_TRANSCFG_LO / 4] = 1; as[MALI_AS_TRANSCFG_HI / 4] = 0;   /* ADRMODE_UNMAPPED */
    as[MALI_AS_COMMAND / 4] = MALI_AS_CMD_UPDATE;
    b2l_mali_wait(g, MALI_AS(0) + MALI_AS_STATUS, 1, 0, 50);
}

static void b2l_mali_first_job(volatile uint32_t *g, uint64_t shaders)
{
    const uint64_t va_base = 0x100000ULL, va_job = va_base + 4 * 4096, va_out = va_base + 5 * 4096;
    const uint32_t magic = 0xB21CAFE5u;
    IOBufferMemoryDescriptor *mem = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
        kIODirectionInOut | kIOMemoryPhysicallyContiguous, 6 * 4096, 0x000000FFFFFFF000ULL);
    if (!mem) { b2l_early_trace("B2L GPU G2b: no contiguous memory\n"); return; }
    uint8_t *v = (uint8_t *)mem->getBytesNoCopy();
    IOByteCount seg = 0;
    uint64_t pa = mem->getPhysicalSegment(0, &seg, kIOMemoryMapperNone);
    if (!v || !pa || seg < 6 * 4096 || (pa & 0xfff)) { b2l_early_trace("B2L GPU G2b: bad buffer pa=%llx seg=%llu\n", pa, (uint64_t)seg); mem->release(); return; }
    bzero(v, 6 * 4096);
    uint64_t *l0 = (uint64_t *)(v), *l1 = (uint64_t *)(v + 4096), *l2 = (uint64_t *)(v + 8192), *l3 = (uint64_t *)(v + 12288);
    l0[(va_base >> 39) & 511] = (pa + 4096) | MALI_PTE_TABLE;
    l1[(va_base >> 30) & 511] = (pa + 8192) | MALI_PTE_TABLE;
    l2[(va_base >> 21) & 511] = (pa + 12288) | MALI_PTE_TABLE;
    for (unsigned k = 0; k < 6; k++) l3[((va_base >> 12) & 511) + k] = (pa + 4096ULL * k) | MALI_PTE_PAGE;
    uint32_t *job = (uint32_t *)(v + 4 * 4096);
    job[4] = 1u | (MALI_JOB_TYPE_WRITE_VALUE << 1) | (1u << 16);      /* 64-bit descriptor, type, index 1 */
    job[8] = (uint32_t)va_out; job[9] = (uint32_t)(va_out >> 32);        /* payload: address */
    job[10] = MALI_WRITE_VALUE_IMM32;                                    /* type */
    job[12] = magic; job[13] = 0;                                        /* immediate value */
    *(volatile uint32_t *)(v + 5 * 4096) = 0x11111111u;
    __asm__ volatile("dsb sy" ::: "memory");
    FlushPoC_DcacheRegion((vm_offset_t)v, 6 * 4096);

    volatile uint32_t *as = g + MALI_AS(0) / 4;
    as[MALI_AS_TRANSTAB_LO / 4] = (uint32_t)pa; as[MALI_AS_TRANSTAB_HI / 4] = (uint32_t)(pa >> 32);
    as[MALI_AS_MEMATTR_LO / 4] = (uint32_t)MALI_MEMATTR_NC_ALL; as[MALI_AS_MEMATTR_HI / 4] = (uint32_t)(MALI_MEMATTR_NC_ALL >> 32);
    as[MALI_AS_TRANSCFG_LO / 4] = MALI_TRANSCFG_AARCH64_4K | MALI_TRANSCFG_INA_BITS(55 - 48) | MALI_TRANSCFG_PTW_NC;
    as[MALI_AS_TRANSCFG_HI / 4] = 0;
    as[MALI_AS_COMMAND / 4] = MALI_AS_CMD_UPDATE;
    bool asok = b2l_mali_wait(g, MALI_AS(0) + MALI_AS_STATUS, 1, 0, 50);

    g[MALI_JOB_IRQ_CLEAR / 4] = 0xffffffffu; g[MALI_MMU_IRQ_CLEAR / 4] = 0xffffffffu;
    volatile uint32_t *js = g + MALI_JS(0) / 4;
    js[MALI_JS_HEAD_NEXT_LO / 4] = (uint32_t)va_job; js[MALI_JS_HEAD_NEXT_HI / 4] = (uint32_t)(va_job >> 32);
    js[MALI_JS_AFFINITY_NEXT_LO / 4] = (uint32_t)shaders; js[MALI_JS_AFFINITY_NEXT_HI / 4] = (uint32_t)(shaders >> 32);
    js[MALI_JS_CONFIG_NEXT / 4] = MALI_JS_CONFIG(0);
    js[MALI_JS_COMMAND_NEXT / 4] = MALI_JS_CMD_START;
    uint64_t deadline; nanoseconds_to_deadline(200 * NSEC_PER_MSEC, &deadline);
    uint32_t raw = 0;
    while (!((raw = g[MALI_JOB_IRQ_RAWSTAT / 4]) & (MALI_JOB_IRQ_DONE(0) | MALI_JOB_IRQ_FAIL(0)))) {
        if (mach_absolute_time() > deadline) break;
        IODelay(10);
    }
    uint32_t jsstatus = js[MALI_JS_STATUS / 4], mmuraw = g[MALI_MMU_IRQ_RAWSTAT / 4];
    uint32_t fs = as[MALI_AS_FAULTSTATUS / 4], falo = as[MALI_AS_FAULTADDR_LO / 4], fahi = as[MALI_AS_FAULTADDR_HI / 4];
    FlushPoC_DcacheRegion((vm_offset_t)v, 6 * 4096);
    uint32_t got = *(volatile uint32_t *)(v + 5 * 4096), exc = *(volatile uint32_t *)(v + 4 * 4096);
    b2l_early_trace("B2L GPU G2b: as=%s jobirq=%08x js_status=%02x mmuirq=%08x as_fault=%08x@%08x%08x desc_exc=%08x result=%08x (%s)\n",
        asok ? "ok" : "TIMEOUT", raw, jsstatus, mmuraw, fs, fahi, falo, exc, got,
        got == magic ? "WRITE_VALUE executed" : "no write");
    g[MALI_JOB_IRQ_CLEAR / 4] = 0xffffffffu; g[MALI_MMU_IRQ_CLEAR / 4] = 0xffffffffu;
    if (!(raw & MALI_JOB_IRQ_DONE(0)) || got != magic) {
        g[MALI_GPU_COMMAND / 4] = MALI_CMD_SOFT_RESET;           /* recover a stuck slot */
        bool r = b2l_mali_wait(g, MALI_GPU_IRQ_RAWSTAT, MALI_IRQ_RESET_DONE, MALI_IRQ_RESET_DONE, 100);
        g[MALI_GPU_IRQ_CLEAR / 4] = 0xffffffffu;
        b2l_early_trace("B2L GPU G2b: recovery reset %s\n", r ? "done" : "TIMEOUT");
    } else {
        b2l_mali_as_disable(g);
    }
    mem->release();
}

static void b2l_gpu_probe(void)
{
    volatile uint32_t *pd = (volatile uint32_t *)ml_io_map(0x15862100ULL, 0x1000);
    if (!pd) { b2l_early_trace("B2L GPU: PMU map failed\n"); return; }
    uint32_t conf = pd[0], status = pd[1];
    b2l_early_trace("B2L GPU: pd-embedded_g3d conf=%08x status=%08x (%s)\n", conf, status, (status & 1) ? "on" : "off");
    if (!(status & 1)) return;
    volatile uint32_t *g = (volatile uint32_t *)ml_io_map(B2L_MALI_PA, B2L_MALI_SIZE);
    if (!g) return;
    uint32_t id = g[MALI_GPU_ID / 4];
    uint64_t shaders = ((uint64_t)g[(MALI_SHADER_PRESENT + 4) / 4] << 32) | g[MALI_SHADER_PRESENT / 4];
    uint32_t tiler = g[MALI_TILER_PRESENT / 4], l2 = g[MALI_L2_PRESENT / 4];
    b2l_early_trace("B2L GPU: GPU_ID=%08x L2_FEATURES=%08x SHADER_PRESENT=%016llx (%u cores) TILER=%08x L2=%08x AS=%08x JS=%08x\n",
        id, g[MALI_L2_FEATURES / 4], shaders, b2l_mali_popcount64(shaders), tiler, l2,
        g[MALI_AS_PRESENT / 4], g[MALI_JS_PRESENT / 4]);
    b2l_early_trace("B2L GPU: CORE_FEATURES=%08x TILER_FEATURES=%08x MEM_FEATURES=%08x MMU_FEATURES=%08x THREAD_MAX=%08x STATUS=%08x FAULT=%08x\n",
        g[MALI_CORE_FEATURES / 4], g[MALI_TILER_FEATURES / 4], g[MALI_MEM_FEATURES / 4], g[MALI_MMU_FEATURES / 4],
        g[MALI_THREAD_MAX_THREADS / 4], g[MALI_GPU_STATUS / 4], g[MALI_GPU_FAULTSTATUS / 4]);
    uint32_t enable = 1;
    PE_parse_boot_argn("b2l_gpu", &enable, sizeof(enable));
    if (!enable || !b2l_mali_is_g76(id) || !shaders || !l2) return;

    /* G2a: IRQs masked (polled), soft reset, then power L2 -> tiler -> shaders. */
    g[MALI_GPU_IRQ_MASK / 4] = 0; g[MALI_JOB_IRQ_MASK / 4] = 0; g[MALI_MMU_IRQ_MASK / 4] = 0;
    g[MALI_GPU_IRQ_CLEAR / 4] = 0xffffffffu; g[MALI_JOB_IRQ_CLEAR / 4] = 0xffffffffu; g[MALI_MMU_IRQ_CLEAR / 4] = 0xffffffffu;
    g[MALI_GPU_COMMAND / 4] = MALI_CMD_SOFT_RESET;
    bool reset = b2l_mali_wait(g, MALI_GPU_IRQ_RAWSTAT, MALI_IRQ_RESET_DONE, MALI_IRQ_RESET_DONE, 100);
    g[MALI_GPU_IRQ_CLEAR / 4] = 0xffffffffu;
    b2l_early_trace("B2L GPU G2: soft reset %s rawstat=%08x status=%08x\n", reset ? "done" : "TIMEOUT",
        g[MALI_GPU_IRQ_RAWSTAT / 4], g[MALI_GPU_STATUS / 4]);
    if (!reset) return;
    g[MALI_L2_PWRON / 4] = l2;
    bool l2ok = b2l_mali_wait(g, MALI_L2_READY, l2, l2, 100);
    g[MALI_TILER_PWRON / 4] = tiler;
    bool tok = b2l_mali_wait(g, MALI_TILER_READY, tiler, tiler, 100);
    g[MALI_SHADER_PWRON / 4] = (uint32_t)shaders; g[(MALI_SHADER_PWRON + 4) / 4] = (uint32_t)(shaders >> 32);
    bool sok = b2l_mali_wait(g, MALI_SHADER_READY, (uint32_t)shaders, (uint32_t)shaders, 100);
    b2l_early_trace("B2L GPU G2: power L2=%s(%08x) tiler=%s(%08x) shaders=%s(%08x) trans=%08x\n",
        l2ok ? "ready" : "TIMEOUT", g[MALI_L2_READY / 4], tok ? "ready" : "TIMEOUT", g[MALI_TILER_READY / 4],
        sok ? "ready" : "TIMEOUT", g[MALI_SHADER_READY / 4], g[MALI_SHADER_PWRTRANS / 4]);
    if (l2ok && sok) {
        uint32_t job = 1;
        PE_parse_boot_argn("b2l_gpu_job", &job, sizeof(job));
        if (job) b2l_mali_first_job(g, shaders);
    }
    for (unsigned as = 0; as < 8; as++)
        if (g[MALI_AS_PRESENT / 4] & (1u << as))
            b2l_early_trace("B2L GPU G2: AS%u status=%08x fault=%08x\n", as,
                g[(MALI_AS(as) + MALI_AS_STATUS) / 4], g[(MALI_AS(as) + MALI_AS_FAULTSTATUS) / 4]);
    /* Leave the cores off until the job service exists (no heat, no idle power). */
    g[MALI_SHADER_PWROFF / 4] = (uint32_t)shaders; g[(MALI_SHADER_PWROFF + 4) / 4] = (uint32_t)(shaders >> 32);
    b2l_mali_wait(g, MALI_SHADER_READY, (uint32_t)shaders, 0, 100);
    g[MALI_TILER_PWROFF / 4] = tiler;
    b2l_mali_wait(g, MALI_TILER_READY, tiler, 0, 100);
    g[MALI_L2_PWROFF / 4] = l2;
    bool off = b2l_mali_wait(g, MALI_L2_READY, l2, 0, 100);
    b2l_early_trace("B2L GPU G2: cores powered %s (shader=%08x tiler=%08x l2=%08x)\n", off ? "off" : "OFF-TIMEOUT",
        g[MALI_SHADER_READY / 4], g[MALI_TILER_READY / 4], g[MALI_L2_READY / 4]);
}

void B2LCPU::finishStartup(void *, wait_result_t)
{
    if (cpu_number() != 0 || __atomic_load_n(&providers_ready, __ATOMIC_ACQUIRE) != MAX_CPUS)
        panic("B2L SMP: coordinator before complete CPU provider topology");

    /* Announce known topology, without claiming any secondary is online. */
    ml_set_max_cpus(MAX_CPUS);
    prepareSecondaryPage();
    for (unsigned id = 0; id < MAX_CPUS; ++id) {
        B2LCPU *c = __atomic_load_n(&b2l_cpus[id], __ATOMIC_ACQUIRE);
        if (!c) panic("B2L SMP: missing provider %u", id);
        c->registerProcessor();
        /* Do not overwrite the shared low-entry diagnostic or start the next
         * core until this core has reached C and serviced its self-SGI. */
        if(id)waitForStartup(id);
    }
    uint64_t deadline, last_stage[MAX_CPUS];
    for (unsigned i = 0; i < MAX_CPUS; ++i) last_stage[i] = UINT64_MAX;
    nanoseconds_to_deadline(20 * NSEC_PER_SEC, &deadline);
    for (;;) {
        if (b2l_smp_rejected()) panic("B2L SMP: unexpected secondary affinity rejected before CPU-data lookup");
        for (unsigned i = 1; i < MAX_CPUS; ++i) {
            uint64_t stage = b2l_smp_progress(i);
            if (stage != last_stage[i]) {
                b2l_early_trace("B2L SMP: CPU%u secondary-stage=%llu\n", i, stage);
                last_stage[i] = stage;
            }
        }
        bool ready = __atomic_load_n(&registered_cpus, __ATOMIC_ACQUIRE) == MAX_CPUS;
        for (unsigned i = 0; ready && i < MAX_CPUS; ++i) {
            B2LCPU *c = __atomic_load_n(&b2l_cpus[i], __ATOMIC_ACQUIRE);
            ready = c && __atomic_load_n(&c->cpuInitialized, __ATOMIC_ACQUIRE) &&
                b2l_cpu_ipi_ready(i);
        }
        if (ready) break;
        if (mach_absolute_time() > deadline)
            panic("B2L CPU: CPU registration/reset/MMU/timer/SGI startup timed out");
        IOSleep(1);
    }
    /* Real timers on every CPU, plus CPU0 -> each secondary and each -> CPU0.
     * Only one xcall is outstanding; reset its acknowledgement before send. */
    for (unsigned sender = 0; sender < 1; ++sender) {
        processor_t previous = b2l_bind_cpu(b2l_cpus[sender]->machProcessor);
        thread_block(THREAD_CONTINUE_NULL);
        if ((unsigned)cpu_number() != sender) panic("B2L CPU: IPI proof thread not bound");
        IOSleep(20);
        nanoseconds_to_deadline(NSEC_PER_SEC, &deadline);
        while (!b2l_cpu_irq_count(sender, TRUE)) {
            if (mach_absolute_time() > deadline) panic("B2L CPU: per-CPU timer proof timeout");
            IOSleep(1);
        }
        unsigned first = sender ? 0 : 1, end = sender ? 1 : MAX_CPUS;
        for (unsigned target = first; target < end; ++target) {
            __atomic_store_n(&xcall_observed[target], 0, __ATOMIC_RELEASE);
            kern_return_t kr = cpu_xcall((int)target, (broadcastFunc)xcall_proof,
                (void *)(uintptr_t)(target + 1));
            if (kr != KERN_SUCCESS) panic("B2L CPU: remote xcall rejected (%d)", kr);
            nanoseconds_to_deadline(NSEC_PER_SEC, &deadline);
            while (!__atomic_load_n(&xcall_observed[target], __ATOMIC_ACQUIRE)) {
                if (mach_absolute_time() > deadline) panic("B2L CPU: remote xcall timeout");
                IOSleep(1);
            }
        }
        b2l_early_trace("B2L SMP: CPU%u timer and remote xcall verified\n", sender);
        uint64_t khz = 0;
        for (unsigned k = 0; k < 5; ++k) { uint64_t m = b2l_measure_cpu_khz(); if (m > khz) khz = m; IOSleep(1); }
        b2l_early_trace("B2L CPUFREQ: cpu=%u %s ~%llu MHz\n", sender,
            sender < 4 ? "A55" : sender < 6 ? "A75" : "M4", khz / 1000);
        IOLog("B2L CPUFREQ: cpu=%u %s ~%llu MHz\n", sender,
            sender < 4 ? "A55" : sender < 6 ? "A75" : "M4", khz / 1000);
        b2l_bind_cpu(previous);
    }
    ml_cpu_init_completed();
    b2l_cpus[0]->cpusRunning();
    IOService::publishResource(gIOAllCPUInitializedKey, kOSBooleanTrue);
    IOService::publishResource("IOBSD");
    b2l_early_trace("B2L CPU: 8/8 CPUs initialized, per-CPU timer and bidirectional SGI1 verified\n");
    IOLog("B2L CPU: 8/8 CPUs initialized, per-CPU timer and bidirectional SGI1 verified\n");
    {
        processor_t procs[MAX_CPUS];
        for (unsigned i = 0; i < MAX_CPUS; ++i) procs[i] = b2l_cpus[i]->machProcessor;
        b2l_dvfs_start(procs);
        b2l_gpu_probe();
        b2l_audio_probe();
        /* G3 (B1L-0029): HakiOS-derived Mali service (bsd/dev/b2l_mali_core.c); its own thread, b2l_mali=0 off. */
        hakios_mali_init();
    }
    thread_terminate(current_thread());
}

void B2LCPU::free(void) { OSSafeReleaseNULL(cpuName); IOCPU::free(); }
void B2LCPU::quiesceCPU(void) { panic("B2L CPU: suspend is not implemented"); }
void B2LCPU::haltCPU(void) { panic("B2L CPU: halt is not implemented"); }
const OSSymbol *B2LCPU::getCPUName(void) { return cpuName; }
#endif
