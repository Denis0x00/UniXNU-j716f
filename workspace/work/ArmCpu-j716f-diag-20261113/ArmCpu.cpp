// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <IOKit/IOCPU.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOLocks.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSSymbol.h>
#include <libkern/libkern.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>

// Exact private entry from Darwin 23 arm/rtclock.h. Its full data header is
// not exported to IOKit; the IRQ path calls the real kernel implementation.
extern "C" void rtclock_intr (unsigned int);
// Kernel-internal entries used for secondary CPU startup (Darwin 23 osfmk).
extern "C" vm_offset_t ml_static_vtop (vm_offset_t);
extern "C" char LowResetVectorBase[], LowResetVectorEnd[], ResetHandlerData[], const_boot_args[];
extern "C" kern_return_t processor_start (processor_t);
extern "C" kern_return_t kernel_thread_start (void (*) (void *, wait_result_t), void *, thread_t *);
extern "C" void thread_deallocate (thread_t);
extern "C" thread_t current_thread (void);
extern "C" kern_return_t thread_terminate (thread_t);

#include "J716FCpu0DiagRuntime.h"

extern "C" {
volatile uint32_t armcpu_registered = 0;
volatile uint32_t armcpu_gic_ready = 0;
volatile uint64_t armcpu_timer_interrupts = 0;
volatile uint64_t armcpu_ipi_interrupts = 0;
volatile uint64_t armcpu_ipi_sent = 0;       // SGIs the kernel asked ArmCpu::signalCPU to send
volatile uint64_t armcpu_ipi_selftest_sent = 0;
// Boot core's own GIC registers read back once after enabling (own frame, the same one just written): 0 IGROUPR0, 1 ISENABLER0, 2 GICD_CTLR,
// 3 GICD_TYPER, 4 IGRPMODR0, 5 ICC_PMR_EL1, 6 ICC_IGRPEN1_EL1, 7 ICC_SRE_EL1.
volatile uint32_t armcpu_gic_boot[8];
volatile uint32_t armcpu_core_timer[8], armcpu_core_sgi[8];   // per logical core: architectural timer interrupts and SGI 0 taken
static bool gSpiIpi = false;                // boot-arg armcpu-spi-ipi=1: IPIs are software-pended SPIs 1008+core routed to that core (ICC_SGI1R_EL1 generates nothing on this board)
static bool gSelfIpi = false;               // boot-arg armcpu-self-ipi=1: every core sends itself SGI 0 from initCPU
volatile uint32_t armcpu_irq_phase = 0,armcpu_irq_id = 1023;
// IRQ drains (ArmCpu::interrupt takes interrupts until none is pending) that took more than 128, and the most one took. A burst
// of timer pops is normal under a hypervisor: a vCPU held off its core finds the next deadline rtclock_intr set already past
// when it signals end of interrupt, and the virtual timer fires again.
volatile uint64_t armcpu_irq_long_drains = 0;
volatile uint32_t armcpu_irq_drain_most = 0;
volatile uint32_t armcpu_quiesce_calls = 0,armcpu_halt_calls = 0;
// Secondary-CPU bring-up evidence (smp=1). Step values: 1 nub started, 2 registered, 3 start thread running,
// 4 PSCI CPU_ON issued, 5 entered initCPU on the new core, 6 GIC redistributor ready, 7 running.
volatile uint32_t armcpu_smp_requested = 0, armcpu_smp_running = 0, armcpu_smp_conduit = 0;
volatile uint32_t armcpu_smp_step[8], armcpu_smp_mpidr[8], armcpu_smp_seen_mpidr[8], armcpu_smp_fail[8], armcpu_boot_mpidr = 0;
// System registers seen on each secondary core at initCPU entry: CurrentEL, SCTLR_EL1, TCR_EL1, MAIR_EL1 (low words), MIDR_EL1.
volatile uint32_t armcpu_smp_sys[8][5], armcpu_boot_sys[5];
volatile int32_t armcpu_smp_psci_rc[8];
// 1 once the scheduler was told that every core is registered and started (ml_cpu_init_completed, boot-arg armcpu-edge=1). Until then
// it keeps its start-up state: no thread moves between the clusters.
volatile uint32_t armcpu_cpu_init_completed = 0;
// PSCI as the firmware answers it, asked once with boot-arg diag=1: 0 how far the questions went (0 not asked, 1 version only, 2 all),
// 1 PSCI_VERSION, 2 PSCI_FEATURES of CPU_SUSPEND (negative: absent; bit 0 OS-initiated mode, bit 1 extended state IDs),
// 3 PSCI_FEATURES of PSCI_SET_SUSPEND_MODE (0: present).
volatile int32_t armcpu_psci[4];
}

#define ARMCPU_SMP_SLOTS 8

// Wake and idle statistics of one logical core, on cache lines of its own: the core writes them on every interrupt and every IPI it
// sends. The counts are always kept. The times need boot-arg diag=1: the kernel then calls idleNotify around every WFI and each IPI
// stamps its target, so the rows say what ends a core's idle periods, how long they last, what a send costs the sender and how long
// the target takes to handle it, awake or out of idle. The last idle range is the one a core power-down state (PSCI CPU_SUSPEND) could
// use: such a state has a minimum residency of several milliseconds.
struct ArmCpuCoreStats {
  uint64_t idleSince;          // counter at the idle entry, 0 while not idle
  uint32_t wakeOpen;           // 1 from an idle exit until this core takes an interrupt
  uint32_t deviceInterrupts;   // shared peripheral interrupts taken
  uint32_t idlePeriods[4];     // idle periods under 100 us, under 1 ms, under 8 ms, longer
  uint64_t idleTicks[4];       // counter ticks spent in them
  uint32_t wokenBy[4];         // idle periods ended by the timer, an IPI, a device interrupt, no interrupt of this core
  uint32_t sent;               // IPIs this core sent
  uint32_t sentDeferred;       // those of them the scheduler asked to defer
  uint32_t cancelled;          // deferred IPIs the scheduler asked this core to take back
  uint32_t sendMost;           // counter ticks of the slowest timed send
  uint32_t sendsTimed;         // sends to another core while the times are kept
  uint64_t sendTicks;          // counter ticks of all timed sends
  uint64_t sentAt;             // counter at the oldest IPI sent to this core that it has not handled; 0 none. Atomic.
  uint32_t taken[2][4];        // IPIs handled under 50 us, under 200 us, under 1 ms, later after the send: [0] awake, [1] out of idle
  uint32_t takenMost[2];       // counter ticks of the slowest
  uint32_t timerLate[2][4];    // timer interrupts taken under 50 us, under 200 us, under 1 ms, later after their deadline: [0] awake, [1] out of idle
  uint32_t timerLateMost[2];   // counter ticks of the latest
} __attribute__ ((aligned (128)));
static ArmCpuCoreStats gCore[ARMCPU_SMP_SLOTS];
static volatile uint32_t gCoreSeen = 0;   // logical cores that registered, one bit each
static bool gStats = false;               // boot-arg diag=1
static uint64_t gCounterHz = 0;           // CNTFRQ_EL0
// Boot-arg armcpu-idle-latency-us: the time a core needs to leave idle, given to the kernel as the processor's power gate latency.
// The kernel then spins through a delay shorter than this instead of blocking the thread on a timer (ml_delay_should_spin), sets an
// idle core's timer this much ahead of the deadline and waits out the rest awake (SetIdlePop, rtclock_intr), and does not enter WFI
// for a deadline closer than this. 0, the default, registers no latency: every delay of a thread blocks and a timer is taken as late
// as the core takes to leave idle. The rows of armcpu_stats_row measure that time (timer late, in idle).
static uint32_t gIdleLatencyNs = 0;

class ArmCpu : public IOCPU {
  OSDeclareDefaultStructors (ArmCpu)
  OSSharedPtr<IOMemoryMap> distributor;
  OSSharedPtr<IOMemoryMap> redistributors;
  IOCPUInterruptController *controller = nullptr;
  const OSSymbol *cpuName = nullptr;
  uintptr_t dist = 0;
  uintptr_t redist = 0;
  uint64_t affinity = 0;
  uintptr_t redistBase = 0;
  uint64_t redistSize = 0;
  bool skipGic = false;
  bool diagnosePower = false;
  bool secondary = false;
  bool bootInitialized = false;
  uint32_t logicalId = 0;
  bool mapGic (void);
  bool initDistributor (void);
  bool wakeRedistributor (bool local);
  bool finishSecondary (void);
  static void drainPending (void);
  bool prepareGic (void);
  void startSecondary (void);
  static void startThread (void *argument, wait_result_t);
  void enableGic (void);
  void interrupt (void);
  static void dispatch (void *target, void *, void *, int);
  static void idleNotify (cpu_id_t cpu, boolean_t enter, uint64_t *timeout);
public:
  IOService *probe (IOService *provider, SInt32 *score) override;
  bool start (IOService *provider) override;
  void initCPU (bool boot) override;
  void quiesceCPU (void) override;
  void haltCPU (void) override;
  kern_return_t startCPU (vm_offset_t, vm_offset_t) override;
  void signalCPU (IOCPU *target) override;
  void signalCPUDeferred (IOCPU *target) override;
  void signalCPUCancel (IOCPU *target) override;
  uint64_t signalTarget (void) const { return affinity; }
  void enableCPUTimeBase (bool enabled) override;
  const OSSymbol *getCPUName (void) override;
};

