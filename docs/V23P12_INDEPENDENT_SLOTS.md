# V23P12: independent commit witnesses and matched BCM control

## Status

P12A/B are built and host verified. **P12A has been flashed only to boot_b**;
B was activated/verified bootable and the reboot command succeeded. The initial
P12 reader is observed VALID but saturated (next0x26e9, size0x3fff4); the
linear-tail Query/ARM gate rejects this baseline. Kernel launch is pending. P12B remains unflashed. No P12 kernel progress,
retention, reset-source or MMU-on result exists. P11 evidence remains separate:
A retained published stage5; B/D retained only matching raw stage1.
Exact images, hashes and source identities are in
[the P12 record](../research/v23p12-independent-slots.json).

## Matched controls

| Image | BCM operation | Terminal path |
| --- | --- | --- |
| P12A, first trial | NOP | six independent commits, then SYSTEM_RESET |
| P12B, after A review | original STR address/value | same six commits and barriers |

Both hook the original STR site `0xfffffff0073fe32c`. Before validation, the
terminal envelope captures original x0/w1 in x27/w28. B executes
`str w28,[x27]`; A executes one NOP in the same probe location. The probes differ
by exactly that one instruction. The entry/hook branches, linked reader, power
application, ADT and all nonpayload firmware modules match across the pair.
The original following ISB and normal kernel continuation are not executed.
First-barrier changes are not mixed into this BCM comparison.

## Prepared records and independent commits

The dedicated `J716fSlotTrace.efi` defaults to read-only. A fresh successful
PSCI Query and manual ARM are required for each attempt. P12's ARM cookie is
`0x5031324b45524e4c`; old P9/P11 ARM records do not authorize P12.

After appending/readback of ARM, the reader prepares six 64-byte templates in
the unused linear tail of the existing console. Each has its own phase, ID,
physical offset, prior-header position and CRC. Its value is
`0x5031325300000000 | ARM_CRC32`. Bit0 of the CRC-complement byte at offset52
is deliberately wrong. Therefore preparation alone produces **no CRC-valid
stage record** and does not extend the logical header to cover the templates.
Each template is read back before preparation reports success.

The entry writer consumes the exact CRC-valid ARM and writes ENTRY with
**low32=SCTLR_EL1, high32=validated ARM CRC**. This intentionally changes the
P9/P11 ENTRY value format. It rejects M=1 and nonzero SCTLR upper32, and preserves
boot_args, incoming SP and NZCV before executing the original first-CPU prefix.

Before any BCM operation, the terminal envelope validates the immediate ENTRY
and every template, including ID, ARM CRC binding, phase, exact offset, full CRC
and uncommitted complement. It preloads all six final complement bytes. No
template preparation, CRC loop, stack access or call occurs between BCM and
the six commits/reset setup. The only stage mutation is a single STRB to that
stage's own complement byte; earlier records are never rewritten.

| Phase / reader row | Commit follows |
| --- | --- |
| 16 PRE_BCM | successful terminal/entry/template validation, before STR/NOP |
| 17 POST_BCM | STR/NOP instruction |
| 18 POST_DSB | first terminal DSB SY |
| 19 COMMIT_READBACK | readback of the first three **commit bytes** |
| 20 HEADER_STORED | size and next stores covering all six slots |
| 21 POST_FINAL_DSB | final publication DSB SY |

Phase19 does not certify a full-record kernel readback. The reader independently
checks full CRCs. Slots whose commits have not executed remain invalid even
after the header extends over them. The subsequent SYSTEM_RESET setup retains
a DSB; if the call returns, the ordinary bounded writer attempts phase14.

## Memory and execution contract

- Only the already qualified console `0xb00be000+0x40000` is used, with a 12-byte
  DBGC header and payload capacity `0x3fff4`. No new physical RAM reservation,
  allocator, page-table mapping, cache maintenance or PMIC operation is added.
- Query requires a valid linear header (`next=size`) and at least832 unused
  bytes. ARM rechecks for576 bytes. After ARM,512 bytes cover ENTRY, six slots
  and an optional reset-return record. Witnesses do not wrap or overwrite the
  existing logical log. Saturated/nonlinear logs are rejected without repair.
  At the exact capacity boundary a returned-reset append may fill/wrap the
  ordinary ring; the bound reader then conservatively reports raw witnesses.
