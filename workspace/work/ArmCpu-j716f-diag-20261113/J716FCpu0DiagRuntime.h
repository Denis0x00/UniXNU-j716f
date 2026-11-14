// SPDX-License-Identifier: BSD-2-Clause-Patent
// Included after ArmCpu's existing XNU entry declarations, in one TU only.
#ifndef J716F_CPU0_DIAG_RUNTIME_H
#define J716F_CPU0_DIAG_RUNTIME_H
#include <IOKit/IOLib.h>
#include "J716FCpu0DiagCore.h"

namespace j716fdiag {
// These are transient owned kernel data, not persistent/exported logs.
static Record record={};
static uint32_t irqClaim=0, irqReady=0, irqArmed=0, attempted=0;
static bool active=false;
struct HardwareClock {
  uint64_t physical() {
    uint64_t v; __asm__ volatile("isb\n\tmrs %0, cntpct_el0" : "=r"(v)); return v;
  }
  void relax() { __asm__ volatile("yield" ::: "memory"); }
};
static inline uint64_t maskIrq() {
  uint64_t value;
  __asm__ volatile("mrs %0, daif\n\tmsr daifset, #3\n\tisb" : "=r"(value) :: "memory");
  return value;
}
static inline void restoreDaif(uint64_t value) { __asm__ volatile("msr daif, %0\n\tisb" :: "r"(value) : "memory"); }
static inline uint64_t virtualCounter() {
  uint64_t value; __asm__ volatile("isb\n\tmrs %0, cntvct_el0" : "=r"(value)); return value;
}
static inline void maskTimer() { uint64_t v=2; __asm__ volatile("msr cntv_ctl_el0, %0\n\tisb" :: "r"(v) : "memory"); }
static void note(const char *where) {
  // No promise that this reaches a screen/serial sink; guarded reset below is
  // the independent observable result. Never use UEFI Boot Services here.
  IOLog("J716F CPU0 diag v1 mode=%u stage=%u error=%u at=%s\n",
        record.mode,record.stage,record.error,where);
}
static bool refused(Error error, const char *where) {
  fail(record,error); note(where); return false;
}
static bool requestReset() {
  if (!mayReset(record)) return refused(IrqOrder,"reset-guard");
  // A diagnostic stop, not an OS reboot API. Quiesce the virtual source only
  // after the selected acceptance conditions have all passed.
  const uint64_t daif=maskIrq();
  maskTimer();
  HardwareClock clock;
  const Error wait=waitTicks(clock,19200000ULL*6,500000000U);
  if (wait!=Ok) { restoreDaif(daif); return refused(wait,"reset-delay"); }
  // Only this site issues SYSTEM_RESET. If firmware returns, restore the CPU
  // interrupt mask, retain the source masked, and report failure; never claim
  // reset or allow an infinite wait to stand in for a result.
  register uint64_t x0 __asm__("x0")=0x84000009ULL;
  register uint64_t x1 __asm__("x1")=0;
  register uint64_t x2 __asm__("x2")=0;
  register uint64_t x3 __asm__("x3")=0;
  __asm__ volatile("smc #0" : "+r"(x0),"+r"(x1),"+r"(x2),"+r"(x3) ::
                   "x4","x5","x6","x7","x8","x9","x10","x11","x12","x13","x14","x15","x16","x17","cc","memory");
  restoreDaif(daif);
  return refused(ResetReturned,"SYSTEM_RESET-returned");
}
static bool begin(uint32_t cpu, bool secondary, bool skipGic) {
  unsigned mode=0;
  PE_parse_boot_argn("j716f-cpu0-diag",&mode,sizeof(mode));
  if (!mode) return true;
  if (!claimAttempt(attempted)) {
    IOLog("J716F CPU0 diag duplicate start refused; prior record is not reused\n");
    return false;
  }
  record.version=1; record.mode=mode; record.stage=Empty; record.error=Ok;
  if (mode>FirstIrq) return refused(ModeInvalid,"mode");
  if (secondary || cpu!=0 || ml_get_max_cpu_number()!=0) return refused(CpuScope,"cpu-scope");
  const char *incompatible[]={"smp","smp-gic","armcpu-self-ipi","armcpu-spi-ipi","armcpu-edge","armcpu-skip-gic"};
  for (unsigned i=0;i<sizeof(incompatible)/sizeof(incompatible[0]);++i) {
    unsigned value=0;
    if (PE_parse_boot_argn(incompatible[i],&value,sizeof(value)) && value) return refused(IncompatibleArgs,incompatible[i]);
  }
  if (skipGic) return refused(IncompatibleArgs,"skip-gic");
  const uint64_t daif=maskIrq();
  Counter &c=record.counter;
  __asm__ volatile("mrs %0, CurrentEL" : "=r"(c.el));
  __asm__ volatile("mrs %0, mpidr_el1" : "=r"(c.mpidr));
  __asm__ volatile("mrs %0, midr_el1" : "=r"(c.midr));
  __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(c.frequency));
  __asm__ volatile("mrs %0, daif" : "=r"(c.daif));
  HardwareClock clock;
  c.p0a=clock.physical(); c.v0=virtualCounter(); c.p0b=clock.physical();
  for (unsigned n=0;n<16384;++n) __asm__ volatile("nop" ::: "memory");
  c.p1a=clock.physical(); c.v1=virtualCounter(); c.p1b=clock.physical();
  const Error result=counterCheck(c);
  if (result!=Ok) { restoreDaif(daif); return refused(result,"counter"); }
  record.stage=CounterPassed;
  active=true;
  // fiq_context_init already enabled CNTV_CTL in this kernel. Keep it masked
  // while the real ArmCpu prepares GIC and installs its normal handler.
  maskTimer();
  restoreDaif(daif);
  note("counter-passed");
  return mode==CounterOnly ? requestReset() : true;
}
static bool failed() { return record.mode!=Disabled && record.stage==Failed; }
static bool gicPrepared(bool success) {
  return success || !active ? success : refused(GicPreparation,"prepareGic");
}
static bool pollLimitReached(unsigned polls) {
  if (!active || polls<1000000U) return false;
  refused(IterationLimit,"GIC-poll-limit");
  return true;
}
static bool checkGic(uint32_t cpu, uint64_t dist, uint64_t distBytes,
                    uint64_t frames, uint64_t frameBytes, uint64_t selected) {
  if (!active) return true;
  if (cpu!=0 || record.stage!=CounterPassed) return refused(CpuScope,"gic-context");
  if (!mappingBounds(dist,distBytes,frames,frameBytes,selected)) return refused(MappingBounds,"gic-bounds");
  const uint64_t daif=maskIrq();
  Gic &g=record.gic;
  __asm__ volatile("mrs %0, mpidr_el1" : "=r"(g.mpidr));
  __asm__ volatile("mrs %0, daif" : "=r"(g.daif));
  g.typer=*reinterpret_cast<volatile uint64_t *>(selected+8);
  g.pidr2=*reinterpret_cast<volatile uint32_t *>(dist+0xffe8);
  g.dctlr=*reinterpret_cast<volatile uint32_t *>(dist);
  g.dtyper=*reinterpret_cast<volatile uint32_t *>(dist+4);
  g.rctlr=*reinterpret_cast<volatile uint32_t *>(selected);
  g.waker=*reinterpret_cast<volatile uint32_t *>(selected+0x14);
  const uint64_t sgi=selected+0x10000;
  g.group=*reinterpret_cast<volatile uint32_t *>(sgi+0x80);
  g.modifier=*reinterpret_cast<volatile uint32_t *>(sgi+0xd00);
  g.enable=*reinterpret_cast<volatile uint32_t *>(sgi+0x100);
  g.config=*reinterpret_cast<volatile uint32_t *>(sgi+0xc04);
  g.priority=(*reinterpret_cast<volatile uint32_t *>(sgi+0x418)>>24)&255;
  __asm__ volatile("mrs %0, icc_sre_el1" : "=r"(g.sre));
  __asm__ volatile("mrs %0, icc_pmr_el1" : "=r"(g.pmr));
  __asm__ volatile("mrs %0, icc_igrpen1_el1" : "=r"(g.groupEnable));
  __asm__ volatile("mrs %0, icc_ctlr_el1" : "=r"(g.cpuControl));
  const Error result=gicCheck(g);
  restoreDaif(daif);
  if (result!=Ok) return refused(result,"gic-readback");
  record.stage=GicPassed;
  if (record.mode==FirstIrq) __atomic_store_n(&irqArmed,1,__ATOMIC_RELEASE);
  note("gic-passed");
  return record.mode==GicOnly ? requestReset() : true;
}
static void irqBegin(uint32_t cpu, uint32_t id, uint64_t countBefore) {
  if (!__atomic_load_n(&irqArmed,__ATOMIC_ACQUIRE) || cpu!=0 || id!=27) return;
  uint32_t expected=0;
  if (!__atomic_compare_exchange_n(&irqClaim,&expected,1,false,__ATOMIC_ACQ_REL,__ATOMIC_RELAXED)) return;
  Irq &i=record.irq;
  i.cpu=cpu; i.id=id; i.countBefore=countBefore; i.virtualCount=virtualCounter();
  __asm__ volatile("mrs %0, cntv_tval_el0" : "=r"(i.timerValue));
  __asm__ volatile("mrs %0, cntv_ctl_el0" : "=r"(i.timerControl));
}
static void irqReturned(uint64_t countAfter) {
  if (__atomic_load_n(&irqClaim,__ATOMIC_RELAXED)!=1 || __atomic_load_n(&irqReady,__ATOMIC_RELAXED)) return;
  record.irq.countAfter=countAfter; record.irq.returned=1;
}
static void irqEoi(uint32_t cpu, uint32_t id) {
  if (cpu!=0 || id!=27 || __atomic_load_n(&irqClaim,__ATOMIC_RELAXED)!=1 ||
      __atomic_load_n(&irqReady,__ATOMIC_RELAXED)) return;
  record.irq.eoi=1;
  __atomic_store_n(&irqReady,1,__ATOMIC_RELEASE);
}
struct Ready { bool complete() { return __atomic_load_n(&irqReady,__ATOMIC_ACQUIRE)!=0; } };
static void worker(void *, wait_result_t) {
  HardwareClock clock; Ready ready;
  uint64_t mpidr, daif;
  __asm__ volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
  __asm__ volatile("mrs %0, daif" : "=r"(daif));
  // Normal thread context; no scheduler timer, sleep or IRQ is used to measure
  // the timeout. A separate iteration cap bounds a frozen counter.
  Error result=affinity(mpidr)!=0 || (!ready.complete() && (daif & 0x80)) ? CounterContext :
      waitIrq(clock,ready,19200000ULL*2,100000000U);
  if (result==Ok) result=irqCheck(record.irq);
  if (result!=Ok) refused(result,"first-irq-wait");
  else { record.stage=IrqPassed; note("first-irq-passed"); requestReset(); }
  thread_terminate(current_thread());
}
static bool startWorker() {
  if (!active || record.mode!=FirstIrq) return !failed();
  if (record.stage!=GicPassed) return refused(IrqOrder,"worker-before-gic");
  thread_t thread=nullptr;
  if (kernel_thread_start(&worker,nullptr,&thread)!=KERN_SUCCESS) return refused(ThreadCreation,"worker-create");
  thread_deallocate(thread);
  return true;
}
} // namespace j716fdiag
#endif
