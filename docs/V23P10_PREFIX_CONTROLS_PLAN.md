# V23P10 terminal prefix controls

## Why

Fresh P9M ID=3 retained matching ARM `671D1670` and KERNEL_ENTRY
`D1AFE339`, SCTLR=`0xc00808`, with a valid header and no PRE/POST/MAP_FAILURE.
See `research/v23p9-kernel-trace.json` for the three photo hashes and limits.
The missing PRE is not an exact PC: the original firstcpu prefix, pre-hook stack
conversion, mapping setup/barriers and record append all precede it.

## Independent terminal controls

Keep P9 binaries and source unchanged. Build independent kernels and visibly
identify the hook in each menu. Reuse the audited P9 reader and phases 9/10/11;
phase 11 in these controls means the named terminal pre-MMU prefix checkpoint,
not successful execution of P9M console mapping. Do not combine these results
with a P9M POST result or claim MMU-on execution.

| Control | Terminal branch site | Original prefix executed |
| --- | --- | --- |
| P10A | `0xfffffff0073fe32c`, immediately before legacy BCM STR | Page-table creation, TCR/TTBR/MAIR setup and original DSB/TLBI prefix |
| P10B | `0xfffffff0073fe330`, immediately after legacy BCM STR | Same prefix plus original STR, before following ISB/TLBI/IC |
| P10C | `0xfffffff0073fe374`, original SCTLR write | Full original pre-MMU prefix, without P9M console mapping |

Each candidate first logs phase 10 through the preserved P9 ARM chain, then
runs the original prefix up to its one terminal branch. The terminal probe must
convert the already-virtual SP using `SP - x22 + x23` before any stack access,
append phase 11 with actual M=0 SCTLR and required previous phase 10, then issue
direct SYSTEM_RESET. It must not enable MMU or call map_console. Record failure
must retain the refusal halt. A returned reset can use existing phase 14.

Only entry branch, selected terminal branch, verified empty cave and crypto HMAC
may change from the pinned baseline. Original STR stays present for B/C and is
not executed by A. No normal original-code continuation after a terminal hook
is claimed. Place explicit source/image audit assertions at each displaced site.

Test A first. If A has ENTRY only, split the earlier prefix at stack/argument
setup and page-table completion. If A's terminal record survives, test B. If B
fails to log, the range includes the STR, pending-transaction barriers and probe
execution; STR transaction causality is not proven by absence alone. If A/B
succeed, C separates the remaining original prefix from P9M's mapping wrapper.

## Required checks before hardware use

- Preserve entry ABI and execute actual original firstcpu prefix in the host
  check with explicit privileged-register/MMIO mocks; separately validate the
  real terminal append, SP conversion and reset behavior.
- Compare resulting record chains/CRCs against the actual linked P9 reader.
- Audit the complete packaged image, crypto seal, menu variant/hook identity,
  and P9-matching nonpayload firmware modules/compact SEC.
- Keep all new artifacts private. Record build and source hashes before flash.
- boot_b only, fresh Query/ARM, one kernel launch, then immediate read-only
  reader capture. Preserve continuous recording when available.

Status: A/B/C built and host verified. A flashed only to boot_b and rebooted to menu; manual kernel observation pending. B/C unflashed. Exact hashes in `../research/v23p10-prefix-controls.json`.

## Implementation and host evidence

