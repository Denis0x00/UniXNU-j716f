/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * P11 terminal publication progress; M=0 and no normal kernel continuation.
 * Entry ABI and the CRC writer match P9. No console mapping, MMU enable,
 * counter, graphics, or original-code continuation after prefix_stop.
 */
.text
.global stage1, entry_resume, prefix_stop, reset_call, halt, trace_append, terminal_append
.equ CONSOLE, 0xb00be000
.equ CAPACITY, 0x3fff4
.equ ARM_COOKIE, 0x50394b45524e454c

stage1:
  mov x19, sp
  mov x20, x0
  mrs x21, nzcv
  adrp x17, original_stack
  add x17, x17, :lo12:original_stack
  mov sp, x17
  mov w0, #10
  mrs x1, sctlr_el1
  mov w2, #9
  bl trace_append
  cbz w0, halt
  mov x0, x20
  msr nzcv, x21
  mov sp, x19
entry_resume:
  b original_first

prefix_stop:
  /* All selected sites follow original high virtual SP setup, with M=0.
   * x22/x23 have both received the same slide, so their difference still
   * converts the original SP back to its kernel-owned physical stack.
   */
  sub sp, sp, x22
  add sp, sp, x23
  mov w0, #11
  mrs x1, sctlr_el1
  tbnz x1, #0, halt
  mov w2, #10
  ldr x1, =0x5031315300000001 /* P11S stage 1; not SCTLR */
  bl terminal_append
  cbz w0, halt
  mov w18, #11
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


/* Replace the same phase11 slot with a new CRC-valid progress value.
 * No extra console region, no header repair, and no barrier hidden in this
 * macro. A surviving code is a lower bound, not a precise stopping PC.
 * CRC/complement updates are not atomic: interruption may leave no valid
 * phase11, which must remain inconclusive rather than be repaired.
 */
.macro PROGRESS stage
  ldr x15, =(0x5031315300000000 + \stage)
  str x15, [sp, #24]
  CRC 0
  mvn w7, w11
  stp w11, w7, [sp, #48]
  mov w7, #0
progress_write\@:
  add w8, w13, w7
  cmp w8, w4
  sub w9, w8, w4
  csel w8, w9, w8, hs
  add x8, x3, x8
  mov x10, sp
  ldrb w9, [x10, x7]
  strb w9, [x8, #12]
  add w7, w7, #1
  cmp w7, #64
  b.lo progress_write\@
.endm

terminal_append:
  sub sp, sp, #128
  mov w14, w0
  mov x15, x1
  mov w16, w2
  ldr x3, =CONSOLE
  ldr w4, =CAPACITY
  ldr w7, [x3]
  ldr w8, =0x43474244
  cmp w7, w8
  b.ne term_trace_bad
  ldp w5, w6, [x3, #4]
  cmp w5, w4
  b.hs term_trace_bad
  cmp w6, w4
  b.hi term_trace_bad
  cmp w5, w6
  b.hi term_trace_bad
  cmp w6, #64
  b.lo term_trace_bad
  add w12, w5, w4
  sub w12, w12, #64
  cmp w12, w4
  sub w7, w12, w4
  csel w12, w7, w12, hs
  mov w7, #0
term_copy_previous:
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
  b.lo term_copy_previous
  ldr x7, =0x213850463631374a /* J716FP8! */
  ldr x8, [sp, #64]
  cmp x7, x8
  b.ne term_trace_bad
  ldr x7, =0x0000004000000001
  ldr x8, [sp, #72]
  cmp x7, x8
  b.ne term_trace_bad
  ldp w17, w7, [sp, #80]
  cbz w17, term_trace_bad
  cmp w7, w16
  b.ne term_trace_bad
  ldr w7, [sp, #108]
  cbnz w7, term_trace_bad
  ldr x7, =0x0a21454e4f443850 /* P8DONE!\n */
  ldr x8, [sp, #120]
  cmp x7, x8
  b.ne term_trace_bad
  cmp w16, #9
  b.ne term_check_crc
  ldr x7, =ARM_COOKIE
  ldr x8, [sp, #88]
  cmp x7, x8
  b.ne term_trace_bad
term_check_crc:
  CRC 64
  ldp w7, w8, [sp, #112]
  cmp w7, w11
  b.ne term_trace_bad
  mvn w9, w11
  cmp w8, w9
  b.ne term_trace_bad
  /* Bind the last record to the current header, not stale physical bytes. */
  ldr w7, [sp, #100]
  add w7, w7, #64
  cmp w7, w4
  sub w8, w7, w4
  csel w7, w8, w7, hs
  cmp w7, w5
  b.ne term_trace_bad
  ldr w7, [sp, #104]
  add w7, w7, #64
  cmp w7, w4
  csel w7, w4, w7, hi
  cmp w7, w6
  b.ne term_trace_bad
  mov w13, w5 /* retain original record offset across publication */
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
term_write_record:
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
  b.lo term_write_record
.global term_body_ready, term_first_barrier, term_stage2_done
term_body_ready:
term_first_barrier:
#ifdef TERM_FIRST_DMB
  dmb sy
#else
  dsb sy
#endif
  PROGRESS 2
term_stage2_done:
  mov w7, #0
term_readback:
  add w8, w5, w7
  cmp w8, w4
  sub w9, w8, w4
  csel w8, w9, w8, hs
  add x8, x3, x8
  ldrb w9, [x8, #12]
  mov x10, sp
  ldrb w10, [x10, x7]
  cmp w9, w10
  b.ne term_trace_bad
  add w7, w7, #1
  cmp w7, #64
  b.lo term_readback
.global term_readback_done, term_stage3_done
term_readback_done:
  PROGRESS 3
term_stage3_done:
  add w6, w6, #64
  cmp w6, w4
  csel w6, w4, w6, hi
  add w5, w5, #64
  cmp w5, w4
  sub w7, w5, w4
  csel w5, w7, w5, hs
  str w6, [x3, #8]
  str w5, [x3, #4]
.global term_header_written, term_final_barrier, term_stage5_done
term_header_written:
  PROGRESS 4
term_final_barrier:
  dsb sy
  PROGRESS 5
term_stage5_done:
  mov w0, #1
  add sp, sp, #128
  ret
term_trace_bad:
  mov w0, #0
  add sp, sp, #128
  ret
.ltorg
