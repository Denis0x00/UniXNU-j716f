// SPDX-License-Identifier: BSD-2-Clause-Patent
// Board-owned, bounded live-memory observation ABI. No persistence guarantee.
#ifndef J716F_ARMCPU_STATE_H
#define J716F_ARMCPU_STATE_H
#include <stdint.h>

static constexpr unsigned J716F_TRACE_WORDS = 192;
static constexpr uint64_t J716F_TRACE_MAGIC = 0x315152494636314aULL;
// Header 0..47, followed by nine 16-word snapshots: IRQ 1/2/8 x enter/return/EOI.
// All words are uint64 little endian. A dump is coherent only at an even sequence.
static inline void j716fTraceBegin(volatile uint64_t *t)
{
  t[3] = t[3] + 1;
  __atomic_thread_fence(__ATOMIC_RELEASE);
}
static inline void j716fTraceEnd(volatile uint64_t *t)
{
  __atomic_thread_fence(__ATOMIC_RELEASE);
  t[3] = t[3] + 1;
}
static inline void j716fTraceSetup(volatile uint64_t *t, unsigned phase,
                                 uint64_t detail, const uint64_t *r,
                                 const volatile uint32_t *gic,
                                 uint64_t quiesce, uint64_t halt)
{
  j716fTraceBegin(t);
  t[4] = t[4] | (1ULL << phase);
  t[5] = phase;
  if (phase == 3) { t[13] = detail; }
  t[14] = r[12];
  for (unsigned i = 0; i < 8; ++i) { t[16+i] = gic[i]; }
  for (unsigned i = 0; i < 5; ++i) { t[24+i] = r[4+i]; }
  t[29] = detail; t[30] = r[14]; t[31] = r[13];
  t[32] = r[10]; t[33] = r[11]; t[35] = quiesce; t[36] = halt;
  j716fTraceEnd(t);
}

// Called only from the boot CPU's existing INTID 27 dispatch branch.
// phase 1: before original rtclock_intr; 2: after its return; 3: after original EOI/ISB.
// r[4..14]: CNTFRQ,CNTVCT,CVAL,CTL,DAIF,raw IAR,registered,GIC ready,MPIDR,
// global timer count,upstream IRQ phase. r[0..3] and r[15] are filled here.
static inline bool j716fTraceEvent(volatile uint64_t *t, unsigned phase,
                                 unsigned logical, uint64_t *r)
{
  if (t[8] >= 8) { return false; } // Bound instrumentation, preserve first eight.
  j716fTraceBegin(t);
  if (logical != 0 || (r[9] & 0xffffff) != 27) { t[12] = t[12] | 2; }
  if (!r[10] || !r[11]) { t[12] = t[12] | 8; }
  if (phase == 1) {
    if (t[6] != t[7] || t[7] != t[8] || (t[34] != 0 && t[34] != 3)) {
      t[12] = t[12] | 1;
    }
    t[6] = t[6] + 1;
    if (t[6] == 1) { t[9] = r[5]; }
    else if (r[5] <= t[10]) { t[12] = t[12] | 4; }
    t[10] = r[5];
  } else if (phase == 2) {
    if (t[34] != 1 || t[6] != t[7]+1 || t[7] != t[8]) { t[12] = t[12] | 1; }
    t[7] = t[7] + 1;
  } else if (phase == 3) {
    if (t[34] != 2 || t[6] != t[7] || t[7] != t[8]+1) { t[12] = t[12] | 1; }
    t[8] = t[8] + 1;
  } else { t[12] = t[12] | 1; }
  t[34] = phase;
  unsigned sample = t[6] == 1 ? 0 : t[6] == 2 ? 1 : t[6] == 8 ? 2 : 3;
  if (sample < 3 && phase >= 1 && phase <= 3) {
    unsigned slot = sample*3 + phase-1;
    r[0] = phase; r[1] = t[6]; r[2] = t[7]; r[3] = t[8]; r[15] = logical;
    for (unsigned i = 0; i < 16; ++i) { t[48+slot*16+i] = r[i]; }
    t[15] = t[15] | (1ULL << slot);
  }
  bool ready = phase == 3 && t[8] == 8 && t[6] == 8 && t[7] == 8 &&
               t[12] == 0 && t[10] > t[9] && (t[4] & 0x7e) == 0x7e;
  if (ready) { t[11] = 1; }
  j716fTraceEnd(t);
  return ready;
}
#endif
