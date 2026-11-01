# V23P11: terminal record publication stages

## Status and purpose

A/B/D are built and host verified. A retained published stage5 over an observed reboot. B and D each retained matching raw-only stage1 with logical header at KERNEL_ENTRY over their observed reboots; no later stage or terminal publication is established for either. D's single first-DSB-to-DMB substitution showed no retained-progress improvement in these trials. Next: independent non-overwriting progress witnesses; exact PC and reset source remain unresolved. Historical pending notes below are superseded by the final D observation. J716F remains in
early kernel entry bring-up. Sustained MMU-on execution is not established.
Exact image and source hashes are in
[the text evidence](../research/v23p11-commit-publication.json).

P10A retained a complete terminal record before the legacy BCM STR. P10B
retained a matching raw phase11 record after that STR, while its logical
header remained at KERNEL_ENTRY. This narrows the investigation to record
publication or visibility/retention; it does not identify a precise PC or
prove that the external BCM transaction completed. Preserve all P10 evidence.

## Controls

| Image | Terminal hook | First terminal record barrier |
| --- | --- | --- |
| P11A | Before original BCM STR (`0xfffffff0073fe32c`) | DSB SY |
| P11B | After original BCM STR, before original ISB (`0xfffffff0073fe330`) | DSB SY |
| P11D | Same hook as B | DMB SY |

A establishes the new writer's control result. Test B after a matching A
stage5 result survives an observed reboot. D is a later single-instruction
barrier control, selected after reviewing B. P10C remains gated. B/D probes
differ by exactly one DSB-to-DMB instruction; image/kernel hashes also differ
because of packaging and the required crypto reseal. A/B use identical probes
and different terminal branches. Normal kernel continuation is disabled.

The entry writer and its validation/CRC source remain P9. The new terminal
writer reuses one phase11 slot and updates its value with a fresh CRC:

| Last 64-bit value | At least this stage has executed |
| --- | --- |
| `5031315300000001` | Record body stores, before the first barrier |
| `5031315300000002` | First barrier, followed by stage2 record stores |
| `5031315300000003` | Full record byte readback, followed by stage3 stores |
| `5031315300000004` | Header size/next stores, followed by stage4 stores |
| `5031315300000005` | Final terminal DSB, followed by stage5 stores |

The unchanged reader labels phase11 **PRE_MMU**. In P11 its value is a P11S
stage code, **not SCTLR**, a console mapping result or MMU-on evidence.
KERNEL_ENTRY still holds actual SCTLR. In the host model stages1–3 are raw-only;
stages4–5 are within the logical extent too. Hardware header visibility and
cross-reset preservation still need observation.

Progress record rewrites are not atomic. Reset during a rewrite can leave no
CRC-valid phase11. Missing stages are inconclusive; a surviving code is a
lower bound, not the exact stopping PC. Instrumentation adds stores/CRC work,
so compare P11 controls with one another. Stage5 does not by itself establish
SYSTEM_RESET execution, external MMIO completion or the reset source.

## Host verification

- 40 actual-assembly cases: original prefix and page-table writes, guarded
  stack/console accesses, ARM/CRC rejection, stages1–5, readback mismatch,
  saturated ring/wrap, returned reset and exact B/D instruction difference.
- 18 linked reader cases: read-only defaults, Query/ARM gates, actual
  reader-to-kernel-to-reader flow, frozen snapshots, and raw/logical stage scans.
- 22 linked power application cases: reboot/shutdown arguments, invalid
  options, CurrentEL/version/features gates, zero unused SMC inputs,
  nonreturning/returned calls and ABI preservation.
- Actual FD/FAT/EFI/kernel/ADT, source hashes, HMAC and Android boot image
  fields audited. All nonpayload FV sections and compact SEC match P9/P8;
  the linked reader matches P9 exactly. Firmware version string remains P9
  to preserve those module identities; the parent menu identifies P11A/B/D.

UEFI services, system registers, barriers, MMIO and SMC are mocked. These
checks do not establish hardware barrier completion, power actions or retention.

## Menu additions

The untimed menu defaults to reader; no automatic kernel start. **Reboot** and
**Power off** invoke the independent `J716fPowerAction.efi` with explicit args.
Each invocation queries PSCI VERSION and FEATURES for its action, then uses
SYSTEM_RESET (`84000009`) or SYSTEM_OFF (`84000008`) with unused x1–x17 zero.
There is no RESET2 path. Returned actions display failure and return
EFI_DEVICE_ERROR. The new app is host verified; its device power-off behavior
is untested. Do not use either menu action during checkpoint evidence collection.

