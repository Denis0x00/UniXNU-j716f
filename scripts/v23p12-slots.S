/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * P12 independent pre-staged commit witnesses; M=0 and no normal kernel continuation.
 * Entry ABI and the CRC writer match P9. No console mapping, MMU enable,
 * counter, graphics, or original-code continuation after prefix_stop.
 */
.text
.global stage1, entry_resume, prefix_stop, reset_call, halt, trace_append
.equ CONSOLE, 0xb00be000
.equ CAPACITY, 0x3fff4
.equ ARM_COOKIE, 0x5031324b45524e4c

stage1:
  mov x19, sp
  mov x20, x0
  mrs x21, nzcv
  adrp x17, original_stack
  add x17, x17, :lo12:original_stack
  mov sp, x17
  /* Admission: linear unused tail for ENTRY, six slots and reset-return.
   * No wrapping, allocation, new reservation or header repair. */
  ldr x3, =CONSOLE
  ldp w5, w6, [x3, #4]
  cmp w5, w6
  b.ne halt
  ldr w4, =(CAPACITY - 512)
  cmp w5, w4
  b.hi halt
  mov w0, #10
  mrs x1, sctlr_el1
  tbnz x1, #0, halt
  lsr x2, x1, #32
  cbnz x2, halt
  mov w2, #9
  bl trace_append
  cbz w0, halt
  mov x0, x20
  msr nzcv, x21
  mov sp, x19
entry_resume:
  b original_first

.macro CRC offset
  mov w11, #-1
  mov w7, #0
  ldr w10, =0xedb88320
crc_bytes\@:
  add x8, sp, #\offset
  ldrb w8, [x8, x7]
  eor w11, w11, w8
  mov w9, #8
crc_bits\@:
  and w8, w11, #1
  neg w8, w8
  and w8, w8, w10
  eor w11, w8, w11, lsr #1
  subs w9, w9, #1
  b.ne crc_bits\@
  add w7, w7, #1
  cmp w7, #48
  b.lo crc_bytes\@
  mvn w11, w11
.endm

prefix_stop:
  /* Terminal only: preserve the exact original BCM operands before validation.
   * No original continuation, so all other GPRs/flags are scratch here. */
  mov x27, x0
  mov w28, w1
  sub sp, sp, x22
  add sp, sp, x23
  mrs x1, sctlr_el1
  tbnz x1, #0, halt
  sub sp, sp, #160
  ldr x3, =CONSOLE
  ldr w4, =CAPACITY
  ldr w7, [x3]
  ldr w8, =0x43474244
  cmp w7, w8
  b.ne halt
  ldp w5, w6, [x3, #4]
  cmp w5, w6
  b.ne halt
  ldr w7, =(CAPACITY - 448)
  cmp w5, w7
  b.hi halt
  cmp w5, #128
  b.lo halt
  /* Verify the immediate, CRC-valid ENTRY at next-64. */
  sub w12, w5, #64
  add x18, x3, #12
  add x18, x18, x12
  mov w7, #0
p12_copy_entry:
  ldrb w8, [x18, x7]
  add x9, sp, #64
  strb w8, [x9, x7]
  add w7, w7, #1
  cmp w7, #64
  b.lo p12_copy_entry
  ldr x7, =0x213850463631374a
  ldr x8, [sp, #64]
  cmp x7, x8
  b.ne halt
  ldr x7, =0x0000004000000001
  ldr x8, [sp, #72]
  cmp x7, x8
  b.ne halt
  ldp w17, w7, [sp, #80]
  cbz w17, halt
  cmp w7, #10
  b.ne halt
  ldr w7, [sp, #88]
  tbnz w7, #0, halt
  ldr w16, [sp, #92] /* ARM CRC bound by the entry writer */
  movk x16, #0x3253, lsl #32
  movk x16, #0x5031, lsl #48 /* P12S | ARM CRC */
  ldr w7, [sp, #96]
  ldr w8, =0x43474244
  cmp w7, w8
  b.ne halt
  ldp w7, w8, [sp, #100]
  cmp w7, w12
  b.ne halt
  cmp w8, w12
  b.ne halt
  ldr w7, [sp, #108]
  cbnz w7, halt
  ldr x7, =0x0a21454e4f443850
  ldr x8, [sp, #120]
  cmp x7, x8
  b.ne halt
  CRC 64
  ldp w7, w8, [sp, #112]
  cmp w7, w11
  b.ne halt
  mvn w9, w11
  cmp w8, w9
  b.ne halt
  /* All six templates must be bound, uncommitted and CRC-correct before BCM.
   * They are in unused linear tail, never the adjacent pstore zones. */
  add x18, x3, #12
  add x18, x18, x5
  mov w12, w5
  mov w25, #0
p12_template_loop:
  mov w7, #0
p12_copy_template:
  ldrb w8, [x18, x7]
  mov x9, sp
  strb w8, [x9, x7]
  add w7, w7, #1
  cmp w7, #64
  b.lo p12_copy_template
  ldr x7, =0x213850463631374a
  ldr x8, [sp]
  cmp x7, x8
  b.ne halt
  ldr x7, =0x0000004000000001
  ldr x8, [sp, #8]
  cmp x7, x8
  b.ne halt
  ldp w7, w8, [sp, #16]
  cmp w7, w17
  b.ne halt
  add w7, w25, #16
  cmp w7, w8
  b.ne halt
  ldr x7, [sp, #24]
  cmp x7, x16
  b.ne halt
  ldr w7, [sp, #32]
  ldr w8, =0x43474244
  cmp w7, w8
  b.ne halt
  ldp w7, w8, [sp, #36]
  cmp w7, w12
  b.ne halt
  cmp w8, w12
  b.ne halt
  ldr w7, [sp, #44]
  cbnz w7, halt
  ldr x7, =0x0a21454e4f443850
  ldr x8, [sp, #56]
  cmp x7, x8
  b.ne halt
  CRC 0
  ldp w7, w8, [sp, #48]
  cmp w7, w11
  b.ne halt
  mvn w9, w11
  eor w7, w9, #1 /* prepared complement differs in one low bit */
  cmp w7, w8
  b.ne halt
  add x7, sp, #128
  strb w9, [x7, x25] /* preload the six final commit bytes */
  add w12, w12, #64
  add x18, x18, #64
  add w25, w25, #1
  cmp w25, #6
  b.lo p12_template_loop
  add x18, x3, #12
  add x18, x18, x5
  ldrb w19, [sp, #128]
  ldrb w20, [sp, #129]
  ldrb w21, [sp, #130]
  ldrb w22, [sp, #131]
  ldrb w23, [sp, #132]
  ldrb w24, [sp, #133]
  add w13, w5, #384
  add sp, sp, #160
.global p12_before_bcm, p12_bcm, p12_after_bcm, p12_first_barrier
.global p12_after_barrier, p12_readback, p12_header, p12_final_barrier, p12_after_final
p12_before_bcm:
  strb w19, [x18, #52]
p12_bcm:
#ifdef BCM_SKIP
  nop
#else
  str w28, [x27] /* exact address/value captured at original BCM STR */
#endif
p12_after_bcm:
  strb w20, [x18, #116]
p12_first_barrier:
  dsb sy
p12_after_barrier:
  strb w21, [x18, #180]
  /* Only commit bytes are read here; reader checks every full record CRC. */
  ldrb w7, [x18, #52]
  cmp w7, w19
  b.ne halt
  ldrb w7, [x18, #116]
  cmp w7, w20
  b.ne halt
  ldrb w7, [x18, #180]
  cmp w7, w21
  b.ne halt
p12_readback:
  strb w22, [x18, #244]
  str w13, [x3, #8]
  str w13, [x3, #4]
p12_header:
  strb w23, [x18, #308]
p12_final_barrier:
  dsb sy
p12_after_final:
  strb w24, [x18, #372]
  mov w18, #21
reset_call:
  movz w0, #9
  movk w0, #0x8400, lsl #16
  mov x1, xzr
  mov x2, xzr
  mov x3, xzr
  mov x4, xzr
  mov x5, xzr
  mov x6, xzr
  mov x7, xzr
  dsb sy
  smc #0
  mov x1, x0
  mov w0, #14
  mov w2, w18
  bl trace_append
halt:
  wfe
  b halt

/* x0=phase, x1=value, x2=required preceding phase. No header repair, no
 * new ID inference: the exact previous valid record supplies the armed ID.
 * All accesses remain within the 0x40000 console or caller's bounded stack.
 */
trace_append:
  sub sp, sp, #128
  mov w14, w0
  mov x15, x1
  mov w16, w2
  ldr x3, =CONSOLE
  ldr w4, =CAPACITY
  ldr w7, [x3]
  ldr w8, =0x43474244
  cmp w7, w8
  b.ne trace_bad
  ldp w5, w6, [x3, #4]
  cmp w5, w4
  b.hs trace_bad
  cmp w6, w4
  b.hi trace_bad
  cmp w5, w6
  b.hi trace_bad
  cmp w6, #64
  b.lo trace_bad
  add w12, w5, w4
  sub w12, w12, #64
  cmp w12, w4
  sub w7, w12, w4
  csel w12, w7, w12, hs
  mov w7, #0
copy_previous:
  add w8, w12, w7
  cmp w8, w4
  sub w9, w8, w4
  csel w8, w9, w8, hs
  add x8, x3, x8
  ldrb w9, [x8, #12]
  add x8, sp, #64
  strb w9, [x8, x7]
  add w7, w7, #1
  cmp w7, #64
  b.lo copy_previous
  ldr x7, =0x213850463631374a /* J716FP8! */
  ldr x8, [sp, #64]
  cmp x7, x8
  b.ne trace_bad
  ldr x7, =0x0000004000000001
  ldr x8, [sp, #72]
  cmp x7, x8
  b.ne trace_bad
  ldp w17, w7, [sp, #80]
  cbz w17, trace_bad
  cmp w7, w16
  b.ne trace_bad
  ldr w7, [sp, #108]
  cbnz w7, trace_bad
  ldr x7, =0x0a21454e4f443850 /* P8DONE!\n */
  ldr x8, [sp, #120]
  cmp x7, x8
  b.ne trace_bad
  cmp w16, #9
  b.ne check_crc
  ldr x7, =ARM_COOKIE
  ldr x8, [sp, #88]
  cmp x7, x8
  b.ne trace_bad
check_crc:
  CRC 64
  ldp w7, w8, [sp, #112]
  cmp w7, w11
  b.ne trace_bad
  mvn w9, w11
  cmp w8, w9
  b.ne trace_bad
  /* Bind the last record to the current header, not stale physical bytes. */
  ldr w7, [sp, #100]
  add w7, w7, #64
  cmp w7, w4
  sub w8, w7, w4
  csel w7, w8, w7, hs
  cmp w7, w5
  b.ne trace_bad
  ldr w7, [sp, #104]
  add w7, w7, #64
  cmp w7, w4
  csel w7, w4, w7, hi
  cmp w7, w6
  b.ne trace_bad
  /* P12 ENTRY low32=SCTLR, high32=validated ARM CRC. */
  cmp w14, #10
  b.ne p12_not_entry
  ldr w7, [sp, #112]
  bfi x15, x7, #32, #32
p12_not_entry:
  ldr x7, =0x213850463631374a
  str x7, [sp]
  ldr x7, =0x0000004000000001
  str x7, [sp, #8]
  stp w17, w14, [sp, #16]
  str x15, [sp, #24]
  ldr w7, =0x43474244
  str w7, [sp, #32]
  stp w5, w6, [sp, #36]
  str wzr, [sp, #44]
  CRC 0
  mvn w7, w11
  stp w11, w7, [sp, #48]
  ldr x7, =0x0a21454e4f443850
  str x7, [sp, #56]
  mov w7, #0
write_record:
  add w8, w5, w7
  cmp w8, w4
  sub w9, w8, w4
  csel w8, w9, w8, hs
  add x8, x3, x8
  mov x10, sp
  ldrb w9, [x10, x7]
  strb w9, [x8, #12]
  add w7, w7, #1
  cmp w7, #64
  b.lo write_record
  dsb sy
  mov w7, #0
readback:
  add w8, w5, w7
  cmp w8, w4
  sub w9, w8, w4
  csel w8, w9, w8, hs
  add x8, x3, x8
  ldrb w9, [x8, #12]
  mov x10, sp
  ldrb w10, [x10, x7]
  cmp w9, w10
  b.ne trace_bad
  add w7, w7, #1
  cmp w7, #64
  b.lo readback
  add w6, w6, #64
  cmp w6, w4
  csel w6, w4, w6, hi
  add w5, w5, #64
  cmp w5, w4
  sub w7, w5, w4
  csel w5, w7, w5, hs
  str w6, [x3, #8]
  str w5, [x3, #4]
  dsb sy
  mov w0, #1
  add sp, sp, #128
  ret
trace_bad:
  mov w0, #0
  add sp, sp, #128
  ret
.ltorg
