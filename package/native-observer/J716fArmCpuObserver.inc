// SPDX-License-Identifier: BSD-2-Clause-Patent
// Inspired by reAAAq/UniXNU-umi 3eb7257 UmiArmCpuTrace.inc and its dispatch
// observation points. This implementation has its own ABI and no phone logger.
#include "J716fArmCpuState.h"
extern "C" {
volatile uint64_t j716f_armcpu_trace[J716F_TRACE_WORDS] = {
  J716F_TRACE_MAGIC, 1, J716F_TRACE_WORDS
};
// Debugger may break here; invoking it manually does not qualify an interrupt.
__attribute__((noinline)) void j716f_armcpu_timer_checkpoint(void)
{
  __asm__ volatile ("" ::: "memory");
}
}
static void j716fArmCpuRead(uint64_t *r, uint64_t daif, uint64_t id)
{
  __asm__ volatile ("mrs %0, cntfrq_el0" : "=r"(r[4]));
  __asm__ volatile ("isb; mrs %0, cntvct_el0" : "=r"(r[5]) :: "memory");
  __asm__ volatile ("mrs %0, cntv_cval_el0" : "=r"(r[6]));
  __asm__ volatile ("mrs %0, cntv_ctl_el0" : "=r"(r[7]));
  r[8] = daif; r[9] = id; r[10] = armcpu_registered; r[11] = armcpu_gic_ready;
  __asm__ volatile ("mrs %0, mpidr_el1" : "=r"(r[12]));
  r[13] = armcpu_timer_interrupts; r[14] = armcpu_irq_phase;
}
static void j716fArmCpuSetup(unsigned phase, uint64_t detail)
{
  uint64_t daif, r[16] = {};
  __asm__ volatile ("mrs %0, daif; msr daifset, #3; isb" : "=r"(daif) :: "memory");
  j716fArmCpuRead(r, daif, armcpu_irq_id);
  j716fTraceSetup(j716f_armcpu_trace, phase, detail, r, armcpu_gic_boot,
                  armcpu_quiesce_calls, armcpu_halt_calls);
  __asm__ volatile ("msr daif, %0; isb" :: "r"(daif) : "memory");
}
static void j716fArmCpuEvent(unsigned phase, unsigned logical, uint64_t id)
{
  if (logical != 0 || j716f_armcpu_trace[8] >= 8) { return; }
  uint64_t daif, r[16] = {};
  __asm__ volatile ("mrs %0, daif; msr daifset, #3; isb" : "=r"(daif) :: "memory");
  j716fArmCpuRead(r, daif, id);
  bool ready = j716fTraceEvent(j716f_armcpu_trace, phase, logical, r);
  if (ready) { j716f_armcpu_timer_checkpoint(); }
  __asm__ volatile ("msr daif, %0; isb" :: "r"(daif) : "memory");
}
