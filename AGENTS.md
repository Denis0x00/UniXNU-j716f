# J716F device repository guide

## Latest device checkpoint (2026-10-29)

Read `research/native-maint-complete-20261029.json` first. The new video alone
yielded256/256 frames /262144 bytes on the first pass; independent reconstruction,
all frame CRCs and whole SHA5583ff6e...197e18811 /CRC B6D19E53 match. Pinned
profile fb6ea084/candidate1c19ee51/transfer000000039CA7ACD9 match the prior photo,
whose first-frame text is exactly equal. Raw/frames/report are fsynced/read back
and copied to a second private directory on the SAME volume. Original video
also copied/hash-checked. Do not ask to re-record this already complete snapshot.
Header00B7266B/8E025595/2A002821 remains INVALID; exact current native-session,
event and Init markers are absent across all bytes. No Init token, ARM or native
result; private receipt.json contains a NULL token, not permission to initialize.
User reported complete power-off/on before Read; this is not a retention/crash
test. Video at264s still shows PLAY; current live pause state is not confirmed.
Keep the app paused; no Init/ARM/native/exit/reset/reflash. Next review explicit
cold-start session establishment and region ownership on the SAME kernel/ADT/ABI.
Do not simply relax INVALID_HEADER, reuse a receipt for changed live bytes or
start successor probes. Preserve earlier partial/photo/build records unchanged.
The older valid-full/exhausted-header Init instructions below do not apply.

## Current mainline (2026-10-27)

LATEST CHECKPOINT supersedes the earlier plan-only / Init-only action wording below:
user entered fastboot and authorized the recommended minimal maintenance test.
Read `docs/NATIVE_MAINTENANCE_V2.md` and
`research/native-maintenance-v2-20261027.json` first. MAINT-v2 image627dc31c
(8380416 bytes) is built, reverse-audited and boot_b-only flash/B activation/reboot
commands succeeded. Menu/Read photos and a fresh complete J716F2 export are pending.
No physical Init, Query/ARM or native launch occurred. Do not repeat the flash.
Keep the selected kernel150c5c5f / ADT806ad48c / native ABI unchanged.
Entry3 now owns Export and Init in ONE still-open EFI invocation; entry2 remains
historical read-only v1 Export and cannot produce a v2 receipt. No menu-owner
protocol/NVRAM ticket was added. Profile fb6ea084...18946537 binds the new inputs.
GCC/Clang production tests, 14 maintenance + 14 Read/ARM linked-EFI simulations,
actual QR independent decode and v2 Read readback passed on host only. All 961
shared dependency inputs match v1; the documented dirty dependency limit remains.
Old unexported0x3f56e was interrupted by the user's reboot; never claim it backed
up or reuse the old receipt. Require new256/256-frame durable export in this
invocation, receipt and two direction-separated confirmations before Init.
Then photograph independent Read; fresh Query/ARM and native launch need current
onscreen review. No automatic launch/reset, P22, UFS or driver upgrade. Smaller
main menu and power items are explicitly deferred to keep this change minimal.
The remainder preserves chronological checkpoints, not conflicting current actions.

P22M is CLOSED, not waiting for Query/ARM. Read
`docs/P22_CLOSED_NATIVE_MAINLINE.md`, `research/p22-closed-20261027.json`
and `docs/NATIVE_MAINLINE.md` first. Preserve the 51-file private freeze manifest
and every historical artifact; do not rerun result/build/session recorders.

P22 proves the bounded original arm_vm_init RET reached caller
0xfffffff0079e1bbc, six slots L1/R1, nine independently reconstructed CRCs,
fresh ID1/ARM B70F9702, SCTLR 0x3454593d/M1, and retention over one observed
reboot. It does not prove native drivers, IRQ, scheduling, long-lived mappings
or the reset source. No repeat P22 or default P23 function/reset probe.

The main board now selects ArmDTPlatform + ArmCpu + native timer. Legacy V17
payload settings are archived in docs/archive/board-v17.toml; P22 is a different
historical packaging path. Native packaging must never inherit timed entry
reset or binary cave patches. Keep the UniXNU source pin unchanged.

Selected native logger: run40458123294/attempt1 at
474c8a65b731d293987afdf550112f7b02f9ea32, locked separately in
configs/native-log-candidate-40458123294.json. Receive, XnuPrelink, separate
reader/ARM, dedicated FD and final Android image reverse-audit now pass on the
host. Final boot SHA256 is
801ddb86b29844e316053f4d061a9fd2cb266e47f1c27ed1d46745b036dbb8e7.
There has been NO native hardware launch, retention, IRQ or scheduling result.
The older run40448429664 lock/receipt remains historical, without this logger.

Reader-only boot_b flash/activation/reboot was separately authorized and completed.
Read `research/native-reader-40458123294-20261027.json`: both new menu and reader
are photographed; STABLE=1, next=0x2a354, size=0x3fff4. The full non-linear ring
fails the production ARM admission gate. Expected candidate words are compiled-in,
not a recovered session. No Query/ARM or XNU launch occurred. Preserve the original
host-only audit and fastboot record; this later device observation is separate.