## First device trial: A

1. Prepare fastboot and continuous recording. Flash only the audited P11A
   image to boot_b, verify B active/bootable, then boot the untimed menu.
2. Confirm the parent menu says V23P11A. Open **P9 reader FIRST**, photograph
   the entire frozen ENTRY/NOW page before any Query/ARM.
3. If NOW is VALID, Query once, ARM once, and photograph the actual ID/CRC
   plus ARM readback OK. If invalid or rejected, capture the page and stop;
   do not initialize or repair the log to continue.
4. Return to the menu, select P11A once, and keep recording through any
   restart. Each attempt needs a fresh Query/ARM; do not relaunch a consumed chain.
5. After reboot, open reader FIRST and photograph the whole page before any
   new writes. Match ARM ID/CRC, KERNEL_ENTRY, phase11 raw/logical counts,
   P11S value, header next/size and RESET_RETURN. If stalled, preserve the last
   display and video before recovery.
6. Review this evidence before B; review B before deciding whether D is useful.

No partition write, device test, Git commit or push occurred in the build task.

## P11A device operation; kernel result pending

V23P11A was flashed only to boot_b after exact SHA256, fastbootd product/unlock/slot-count/partition-size gates. B active, current-slot b and slot-unbootable:b=no were verified; reboot command succeeded. Only the untimed menu was requested; menu identity, initial reader, Query/ARM, kernel launch and retained P11 stages remain unobserved. P11B/D remain unflashed; P10C gated. Image SHA256=e19591e47ae931348d1c1e5a8f01ab9a2e397a2108c21cb0a9b75af187bdbea5.
Next: At P11A menu open P9 reader FIRST and photograph full frozen ENTRY/NOW before Query/ARM or power actions. Verify menu identity and log validity before fresh Query/ARM and one A launch. B/D unflashed; P10C gated.

## P11A initial read-only reader

P11A initial reader photo shows common P9 reader, ENTRY/NOW VALID next=size=0x2baf2, whole-console CRC32=413A1021, logical0/raw7. Raw ID1 ARM DE027AE6, ENTRY F3461909 and phase11 D8F17B33 (value SCTLR 0xc00808) match historical P10A; no P11S stage code. THIS RUN ID2/query0/armed0/CRC0 at EL1, read-only. Parent P11A menu identity not visible; expected variant comes from recorded flash. No current kernel or retention result.
Next: In this NOW VALID reader invocation ID2, Query PSCI + record (NO RESET), then ARM next kernel attempt once after Query succeeds. Photograph the complete ARM page with actual ID/CRC and ARM readback OK before launching P11A. Historical raw ID1/DE027AE6/F3461909/D8F17B33 is P10A, not P11 progress.

## P11A fresh Query/ARM ID2

P11A Query/ARM photo confirms NOW VALID next=size=0x2bc32 (+0x140), logical5/raw12, THIS RUN ID2/query1/armed1/CRC58E2DBCF at EL1, PSCI1.1 and SYSTEM_RESET FEATURES0. ARM readback OK. Reconstructed phase9 ID2/cookie/prior0x2bbf2 CRC matches exactly. Frozen ENTRY remains 0x2baf2/CRC413A1021 with historical P10A raw ID1 rows; no kernel launch or P11S stage yet.
Next: Continue recording; Return to menu then select V23P11A: STOP before BCM STR once, without another Query/ARM. Record through any reboot; open P9 reader FIRST and photograph complete frozen ENTRY/NOW before new writes. Match ID2 ARM 58E2DBCF then KERNEL_ENTRY and P11S phase11 stage value. If stalled preserve final display/video before recovery. B/D remain gated.

## P11A stage5 retained across observed reboot

P11A 10.565s video confirms A before-BCM menu selection, screen loss, Lenovo/Android splash, return to A menu and manual read-only reader. Frame316 (~10.532s): ENTRY/NOW VALID next=size=0x2bcb2, console CRC94CC810C, logical7/raw14, ID2 phase11 CRC43AA3200. Frozen ARM/ENTRY/PRE_MMU each L1/R1: ARM58E2DBCF matches prelaunch, ENTRY AAC67917 value SCTLR0xc00808 (M0), terminal value5031315300000005 stage5. All three CRCs reconstruct exactly; new invocation ID3/query0/armed0 read-only. One observed reboot preserves the new writer stage5 and logical header before BCM STR. PMIC02000001->02000041; independent reset attribution unknown. No original BCM store, MMU enable, sustained XNU or power-app result. P11B is next; D/P10C remain gated.
Next: Enter fastboot and prepare continuous recording for the already-built P11B only. After boot_b flash, photograph reader FIRST, fresh Query/ARM ID/CRC, then one B launch and immediate read-only reader capture. Compare P11S stage/header visibility with A. Review B before considering D; P10C remains gated.

