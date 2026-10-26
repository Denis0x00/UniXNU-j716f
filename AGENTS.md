# J716F device repository guide

## Current baseline

Read `docs/PROGRESS_AFTER_V23P16_20261026.md`, `docs/V23P16_TIMER_WFE.md` and
`research/v23p16-timer-wfe.json` first. P16M original WFE configure/init and misc
return checkpoints plus CNTKCTL_EL1=0x4f survived one video-observed reboot.
The diagnostic intentionally stops before /chosen. Timer IRQs, real WFE wait,
ArmCpu/GIC, sustained initialization, reset source and userspace are unverified.

P13–P16 are terminal instrumentation on a frozen source-built/prelinked baseline.
The native platform candidate now enables ArmCpu/ArmDTPlatform and their archive recipes;
actual Context/component/archive-plan checks pass, compilation and hardware are pending.
Read `docs/NATIVE_PLATFORM_SOURCE_CANDIDATE.md`.
P17M grouped /chosen/stall/topology diagnostic is built and flashed only to boot_b.
Initial P17 reader is observed: VALID saturated ring, no P17 records, query0/armed0.
Explicit new linear session succeeded; INIT CRC reconstructed.
Next: fresh Query/ARM photo and one P17M launch; native platform remains separate/single-core.

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
./unixnu defconfig j716f_source_defconfig -D ../UniXNU-j716f
./unixnu status --json -D ../UniXNU-j716f
./unixnu tree -D ../UniXNU-j716f
python3 scripts/porting-record.py check --device ../UniXNU-j716f
```

Use a private local configuration for input paths. Build kernel, package, firmware,
payload and boot image in dependency order; source compilation alone is not a
prelink, hardware boot or userspace result.
