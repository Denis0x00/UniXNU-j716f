/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * P14 bridge on the frozen source-built baseline. Remove the WHOLE BCM route
 * as upstream's native timer entry guard does. This is not a rebuilt native
 * timer kernel, nor a claim that ArmCpu/GIC is integrated. Resume original
 * post-MMU assembly and arm_init; stop after cpu_data_init, before PE_init_platform.
 * Six independent P14S templates; no slot reuses another slot's commit byte.
 */
.text
.global stage1, entry_resume, after_bcm, bcm_resume, pre, pre_resume, post
.global post_readback, post_resume, reset_call, halt, trace_append
.global validate_slots, map_console, mapping_failed, publish
.equ CONSOLE, 0xb00be000
.equ CAPACITY, 0x3fff4
.equ ARM_COOKIE, 0x5031344b45524e4c
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
  ldr w4, =(CAPACITY - 576)
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


/* 256 bytes of architectural state plus 32 bytes of pre/post handoff.
 * The handoff is below SP and is not live across original XNU code: pre
 * writes it, restores state and executes just MSR/ISB before post saves into
 * the same frame via the kernel's high stack alias. No kernel text writes.
 */
.macro SAVE
  sub sp, sp, #288
  stp x0, x1, [sp, #0]
  stp x2, x3, [sp, #16]
  stp x4, x5, [sp, #32]
  stp x6, x7, [sp, #48]
  stp x8, x9, [sp, #64]
  stp x10, x11, [sp, #80]
  stp x12, x13, [sp, #96]
  stp x14, x15, [sp, #112]
  stp x16, x17, [sp, #128]
  stp x18, x19, [sp, #144]
  stp x20, x21, [sp, #160]
  stp x22, x23, [sp, #176]
  stp x24, x25, [sp, #192]
  stp x26, x27, [sp, #208]
  stp x28, x29, [sp, #224]
  str x30, [sp, #240]
  mrs x17, nzcv
  str x17, [sp, #248]
.endm
.macro RESTORE
  ldr x17, [sp, #248]
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
  ldp x18, x19, [sp, #144]
  ldp x20, x21, [sp, #160]
  ldp x22, x23, [sp, #176]
  ldp x24, x25, [sp, #192]
  ldp x26, x27, [sp, #208]
  ldp x28, x29, [sp, #224]
  ldr x30, [sp, #240]
  add sp, sp, #288
.endm

after_bcm:
  sub sp, sp, x22
  add sp, sp, x23
  str x30, [sp, #-16]!
  bl commit_bound
.global bcm_commit_return
bcm_commit_return:
  ldr x30, [sp], #16
  sub sp, sp, x23
  add sp, sp, x22
bcm_resume:
  b original_after_bcm

pre:
  sub sp, sp, x22
  add sp, sp, x23
  SAVE
  mov w0, #1
  bl validate_slots
  ldr x0, [sp, #200]
  bl map_console
  cbnz w0, mapping_failed
  mrs x1, sctlr_el1
  tbnz x1, #0, halt
  str x18, [sp, #256]
  str w20, [sp, #272]
  dsb sy
  RESTORE
  sub sp, sp, x23
  add sp, sp, x22
pre_resume:
  msr sctlr_el1, x0
  isb
post:
  SAVE
  ldr x18, [sp, #256]
  ldr w20, [sp, #272]
post_readback:
  mrs x1, sctlr_el1
  tbz x1, #0, halt
.global slot17
slot17:
  strb w20, [x18, #116]
  dsb sy
  RESTORE
post_resume:
  b original_post_mmu

/* Original x30 is saved before BL; shared SAVE preserves the other GPRs,
 * NZCV and the BL continuation. No context lives below SP across XNU code.
 * Each invocation revalidates ENTRY and all six bound templates in RAM.
 */
.global sysregs, sysregs_resume, arm_entry, arm_entry_resume, fixups, fixups_resume, init_stop
sysregs:
  str x30, [sp, #-16]!
  bl commit_bound
sysregs_commit_return:
  ldr x30, [sp], #16
sysregs_resume:
  add x0, x20, x22
  b original_sysregs_next
arm_entry:
  str x30, [sp, #-16]!
  bl commit_bound
arm_commit_return:
  ldr x30, [sp], #16
arm_entry_resume:
  sub sp, sp, #208
  b original_arm_next
fixups:
  str x30, [sp, #-16]!
  bl commit_bound
fixups_commit_return:
  ldr x30, [sp], #16
fixups_resume:
  adrp x8, original_slide_global
  b original_fixups_next
init_stop:
  str x30, [sp, #-16]!
  bl commit_bound
stop_commit_return:
  b halt

.global commit_bound, commit_byte
commit_bound:
  SAVE
  mov w0, #0
  adr x1, bcm_commit_return
  cmp x30, x1
  b.eq commit_selected
  mov w0, #2
  adr x1, sysregs_commit_return
  cmp x30, x1
  b.eq commit_selected
  mov w0, #3
  adr x1, arm_commit_return
  cmp x30, x1
  b.eq commit_selected
  mov w0, #4
  adr x1, fixups_commit_return
  cmp x30, x1
  b.eq commit_selected
  mov w0, #5
  adr x1, stop_commit_return
  cmp x30, x1
  b.ne halt
commit_selected:
  str w0, [sp, #256]
  mrs x1, sctlr_el1
  and w1, w1, #1
  cmp w0, #0
  cset w2, ne
  cmp w1, w2
  b.ne halt
  bl validate_slots
  ldr w0, [sp, #256]
  lsl x1, x0, #6
  add x18, x18, x1
  ldrb w1, [x18, #52]
  eor w1, w1, #1
commit_byte:
  strb w1, [x18, #52]
  dsb sy
  cmp w0, #5
  b.eq terminal_stop
  RESTORE
  ret
terminal_stop:
  mrs x1, sctlr_el1
  tbz x1, #0, halt
  bl publish
  mov w0, #12
  mov w2, #21
  bl trace_append
  cbz w0, halt
  mov w18, #12
  b reset_call

mapping_failed:
  mov x1, x0
  sub w13, w13, #320          /* only BCM_SKIPPED completed */
  bl publish
  mov w0, #13
  mov w2, #16
  bl trace_append
  cbz w0, halt
  mov w18, #13
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

/* x1 (observed SCTLR/failure code) and x30 must survive publication. */
publish:
  ldr x3, =CONSOLE
  dsb sy
  str w13, [x3, #8]
  str w13, [x3, #4]
  dsb sy
  ret

validate_slots:
  sub sp, sp, #160
  str w0, [sp, #136]
  ldr x3, =CONSOLE
  ldr w4, =CAPACITY
  ldr w7, [x3]
  ldr w8, =0x43474244
  cmp w7, w8
  b.ne halt
  ldp w5, w6, [x3, #4]
  cmp w5, w6
  b.ne halt
  ldr w7, =(CAPACITY - 512)
  cmp w5, w7
  b.hi halt
  cmp w5, #128
  b.lo halt
  /* Verify the immediate, CRC-valid ENTRY at next-64. */
  sub w12, w5, #64
  add x18, x3, #12
  add x18, x18, x12
  mov w7, #0
p14_copy_entry:
  ldrb w8, [x18, x7]
  add x9, sp, #64
  strb w8, [x9, x7]
  add w7, w7, #1
  cmp w7, #64
  b.lo p14_copy_entry
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
  movk x16, #0x3453, lsl #32
  movk x16, #0x5031, lsl #48 /* P14S | ARM CRC */
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
p14_template_loop:
  mov w7, #0
p14_copy_template:
  ldrb w8, [x18, x7]
  mov x9, sp
  strb w8, [x9, x7]
  add w7, w7, #1
  cmp w7, #64
  b.lo p14_copy_template
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
  ldr w8, [sp, #136]
  cmp w25, w8
  eor w7, w9, #1
  csel w7, w9, w7, lo
  ldr w8, [sp, #52]
  cmp w7, w8
  b.ne halt
  add x7, sp, #128
  strb w9, [x7, x25] /* preload the six final commit bytes */
  add w12, w12, #64
  add x18, x18, #64
  add w25, w25, #1
  cmp w25, #6
  b.lo p14_template_loop
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
  ret
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
  /* P14 ENTRY low32=SCTLR, high32=validated ARM CRC. */
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