## P11B device operation; observation pending

V23P11B was flashed only to boot_b after observed P11A stage5/ARM/CRC/reset-retention prerequisite and exact image SHA256, fastbootd product/unlock/slot-count/partition-size gates. B activation, current-slot b and slot-unbootable:b=no verified; reboot command succeeded. User confirmed fastboot/recording readiness. Untimed menu only; B menu identity, initial reader, fresh Query/ARM and kernel/stage observations pending. Original BCM STR is present in B, but its execution has not yet been observed in this trial. A result preserved, D/P10C gated. B image SHA256=94c97bacfef84deab3bceaacfee5f1f95b20f4e74a57c01f9beb5bbabc64a40c.
Next: At the P11B menu open P9 reader FIRST and photograph full frozen ENTRY/NOW before Query/ARM or menu power actions. Confirm B menu identity and current log validity, then fresh B Query/ARM ID/CRC before one kernel launch. Historical P11A records must remain distinct. D and P10C remain gated.

## P11B initial read-only reader

P11B initial reader photo confirms ENTRY/NOW VALID next=size=0x2c55a, console CRCAC4189D6, logical0/raw7. Raw ID1 ARM DE027AE6, ENTRY F3461909 and phase11 D8F17B33/value0xc00808 match historical P10A, not B progress. THIS RUN ID2/query0/armed0/CRC0 at EL1 read-only. Parent B menu identity not visible; expected B follows recorded boot_b flash. Fresh B ARM CRC required because ID2 is reused after the intervening fastboot/boot path; earlier A immediate stage5 retention stays independently established. No B kernel launch or P11S stage observed.
Next: In this NOW VALID reader invocation ID2, select Query PSCI + record (NO RESET), then ARM next kernel attempt once after Query succeeds. Photograph full ARM readback page including actual ID/CRC before launching B. Do not reuse earlier P11A ID2/58E2DBCF; B fresh CRC must be recorded.

## P11B fresh Query/ARM ID2

P11B Query/ARM photos confirm Query NOW VALID next=size0x2c65a logical4/raw11 ID2/query1/armed0 CRC9E81CF4A, then ARM NOW VALID next=size0x2c69a logical5/raw12 ID2/query1/armed1 CRCE7B94B0F. PSCI1.1/FEATURES0 and ARM readback OK. Both phase8 and phase9 CRCs reconstruct exactly using prior0x2c61a/0x2c65a. Frozen ENTRY remains0x2c55a/CRCAC4189D6 with historical P10A raw ID1 rows. This B CRC differs from prior A ID2/58E2DBCF; no B launch or terminal stage observed yet.
Next: Continue recording. Return to menu and select V23P11B: STOP after BCM STR before ISB exactly once, without Query/ARM again. After any reboot open P9 reader FIRST and photograph full frozen ENTRY/NOW before writes. Match B ARM ID2/E7B94B0F then ENTRY and P11S phase11 stage/logical/raw/header status. If stalled preserve final display/video before recovery.

## P11B raw-only stage1; D is the next control

P11B 31.562s video confirms B after-STR/before-ISB menu identity, green handoff/DarwinBoot inputs, screen loss, Lenovo/Android splash, B menu return and read-only reader. Frame946 (~31.528s): ENTRY/NOW VALID next=size0x2c6da, CRC D603FA7F, logical6 latest phase10 F901C2DA/raw14 latest phase11 796B1792. Frozen ID2 ARM E7B94B0F and ENTRY F901C2DA(SCTLR0xc00808/M0) are L1/R1; phase11 is L0/R1 value5031315300000001 stage1. All CRCs reconstruct exactly; new reader ID3/query0/armed0 read-only. Header grew only64 from ARM, not expected terminal publication to0x2c71a. Original BCM STR was crossed and terminal body stored; external completion, first DSB completion, stage2+ execution/visibility/retention and reset source unresolved. A stage5 versus B raw-only stage1 motivates P11D single first-DSB-to-DMB control. No terminal SYSTEM_RESET, MMU-on or power-app result.
Next: Preserve the B reader/video. Prepare fastboot and continuous recording for already-built P11D, the B control with only terminal first DSB SY changed to DMB SY. Flash boot_b only; reader FIRST baseline, fresh Query/ARM ID/CRC, one D launch, immediate read-only reader capture. Compare stage and logical header against B; keep P10C gated. No additional B launch required now.