- Public builder: `scripts/build-v23p10-kernel.py`, assembly: `scripts/v23p10-prefix.S`.
- Each probe is 900 bytes. CRC writer source from `.macro CRC` onward is exactly P9.
- Eighteen cases execute the actual entry branch and original firstcpu instructions, including original page-table writes and the TTBR0 helper, then the real terminal writer. Tests assert incoming x0/SP/NZCV restored, physical stack accesses, guarded console bounds, expected A=0/B=1/C=1 mocked BCM stores, and no reachable SCTLR write. Rejected ARM, terminal M/CRC rejection and returned reset are covered.
- Thirteen linked-reader cases check Query/ARM gates and each actual reader鈫抩riginal-prefix/kernel鈫抐rozen-reader flow; frozen ARM/ENTRY/phase11 counts are 1/1/1, later rows zero. Reader bytes equal P9.
- Privileged operations/barriers/MMIO/SMC/UEFI are mocked; boot args are synthetic. These are software checks, not hardware retention or completion results.
- Final packed FD/FAT/menu/kernel/ADT/EFI and HMAC are verified. Nonpayload FV sections and compact SEC match P9 and P8. Independent firmware output profiles preserve prior artifacts.

## First hardware trial: A only

1. Enter fastboot with USB and prepare continuous recording. Flash only the audited P10A boot_b image and verify B active/bootable. The firmware boots an untimed menu with reader selected; kernel launch is manual.
2. Open **P9 reader FIRST** and photograph the whole page, especially frozen ENTRY and NOW validity. The reader title remains P9 deliberately; the parent menu identifies P10A and its exact hook.
3. With NOW VALID, select **Query PSCI + record (NO RESET)**, then **ARM next kernel attempt** once. Photograph the complete page, this-run ID/CRC and ARM readback OK.
4. Return to menu; select **V23P10A: STOP before BCM STR** exactly once. Retain the recording through any restart.
5. After restart, open reader FIRST and photograph the complete page before Query/ARM or another kernel launch. Match this run's ARM CRC and ID, then inspect ENTRY and **PRE_MMU (phase11)**. In P10 phase11 means **named prefix stop**, not console mapping or MMU enabled.
6. If it stalls, preserve the last display/video before recovery. Never reuse a consumed ARM chain for a second launch. If NOW INVALID, stop and capture the full page; do not initialize merely to continue.

An A ENTRY-only result calls for an earlier split. Advance to B after an observed matching A terminal record; advance to C after B. Do not classify an A/B difference as proof of BCM transaction causality.

## First A device operation

V23P10A was flashed only to boot_b after exact image SHA256, fastbootd product/unlock/slot-count/partition-size gates. Image SHA256=6637e8055846c79d64f09a8115d311cc92c4ac8ba91977ec9dd81f4ef9c98e0b. B activation, current-slot b and slot-unbootable:b=no were verified; reboot command succeeded. User confirmed fastboot and recording readiness. Only the untimed menu was launched; no manual Query/ARM, kernel execution, terminal record or reset retention is observed yet. B/C remain unflashed.

## P10A initial read-only baseline

P10A initial reader photo shows common J716F V23P9 KERNEL TRACE r1 reader; parent-menu variant is not visible and expected A identity is tied to the recorded flash. Frozen ENTRY/NOW VALID: DBGC=0x43474244, next=size=0x2e137, whole-console CRC=0x5104221a. Logical/raw record counts are both zero; latest ID/phase/CRC and all six frozen checkpoint rows are zero. THIS RUN ID=1/query=0/armed=0/CRC=0 at EL1, read-only status; PSCI/features zeros are unqueried defaults. This is a valid starting baseline after the fastboot path, not a controlled retention result or proof of P10 kernel execution. Fresh Query/ARM and kernel launch remain pending.
Next: In this valid reader invocation ID1 select Query PSCI + record (NO RESET), then ARM next kernel attempt once after Query succeeds. Photograph complete ARM page with NOW VALID, THIS RUN ID/CRC and ARM readback OK before kernel selection. Stop and preserve the page if Query/ARM fails or header becomes INVALID.

## P10A Query/ARM ID1

