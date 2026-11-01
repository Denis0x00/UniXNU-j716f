// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef J716F_NATIVE_HOOKS_H
#define J716F_NATIVE_HOOKS_H
#ifdef __cplusplus
extern "C" {
#endif
int j716f_native_begin(unsigned long long boot_args);
int j716f_native_bootstrap(unsigned long long root, unsigned long long delta);
void j716f_native_event(unsigned int event, unsigned long long a,
    unsigned long long b, unsigned long long c);
void j716f_native_irq(unsigned int phase, unsigned int logical,
    const volatile unsigned long long *trace, const unsigned long long *regs);
void j716f_native_early_fault(unsigned long long esr, unsigned long long far,
    unsigned long long elr, unsigned long long spsr);
void j716f_native_sched(unsigned int phase, unsigned long long from, unsigned long long to);
#ifdef __cplusplus
}
#endif
#endif