## P11D device operation; observation pending

V23P11D was flashed only to boot_b after reviewed B fresh raw-stage1/ARM/CRC/observed-reboot prerequisite and image SHA256, fastbootd product/unlock/slot-count/partition-size gates. B/D assembled probe difference rechecked: only terminal first DSB SY changed to DMB SY; subsequent barriers retained. B activation, current-slot b and slot-unbootable:b=no verified; reboot command succeeded. User confirmed fastboot/testing readiness following recording request. Untimed menu only; D menu identity, reader, fresh ARM and kernel results pending. A/B observations preserved; P10C gated. D image SHA256=0a0b647be892107591787f05b4709286b20ab49c25125822e81a306bc65c73af.
Next: At P11D menu open P9 reader FIRST and photograph full frozen ENTRY/NOW before Query/ARM or power actions. Confirm D menu identity and current log validity, then fresh Query/ARM ID/CRC before one D kernel launch. D differs from B only by terminal first DSB SY to DMB SY; later barriers remain. P10C remains gated.

## P11D initial read-only reader

P11D initial reader photo shows ENTRY/NOW VALID next=size0x2f1fb, whole-console CRC65E3F901, logical0/raw0, frozen checkpoint ID0 and all six rows zero. THIS RUN ID1/query0/armed0/CRC0 at EL1 read-only. Valid new baseline after intervening fastboot/firmware boot; no trace records recognized and no D execution or controlled retention conclusion. Earlier A stage5/B raw-stage1 immediate reboot observations remain independently preserved. Expected D variant tied to recorded flash; parent menu not visible.
Next: In this NOW VALID reader invocation ID1, select Query PSCI + record (NO RESET), then ARM next kernel attempt once after Query success. Photograph full ARM readback page including actual ID/CRC before launching P11D. No initialization, power action or kernel launch before ARM evidence.

## P11D fresh Query/ARM ID1

P11D Query/ARM photos confirm Query NOW VALID next=size0x2f2fb logical4/raw4 ID1/query1/armed0 CRC16B5614F, then ARM NOW VALID next=size0x2f33b logical5/raw5 ID1/query1/armed1 CRC6F8DE50A. PSCI1.1/FEATURES0 and ARM readback OK. Phase8/phase9 CRCs reconstruct exactly with prior0x2f2bb/0x2f2fb. Frozen ENTRY remains0x2f1fb with all rows zero. Full-resolution photo review corrects earlier console-CRC transcription55C3F901 to65E3F901; no image/hash or device log altered. No D kernel/stage result yet.
Next: Continue recording; Return to menu and select V23P11D: after BCM STR, FIRST DMB control exactly once, without Query/ARM again. After reboot open P9 reader FIRST and photograph complete frozen ENTRY/NOW before writes. Match D ID1/ARM6F8DE50A then ENTRY, raw/logical P11S stage and header. If stalled preserve final display/video before recovery.

## P11D raw-only stage1; B/D comparison

P11D 30.83s continuous video confirms D first-DMB menu, handoff, screen loss, Lenovo/Android splash and D menu return. Frame924: frozen ENTRY/NOW VALID next=size0x2f37b, console CRC995CCD03, logical6 latest phase10/16CCB8AB, raw7 latest phase11/96A66DE3. ID1 ARM6F8DE50A and ENTRY16CCB8AB(SCTLR0xc00808/M0) L1/R1; phase11 L0/R1 value5031315300000001 stage1. All three CRCs reconstruct exactly; reader ID2/query0/armed0. Header grew64 from ARM, not terminal publication to0x2f3bb. B and D retain the same stage1-only result; first DSB-to-DMB substitution did not improve observed progress. Same-slot non-atomic updates prevent exact PC/barrier attribution. Original BCM STR crossed, external completion and reset source unknown. No terminal SYSTEM_RESET or MMU-on evidence.
Next: Preserve the D reader/video; no repeat launch needed. Design independent, non-overwriting progress slots around the first terminal barrier and stage2 construction/store so later writes cannot erase earlier witnesses. Qualify reserved storage and reader/CRC/visibility semantics, and retain before-BCM control plus single-factor barrier variants. Build/audit before a new boot_b trial. P10C remains gated; no MMU-on claim.