P10A ARM photo confirms NOW VALID next=size=0x2e277 (+0x140 from baseline), logical/raw counts=5/5, THIS RUN ID=1/query=1/armed=1, CRC=0xde027ae6, EL1, PSCI_VERSION=0x00010001 and SYSTEM_RESET FEATURES=0. ARM readback OK is visible. Frozen ENTRY remains next=size=0x2e137/CRC=0x5104221a and all frozen checkpoint rows are zero, as expected before these writes. Reconstructing phase9 with ID1/cookie=0x50394b45524e454c/prior next=size=0x2e237 reproduces CRC DE027AE6. No kernel launch or reset is observed in this photo; use ID and CRC together to identify this A attempt.
Next: Continue recording. Return to menu and select V23P10A: STOP before BCM STR exactly once, without Query/ARM again. After reboot open reader FIRST and photograph full frozen ENTRY before new writes; match ARM ID1 CRC DE027AE6 and inspect ENTRY/PRE_MMU (P10 prefix stop). If it stalls preserve last display/video before recovery.

## P10A terminal prefix result

P10A continuous video confirms A menu/kernel identity, green handoff band, screen loss, Lenovo/Android splash, return to A menu and read-only reader reentry. At frame240 (~7.999s), frozen ENTRY/NOW VALID next=size=0x2e2f7, whole-console CRC=0x4e5c6046, logical/raw counts=7/7, latest ID1/phase11/CRC=D8F17B33. ARM/ENTRY/PRE_MMU each have logical/raw hits 1/1: ARM DE027AE6 matches prelaunch photo, ENTRY F3461909 and terminal phase11 D8F17B33 both have SCTLR=0xc00808 (M=0). Reconstructed record CRCs match all three. POST/MAP_FAILURE/RESET_RETURN have no hits; new reader ID2/query0/armed0 and read-only status preserve the first snapshot. PMIC changes 0x02000001 to 0x02000041 without independent reset-source attribution. In A phase11 is the terminal prefix stop BEFORE BCM STR; it proves this prefix/physical-stack/logger path and one observed reset retention cycle. A does not execute BCM STR, map_console or MMU enable. Advance to already-built B with a fresh Query/ARM; B/C remain unflashed.
Next: Prepare fastboot and recording for already-built P10B (original STR executed, terminal branch before following ISB). Flash only boot_b; reader FIRST baseline; fresh Query/ARM and photo; one B launch then immediate read-only reader capture. No further A retry required.

## P10B device launch

V23P10B was flashed only to boot_b after exact image SHA256, fastbootd product/unlock/slot-count/partition-size gates and the observed A terminal-record prerequisite. B image SHA256=e77d5b67e8b6a044ef1ca1df10e143f07bc78e060ad371214c5634bd9f690cb4. B activation, current-slot b and slot-unbootable:b=no were verified; reboot command succeeded. User confirmed fastboot and recording readiness. Only the untimed menu was launched; B menu identity, initial reader, fresh Query/ARM and kernel/BCM terminal result await observation. A retained phase11 result remains preserved; C remains unflashed.
Next: At the P10B menu open P9 reader FIRST and photograph full frozen ENTRY/NOW before any Query/ARM. If NOW VALID, prepare a fresh B Query/ARM once and photograph ID/CRC before launching B exactly once. The A ARM is consumed and must not be reused.

## P10B initial reader baseline

P10B initial reader photo shows common P9 reader, expected B identity tied to recorded flash (parent menu not visible). ENTRY/NOW VALID next=size=0x27fe9, whole-console CRC=0x2394a7e1, logical count0/raw count7. Frozen checkpoint ID1 ARM/ENTRY/PRE_MMU have L0/R1 with raw CRCs DE027AE6/F3461909/D8F17B33 matching historical A; later rows are zero. THIS RUN ID2/query0/armed0/CRC0 at EL1 and read-only status; PSCI/features zeros are unqueried defaults. A bytes are raw-only outside current logical extent, not B progress. Header changes across intervening fastboot/boot are not isolated to a cause; A immediate observed retention remains independently established. Next fresh B Query/ARM; no B kernel launch or retained terminal result yet.
Next: In this NOW VALID reader invocation ID2, Query PSCI + record (NO RESET), then ARM next kernel attempt once after Query success. Photograph full ARM readback page including actual ID/CRC and NOW VALID before B launch. Frozen raw ID1 rows are historical A; identify B using fresh ARM ID and CRC.