Latest increment: the separate RAW-v1 image63399ed5 (8372224 bytes) was authorized
for boot_b-only deployment and read-only optical testing. Read
`docs/NATIVE_CONSOLE_EXPORT.md` and `research/native-console-optical-20261027.json`.
Menu/reader/Export are photographed; the full video transfer1AD5A00BB now yields
all 256 frames / 262144 bytes. Read `research/native-console-complete-20261027.json`:
SHA0ebb2cdc...25382fa and CRC1CC58AC7 independently match; raw header is
43474244/0003CBD2/0003FFF4. Initial 250 frames plus six denser-sampled recoveries
are preserved. Photo transfer71E24046 was NOT mixed into the complete capture.
Dump/frames/private receipt are fsynced/readback-verified, with a second private
directory copy. No intact unconsumed ARM under the declared ABI was found.
Kernel/ADT and Read/Arm are unchanged; previous image801ddb86 remains historical.

Latest user authorization permits Init ONLY, but the first Init photo is BLOCKED
with ready0/used0, no receipt entry and no confirmations. Read
`research/native-init-blocked-20261027.json`. User reports an intervening reboot
and fastboot visit. The new header is next=size0x3f56e, SHAa6c467c4...83d3e567,
NOT the previous full backup. Init v1 rejects non-full linear logs; aligned
remaining2688 bytes also cannot fit an8192-byte ARM session. Do not reuse the old
receipt or deliberately fill the ring. Re-export current bytes separately after
Read; re-export alone does not change this unsupported header policy. Preserve
the previous complete dump and Android-log analysis as historical checkpoints.
No new receipt, Init commit, ARM or native result exists. Supporting an exhausted
linear log requires separately reviewed implementation, not forced admission.
Do not relax admission, reuse P22 initialization, reflash or reset to pass this
gate. Query/ARM/native remain outside Init-only authorization. Physical RAM
retention and recovery after a hang remain unqualified.
Then verify platform
matching -> CPU registration -> GIC -> real timer IRQ/rtclock/EOI -> single-core
scheduling on the SAME candidate. UFS/root/userspace follow that gate.
No device write/reboot is authorized by a host build or checker.

User now asks to PREPARE the next version plan. Read
`docs/NATIVE_CONSOLE_V2_PLAN.md` and `research/native-console-v2-plan-20261027.json`.
Design only: classify full wrapped/full linear/exhausted linear; retain all
backup/receipt/pending-ARM/suspect-record/byte-compare gates, add same-menu-run
export ticket, explicit refusal reasons, smaller menu and confirmed power actions.
`scripts/check-native-init-v2-design.py` checks mathematics/abstract state only,
NOT production C or hardware. Existing v1 production sources and image remain
unchanged. No new build, push, deployment, reset, ARM or XNU test is authorized
by plan preparation. Current changed snapshot still needs separate full export.

User request for the NEXT firmware build: smaller menu text / official UniXNU
layout, plus explicit Restart and Shutdown items. Review upstream actions and
the actual J716F reset/power-off implementation first; require confirmation,
no countdown/fallback/automatic reset. Do not rebuild/reflash or alter the
current capture just to deliver this UI change. Record a new image identity.

The generic board.toml firmware route still names the historical DSC; its
preflight now FAILS CLOSED before reading vendor inputs. Do not bypass it.
Use only the documented dedicated staging/packaging scripts for this candidate.
Do not alter board.toml merely to modernize comments: it is part of the selected
logger identity. Standard initrd, Advanced Logger/QR and a clean pinned shared
firmware dependency closure remain open. The actual build used documented local
dependency modifications; its post-build hash snapshot is not a clean-rebuild claim.

Previous agent instructions are archived verbatim in
docs/archive/AGENTS-before-native-mainline.md as historical context, not actions.

## Rules

- Preserve all prior hardware evidence and version-specific source/artifact hashes.
  Never rerun build-only recorders over hardware results. Earlier notes in
  `docs/AGENT_CHECKPOINT_HISTORY_THROUGH_P16.md` are chronological, not current actions.
- Each launch requires fresh version-specific Query/ARM; consumed ARM intentionally
  halts admission. Photograph the read-only reader first after reboot, before writes.
- Hardware tests remain boot_b only. Do not write boot_a, vbmeta or userdata.
  TzDxe is outside the current scope.
- Tie physical addresses, CPU affinity, GIC ranges, reservations and firmware paths
  to checked-in evidence. Do not copy sheng/SM8550 constants into SM8250.
- Keep `socs/sm8250` marked as a transition snapshot until a reusable public source
  is reviewed and pinned. Reusable drivers belong in canonical component repositories;
  board wiring and package policy belong here.
- Publish no Apple/Lenovo binary inputs, raw DTBs, payload images, photos, raw logs,
  seeds, personal/device identifiers or credentials. Use sanitized summaries/hashes.
- `porting.toml` is authoritative. Update affected findings/evidence with changes;
  keep source, host and device evidence separate and bound to their tested revision.

## Upstream references and commands

Read the official porting, migration, device-repository, device-tree, UEFI and
bring-up guides before board changes: https://unixnu.com/zh/porting/ .
Do not silently change the pinned source-build revision. The historical board
payload recipe and independent diagnostic profiles are different build paths.

From a reviewed UniXNU checkout:

```sh
./unixnu defconfig j716f_arch_timer_source_defconfig -D ../UniXNU-j716f
./unixnu status --json -D ../UniXNU-j716f
./unixnu tree -D ../UniXNU-j716f
python3 scripts/porting-record.py check --device ../UniXNU-j716f
```

Use a private local configuration for input paths. Build kernel, package, firmware,
payload and boot image in dependency order; source compilation alone is not a
prelink, hardware boot or userspace result.