- The existing kernel-owned physical stack is used before the original prefix.
  At the terminal hook, x22/x23 convert original virtual SP to physical SP;
  the envelope uses160 stack bytes and releases them before PRE_BCM.
  There is no original continuation, so captured operands and template registers
  may replace the remaining kernel register state after that terminal hook.
- M remains0. No counter reads, graphics writes, MMU enable, TTBR mapping change
  or normal XNU continuation is introduced by the probe. Existing original
  bootstrap table and register instructions still execute before the hook.

## Reader attribution and interpretation

The frozen reader displays ARM, ENTRY and all six stages separately with
logical/raw counts, CRC and value. It requires a unique CRC-valid P12 ARM at its
recorded offset, matching ENTRY high32 ARM CRC, and each stage at the expected
physical slot with matching ID/tag/ARM CRC/offset before attributing that stage.
Host decoding is available through `scripts/decode-j716f-v23p12.py`.

An observed valid stage is a progress lower bound. Missing stages do not prove
an exact stopping PC. Independent slots remove same-slot overwrite ambiguity;
they do not independently prove store ordering, external BCM completion,
cross-reset visibility or reset source. No DSB is inserted between PRE_BCM and
the BCM operation to force the earlier witness visible. One-byte STRB commits
are not a claim of whole-record persistent atomicity across reset.

## Host verification

- 58 actual-assembly cases: original prefix and table writes, gates, each commit
  interruption, damaged/uncommitted templates, old ARM cookie, unaligned slots,
  exact tail capacity, rejected readback and reset return. Pair audit verifies
  only one STR/NOP probe delta and no post-BCM stack/CRC/call work.
- 22 actual linked-reader cases: read-only defaults, Query/ARM gates, invalid
  prepared templates, duplicate ARM prevention, reader-to-kernel-to-reader flow,
  independent raw/logical slot snapshots and wrong-attempt rejection.
- Final FD/FAT/EFI/kernel/ADT/HMAC/source and Android boot image audits passed.
  Nonpayload FV sections and compact SEC match P8/P9. The new reader is identical
  across A/B; it intentionally differs from P9. Power app matches P11 byte for
  byte, so its22 linked-app host cases are reused rather than rerun.

Privileged registers, MMIO, barriers, SMC and UEFI services are mocked in host
checks. There is no new hardware qualification from these results.

## First trial: P12A only, boot_b only

1. Connect in fastboot and prepare continuous recording. Flash only the audited
   P12A boot_b image; verify B active/bootable and boot the untimed menu.
2. Confirm **P12A / BCM SKIP**. Open **P12 reader FIRST** and photograph the full
   frozen ENTRY/NOW page before Query, ARM, reboot or shutdown actions.
3. If NOW is VALID and the linear-tail gate accepts, Query once and ARM once.
   Photograph actual ID/CRC and **ARM + six invalid templates OK**. On any
   preparation/gate/readback failure, preserve the page; do not launch or repair.
4. Return to the menu, launch P12A once, and record through any restart.
5. Open P12 reader FIRST after reboot. Photograph the full frozen page and match
   ARM ID/CRC, ENTRY binding, six individual logical/raw rows and header.
   If stalled, preserve video/final screen before recovery. Never reuse ARM.
6. Review A before B. B's flasher requires a reviewed matching A six-slot,
   published-header, observed-reboot result. P10C remains gated.

Reboot and Power off remain available through the byte-identical P11 power app;
avoid those actions while collecting checkpoint evidence. Firmware version
string may still identify P9 to preserve nonpayload identities; the parent
menu and dedicated P12 reader identify this experiment.

## Initial P12A reader observation

P12A initial photo confirms V23P12 reader at EL1, frozen ENTRY/NOW VALID next0x26e9 size0x3fff4 consoleCRC50977087, logical0/raw0 and all eight rows zero. THIS RUN ID1/query0/armed0. This is a saturated circular console, not an empty log. P12 linear-tail requirement fails before Query (832 bytes required); no Query block message or kernel launch observed. Parent A menu not pictured; variant follows recorded flash. Preserve baseline; qualify full-ring handling before a new attempt, B/P10C remain gated.
Next: Preserve the saturated P12A initial reader baseline; do not Query/ARM or launch kernel with this ineligible header. Qualify explicit session storage or bounded circular-slot handling before a new P12 attempt. B and P10C remain gated.
