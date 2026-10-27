// SPDX-License-Identifier: BSD-2-Clause-Patent
// Private cold-boot vectors. Do NOT change the vectors XNU copies for reset/SMP.
.text
.align 11
.globl EXT(j716f_native_early_vectors)
LEXT(j716f_native_early_vectors)
.rept 16
  b Lj716f_native_fault
  .space 124
.endr
Lj716f_native_fault:
  msr daifset, #15
  adrp x9, EXT(j716f_native_fault_latch)@page
  add x9, x9, EXT(j716f_native_fault_latch)@pageoff
  ldr w10, [x9]
  cbnz w10, Lj716f_native_halt
  mov w10, #1
  str w10, [x9]
  dsb sy
  mrs x0, esr_el1
  mrs x1, far_el1
  mrs x2, elr_el1
  mrs x3, spsr_el1
  adrp x9, EXT(j716f_native_fault_stack)@page
  add x9, x9, EXT(j716f_native_fault_stack)@pageoff
  add x9, x9, #4096
  msr spsel, #0
  mov sp, x9
  bl EXT(j716f_native_early_fault)
Lj716f_native_halt:
  wfe
  b Lj716f_native_halt
.data
.align 3
LEXT(j716f_native_fault_latch)
  .quad 0
.align 12
LEXT(j716f_native_fault_stack)
  .space 4096
.text