OSDefineMetaClassAndStructors (ArmCpu, IOCPU)

// Redistributor ISENABLER0: SGIs 0..15 (only SGI 0 is used by the kernel; the others let the self-SGI probe tell which INTIDs the GIC accepts) and PPI 27.
static const uint32_t kSgiPpiEnable = 0xffffU | (1U << 27);

static const uint32_t kIpiSpiBase = 1008;   // INTIDs 1008..1015 stand for 'IPI to logical core 0..7'

static uint32_t read32 (uintptr_t base, unsigned offset)
{
  return *reinterpret_cast<volatile uint32_t *>(base + offset);
}

static void write32 (uintptr_t base, unsigned offset, uint32_t value)
{
  *reinterpret_cast<volatile uint32_t *>(base + offset) = value;
}

// Distributor side of the software IPI of logical core `core`: group 1, edge-triggered, priority 0x80, routed to affinity `aff`, enabled.
static void spiIpiConfigure (uintptr_t dist, uint32_t core, uint64_t aff)
{
  const uint32_t id = kIpiSpiBase + core;
  write32 (dist, 0x80 + (id / 32) * 4, read32 (dist, 0x80 + (id / 32) * 4) | (1U << (id % 32)));
  write32 (dist, 0xc00 + (id / 16) * 4, read32 (dist, 0xc00 + (id / 16) * 4) | (2U << ((id % 16) * 2)));
  write32 (dist, 0x400 + (id / 4) * 4, 0x80808080);
  *reinterpret_cast<volatile uint64_t *>(dist + 0x6000 + 8ULL * id) = aff & 0xff00ffffffULL;
  __asm__ volatile ("dsb sy" ::: "memory");
  write32 (dist, 0x100 + (id / 32) * 4, 1U << (id % 32));
  __asm__ volatile ("dsb sy" ::: "memory");
}

static void spiIpiSend (uintptr_t dist, uint32_t core)
{
  const uint32_t id = kIpiSpiBase + core;
  __asm__ volatile ("dsb ishst" ::: "memory");
  write32 (dist, 0x200 + (id / 32) * 4, 1U << (id % 32));
  __asm__ volatile ("dsb sy" ::: "memory");
}

// The cluster type the ADT gives this core (cluster-type 'E' or 'P'), so each cluster registers as its own processor set.
static cluster_type_t clusterType (uint32_t physical)
{
  const ml_topology_info_t *topology = ml_get_topology_info ();
  for (unsigned i = 0; i < topology->num_cpus; i++) {
    if (topology->cpus[i].phys_id == physical) { return topology->cpus[i].cluster_type; }
  }
  return ml_get_boot_cluster_type ();
}

static uint64_t counter (void)
{
  uint64_t value;
  __asm__ volatile ("isb\n\tmrs %0, cntvct_el0" : "=r" (value));
  return value;
}

// Which of four ranges a duration falls in: under a, under b, under c, longer.
static unsigned range (uint64_t ticks, uint64_t a, uint64_t b, uint64_t c)
{
  return ticks < a ? 0 : ticks < b ? 1 : ticks < c ? 2 : 3;
}

// The first interrupt a core takes after an idle exit is what ended that idle period: 0 timer, 1 IPI, 2 device.
static inline void wokenBy (ArmCpuCoreStats &core, unsigned source)
{
  if (core.wakeOpen != 0) { core.wakeOpen = 0; core.wokenBy[source] = core.wokenBy[source] + 1; }
}

// The delay from the oldest unhandled send to this core until its handler runs, for a core that was awake or came out of idle.
static inline void ipiTaken (ArmCpuCoreStats &core, bool fromIdle)
{
  if (!gStats) { return; }
  const uint64_t sent = __atomic_exchange_n (&core.sentAt, 0ULL, __ATOMIC_ACQ_REL);
  if (sent == 0) { return; }
  const uint64_t ticks = counter () - sent;
  const unsigned which = fromIdle ? 1 : 0, slot = range (ticks, gCounterHz / 20000, gCounterHz / 5000, gCounterHz / 1000);
  core.taken[which][slot] = core.taken[which][slot] + 1;
  if (ticks > core.takenMost[which]) { core.takenMost[which] = ticks > 0xffffffffULL ? 0xffffffffU : static_cast<uint32_t> (ticks); }
}

// How long after its deadline this core takes the timer interrupt, awake or out of idle. CNTV_TVAL_EL0 is the deadline minus the
// counter as a signed 32-bit value and goes on counting down after the timer fired, so here it is the delay, negated. The timer is
// one per core and one-shot: the kernel writes each next deadline (ml_set_decrementer).
static inline void timerTaken (ArmCpuCoreStats &core, bool fromIdle)
{
  if (!gStats) { return; }
  uint64_t value;
  __asm__ volatile ("mrs %0, cntv_tval_el0" : "=r" (value));
  const int64_t left = static_cast<int32_t> (static_cast<uint32_t> (value));
  const uint64_t ticks = left < 0 ? static_cast<uint64_t> (-left) : 0;
  const unsigned which = fromIdle ? 1 : 0, slot = range (ticks, gCounterHz / 20000, gCounterHz / 5000, gCounterHz / 1000);
  core.timerLate[which][slot] = core.timerLate[which][slot] + 1;
  if (ticks > core.timerLateMost[which]) { core.timerLateMost[which] = static_cast<uint32_t> (ticks); }
}

// Polls until the bits clear. The limit counts polls as well as time: the counter runs on while the host has this vCPU's thread
// descheduled, so a time limit alone (it was 100 ms) can fail a wake that had no chance to complete.
static bool waitClear (uintptr_t base, unsigned offset, uint32_t mask)
{
  uint64_t frequency;
  __asm__ volatile ("mrs %0, cntfrq_el0" : "=r" (frequency));
  uint64_t started = counter ();
  for (unsigned polls = 0; (read32 (base, offset) & mask) != 0; ++polls) {
    if (j716fdiag::pollLimitReached (polls)) { return false; }
    if (polls > 100000 && counter () - started > frequency) {
      return false;
    }
  }
  return true;
}

// Parks a core that cannot join for good, in WFI with its virtual timer and Group 1 interrupts off, so that it waits blocked:
// Gunyah traps WFI and blocks the vCPU but runs WFE in the guest, and a vCPU left in WFE kept its physical core until the host's
// watchdog rebooted the phone.
static void __attribute__ ((noreturn)) parkCore (void)
{
  __asm__ volatile ("msr daifset, #0xf\n msr cntv_ctl_el0, xzr\n isb" ::: "memory");
  uint64_t pfr0;
  __asm__ volatile ("mrs %0, id_aa64pfr0_el1" : "=r" (pfr0));
  if (((pfr0 >> 24) & 0xf) != 0) {
    uint64_t sre;
    __asm__ volatile ("mrs %0, S3_0_C12_C12_5" : "=r" (sre));           // ICC_SRE_EL1
    if ((sre & 1) != 0) { __asm__ volatile ("msr S3_0_C12_C12_7, xzr\n isb" ::: "memory"); }   // ICC_IGRPEN1_EL1
  }
  for (;;) { __asm__ volatile ("dsb sy\n wfi" ::: "memory"); }
}

// GIC state shared by all CPU nubs. The boot CPU maps the distributor and redistributor frames; a secondary core
// only needs the virtual addresses and its own redistributor frame.
static OSSharedPtr<IOMemoryMap> gDistMap, gRedistMap;
static uintptr_t gDist = 0, gFrames = 0;
static uint64_t gFramesSize = 0;
static IOCPUInterruptController *gController = nullptr;
static volatile uint32_t gGicShared = 0;
static uint64_t gRedistStride = 0; // DT redistributor-stride; 0 = infer from GICR_TYPER.VLPIS
volatile uint32_t armcpu_gic_stride = 0, armcpu_gic_typer_probe[2];
static bool gSmpWanted = false;
// Secondary nubs never wait for the boot CPU (IOSleep needs the timer that only the boot nub enables): they queue here and
// the boot nub, or a late secondary once the boot CPU is registered, finishes them.
static class ArmCpu *volatile gPending[ARMCPU_SMP_SLOTS];
static volatile uint32_t gBootRegistered = 0;
static bool gRemoteGic = false; // smp-gic=1: the boot CPU prepares a new core's redistributor before CPU_ON

