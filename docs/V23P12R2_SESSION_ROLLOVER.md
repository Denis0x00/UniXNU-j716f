# V23P12 r2: explicit session rollover for a full console

## Reason and scope

P12A r1 initial device photo showed a VALID saturated circular console:
next `0x26e9`, size/capacity `0x3fff4`, console CRC32 `50977087` and zero
recognized logical/raw trace records. Zero recognized records does not mean
empty storage. No Query/ARM or kernel trial was observed. See the preserved
[r1 record](../research/v23p12-independent-slots.json).

r2 adds only a reader-side, manually confirmed new-session operation and menu
identity. The A/B kernel, ADT and six-slot probe remain byte-identical to r1;
the A/B probe difference remains one NOP versus original-operand BCM STR.
The kernel still accepts only unused linear tail. No circular kernel writer,
new RAM reservation, MMU change, cache maintenance or reset call is added.
Host/build/flash state is recorded independently in
[the r2 record](../research/v23p12r2-session-rollover.json).

## New-session contract

- Reader entry and Refresh are read-only. Original ENTRY, checksum, ID and eight
  row snapshots stay frozen even after session rollover and Query/ARM.
- The fifth option is `New linear session (drops logical history; press twice)`.
  It is allowed only for an already VALID saturated circular header, or a
  VALID linear header lacking the existing 832-byte Query budget. Partial
  nonlinear headers, invalid headers, exhausted IDs and already eligible logs
  are refused. This operation is not invalid-header repair.
- First press only requests confirmation and snapshots current next/size and
  whole-console CRC32. Navigation cancels it. The second consecutive press
  revalidates the console and checks that all those values are unchanged.
  No header reset occurs if the log changed in between.
- Confirmation drops OLD LOGICAL HISTORY by storing next=0, DSB, size=0, DSB
  and checking the header. Old payload bytes are not bulk-cleared. Future
  records/templates overwrite their own bounded locations, so this does not
  preserve all old payload bytes indefinitely. Raw scans may find stale data.
- A CRC-valid INIT record is appended and read back at offset0 using the
  existing bounded writer. Its value is `old_size << 32 | old_next`; it is
  neither an ARM nor a kernel checkpoint. Successful session state starts at
  next=size64. ID remains the entry-time maximum logical/raw ID plus1, so old
  raw IDs are not silently reused. ID overflow blocks all new writes.
- Only one session operation is allowed per invocation. After any admitted
  Query, session rollover is blocked even if PSCI returns failure. Once the
  session header/readback or INIT append fails, Query/ARM stay locked for the
  invocation. No kernel launch should follow a failed operation.
- Session rollover does not perform SMC, restart or automatic Query/ARM.
  Successful Query and a fresh ARM still prepare six deliberately CRC-invalid
  templates, and the unchanged kernel independently validates that ARM.

The old logical log is deliberately discarded only after the manual action.
Preserve its frozen page before confirmation. No claim is made that a photo
exports the old log contents, or that stale raw bytes are retained across reset.

## Test sequence: A before B

1. Flash only the audited r2A image to boot_b, activate/verify B and boot the
   untimed menu. Confirm `V23P12R2A / BCM SKIP` and open `P12 r2 reader` FIRST.
2. Photograph the complete initial frozen ENTRY/NOW page. Do not Query/ARM yet.
3. If NOW is VALID but full/nearly full, select the new-session option. Press
   once, preserve the confirmation page, then press again without navigating.
   Photograph `New session OK`, NOW next=size64 and THIS RUN ID/CRC. Stop on
   any refusal, changed-console message or failed readback. If the initial log
   already has enough unused linear tail, skip new-session creation.
4. Prepare continuous recording. Query once, ARM once; photograph actual
   ID/CRC and `ARM + six invalid templates OK`. Do not launch on failure.
5. Return to menu and launch r2A once. After any reboot open the reader FIRST
   and photograph the full frozen page before Query/ARM or power operations.
   Match fresh ARM CRC, ENTRY binding, six slots and logical header publication.
6. Review A's matching six-slot, publication and observed-reboot result before
   r2B. B uses the same reader and unchanged B kernel. Fresh Query/ARM is required
   for every attempt; the B flasher enforces reviewed r2A prerequisites.

Reboot/Power off remain available through the unchanged P11 power application.
Avoid them while collecting evidence. P10C remains gated. This probe does not
enable the MMU or establish the source of any device reset.

## Verification limits

