/* SPDX-License-Identifier: BSD-2-Clause-Patent
 * Terminal, single-CPU J716F diagnostics. No normal XNU continuation.
 * MODE: 5=V23P2, PSCI prefix isolation with inline checkpoint squares.
 * x18=row bytes, x19=framebuffer, x20=boot args, x28=incoming SCTLR value.
 * draw_line clobbers only x0..x16/NZCV. No stack or firmware calls.
 */
.text
.global stage1, pre, post, exception, draw_line, font, halt
stage1:
  ldr x1, [x0, #0x28]
  ldr x2, [x0, #0x38]
  ldr x3, [x0, #0x48]
  mul x2, x2, x3
  lsr x2, x2, #4
  bic x2, x2, #7
  mov x4, #1
  mul x5, x2, x4
  add x1, x1, x5
  movz w6, #0x00ff
  movk w6, #0xff00, lsl #16
1:
  stp w6, w6, [x1], #8
  subs x2, x2, #8
  b.hi 1b
  dsb sy
  b original_first
.org 0x60

.macro SHOW label, row, value
  mov x0, \value
  mov w1, #\row
  adr x2, \label
  bl draw_line
.endm
.macro STAGE number
  mov x17, #\number
  SHOW tag_stage, 1, x17
.endm


/* Twelve 96x96 squares at (1280+(n-1)*104,32). Each fills independently.
 * Inline integer stores only: no BL/RET, MRS, stack, counter or barrier.
 * Clobbers x9..x14/NZCV only. A visible square proves stores, not completion
 * of a later barrier. The exception square uses validated existing x18/x19.
 */
.macro RAW number
.global prefix_\number
prefix_\number:
  mov x9, #(1280 + (\number - 1) * 104)
  add x9, x19, x9, lsl #2
  add x9, x9, x18, lsl #5
  mov w12, #-1
  mov x10, #96
.Lraw_y\@:
  mov x11, x9
  mov x13, #48
.Lraw_x\@:
  stp w12, w12, [x11], #8
  subs x13, x13, #1
  b.ne .Lraw_x\@
  add x9, x9, x18
  subs x10, x10, #1
  b.ne .Lraw_y\@
.endm

pre:
  mov x28, x0
  bl setup_fb
  mov x9, x19
  mov x10, #0xfa0000     /* 2560 * 1600 * 4 */
  add x10, x19, x10
clear:
  stp xzr, xzr, [x9], #16
  cmp x9, x10
  b.lo clear
  RAW 1                    /* Before first title call. */
  mov x0, #MODE
  mov w1, #0
  adr x2, tag_title
  bl draw_line
  RAW 4                    /* Title call returned; next instruction reads EL. */
  mrs x21, CurrentEL
  RAW 5                    /* CurrentEL read completed. */
  SHOW tag_el, 10, x21
  RAW 6                    /* EL line returned; next read is SCTLR. */
  mrs x21, sctlr_el1
  RAW 7                    /* Initial SCTLR read completed. */
  SHOW tag_sctlr, 11, x21
  RAW 8                    /* SCTLR line returned. */

.if MODE == 1 || MODE == 5
  movz w21, #9
  movk w21, #0x8400, lsl #16
  SHOW tag_func, 2, x21
  RAW 9                    /* Function line returned. */
  STAGE 0xa0                /* Last visible state before SMC; no counter. */
  RAW 10                   /* A0 line returned, before SMC setup. */
  mov x0, x21
  mov x1, xzr
  mov x2, xzr
  mov x3, xzr
  dsb sy
  smc #0
  mov x21, x0
  RAW 11                   /* SMC returned; x0 retained in x21. */
  STAGE 0xa1                /* A return is failure to reset, record result. */
  SHOW tag_ret, 3, x21
  b halt
.elseif MODE == 2 || MODE == 3
  STAGE 0xb0                /* Before CNTFRQ access. */
  mrs x25, cntfrq_el0
  SHOW tag_freq, 2, x25
  STAGE 0xb1                /* Before the selected counter access. */
.if MODE == 2
  mrs x21, cntpct_el0
.else
  mrs x21, cntvct_el0
.endif
  mov x22, x21
  mov x24, #0
  mov x26, #0x100000        /* Fixed iteration bound, not a timer deadline. */
counter_loop:
  isb
.if MODE == 2
  mrs x23, cntpct_el0
.else
  mrs x23, cntvct_el0
.endif
  cmp x23, x22
  cset x9, lo
  orr x24, x24, x9
  mov x22, x23
  subs x26, x26, #1
  b.ne counter_loop
  sub x23, x22, x21
  SHOW tag_first, 3, x21
  SHOW tag_last, 4, x22
  SHOW tag_delta, 5, x23
  SHOW tag_back, 6, x24
  cmp x23, #0
  cset x21, ne
  SHOW tag_move, 7, x21
  STAGE 0xb2                /* Collection complete, no SMC, stop here. */
  b halt
.else
  STAGE 0xd0
  mrs x21, ttbr0_el1
  SHOW tag_ttbr, 2, x21
  mrs x21, tcr_el1
  SHOW tag_tcr, 3, x21
  mrs x22, mair_el1
  SHOW tag_mair, 4, x22
  /* This exact source kernel builds a 4K, L1-root TTBR0 (T0SZ=26).
   * Refuse any different layout. MAIR index 1 must be Normal non-cacheable.
   */
  ldr x9, =0x00000022a51a251a
  cmp x21, x9
  b.ne mapping_bad
  ubfx x9, x22, #8, #8
  cmp x9, #0x44
  b.ne mapping_bad
  mrs x9, ttbr0_el1
  and x9, x9, #0x0000fffffffff000
  cmp x9, x25
  b.ne mapping_bad
  /* Identity-map the measured display reservation with a spare bootstrap L2.
   * The source owns 64 KiB of bootstrap tables. Never replace a valid slot.
   */
  ldr x10, [x9, #16]        /* L1 slot 2, physical VA 0x80000000..bfffffff */
  cbnz x10, mapping_bad
  add x23, x25, #0xf, lsl #12
  mov x10, x23
  add x11, x23, #1, lsl #12
check_spare:
  ldr x12, [x10], #8
  cbnz x12, mapping_bad
  cmp x10, x11
  b.lo check_spare
  /* Both L1 roots must not already refer to that spare page. */
  mov x10, x25
  add x11, x25, #2, lsl #12
check_refs:
  ldr x12, [x10], #8
  and x12, x12, #0x0000fffffffff000
  cmp x12, x23
  b.eq mapping_bad
  cmp x10, x11
  b.lo check_refs
  /* Map the complete display reservation, all within one L1 slot. */
  mov x10, #0x9c000000
  mov x11, #0x9e400000
  ldr x12, =0x0060000000000605 /* PXN/UXN, AF, outer-shareable, AttrIndx=1 */
map_blocks:
  ubfx x13, x10, #21, #9
  orr x14, x10, x12
  str x14, [x23, x13, lsl #3]
  add x10, x10, #0x200, lsl #12
  cmp x10, x11
  b.lo map_blocks
  orr x10, x23, #3
  str x10, [x9, #16]
  dsb sy
  tlbi vmalle1
  dsb sy
  isb
  STAGE 0xd1                /* Explicit mapping ready, MMU still off. */
  mov x0, x28
  msr sctlr_el1, x0
  isb
post:
  STAGE 0xd2                /* Direct display write after MMU+ISB. */
  mrs x21, sctlr_el1
  SHOW tag_after, 5, x21
  mrs x21, ttbr0_el1
  SHOW tag_ttbr, 6, x21
  STAGE 0xd3                /* Register collection complete; no timer/SMC. */
  b halt
mapping_bad:
  STAGE 0xdf                /* Mapping preconditions failed; MMU not enabled. */
  b halt
.endif

/* These are terminal probes. Preserve exception evidence before drawing.
 * VBAR entries supply vector index in x27. No exception return is attempted.
 */
exception:
  RAW 12                   /* Before any exception-register access. */
  mov x26, x17
  mrs x21, esr_el1
  mrs x22, far_el1
  mrs x23, elr_el1
  mrs x24, spsr_el1
  bl setup_fb
  STAGE 0xe0
  SHOW tag_prior, 7, x26
  SHOW tag_vector, 8, x27
  SHOW tag_esr, 12, x21
  SHOW tag_far, 13, x22
  SHOW tag_elr, 14, x23
  SHOW tag_spsr, 15, x24
halt:
  dsb sy
  wfe
  b halt

setup_fb:
  ldr x19, [x20, #0x28]
  ldr x18, [x20, #0x38]
  mov x9, #10240
  cmp x18, x9
  b.ne halt
  ldr x9, [x20, #0x40]
  cmp x9, #2560
  b.ne halt
  ldr x9, [x20, #0x48]
  cmp x9, #1600
  b.ne halt
  ldr x9, [x20, #0x50]
  cmp x9, #32
  b.ne halt
  tst x19, #15
  b.ne halt
  mov x9, #0x9c000000
  cmp x19, x9
  b.lo halt
  mov x10, #0xfa0000
  add x10, x19, x10
  mov x9, #0x9e400000
  cmp x10, x9
  b.hi halt
  ret

/* x0=value, x1=row, x2=exactly six ASCII label bytes. White on dark grey.
 * 5x7 glyphs scaled 8x; 22 characters, 1056 pixels, 80 pixels per row.
 */
draw_line:
  mov x3, x0
  mov x4, #0
draw_char:
  cmp x4, #6
  b.hs draw_hex
  ldrb w5, [x2, x4]
  b glyph_index
draw_hex:
  mov x6, #21
  sub x6, x6, x4
  lsl x6, x6, #2
  lsr x5, x3, x6
  and w5, w5, #15
  add w5, w5, #48
  cmp w5, #57
  add w6, w5, #7
  csel w5, w6, w5, hi
glyph_index:
  cmp w5, #32
  b.eq space_index
  cmp w5, #57
  b.hi letter_index
  sub w5, w5, #48
  b glyph_address
letter_index:
  sub w5, w5, #55
  b glyph_address
space_index:
  mov w5, #36
glyph_address:
  adr x6, font
  lsl x7, x5, #3
  sub x7, x7, x5
  add x6, x6, x7
  mov x0, #80
  mul x10, x1, x0
  add x10, x10, #32
  mul x10, x10, x18
  add x10, x10, x19
  add x10, x10, #128
  mov x0, #192
  madd x10, x4, x0, x10
  mov w14, #0x2020
  movk w14, #0x20, lsl #16
  mov w15, #-1
  mov x7, #0
glyph_y:
  lsr x0, x7, #3
  ldrb w11, [x6, x0]
  madd x0, x7, x18, x10
  mov w8, #0
glyph_x:
  lsr w12, w8, #3
  mov w13, #5
  sub w12, w13, w12
  lsr w13, w11, w12
  tst w13, #1
  csel w13, w15, w14, ne
  str w13, [x0, w8, uxtw #2]
  add w8, w8, #1
  cmp w8, #48
  b.lo glyph_x
  add x7, x7, #1
  cmp x7, #56
  b.lo glyph_y
  add x4, x4, #1
  cmp x4, #22
  b.lo draw_char
  cbnz x1, .Lnot_title_before_dsb
  RAW 2                    /* Title stores done; original DSB is next. */
.Lnot_title_before_dsb:
  dsb sy
  cbnz x1, .Lnot_title_after_dsb
  RAW 3                    /* Original title DSB completed; RET follows. */
.Lnot_title_after_dsb:
  ret

tag_title: .ascii "V23   "
tag_stage: .ascii "STAGE "
tag_el: .ascii "EL    "
tag_sctlr: .ascii "SCTLR "
tag_func: .ascii "FUNC  "
tag_ret: .ascii "RETURN"
tag_freq: .ascii "FREQ  "
tag_first: .ascii "FIRST "
tag_last: .ascii "LAST  "
tag_delta: .ascii "DELTA "
tag_back: .ascii "BACK  "
tag_move: .ascii "MOVES "
tag_ttbr: .ascii "TTBR0 "
tag_tcr: .ascii "TCR   "
tag_mair: .ascii "MAIR  "
tag_after: .ascii "AFTER "
tag_prior: .ascii "PRIOR "
tag_vector: .ascii "VECTOR"
tag_esr: .ascii "ESR   "
tag_far: .ascii "FAR   "
tag_elr: .ascii "ELR   "
tag_spsr: .ascii "SPSR  "
font:
/* 0..9, A..Z, space; five bits per row, hand-written basic 5x7 alphabet. */
.byte 14,17,19,21,25,17,14, 4,12,4,4,4,4,14, 14,17,1,2,4,8,31
.byte 30,1,1,14,1,1,30, 2,6,10,18,31,2,2, 31,16,16,30,1,1,30
.byte 14,16,16,30,17,17,14, 31,1,2,4,8,8,8, 14,17,17,14,17,17,14
.byte 14,17,17,15,1,1,14
.byte 14,17,17,31,17,17,17, 30,17,17,30,17,17,30, 14,17,16,16,16,17,14
.byte 30,17,17,17,17,17,30, 31,16,16,30,16,16,31, 31,16,16,30,16,16,16
.byte 14,17,16,23,17,17,15, 17,17,17,31,17,17,17, 14,4,4,4,4,4,14
.byte 7,2,2,2,18,18,12, 17,18,20,24,20,18,17, 16,16,16,16,16,16,31
.byte 17,27,21,21,17,17,17, 17,25,21,19,17,17,17, 14,17,17,17,17,17,14
.byte 30,17,17,30,16,16,16, 14,17,17,17,21,18,13, 30,17,17,30,20,18,17
.byte 15,16,16,14,1,1,30, 31,4,4,4,4,4,4, 17,17,17,17,17,17,14
.byte 17,17,17,17,17,10,4, 17,17,17,21,21,21,10, 17,17,10,4,10,17,17
.byte 17,17,10,4,4,4,4, 31,1,2,4,8,16,31, 0,0,0,0,0,0,0
.balign 8
.ltorg