// Shared peripheral interrupts (GICv3 SPIs, INTID 32..1019). A driver attaches a handler for one INTID; the line is set to
// Group 1 non-secure, priority 0xa0, the given trigger and routed to the boot CPU (GICD_IROUTER, affinity routing is on),
// then enabled. The handler runs in the primary interrupt context, before the EOI, and must clear a level source.
// An SPI without a handler is counted and disabled instead of being acknowledged forever.
#define ARMCPU_SPI_SLOTS 24
struct ArmCpuSpi {uint32_t id; void (*handler) (void *); void *context; volatile uint64_t count;};
static ArmCpuSpi gSpi[ARMCPU_SPI_SLOTS];
static uint32_t gSpiRoutes[1024];
static volatile uint32_t gSpiUsed = 0;
static uint64_t gBootAffinity = 0;
static IOLock *gSpiRegistrationLock;
static IOLock *spiRegistrationLock (void)
{
  IOLock *lock = __atomic_load_n (&gSpiRegistrationLock, __ATOMIC_ACQUIRE);
  if (lock != nullptr) { return lock; }
  IOLock *candidate = IOLockAlloc ();
  if (candidate == nullptr) { return nullptr; }
  if (!__atomic_compare_exchange_n (&gSpiRegistrationLock, &lock, candidate, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
    IOLockFree (candidate); return lock;
  }
  return candidate;
}
static bool spiDispatch (uint32_t intid)
{
  bool handled = false;
  if (intid < 32 || intid >= 1024) { return false; }
  uint32_t routes = __atomic_load_n (&gSpiRoutes[intid], __ATOMIC_ACQUIRE);
  while (routes) {
    const uint32_t slot = __builtin_ctz (routes); routes &= routes - 1;
    ArmCpuSpi &spi = gSpi[slot];
    spi.count = spi.count + 1;
    spi.handler (spi.context); handled = true;
  }
  return handled;
}
extern "C" {
volatile uint64_t armcpu_spi_unhandled = 0;
volatile uint32_t armcpu_spi_unhandled_id = 0;
}

IOService *ArmCpu::probe (IOService *provider, SInt32 *score)
{
  OSData *type = OSDynamicCast (OSData, provider->getProperty ("device_type"));
  OSData *reg = OSDynamicCast (OSData, provider->getProperty ("reg"));
  uint32_t id;
  if ((type == nullptr) || (type->getLength () != 4) ||
      (memcmp (type->getBytesNoCopy (), "cpu", 4) != 0) ||
      (reg == nullptr) || (reg->getLength () < sizeof (id))) {
    return nullptr;
  }
  memcpy (&id, reg->getBytesNoCopy (), sizeof (id));
  if (ml_get_cpu_number (id) != ml_get_boot_cpu_number ()) {
    unsigned smp = 0;
    PE_parse_boot_argn ("smp", &smp, sizeof (smp));
    if (smp == 0 || ml_get_cpu_number (id) < 0 || ml_get_cpu_number (id) >= ARMCPU_SMP_SLOTS) {
      return nullptr; // Secondary startup is opt-in (smp=1).
    }
  }
  return IOCPU::probe (provider, score);
}

bool ArmCpu::mapGic (void)
{
  IORegistryEntry *soc = IORegistryEntry::fromPath ("/arm-io", gIODTPlane);
  IORegistryEntry *gic = IORegistryEntry::fromPath ("/arm-io/intc", gIODTPlane);
  if ((soc == nullptr) || (gic == nullptr)) {
    if (soc != nullptr) { soc->release (); }
    if (gic != nullptr) { gic->release (); }
    return false;
  }
  OSData *ranges = OSDynamicCast (OSData, soc->getProperty ("ranges"));
  OSData *reg = OSDynamicCast (OSData, gic->getProperty ("reg"));
  OSData *compatible = OSDynamicCast (OSData, gic->getProperty ("compatible"));
  uint64_t bus[3];
  uint64_t regions[4];
  bool valid = (ranges != nullptr) && (ranges->getLength () == sizeof (bus)) &&
               (reg != nullptr) && (reg->getLength () == sizeof (regions)) &&
               (compatible != nullptr) && (compatible->getLength () == 11) &&
               (memcmp (compatible->getBytesNoCopy (), "arm,gic-v3", 11) == 0);
  if (valid) {
    memcpy (bus, ranges->getBytesNoCopy (), sizeof (bus));
    memcpy (regions, reg->getBytesNoCopy (), sizeof (regions));
  }
  OSData *strideProperty = OSDynamicCast (OSData, gic->getProperty ("redistributor-stride"));
  if (strideProperty != nullptr && strideProperty->getLength () == sizeof (uint64_t)) {
    memcpy (&gRedistStride, strideProperty->getBytesNoCopy (), sizeof (uint64_t));
    if (gRedistStride != 0x20000 && gRedistStride != 0x30000 && gRedistStride != 0x40000) { gRedistStride = 0; }
  }
  armcpu_gic_stride = static_cast<uint32_t> (gRedistStride);
  soc->release ();
  gic->release ();
  if (!valid || (regions[1] < 0x10000) || (regions[3] < 0x20000)) {
    return false;
  }
  for (unsigned index = 0; index < 4; index += 2) {
    if ((regions[index] < bus[0]) || (regions[index] - bus[0] > bus[2]) ||
        (regions[index + 1] > bus[2] - (regions[index] - bus[0])) ||
        (bus[1] + regions[index] - bus[0] < bus[1])) {
      return false;
    }
  }
  OSSharedPtr<IOMemoryDescriptor> distMemory (IOMemoryDescriptor::withPhysicalAddress (
      bus[1] + regions[0] - bus[0], regions[1], kIODirectionInOut), OSNoRetain);
  OSSharedPtr<IOMemoryDescriptor> rdMemory (IOMemoryDescriptor::withPhysicalAddress (
      bus[1] + regions[2] - bus[0], regions[3], kIODirectionInOut), OSNoRetain);
  if (!distMemory || !rdMemory) {
    return false;
  }
  distributor = OSSharedPtr<IOMemoryMap> (distMemory->map (kIOMapAnywhere | kIOMapInhibitCache), OSNoRetain);
  redistributors = OSSharedPtr<IOMemoryMap> (rdMemory->map (kIOMapAnywhere | kIOMapInhibitCache), OSNoRetain);
  if (!distributor || !redistributors) {
    return false;
  }
  dist = static_cast<uintptr_t> (distributor->getVirtualAddress ());
  if (((read32 (dist, 0xffe8) >> 4) & 15) != 3) {
    return false;
  }
  redistBase = static_cast<uintptr_t> (redistributors->getVirtualAddress ());
  redistSize = regions[3];
  gDistMap = distributor;
  gRedistMap = redistributors;
  gDist = dist;
  gFrames = redistBase;
  gFramesSize = redistSize;
  return true;
}

bool ArmCpu::initDistributor (void)
{
  uint32_t ctl = read32 (dist, 0);
  write32 (dist, 0, ctl & ~3U);
  if (!waitClear (dist, 0, 1U << 31)) { return false; }
  write32 (dist, 0, (ctl & ~3U) | 16U);
  if (!waitClear (dist, 0, 1U << 31)) { return false; }
  unsigned interrupts = ((read32 (dist, 4) & 31) + 1) * 32;
  for (unsigned id = 32; id < interrupts && id < 1020; id += 32) {
    write32 (dist, 0x180 + id / 8, 0xffffffff);
  }
  return true;
}

// Runs on the core whose redistributor is configured: the frame is selected by that core's own MPIDR affinity.
bool ArmCpu::wakeRedistributor (bool local)
{
  uintptr_t frames = redistBase;
  // local: this core finds its own frame from MPIDR. Remote: the boot CPU prepares a not-yet-started core's frame
  // from the DT affinity, so the new core never needs GIC MMIO of its own.
  if (local) { __asm__ volatile ("mrs %0, mpidr_el1" : "=r" (affinity)); }
  uint32_t target = static_cast<uint32_t> ((affinity & 0xffffff) | ((affinity >> 8) & 0xff000000));
  redist = 0;
  for (uint64_t offset = 0; offset + 0x20000 <= redistSize;) {
    armcpu_gic_typer_probe[0] = static_cast<uint32_t> (offset);
    uint64_t typer = *reinterpret_cast<volatile uint64_t *> (frames + offset + 8);
    armcpu_gic_typer_probe[1] = static_cast<uint32_t> (typer);
    if (static_cast<uint32_t> (typer >> 32) == target) {
      redist = frames + offset;
      break;
    }
    if ((typer & 16) != 0) { break; }
    offset += gRedistStride != 0 ? gRedistStride : ((typer & 2) != 0 ? 0x40000 : 0x20000);
  }
  if (redist == 0) { return false; }
  write32 (redist, 0x14, read32 (redist, 0x14) & ~2U);
  if (!waitClear (redist, 0x14, 4)) { return false; }
  uintptr_t sgi = redist + 0x10000;
  write32 (sgi, 0x180, 0xffffffff);
  write32 (sgi, 0x80, 0xffffffff);
  write32 (sgi, 0xd00, 0);
  write32 (sgi, 0x280, 0xffffffff);
  for (unsigned offset = 0x400; offset < 0x420; offset += 4) {
    write32 (sgi, offset, 0x80808080);
  }
  if (!local) { write32 (sgi, 0x100, kSgiPpiEnable); }
  __asm__ volatile ("dsb sy" ::: "memory");
  return waitClear (redist, 0, 8);
}

bool ArmCpu::prepareGic (void)
{
  if (!mapGic () || !initDistributor () || !wakeRedistributor (true)) { return false; }
  gGicShared = 1;
  return true;
}

void ArmCpu::enableGic (void)
{
  uint64_t value;
  __asm__ volatile ("mrs %0, icc_sre_el1" : "=r" (value));
  value |= 1;
  __asm__ volatile ("msr icc_sre_el1, %0\n\tisb" :: "r" (value) : "memory");
  __asm__ volatile ("mrs %0, icc_sre_el1" : "=r" (value));
  if ((value & 1) == 0) { panic ("ArmCpu: GICv3 system register interface unavailable"); }
  __asm__ volatile ("mrs %0, icc_ctlr_el1" : "=r" (value));
  value &= ~2ULL; // EOImode 0: EOIR also deactivates the interrupt.
  __asm__ volatile ("msr icc_ctlr_el1, %0" :: "r" (value) : "memory");
  value = 0xff;
  __asm__ volatile ("msr icc_pmr_el1, %0" :: "r" (value) : "memory");
  value = 0;
  __asm__ volatile ("msr icc_bpr1_el1, %0" :: "r" (value) : "memory");
  if (!secondary) { gBootAffinity = affinity & 0xff00ffffffULL; }
  if (!secondary || !gRemoteGic) {
    write32 (redist + 0x10000, 0x100, kSgiPpiEnable);
    write32 (dist, 0, read32 (dist, 0) | 0x12U);
  }
  value = 1;
  __asm__ volatile ("msr icc_igrpen1_el1, %0\n\tdsb sy\n\tisb" :: "r" (value) : "memory");
  if (!secondary) {
    uint64_t pmr, ig, sre;
    __asm__ volatile ("mrs %0, icc_pmr_el1" : "=r" (pmr));
    __asm__ volatile ("mrs %0, icc_igrpen1_el1" : "=r" (ig));
    __asm__ volatile ("mrs %0, icc_sre_el1" : "=r" (sre));
    armcpu_gic_boot[0] = read32 (redist + 0x10000, 0x80);
    armcpu_gic_boot[1] = read32 (redist + 0x10000, 0x100);
    armcpu_gic_boot[2] = read32 (dist, 0);
    armcpu_gic_boot[3] = read32 (dist, 4);
    armcpu_gic_boot[4] = read32 (redist + 0x10000, 0xd00);
    armcpu_gic_boot[5] = static_cast<uint32_t> (pmr);
    armcpu_gic_boot[6] = static_cast<uint32_t> (ig);
    armcpu_gic_boot[7] = static_cast<uint32_t> (sre);
  }
  armcpu_gic_ready = 1;
}


bool ArmCpu::start (IOService *provider)
{
  unsigned skip = 0;
  PE_parse_boot_argn ("armcpu-skip-gic", &skip, sizeof (skip));
  skipGic = skip != 0;
  unsigned selfIpi = 0;
  PE_parse_boot_argn ("armcpu-self-ipi", &selfIpi, sizeof (selfIpi));
  gSelfIpi = selfIpi == 1;
  unsigned spiIpi = 0;
  PE_parse_boot_argn ("armcpu-spi-ipi", &spiIpi, sizeof (spiIpi));
  gSpiIpi = spiIpi == 1;
  unsigned diagnostic = 0;
  PE_parse_boot_argn("armcpu-diagnostics", &diagnostic, sizeof(diagnostic));
  diagnosePower=diagnostic==1;
  unsigned stats = 0;
  PE_parse_boot_argn ("diag", &stats, sizeof (stats));
  __asm__ volatile ("mrs %0, cntfrq_el0" : "=r" (gCounterHz));
  gStats = stats == 1 && gCounterHz != 0;
  unsigned idleLatencyUs = 0;
  PE_parse_boot_argn ("armcpu-idle-latency-us", &idleLatencyUs, sizeof (idleLatencyUs));
  gIdleLatencyNs = idleLatencyUs > 4000000U ? 4000000000U : idleLatencyUs * 1000U;
  uint32_t physical;
  OSData *reg = OSDynamicCast (OSData, provider->getProperty ("reg"));
  memcpy (&physical, reg->getBytesNoCopy (), sizeof (physical));
  logicalId = static_cast<uint32_t> (ml_get_cpu_number (physical));
  secondary = logicalId != static_cast<uint32_t> (ml_get_boot_cpu_number ());
  if (!j716fdiag::begin (logicalId, secondary, skipGic)) { return false; }
  if (secondary) {
    if (!IOCPU::start (provider)) { return false; }
    armcpu_smp_step[logicalId] = 1;
    affinity = physical;
    armcpu_smp_mpidr[logicalId] = physical;
    cpuName = OSSymbol::withCStringNoCopy ("ARM architectural secondary CPU");
    if (cpuName == nullptr) { return false; }
    retain ();
    __atomic_store_n (&gPending[logicalId], this, __ATOMIC_RELEASE);
    if (gBootRegistered != 0) { drainPending (); }
    return true;
  } else {
    uint64_t bootMpidr;
    __asm__ volatile ("mrs %0, mpidr_el1" : "=r" (bootMpidr));
    armcpu_boot_mpidr = static_cast<uint32_t> (bootMpidr);
    if ((!skipGic && !j716fdiag::gicPrepared (prepareGic ())) || !IOCPU::start (provider)) { return false; }
  }
  cpuName = OSSymbol::withCStringNoCopy ("ARM architectural boot CPU");
  if (cpuName == nullptr) { return false; }
  ml_processor_info_t info = {};
  info.cpu_id = this;
  info.phys_id = physical;
  info.log_id = logicalId;
  info.cluster_id = static_cast<uint32_t> (ml_get_cluster_number (physical));
  info.cluster_type = clusterType (physical);
  info.powergate_latency = gIdleLatencyNs;
  if (gStats) { info.processor_idle = &ArmCpu::idleNotify; }
  __atomic_fetch_or (&gCoreSeen, 1U << (logicalId & 7), __ATOMIC_RELEASE);
  setCPUNumber (info.log_id);
  unsigned smp = 0;
  PE_parse_boot_argn ("smp", &smp, sizeof (smp));
  gSmpWanted = smp != 0;
  unsigned remote = 0;
  PE_parse_boot_argn ("smp-gic", &remote, sizeof (remote));
  gRemoteGic = remote != 0;
  armcpu_smp_requested = gSmpWanted ? 1 : 0;
  unsigned cpuCount = static_cast<unsigned> (ml_get_max_cpu_number ()) + 1;
  controller = new IOCPUInterruptController;
  if ((controller == nullptr) || (controller->initCPUInterruptController (cpuCount, 1) != kIOReturnSuccess)) {
    panic ("ArmCpu: CPU interrupt controller allocation failed");
  }
  controller->registerCPUInterruptController ();
  controller->enableCPUInterrupt (this);
  if (controller->registerInterrupt (provider, logicalId, this, &ArmCpu::dispatch, nullptr) != kIOReturnSuccess) {
    panic ("ArmCpu: CPU IRQ action registration failed");
  }
  gController = controller;
  // Newer kernels boot the processor from ml_processor_register, which calls initCPU and enables the GIC and
  // timer; the handler above must already be installed. Older kernels leave initCPU to the driver.
  perfmon_interrupt_handler_func pmi = nullptr;
  kern_return_t status = ml_processor_register (&info, &machProcessor, &ipi_handler, &pmi);
  if (status != KERN_SUCCESS) { panic ("ArmCpu: CPU %u registration failed: %d", logicalId, status); }
  if (j716fdiag::failed ()) { return false; }
  armcpu_registered = 1;
  if (!bootInitialized) { initCPU (true); }
  if (j716fdiag::failed ()) { return false; }
  registerService ();
  __atomic_store_n (&gBootRegistered, 1U, __ATOMIC_RELEASE);
  drainPending ();
  if (!j716fdiag::startWorker ()) { return false; }
  return true;
}

void ArmCpu::drainPending (void)
{
  for (unsigned id = 1; id < ARMCPU_SMP_SLOTS; id++) {
    ArmCpu *cpu = __atomic_exchange_n (&gPending[id], static_cast<ArmCpu *> (nullptr), __ATOMIC_ACQ_REL);
    if (cpu != nullptr) { cpu->finishSecondary (); }
  }
}

bool ArmCpu::finishSecondary (void)
{
  dist = gDist;
  redistBase = gFrames;
  redistSize = gFramesSize;
  if (gRemoteGic && !wakeRedistributor (false)) { armcpu_smp_fail[logicalId] = 3; return false; }
  ml_processor_info_t info = {};
  info.cpu_id = this;
  info.phys_id = static_cast<uint32_t> (affinity);
  info.log_id = logicalId;
  info.cluster_id = static_cast<uint32_t> (ml_get_cluster_number (static_cast<uint32_t> (affinity)));
  info.cluster_type = clusterType (static_cast<uint32_t> (affinity));
  info.powergate_latency = gIdleLatencyNs;
  if (gStats) { info.processor_idle = &ArmCpu::idleNotify; }
  __atomic_fetch_or (&gCoreSeen, 1U << (logicalId & 7), __ATOMIC_RELEASE);
  perfmon_interrupt_handler_func pmi = nullptr;
  kern_return_t status = ml_processor_register (&info, &machProcessor, &ipi_handler, &pmi);
  if (status != KERN_SUCCESS) { armcpu_smp_fail[logicalId] = 4; return false; }
  setCPUNumber (info.log_id);
  armcpu_smp_step[logicalId] = 2;
  setCPUState (kIOCPUStateUninitalized);
  registerService ();
  startSecondary ();
  return true;
}

// PSCI conduit comes from the /psci node of the device tree ("smc" or "hvc"); no default is assumed.
static bool psciConduit (bool *hvc)
{
  IORegistryEntry *psci = IORegistryEntry::fromPath ("/psci", gIODTPlane);
  if (psci == nullptr) { return false; }
  OSData *method = OSDynamicCast (OSData, psci->getProperty ("method"));
  bool ok = false;
  if (method != nullptr && method->getLength () == 4 && memcmp (method->getBytesNoCopy (), "smc", 4) == 0) {
    *hvc = false; ok = true;
  } else if (method != nullptr && method->getLength () == 4 && memcmp (method->getBytesNoCopy (), "hvc", 4) == 0) {
    *hvc = true; ok = true;
  }
  psci->release ();
  return ok;
}

// The callee may change x0 to x17 (SMC calling convention): x0 to x3 carry results, the others are scratch.
static int64_t psciCall (bool hvc, uint64_t function, uint64_t a, uint64_t b, uint64_t c)
{
  register uint64_t x0 __asm__ ("x0") = function;
  register uint64_t x1 __asm__ ("x1") = a;
  register uint64_t x2 __asm__ ("x2") = b;
  register uint64_t x3 __asm__ ("x3") = c;
  if (hvc) {
    __asm__ volatile ("hvc #0" : "+r" (x0), "+r" (x1), "+r" (x2), "+r" (x3) : : "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
  } else {
    __asm__ volatile ("smc #0" : "+r" (x0), "+r" (x1), "+r" (x2), "+r" (x3) : : "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
  }
  return static_cast<int64_t> (x0);
}

void ArmCpu::startThread (void *argument, wait_result_t)
{
  ArmCpu *self = static_cast<ArmCpu *> (argument);
  armcpu_smp_step[self->logicalId] = 3;
  // Kernels that leave the start to the driver block here until the new core reaches the scheduler; a core that never arrives only
  // stalls this helper thread. Kernels that start a core when it registers return at once.
  processor_start (self->machProcessor);
  // Once every secondary core has started, the scheduler sets up its cluster edges (Edge migration and steal rules).
  static uint32_t started = 0;
  // armcpu-edge=0 leaves the Edge matrix all-disallowed (no thread moves between clusters). On iPadOS 27 the matrix fails the scheduler's
  // own consistency check, which the kernel now reports instead of asserting.
  unsigned edge = 0;
  PE_parse_boot_argn ("armcpu-edge", &edge, sizeof (edge));
  if (__atomic_add_fetch (&started, 1U, __ATOMIC_ACQ_REL) + 1 == ml_get_topology_info ()->num_cpus && edge != 0) {
    ml_cpu_init_completed ();
    armcpu_cpu_init_completed = 1;
  }
  thread_terminate (current_thread ());
}

void ArmCpu::startSecondary (void)
{
  thread_t thread;
  if (kernel_thread_start (&ArmCpu::startThread, this, &thread) != KERN_SUCCESS) {
    armcpu_smp_psci_rc[logicalId] = -1000;
    return;
  }
  thread_deallocate (thread);
}

void ArmCpu::initCPU (bool boot)
{
  if (secondary) {
    if (!boot) { panic ("ArmCpu: secondary resume is unsupported"); }
    armcpu_smp_step[logicalId] = 5;
    uint64_t mpidr;
    __asm__ volatile ("mrs %0, mpidr_el1" : "=r" (mpidr));
    armcpu_smp_seen_mpidr[logicalId] = static_cast<uint32_t> (mpidr);
    // A failure is recorded and the core parked (rather than a panic that hides the other cores' state).
    if ((mpidr & 0xffff) != (affinity & 0xffff)) { armcpu_smp_fail[logicalId] = 1; parkCore (); }
    if (!gRemoteGic && !wakeRedistributor (true)) { armcpu_smp_fail[logicalId] = 2; parkCore (); }
    armcpu_smp_step[logicalId] = 6;
    enableGic ();
    if (gSpiIpi) { spiIpiConfigure (gDist, logicalId, affinity); }
    // The shared IOCPUInterruptController blocks registrations until every core is enabled, so the secondary cores
    // install their handler directly. ml_install_interrupt_handler() records it in the calling core's cpu_data.
    ml_install_interrupt_handler (this, static_cast<int> (logicalId), this, reinterpret_cast<IOInterruptHandler> (&ArmCpu::dispatch), nullptr);
    enableCPUTimeBase (true);
    setCPUState (kIOCPUStateRunning);
    armcpu_smp_step[logicalId] = 7;
    armcpu_smp_running = armcpu_smp_running + 1;
    // XNU clears SIGPdisabled for a core the first time that core takes an IPI, and cpu_signal() silently skips a core until then; Apple's
    // CPU driver provokes it with a self-IPI from initCPU (osfmk/kern/machine.c, processor_up).
    if (gSelfIpi) { signalCPU (this); }
    return;
  }
  if (!boot) { panic ("ArmCpu: secondary/resume CPU startup is unsupported"); }
  if (skipGic) {
    // Explicit A/B hardware diagnostic: do not touch GIC MMIO or enable an
    // interrupt source. CPU registration still happens; the timer is left off.
    setCPUState (kIOCPUStateRunning);
    bootInitialized = true;
    return;
  }
  enableGic ();
  if (!j716fdiag::checkGic (logicalId, dist, distributor ? distributor->getLength () : 0,
                             redistBase, redistSize, redist)) { return; }
  if (gSpiIpi) { spiIpiConfigure (gDist, logicalId, affinity); }
  enableCPUTimeBase (true);
  setCPUState (kIOCPUStateRunning);
  bootInitialized = true;
  if (gSelfIpi) { signalCPU (this); }
}

void ArmCpu::dispatch (void *target, void *, void *, int)
{
  static_cast<ArmCpu *> (target)->interrupt ();
}

// The kernel's idle callback (registered with diag=1 only): called with interrupts masked right before a core's WFI and right after
// it. An idle period that follows one with no interrupt taken in between means the earlier WFI returned without an interrupt for
// this core. The idle timeout is left alone.
void ArmCpu::idleNotify (cpu_id_t cpu, boolean_t enter, uint64_t *)
{
  ArmCpuCoreStats &core = gCore[static_cast<ArmCpu *> (cpu)->logicalId & 7];
  const uint64_t now = counter ();
  if (enter) {
    if (core.wakeOpen != 0) { core.wakeOpen = 0; core.wokenBy[3] = core.wokenBy[3] + 1; }
    core.idleSince = now;
    return;
  }
  if (core.idleSince == 0) { return; }
  const uint64_t ticks = now - core.idleSince;
  const unsigned slot = range (ticks, gCounterHz / 10000, gCounterHz / 1000, gCounterHz / 125);
  core.idlePeriods[slot] = core.idlePeriods[slot] + 1;
  core.idleTicks[slot] = core.idleTicks[slot] + ticks;
  core.idleSince = 0;
  core.wakeOpen = 1;
}

void ArmCpu::interrupt (void)
{
  unsigned drained=0;
  ArmCpuCoreStats &core = gCore[logicalId & 7];
  const bool fromIdle = core.wakeOpen != 0;
  for (;;) {
    armcpu_irq_phase=1;
    uint64_t id;
    __asm__ volatile ("mrs %0, icc_iar1_el1" : "=r" (id));
    armcpu_irq_id=static_cast<uint32_t>(id & 0xffffff);
    if ((id & 0xffffff) >= 1020) {
      armcpu_irq_phase=0;
      if (drained > 128) { armcpu_irq_long_drains = armcpu_irq_long_drains + 1; }
      if (drained > armcpu_irq_drain_most) { armcpu_irq_drain_most = drained; }
      break;
    }
    // Only an interrupt that never stops coming (a level line nobody clears) is a fault: a million in one drain.
    ++drained;
    if(diagnosePower && drained>1000000)panic("ArmCpu: IRQ drain exceeds 1000000 id=%llx timer=%llu IPI=%llu",id,armcpu_timer_interrupts,armcpu_ipi_interrupts);
    if ((id & 0xffffff) == 27) {
      j716fdiag::irqBegin (logicalId, 27, armcpu_timer_interrupts);
      armcpu_timer_interrupts = armcpu_timer_interrupts + 1;
      armcpu_core_timer[logicalId & 7] = armcpu_core_timer[logicalId & 7] + 1;
      timerTaken (core, core.wakeOpen != 0);
      wokenBy (core, 0);
      armcpu_irq_phase=2;
      rtclock_intr (0);
      j716fdiag::irqReturned (armcpu_timer_interrupts);
      armcpu_irq_phase=3;
    } else if ((id & 0xffffff) == 0) {
      armcpu_ipi_interrupts = armcpu_ipi_interrupts + 1;
      armcpu_core_sgi[logicalId & 7] = armcpu_core_sgi[logicalId & 7] + 1;
      wokenBy (core, 1);
      ipiTaken (core, fromIdle);
      armcpu_irq_phase=4;
      if (ipi_handler != nullptr) { ipi_handler (); }
      armcpu_irq_phase=5;
    } else if (gSpiIpi && (id & 0xffffff) >= kIpiSpiBase && (id & 0xffffff) < kIpiSpiBase + 8) {
      armcpu_ipi_interrupts = armcpu_ipi_interrupts + 1;
      armcpu_core_sgi[logicalId & 7] = armcpu_core_sgi[logicalId & 7] + 1;
      wokenBy (core, 1);
      ipiTaken (core, fromIdle);
      armcpu_irq_phase = 4;
      if (ipi_handler != nullptr) { ipi_handler (); }
      armcpu_irq_phase = 5;
    } else {
      const uint32_t intid = static_cast<uint32_t> (id & 0xffffff);
      core.deviceInterrupts = core.deviceInterrupts + 1;
      wokenBy (core, 2);
      armcpu_irq_phase = 7;
      if (spiDispatch (intid)) {
      } else if (intid >= 32 && gDist != 0) {
        armcpu_spi_unhandled = armcpu_spi_unhandled + 1;
        armcpu_spi_unhandled_id = intid;
        write32 (gDist, 0x180 + (intid / 32) * 4, 1U << (intid % 32));
      } else {
        panic ("ArmCpu: unexpected architectural IRQ %llu", id);
      }
    }
    armcpu_irq_phase=6;
    __asm__ volatile ("msr icc_eoir1_el1, %0\n\tisb" :: "r" (id) : "memory");
    j716fdiag::irqEoi (logicalId, static_cast<uint32_t> (id & 0xffffff));
  }
}

void ArmCpu::signalCPU (IOCPU *target)
{
  ArmCpu *peer = OSDynamicCast (ArmCpu, target);
  if (peer == nullptr || (peer->signalTarget () & 0xff) >= 16) {
    panic ("ArmCpu: unsupported CPU signal target");
  }
  uint64_t aff = peer->signalTarget ();
  uint64_t sgi = ((aff & 0xff00000000ULL) << 16) | ((aff & 0xff0000) << 16) |
                 ((aff & 0xff00) << 8) | (1ULL << (aff & 0xff));
  armcpu_ipi_sent = armcpu_ipi_sent + 1;
  // The kernel's cpu_signal path calls this on the sending core with interrupts masked, so that core's statistics need no lock and
  // nothing runs between the two counter reads. The start-up IPI a core sends itself from initCPU is counted but neither stamped
  // nor timed: the core takes it only when it first unmasks interrupts, which is not a cross-core delay, and on the boot core
  // initCPU runs in thread context, where the handler could run inside the timed send.
  ArmCpuCoreStats &core = gCore[logicalId & 7];
  core.sent = core.sent + 1;
  const bool timed = gStats && peer != this;
  uint64_t before = 0;
  if (timed) {
    before = counter ();
    uint64_t none = 0;
    __atomic_compare_exchange_n (&gCore[peer->logicalId & 7].sentAt, &none, before, false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED);
  }
  if (gSpiIpi) {
    spiIpiSend (gDist, peer->logicalId & 7);
  } else {
    __asm__ volatile ("dsb ishst; msr icc_sgi1r_el1, %0; isb" :: "r" (sgi) : "memory");
  }
  if (timed) {
    const uint64_t ticks = counter () - before;
    core.sendsTimed = core.sendsTimed + 1;
    core.sendTicks = core.sendTicks + ticks;
    if (ticks > core.sendMost) { core.sendMost = ticks > 0xffffffffULL ? 0xffffffffU : static_cast<uint32_t> (ticks); }
  }
}

// The scheduler asks for a deferred IPI when it wakes an idle core for a thread that is not realtime, and takes it back when the
// sending core runs that thread itself first. The GIC has no delayed signal: the IPI goes out at once and a cancel finds nothing to
// take back, as IOCPU does by default. Both requests are counted, which says how many wakes a delayed signal would have saved.
void ArmCpu::signalCPUDeferred (IOCPU *target)
{
  ArmCpuCoreStats &core = gCore[logicalId & 7];
  core.sentDeferred = core.sentDeferred + 1;
  signalCPU (target);
}

void ArmCpu::signalCPUCancel (IOCPU *)
{
  ArmCpuCoreStats &core = gCore[logicalId & 7];
  core.cancelled = core.cancelled + 1;
}

// Diagnostic: sends SGI 0 to the core whose MPIDR affinity is aff (the DT "reg" value, e.g. 0x100 for core 1) exactly as signalCPU
// does. The handler on the target runs the kernel's IPI callback with nothing pending, which is harmless. Returns nothing: the caller
// compares armcpu_ipi_interrupts before and after.
// Read-only snapshot of the GIC state that decides whether SGI 0 reaches the core with MPIDR affinity aff: its redistributor's SGI frame and
// the distributor control register. out: 0 IGROUPR0, 1 ISENABLER0, 2 ISPENDR0, 3 IGRPMODR0, 4 GICR_WAKER, 5 GICD_CTLR, 6 IPRIORITYR0, 7 found(1).
// armcpu_gic_diag_stage names the access in progress, so a read that never returns is identified from another thread:
// 0x100+frame walk, 2 IGROUPR0, 3 ISENABLER0, 4 ISPENDR0, 5 IGRPMODR0, 6 WAKER, 7 GICD_CTLR, 8 IPRIORITYR0, 9 done, 0xe not found.
volatile uint32_t armcpu_gic_diag_stage = 0;
extern "C" void armcpu_gic_diag (uint64_t aff, uint32_t *out)
{
  for (unsigned i = 0; i < 8; i++) { out[i] = 0xdeadbeef; }
  if (gDist == 0 || gFrames == 0) { armcpu_gic_diag_stage = 0xe; return; }
  const uint32_t target = static_cast<uint32_t> ((aff & 0xffffff) | ((aff >> 8) & 0xff000000));
  for (uint64_t offset = 0; offset + 0x20000 <= gFramesSize;) {
    armcpu_gic_diag_stage = 0x100 + static_cast<uint32_t> (offset >> 16);
    const uint64_t typer = *reinterpret_cast<volatile uint64_t *> (gFrames + offset + 8);
    if (static_cast<uint32_t> (typer >> 32) == target) {
      const uintptr_t redist = gFrames + offset, sgi = redist + 0x10000;
      armcpu_gic_diag_stage = 2; out[0] = read32 (sgi, 0x80);
      armcpu_gic_diag_stage = 3; out[1] = read32 (sgi, 0x100);
      armcpu_gic_diag_stage = 4; out[2] = read32 (sgi, 0x200);
      armcpu_gic_diag_stage = 5; out[3] = read32 (sgi, 0xd00);
      armcpu_gic_diag_stage = 6; out[4] = read32 (redist, 0x14);
      armcpu_gic_diag_stage = 7; out[5] = read32 (gDist, 0);
      armcpu_gic_diag_stage = 8; out[6] = read32 (sgi, 0x400);
      out[7] = 1;
      armcpu_gic_diag_stage = 9;
      return;
    }
    if ((typer & 16) != 0) { break; }
    offset += gRedistStride != 0 ? gRedistStride : ((typer & 2) != 0 ? 0x40000 : 0x20000);
  }
  armcpu_gic_diag_stage = 0xe;
}

// Does an SGI sent to the calling core itself become pending at its own CPU interface, and which INTIDs? Only system registers are used (no GIC
// MMIO). With IRQs masked, SGIs 0..15 are sent one at a time to this core's own MPIDR affinity through ICC_SGI1R_EL1 (Group 1) and, when that
// shows nothing, through ICC_SGI0R_EL1 (Group 0); ICC_HPPIR1_EL1 / ICC_HPPIR0_EL1 is polled and a pending SGI is acknowledged and ended here so the
// kernel never sees it. out[8], out[9]: CNTVCT ticks of one ICC_SGI1R_EL1 write and of one ICC_PMR_EL1 read (a trapped write costs far more). out: 0 MPIDR, 1 INTIDs pending as Group 1, 2 INTIDs pending as Group 0, 3 ICC_PMR, 4 ICC_IGRPEN1, 5 ICC_SRE, 6 ICC_CTLR, 7 ICC_RPR.
extern "C" void armcpu_sgi_selfprobe (uint64_t *out)
{
  uint64_t mpidr, daif, v;
  __asm__ volatile ("mrs %0, mpidr_el1" : "=r" (mpidr));
  __asm__ volatile ("mrs %0, daif; msr daifset, #2; isb" : "=r" (daif) :: "memory");
  for (unsigned i = 0; i < 10; i++) { out[i] = 0; }
  out[0] = mpidr;
  __asm__ volatile ("mrs %0, icc_pmr_el1" : "=r" (v)); out[3] = v;
  __asm__ volatile ("mrs %0, icc_igrpen1_el1" : "=r" (v)); out[4] = v;
  __asm__ volatile ("mrs %0, icc_sre_el1" : "=r" (v)); out[5] = v;
  __asm__ volatile ("mrs %0, icc_ctlr_el1" : "=r" (v)); out[6] = v;
  __asm__ volatile ("mrs %0, icc_rpr_el1" : "=r" (v)); out[7] = v;
  if ((mpidr & 0xff) < 16) {
    const uint64_t base = ((mpidr & 0xff00000000ULL) << 16) | ((mpidr & 0xff0000) << 16) | ((mpidr & 0xff00) << 8) | (1ULL << (mpidr & 0xff));
    for (uint64_t intid = 0; intid < 16; intid++) {
      const uint64_t sgi = base | (intid << 24);
      uint64_t h = 1023, ta, tb;
      __asm__ volatile ("dsb ishst; isb; mrs %0, cntvct_el0; isb" : "=r" (ta) :: "memory");
      __asm__ volatile ("msr icc_sgi1r_el1, %0; isb; mrs %1, cntvct_el0" : "=r" (tb) : "r" (sgi) : "memory");
      if (intid == 0) { out[8] = tb - ta; }
      for (unsigned polls = 0; polls < 400; polls++) {
        __asm__ volatile ("mrs %0, icc_hppir1_el1" : "=r" (h));
        if ((h & 0xffffff) == intid) { break; }
        __asm__ volatile ("isb" ::: "memory");
      }
      if ((h & 0xffffff) == intid) {
        out[1] |= 1ULL << intid;
        __asm__ volatile ("mrs %0, icc_iar1_el1" : "=r" (v));
        __asm__ volatile ("msr icc_eoir1_el1, %0; isb" :: "r" (v) : "memory");
        continue;
      }
      __asm__ volatile ("dsb ishst; msr icc_sgi0r_el1, %0; isb" :: "r" (sgi) : "memory");
      for (unsigned polls = 0; polls < 400; polls++) {
        __asm__ volatile ("mrs %0, icc_hppir0_el1" : "=r" (h));
        if ((h & 0xffffff) == intid) { break; }
        __asm__ volatile ("isb" ::: "memory");
      }
      if ((h & 0xffffff) == intid) {
        out[2] |= 1ULL << intid;
        __asm__ volatile ("mrs %0, icc_iar0_el1" : "=r" (v));
        __asm__ volatile ("msr icc_eoir0_el1, %0; isb" :: "r" (v) : "memory");
      }
    }
  }
  {
    uint64_t ta, tb;
    __asm__ volatile ("isb; mrs %0, cntvct_el0; isb; mrs %2, icc_pmr_el1; isb; mrs %1, cntvct_el0" : "=r" (ta), "=r" (tb), "=r" (v) :: "memory");
    out[9] = tb - ta;
  }
  __asm__ volatile ("msr daif, %0; isb" :: "r" (daif) : "memory");
}

extern "C" void armcpu_ipi_selftest (uint64_t aff)
{
  uint64_t sgi = ((aff & 0xff00000000ULL) << 16) | ((aff & 0xff0000) << 16) | ((aff & 0xff00) << 8) | (1ULL << (aff & 0xff));
  armcpu_ipi_selftest_sent = armcpu_ipi_selftest_sent + 1;
  if (gSpiIpi) { spiIpiSend (gDist, static_cast<uint32_t> ((aff >> 8) & 7)); return; }
  __asm__ volatile ("dsb ishst; msr icc_sgi1r_el1, %0; isb" :: "r" (sgi) : "memory");
}

void ArmCpu::enableCPUTimeBase (bool enabled)
{
  uint64_t value = enabled ? 1 : 2;
  __asm__ volatile ("msr cntv_ctl_el0, %0\n\tisb" :: "r" (value) : "memory");
}

void ArmCpu::quiesceCPU (void)
{
  armcpu_quiesce_calls=armcpu_quiesce_calls+1;
  if(diagnosePower)panic("ArmCpu: unsupported quiesce requested before timer/GIC disable caller=%p halt=%u timer=%llu phase=%u",__builtin_return_address(0),armcpu_halt_calls,armcpu_timer_interrupts,armcpu_irq_phase);
  enableCPUTimeBase (false);
  uint64_t value = 0;
  __asm__ volatile ("msr icc_igrpen1_el1, %0\n\tdsb sy\n\tisb" :: "r" (value) : "memory");
}

void ArmCpu::haltCPU (void)
{
  armcpu_halt_calls=armcpu_halt_calls+1;
  quiesceCPU ();
  setCPUState (kIOCPUStateStopped);
  __asm__ volatile ("msr daifset, #15" ::: "memory");
  for (;;) { __asm__ volatile ("wfi"); }
}

// The new core runs with its MMU and caches off until start_cpu enables them, so it reads the reset vector, ResetHandlerData
// and the boot arguments from memory. Write those lines back to the point of coherency first: on this SoC the boot CPU's
// dirty cache lines are not guaranteed to be visible to a core with caches off (XNU only flushes the per-CPU data).
static void cleanToMemory (const void *start, size_t length)
{
  uint64_t ctr;
  __asm__ volatile ("mrs %0, ctr_el0" : "=r" (ctr));
  uintptr_t line = static_cast<uintptr_t> (4) << ((ctr >> 16) & 15);
  uintptr_t address = reinterpret_cast<uintptr_t> (start) & ~(line - 1), end = reinterpret_cast<uintptr_t> (start) + length;
  for (; address < end; address += line) { __asm__ volatile ("dc civac, %0" :: "r" (address) : "memory"); }
  __asm__ volatile ("dsb sy; isb" ::: "memory");
}

kern_return_t ArmCpu::startCPU (vm_offset_t, vm_offset_t)
{
  if (!secondary) { return KERN_NOT_SUPPORTED; }
  bool hvc = false;
  if (!psciConduit (&hvc)) { armcpu_smp_psci_rc[logicalId] = -1001; return KERN_NOT_SUPPORTED; }
  armcpu_smp_conduit = hvc ? 2 : 1;
  static volatile uint32_t asked = 0;
  if (gStats && __atomic_exchange_n (&asked, 1U, __ATOMIC_ACQ_REL) == 0) {
    // What the firmware offers for idle. These calls change no state: PSCI_VERSION, then from PSCI 1.0 on PSCI_FEATURES of the SMC64
    // CPU_SUSPEND and of PSCI_SET_SUSPEND_MODE.
    // PSCI_VERSION is a 32-bit call: its answer is the low half of x0, and a negative one is an error.
    const int32_t version = static_cast<int32_t> (psciCall (hvc, 0x84000000ULL, 0, 0, 0));
    armcpu_psci[1] = version;
    armcpu_psci[0] = 1;
    if (version >= 0x10000) {
      armcpu_psci[2] = static_cast<int32_t> (psciCall (hvc, 0x8400000aULL, 0xc4000001ULL, 0, 0));
      armcpu_psci[3] = static_cast<int32_t> (psciCall (hvc, 0x8400000aULL, 0x8400000fULL, 0, 0));
      armcpu_psci[0] = 2;
    }
  }
  // PSCI 0.2 CPU_ON (SMC64 function id); the entry point is the kernel's physical reset vector page, whose reset
  // code selects this core's cpu_data entry by MPIDR affinity. The context id is unused by that code.
  uint64_t entry = static_cast<uint64_t> (ml_static_vtop (reinterpret_cast<vm_offset_t> (LowResetVectorBase)));
  cleanToMemory (LowResetVectorBase, 0x10000);   // reset vector, start_cpu and the code up to the MMU enable
  cleanToMemory (ResetHandlerData, 0x1000);
  cleanToMemory (const_boot_args, 0x1000);
  armcpu_smp_step[logicalId] = 4;
  int64_t result = psciCall (hvc, 0xc4000003ULL, affinity & 0xff00ffffffULL, entry, 0);
  armcpu_smp_psci_rc[logicalId] = static_cast<int32_t> (result);
  return result == 0 ? KERN_SUCCESS : KERN_FAILURE;
}

const OSSymbol *ArmCpu::getCPUName (void)
{
  return cpuName;
}

// Readback after configuration: bit 0 group 1, bit 1 enabled, bit 2 edge, bits 8..15 priority; IROUTER separately.
static IOReturn spiAttachUnlocked (uint32_t intid, bool edge, void (*handler) (void *), void *context,
                                   uint32_t *readback, uint64_t *router)
{
  if (gDist == 0 || armcpu_gic_ready == 0 || handler == nullptr) { return kIOReturnNotReady; }
  const unsigned interrupts = ((read32 (gDist, 4) & 31) + 1) * 32;
  if (intid < 32 || intid >= 1020 || intid >= interrupts) { return kIOReturnBadArgument; }
  const uint32_t used = __atomic_load_n (&gSpiUsed, __ATOMIC_ACQUIRE);
  for (uint32_t slot = 0; slot < used; slot++) {
    if (gSpi[slot].id == intid) { return kIOReturnExclusiveAccess; }
  }
  if (used >= ARMCPU_SPI_SLOTS) { return kIOReturnNoResources; }
  const uint32_t word = (intid / 32) * 4, bit = 1U << (intid % 32);
  write32 (gDist, 0x180 + word, bit); // ICENABLER while it is reconfigured
  if (!waitClear (gDist, 0, 1U << 31)) { return kIOReturnTimeout; }
  write32 (gDist, 0x80 + word, read32 (gDist, 0x80 + word) | bit); // IGROUPR: group 1 (RAZ/WI if secure owns it)
  *reinterpret_cast<volatile uint8_t *> (gDist + 0x400 + intid) = 0xa0;
  const unsigned cfg = 0xc00 + (intid / 16) * 4, shift = (intid % 16) * 2 + 1;
  write32 (gDist, cfg, (read32 (gDist, cfg) & ~(1U << shift)) | ((edge ? 1U : 0U) << shift));
  *reinterpret_cast<volatile uint64_t *> (gDist + 0x6000 + 8 * intid) = gBootAffinity;
  gSpi[used] = {intid, handler, context, 0};
  __atomic_store_n (&gSpiUsed, used + 1, __ATOMIC_RELEASE);
  __atomic_fetch_or (&gSpiRoutes[intid], 1U << used, __ATOMIC_RELEASE);
  __asm__ volatile ("dsb sy" ::: "memory");
  write32 (gDist, 0x100 + word, bit); // ISENABLER
  if (readback != nullptr) {
    *readback = ((read32 (gDist, 0x80 + word) & bit) ? 1U : 0U) | ((read32 (gDist, 0x100 + word) & bit) ? 2U : 0U) |
                (((read32 (gDist, cfg) >> shift) & 1U) << 2) |
                (static_cast<uint32_t> (*reinterpret_cast<volatile uint8_t *> (gDist + 0x400 + intid)) << 8);
  }
  if (router != nullptr) { *router = *reinterpret_cast<volatile uint64_t *> (gDist + 0x6000 + 8 * intid); }
  return kIOReturnSuccess;
}

extern "C" IOReturn armcpu_spi_attach (uint32_t intid, bool edge, void (*handler) (void *), void *context,
                                       uint32_t *readback, uint64_t *router)
{
  IOLock *lock = spiRegistrationLock ();
  if (lock == nullptr) { return kIOReturnNoResources; }
  IOLockLock (lock);
  IOReturn result = spiAttachUnlocked (intid, edge, handler, context, readback, router);
  IOLockUnlock (lock); return result;
}

// Level summary sources acknowledge their own pin/peripheral status. Keep all
// handlers until platform shutdown, so IRQ dispatch needs no locks or allocation.
extern "C" IOReturn armcpu_spi_share (uint32_t intid, void (*handler) (void *), void *context)
{
  if (gDist == 0 || !armcpu_gic_ready || handler == nullptr || intid < 32 || intid >= 1020) { return kIOReturnBadArgument; }
  IOLock *lock = spiRegistrationLock ();
  if (lock == nullptr) { return kIOReturnNoResources; }
  IOLockLock (lock);
  const uint32_t used = __atomic_load_n (&gSpiUsed, __ATOMIC_ACQUIRE);
  bool found = false; IOReturn result = kIOReturnSuccess;
  for (uint32_t slot = 0; slot < used; ++slot) {
    if (gSpi[slot].id != intid) { continue; }
    found = true;
    if (gSpi[slot].handler == handler && gSpi[slot].context == context) { IOLockUnlock (lock); return kIOReturnSuccess; }
  }
  if (!found) { result = spiAttachUnlocked (intid, false, handler, context, nullptr, nullptr); }
  else if (read32 (gDist, 0xc00 + (intid / 16) * 4) & (2U << ((intid % 16) * 2))) { result = kIOReturnBadArgument; }
  else if (used >= ARMCPU_SPI_SLOTS) { result = kIOReturnNoResources; }
  else {
    gSpi[used] = {intid, handler, context, 0};
    __atomic_store_n (&gSpiUsed, used + 1, __ATOMIC_RELEASE);
    __atomic_fetch_or (&gSpiRoutes[intid], 1U << used, __ATOMIC_RELEASE);
  }
  IOLockUnlock (lock); return result;
}

// Usable from primary IRQ context; no lock, allocation or serial-engine access.
extern "C" IOReturn armcpu_spi_mask (uint32_t intid, bool masked)
{
  if (!gDist || !armcpu_gic_ready || intid < 32 || intid >= 1020) { return kIOReturnNotReady; }
  if (!__atomic_load_n (&gSpiRoutes[intid], __ATOMIC_ACQUIRE)) { return kIOReturnNotFound; }
  write32 (gDist, (masked ? 0x180U : 0x100U) + (intid / 32) * 4, 1U << (intid % 32));
  __asm__ volatile ("dsb sy" ::: "memory");
  return kIOReturnSuccess;
}

extern "C" uint64_t armcpu_spi_count (uint32_t intid)
{
  const uint32_t used = __atomic_load_n (&gSpiUsed, __ATOMIC_ACQUIRE);
  for (uint32_t slot = 0; slot < used; slot++) {
    if (gSpi[slot].id == intid) { return gSpi[slot].count; }
  }
  return 0;
}

// One text row of the statistics for a board log. Row 0, always there: the IPI totals, whether the scheduler got
// ml_cpu_init_completed, the idle latency the cores are registered with, and the firmware's PSCI answers. With diag=1 one more row
// per registered core follows, each list in the order of the ranges of ArmCpuCoreStats:
//   idle n= periods, ms= time in them | woken by timer, IPI, device interrupt, none
//   ipi out= sent, def= asked deferred, can= cancel requests, ns= mean and longest send
//   in awake / in idle: IPIs by the delay from the send to the handler, us= the longest
// and after those one row per registered core with its timer interrupts by the delay after their deadline, us= the longest.
// Returns the row's length, 0 past the last row.
extern "C" unsigned armcpu_stats_row (unsigned index, char *row, unsigned size)
{
  if (row == nullptr || size == 0) { return 0; }
  const uint32_t seen = __atomic_load_n (&gCoreSeen, __ATOMIC_ACQUIRE);
  if (index == 0) {
    // Sums of the per-core counters, each written by its own core only. armcpu_ipi_sent and armcpu_ipi_interrupts are shared by
    // all cores without a lock and lose counts.
    uint64_t sent = 0, taken = 0, deferred = 0, cancelled = 0;
    for (unsigned c = 0; c < ARMCPU_SMP_SLOTS; c++) {
      sent += gCore[c].sent; taken += armcpu_core_sgi[c]; deferred += gCore[c].sentDeferred; cancelled += gCore[c].cancelled;
    }
    snprintf (row, size, "[cpu wake] ipi sent=%llu taken=%llu deferred=%llu cancel=%llu | sched init completed=%u | idle latency us=%u | psci ",
              static_cast<unsigned long long> (sent), static_cast<unsigned long long> (taken),
              static_cast<unsigned long long> (deferred), static_cast<unsigned long long> (cancelled), armcpu_cpu_init_completed,
              gIdleLatencyNs / 1000U);
    const unsigned n = static_cast<unsigned> (strnlen (row, size));
    if (n + 1 < size) {
      if (armcpu_psci[0] == 0) {
        snprintf (row + n, size - n, "not asked");
      } else if (armcpu_psci[0] == 1) {
        snprintf (row + n, size - n, "version=%x", static_cast<uint32_t> (armcpu_psci[1]));
      } else {
        snprintf (row + n, size - n, "version=%x suspend=%d mode=%d", static_cast<uint32_t> (armcpu_psci[1]), armcpu_psci[2], armcpu_psci[3]);
      }
    }
    return static_cast<unsigned> (strnlen (row, size));
  }
  if (!gStats) { return 0; }
  const unsigned cores = static_cast<unsigned> (__builtin_popcount (seen));
  const bool timerRow = index > cores;
  if (timerRow) { index -= cores; }
  unsigned cpu = ARMCPU_SMP_SLOTS;
  for (unsigned c = 0, at = 0; c < ARMCPU_SMP_SLOTS; c++) {
    if ((seen & (1U << c)) != 0 && ++at == index) { cpu = c; break; }
  }
  if (cpu == ARMCPU_SMP_SLOTS) { return 0; }
  const ArmCpuCoreStats &core = gCore[cpu];
  const uint64_t hz = gCounterHz;
  if (timerRow) {
    snprintf (row, size, "[cpu%u timer] late awake %u/%u/%u/%u us=%llu | late in idle %u/%u/%u/%u us=%llu", cpu,
              core.timerLate[0][0], core.timerLate[0][1], core.timerLate[0][2], core.timerLate[0][3],
              static_cast<unsigned long long> (core.timerLateMost[0] * 1000000ULL / hz),
              core.timerLate[1][0], core.timerLate[1][1], core.timerLate[1][2], core.timerLate[1][3],
              static_cast<unsigned long long> (core.timerLateMost[1] * 1000000ULL / hz));
    return static_cast<unsigned> (strnlen (row, size));
  }
  const uint32_t sent = core.sent, timed = core.sendsTimed;
  snprintf (row, size, "[cpu%u] idle n=%u/%u/%u/%u ms=%llu/%llu/%llu/%llu woke t=%u i=%u d=%u n=%u | irq t=%u i=%u d=%u | ipi out=%u def=%u can=%u "
            "ns=%llu/%llu | in awake %u/%u/%u/%u us=%llu | in idle %u/%u/%u/%u us=%llu",
            cpu, core.idlePeriods[0], core.idlePeriods[1], core.idlePeriods[2], core.idlePeriods[3],
            static_cast<unsigned long long> (core.idleTicks[0] * 1000 / hz), static_cast<unsigned long long> (core.idleTicks[1] * 1000 / hz),
            static_cast<unsigned long long> (core.idleTicks[2] * 1000 / hz), static_cast<unsigned long long> (core.idleTicks[3] * 1000 / hz),
            core.wokenBy[0], core.wokenBy[1], core.wokenBy[2], core.wokenBy[3],
            armcpu_core_timer[cpu], armcpu_core_sgi[cpu], core.deviceInterrupts, sent, core.sentDeferred, core.cancelled,
            static_cast<unsigned long long> (timed != 0 ? core.sendTicks * 1000ULL / timed * 1000000ULL / hz : 0),
            static_cast<unsigned long long> (core.sendMost * 1000000000ULL / hz),
            core.taken[0][0], core.taken[0][1], core.taken[0][2], core.taken[0][3],
            static_cast<unsigned long long> (core.takenMost[0] * 1000000ULL / hz),
            core.taken[1][0], core.taken[1][1], core.taken[1][2], core.taken[1][3],
            static_cast<unsigned long long> (core.takenMost[1] * 1000000ULL / hz));
  return static_cast<unsigned> (strnlen (row, size));
}