## P10B Query/ARM ID2

P10B Query and ARM photos confirm fresh reader session ID2. Query: NOW VALID next=size=0x280e9, logical/raw=4/11, query1/armed0, CRC=DCAE06F5, PSCI1.1/FEATURES0 and Query OK. ARM: NOW VALID next=size=0x28129, logical/raw=5/12, query1/armed1, CRC=A59682B0 and ARM readback OK. Both phase8/phase9 CRCs reconstruct exactly from ID2/value/prior headers. Frozen ENTRY remains 0x27fe9/CRC2394A7E1 and frozen rows still describe old raw A ID1, as designed. No B kernel launch or reset is shown. Match the coming B attempt using both ID2 and ARM A59682B0, distinct from historical A.
Next: Continue recording. Return to menu and select V23P10B: STOP after BCM STR before ISB exactly once. Do not Query/ARM again before launch. After reboot open reader FIRST before writes and photograph full page, matching B ARM ID2 CRC A59682B0 then ENTRY/phase11. If stalled preserve last screen/video before recovery.

## P10B launch video and reported later reboot

P10B 22.648s video starts on NOW VALID armed ID2/CRC A59682B0, then reader returns through EFI diagnosis START IMAGE/Success/code0 before B kernel selection. B menu and STOP after BCM STR before ISB identity are confirmed. Green handoff band and DarwinBoot inputs validated; exiting boot services remain visible through final frame678 (~22.615s). User reports a later automatic reboot into UniXNU, but supplied clip ends before that later cycle and has no post-reboot reader. Earlier EFI Success is reader return, not B terminal reset evidence. B intentionally resets after successful phase11 append, so reboot alone neither proves nor disproves terminal/BCM progress. Need first read-only post-reboot reader matching B ID2/ARM A59682B0; C stays gated.
Next: After the reported B reboot, open reader FIRST and photograph entire frozen ENTRY/NOW before Query/ARM or another kernel launch. Match B ARM ID2/A59682B0 and inspect ENTRY/phase11/RESET_RETURN. Do not mark B passed/failed or advance C based on reboot alone.

## P10B raw terminal only; C remains gated

B post-reported-reboot reader photo matches fresh ID2 ARM A59682B0 logically/raw. ENTRY/NOW VALID next=size=0x28169, whole-console CRC=F03B33AB; logical6 latest phase10 CRC DCD7DF11, raw14 latest phase11 CRC C49C5711. Frozen ENTRY L1/R1 and PRE_MMU L0/R1 both value SCTLR=0xc00808 (M0); reconstructed ID2 phase10/prior28129 and phase11/prior28169 CRCs exactly match. New reader ID3/query0/armed0 is read-only. This is new B raw terminal data, distinct from old A CRC D8F17B33. The audited path crosses original BCM STR into terminal writer, but phase11 is not published in current logical extent: expected next=size after commit would be 0x281a9. Source order is record byte stores -> DSB SY at 0xfffffff0073fc7d0 -> byte readback -> size/next stores at fc828/fc82c -> DSB SY fc830 -> return -> reset DSB/SMC. Raw bytes plus old header do not isolate exact PC; first DSB, readback, publication/visibility and reset-retention differences remain candidates. Direct SYSTEM_RESET and external BCM transaction completion are unverified. Supplied video does not include later reboot; user reports it. Keep B terminal_record_survived_observed_reset false and C gated; isolate writer publication with an independent terminal-only control.
Next: Preserve this B reader without another launch. Diagnose terminal record publication separately: post-body DSB SY, byte readback and header writes/retention. Prepare a single-factor B writer control before C; keep entry writer unchanged, retain CRC/header binding and do not initialize/repair the device log to turn raw-only into a pass. Next firmware compilation must also honor docs/NEXT_MENU_BUILD.md.