Actual linked EFI UI tests exercise session confirmation/refusal/cancellation,
mutating console and injected failure paths, stale IDs and both unchanged
assembled kernels followed by frozen reader reentry. UEFI services, rendering,
SMC, MMIO, privileged registers and barriers are mocked. These tests establish
software behavior, not physical persistence or hardware progress. Kernel/power
cases are reused only after byte identity is checked; the new reader is tested
again. Final packaging checks embedded FD/FAT/EFI, source hashes, kernel/HMAC,
ADT, nonpayload identities and Android boot image structure.

## Device operation

V23P12R2A BCM SKIP flashed only to boot_b after image SHA256/matched-probe/fastbootd identity gates. B active and bootable verified; reboot command succeeded. Menu-only launch; no independent recording-ready confirmation. Initial r2 reader, manual new-session action, Query/ARM and kernel observations pending. r2B remains unflashed and gated by reviewed r2A six-slot/header/reboot evidence. No new kernel progress, retention or MMU-on result.
Next: Open P12 r2 reader FIRST and photograph full frozen ENTRY/NOW before Query/ARM or new-session action. Review NOW validity/space; if VALID saturated/exhausted, manually confirm New linear session twice only after saving the old page, photograph new-session success, then fresh recorded Query/ARM and one r2A launch. B and P10C gated.

## Initial r2A reader

P12 r2A initial photo confirms r2 reader at EL1; ENTRY/NOW VALID next=size0x38b69, consoleCRC98919B86, logical0/raw0, frozen checkpointID0 and all eight rows zero. THIS RUN ID1/query0/armed0/CRC0. Unused linear tail29835 bytes exceeds Query832-byte budget; no session rollover needed or performed. No kernel launch, r2 retention or hardware rollover qualification; parent A menu not visible, variant follows recorded flash. Earlier r1 saturated baseline stays independent.
Next: In current r2 reader ID1, prepare continuous recording; Query PSCI + record (NO RESET) once, then ARM next kernel attempt once only after successful Query. Photograph full Query and ARM readback including actual ID/CRC and six-template-OK. Skip New linear session; do not launch kernel before fresh ARM evidence is reviewed. B/P10C gated.

## Fresh r2A Query/ARM

P12 r2A Query/ARM photo shows NOW VALID next=size0x38ca9 logical5/raw5, THIS RUN ID1/query1/armed1 CRC063FE873 at EL1, PSCI1.1/FEATURES0 and ARM + six invalid templates OK. Frozen initial ENTRY remains0x38b69/CRC98919B86 with zero rows. Header growth320 bytes matches four Query records plus ARM; ARM phase9 CRC reconstructs exactly with cookie5031324b45524e4c and prior0x38c69. No session rollover or kernel launch observed. Fresh ARM binding saved for one A launch; expected terminal header0x38e69 is derived, not observed.
Next: Continue recording; Return to menu and select V23P12R2A BCM SKIP once, without further Query/ARM/session writes. Record through any reboot, then open P12 r2 reader FIRST and photograph full frozen ENTRY/NOW before writes. Match ID1 ARM063FE873, ENTRY binding, six independent slots and header publication (derived terminal next0x38e69). If stalled preserve video/final display before recovery. B/P10C gated.

## r2A six-slot result

P12 r2A postreboot photo confirms ENTRY/NOW VALID next=size0x38e69 consoleCRCC067D0A6, logical12/raw12 latest ID1/phase21/04FFEC87. Fresh ARM063FE873 matches prelaunch; ENTRY1210179B value063FE87300C00808(M0). All six stages16..21 are L1/R1 with CRCsE66CD221,AA22646F,B1021655,9AB5746F,8742F221,04FFEC87 and ARM-bound value50313253063FE873; all eight CRCs reconstruct exactly. New reader ID2/query0/armed0, read-only. User explicitly reports automatic reboot/menu return; no continuous video reviewed. A skipped BCM and reached all six commits/publication, with matching records present after reported reboot. Kernel SMC/reset source, original BCM completion, MMU-on and manual session rollover remain unqualified. r2B original-STR control is next.
Next: Preserve A reader photo; enter fastboot and prepare continuous recording for audited r2B only. Flash boot_b only, verify B active/bootable, then P12 r2 reader FIRST photo, fresh Query/ARM ID/CRC/templates photo, one B launch and immediate read-only reader capture. Compare six slots and header with A. No more A launch needed; P10C gated.
