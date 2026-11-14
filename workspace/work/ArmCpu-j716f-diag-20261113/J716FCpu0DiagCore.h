// SPDX-License-Identifier: BSD-2-Clause-Patent
// Pure bounded diagnostics: shared by the real ArmCpu caller and host tests.
#ifndef J716F_CPU0_DIAG_CORE_H
#define J716F_CPU0_DIAG_CORE_H
#include <stdint.h>

namespace j716fdiag {
enum : uint32_t { Disabled=0, CounterOnly=1, GicOnly=2, FirstIrq=3 };
enum : uint32_t { Empty=0, CounterPassed=1, GicPassed=2, IrqPassed=3, Failed=0xffffffffU };
enum Error : uint32_t {
  Ok=0, ModeInvalid=1, CpuScope=2, CounterContext=3, CounterFrequency=4,
  CounterStopped=5, CounterDisagreement=6, MappingBounds=7, GicVersion=8,
  GicAffinity=9, GicSleeping=10, GicBusy=11, GicGroup=12, GicTrigger=13,
  GicInterface=14, IrqOrder=15, IrqIdentity=16, IrqCount=17,
  WaitExpired=18, ClockReversed=19, IterationLimit=20, ResetReturned=21,
  ThreadCreation=22, IncompatibleArgs=23, GicPreparation=24, TimerNotExpired=25,
  GicPriority=26
};
struct Counter {
  uint64_t el, mpidr, midr, frequency, daif;
  uint64_t p0a, v0, p0b, p1a, v1, p1b;
};
struct Gic {
  uint64_t mpidr, typer, daif;
  uint32_t pidr2, dctlr, dtyper, rctlr, waker, group, modifier, enable, config;
  uint64_t sre, pmr, groupEnable, cpuControl;
  uint32_t priority;
};
struct Irq {
  uint64_t countBefore, countAfter, virtualCount, timerValue, timerControl;
  uint32_t cpu, id, returned, eoi;
};
struct Record {
  uint32_t version, mode, stage, error;
  Counter counter;
  Gic gic;
  Irq irq;
};
// One diagnostic attempt per boot, shared by the real start caller and tests.
// Reject retries without clearing a previously published IRQ latch or record.
static inline bool claimAttempt(uint32_t &latch) {
  uint32_t expected=0;
  return __atomic_compare_exchange_n(&latch,&expected,1,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE);
}
static inline uint64_t affinity(uint64_t mpidr) { return mpidr & 0xff00ffffffULL; }
static inline uint32_t typerAffinity(uint64_t mpidr) {
  return uint32_t((mpidr & 0xffffffULL) | ((mpidr >> 8) & 0xff000000ULL));
}
static inline Error counterCheck(const Counter &c) {
  if (c.el != 4 || affinity(c.mpidr) != 0 || !(c.daif & 0x80)) return CounterContext;
  if (c.frequency != 19200000ULL) return CounterFrequency;
  if (c.p0a > c.p0b || c.p0b >= c.p1a || c.p1a > c.p1b || c.v0 >= c.v1) return CounterStopped;
  // Bracket each virtual read with physical reads. A constant virtual offset
  // gives an exact interval; no arbitrary tolerance or hardcoded CPU speed.
  const uint64_t dv=c.v1-c.v0;
  if (dv < c.p1a-c.p0b || dv > c.p1b-c.p0a) return CounterDisagreement;
  return Ok;
}
static inline bool mappingBounds(uint64_t dist, uint64_t distBytes,
                                  uint64_t frames, uint64_t frameBytes, uint64_t selected) {
  if (!dist || (dist & 3) || distBytes < 0x10000 || dist > UINT64_MAX-distBytes ||
      !frames || (frames & 7) || frameBytes < 0x20000 || frames > UINT64_MAX-frameBytes ||
      selected < frames || (selected & 7)) return false;
  return selected-frames <= frameBytes-0x20000;
}
static inline Error gicCheck(const Gic &g) {
  const uint32_t ppi=1U<<27;
  if (!(g.daif & 0x80)) return CounterContext;
  if (((g.pidr2>>4)&15)!=3) return GicVersion;
  if (affinity(g.mpidr)!=0 || uint32_t(g.typer>>32)!=typerAffinity(g.mpidr)) return GicAffinity;
  if (g.waker & 6) return GicSleeping;
  if ((g.dctlr & (1U<<31)) || (g.rctlr & 8)) return GicBusy;
  if (!(g.group & ppi) || (g.modifier & ppi) || !(g.enable & ppi)) return GicGroup;
  if (g.config & (1U<<23)) return GicTrigger;
  if (g.priority!=0x80) return GicPriority;
  if ((g.dctlr & 0x12)!=0x12 || !(g.sre & 1) || !(g.groupEnable & 1) ||
      (g.pmr & 255)<=g.priority || (g.cpuControl & 2)) return GicInterface;
  return Ok;
}
static inline Error irqCheck(const Irq &i) {
  if (i.cpu!=0 || i.id!=27) return IrqIdentity;
  if (i.countBefore!=0 || i.countAfter!=1) return IrqCount;
  if (i.returned!=1 || i.eoi!=1) return IrqOrder;
  if ((i.timerControl & 7)!=5 || int32_t(uint32_t(i.timerValue))>0) return TimerNotExpired;
  return Ok;
}
static inline bool mayReset(const Record &r) {
  if (r.version!=1 || r.error!=Ok || counterCheck(r.counter)!=Ok) return false;
  if (r.mode==CounterOnly) return r.stage==CounterPassed;
  if (gicCheck(r.gic)!=Ok) return false;
  if (r.mode==GicOnly) return r.stage==GicPassed;
  return r.mode==FirstIrq && r.stage==IrqPassed && irqCheck(r.irq)==Ok;
}
static inline Error fail(Record &r, Error error) { r.stage=Failed; r.error=error; return error; }
// A physical-counter deadline AND an iteration cap: a stopped counter cannot
// cause an unbounded wait. No timer interrupt, sleep call or event is required.
template<class Clock> Error waitTicks(Clock &clock, uint64_t ticks, uint32_t iterations) {
  const uint64_t first=clock.physical();
  uint64_t previous=first;
  for (uint32_t n=0; n<iterations; ++n) {
    const uint64_t now=clock.physical();
    if (now<previous) return ClockReversed;
    if (now-first>=ticks) return Ok;
    previous=now;
    clock.relax();
  }
  return IterationLimit;
}
template<class Clock, class Ready> Error waitIrq(Clock &clock, Ready &ready, uint64_t ticks, uint32_t iterations) {
  const uint64_t first=clock.physical();
  uint64_t previous=first;
  for (uint32_t n=0; n<iterations; ++n) {
    if (ready.complete()) return Ok;
    const uint64_t now=clock.physical();
    if (now<previous) return ClockReversed;
    if (now-first>=ticks) return WaitExpired;
    previous=now;
    clock.relax();
  }
  return IterationLimit;
}
} // namespace j716fdiag
#endif
