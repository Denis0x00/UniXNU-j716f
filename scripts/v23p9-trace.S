/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Pinned single-CPU, terminal entry/MMU diagnostic. MODE=1 entry control,
 * MODE=2 entry/pre/post. No framebuffer, legacy-store patch or counter.
 * trace_append is a leaf using x0..x17/NZCV and 128 bytes of the active stack.
 * It accepts only the CRC-valid immediately preceding phase in an armed chain.
 */
.text
.global stage1, pre, post, trace_append, map_console, halt, reset_call
.global entry_resume, pre_resume, post_resume
.equ CONSOLE, 0xb00be000
.equ CAPACITY, 0x3fff4
.equ ARM_COOKIE, 0x50394b45524e454c

stage1:
  /* DarwinEnter supplies only x0 as the XNU boot argument. x19/x20/x21/x30
   * are overwritten by start_first_cpu before any original read. Use the
   * same PC-relative physical SP_EL0 stack top that the pinned XNU installs.
   * Preserve incoming SP and NZCV; no dependency on the incoming UEFI stack.
   */
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
.if MODE == 1
  mov w18, #10
  b reset_call
.else
  mov x0, x20
  msr nzcv, x21
  mov sp, x19
entry_resume:
  b original_first
.endif

.macro SAVE
  sub sp, sp, #176
  stp x0, x1, [sp, #0]
  stp x2, x3, [sp, #16]
  stp x4, x5, [sp, #32]
  stp x6, x7, [sp, #48]
  stp x8, x9, [sp, #64]
  stp x10, x11, [sp, #80]
  stp x12, x13, [sp, #96]
  stp x14, x15, [sp, #112]
  stp x16, x17, [sp, #128]
  stp x18, x30, [sp, #144]
  mrs x17, nzcv
  str x17, [sp, #160]
.endm
.macro RESTORE
  ldr x17, [sp, #160]
  msr nzcv, x17
  ldp x0, x1, [sp, #0]
  ldp x2, x3, [sp, #16]
  ldp x4, x5, [sp, #32]
  ldp x6, x7, [sp, #48]
  ldp x8, x9, [sp, #64]
  ldp x10, x11, [sp, #80]
  ldp x12, x13, [sp, #96]
  ldp x14, x15, [sp, #112]
  ldp x16, x17, [sp, #128]
  ldp x18, x30, [sp, #144]
  add sp, sp, #176
.endm

pre:
  /* XNU has installed a high virtual SP although M is still zero. Its
   * boot_args virtBase/physBase are x22/x23. Convert SP without a scratch
   * register or NZCV change, then restore the exact virtual SP before MSR.
   */
  sub sp, sp, x22
  add sp, sp, x23
  SAVE
  mov x0, x25
  bl map_console
  cbnz w0, mapping_failed
  mov w0, #11
  mrs x1, sctlr_el1
  mov w2, #10
  bl trace_append
  cbz w0, halt
  RESTORE
  sub sp, sp, x23
  add sp, sp, x22
pre_resume:
  msr sctlr_el1, x0             /* exact displaced instruction */
  isb
post:
  SAVE
  mov w0, #12
  mrs x1, sctlr_el1
  tbz x1, #0, halt
  mov w2, #11
  bl trace_append
  cbz w0, halt
  RESTORE
post_resume:
  /* Terminal diagnostic: no normal kernel/platform continuation claim. */
  mov w18, #12
  b reset_call
mapping_failed:
  mov x1, x0
  mov w0, #13
  mov w2, #10
  bl trace_append
  b halt
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

/* Exact console-only NC mapping, leaving the original owned-memory mappings
 * intact. Requires the pinned 4K/L1-root TCR, MAIR[1]=Normal NC and two unused
 * zero bootstrap pages. No page-table write occurs until every check passes.
 * Return 0 success, 1 TCR, 2 MAIR, 3 TTBR, 4 L1 occupied, 5 spare nonzero,
 * 6 spare already referenced. x0..x17/NZCV only; no SP/EL/SCTLR change.
 */
map_console:
  mov x14, x0
  mov w0, #1
  mrs x1, tcr_el1
  ldr x2, =0x22a51a251a
  cmp x1, x2
  b.ne map_return
  mov w0, #2
  mrs x1, mair_el1
  ubfx x1, x1, #8, #8
  cmp x1, #0x44
  b.ne map_return
  mov w0, #3
  mrs x1, ttbr0_el1
  and x1, x1, #0x0000fffffffff000
  cmp x1, x14
  b.ne map_return
  mrs x1, ttbr1_el1
  and x1, x1, #0x0000fffffffff000
  add x2, x14, #1, lsl #12
  cmp x1, x2
  b.ne map_return
  mov w0, #4
  ldr x1, [x14, #16]
  cbnz x1, map_return
  add x15, x14, #14, lsl #12
  add x16, x14, #15, lsl #12
  add x17, x14, #16, lsl #12
  mov w0, #5
  mov x1, x15
check_zero:
  ldr x2, [x1], #8
  cbnz x2, map_return
  cmp x1, x17
  b.lo check_zero
  mov w0, #6
  mov x1, x14
check_refs:
  ldr x2, [x1], #8
  and x3, x2, #3
  cmp x3, #3
  b.ne next_ref
  and x2, x2, #0x0000fffffffff000
  cmp x2, x15
  b.eq map_return
  cmp x2, x16
  b.eq map_return
next_ref:
  cmp x1, x15
  b.lo check_refs
  ldr x1, =CONSOLE
  ldr x2, =(CONSOLE+0x40000)
  ldr x3, =0x0060000000000607 /* PXN, UXN, AF, OSH, AttrIndx=1, 4K page */
map_pages:
  ubfx x4, x1, #12, #9
  orr x5, x1, x3
  str x5, [x16, x4, lsl #3]
  add x1, x1, #1, lsl #12
  cmp x1, x2
  b.lo map_pages
  orr x1, x16, #3
  str x1, [x15, #0xc00]       /* L2 slot 384, VA 0xb0000000 */
  dsb sy
  orr x1, x15, #3
  str x1, [x14, #16]         /* L1 slot 2 */
  dsb sy
  tlbi vmalle1
  dsb sy
  isb
  mov w0, #0
map_return:
  ret

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
