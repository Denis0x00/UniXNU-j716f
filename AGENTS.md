# J716F device repository guide

## Current baseline

Read `docs/V23P17_CHOSEN_TOPOLOGY.md` and
`research/v23p17-chosen-topology.json` first. P17M reached original chosen/stall,
CPU topology and boot CPU number returns; actual terminal SCTLR0x3454593d/M1.
Nine CRCs bind fresh ID1/ARM6303C7DA; full logical next=size0x380 and all records
survived one video-observed reboot. This deliberately stops at0xfffffff0079e1a98,
before boot CPU publication/max-CPU check. Reset source remains independent.

P13–P17 are bounded diagnostics on a frozen source-built/prelinked kernel.
Timer IRQ, real WFE wait, ArmCpu/GIC, scheduler/storage/userspace remain unverified.
Native platform candidate is separate/single-core; actual configuration/component/
archive and ADT input checks pass, compilation and runtime pending. Read
`docs/NATIVE_PLATFORM_SOURCE_CANDIDATE.md`.

Next: review grouped boot CPU publication/max-CPU validation, current-thread/CPU
bootstrap and rtclock_early_init boundaries, including logger mapping/stack lifetime.
No successor build/flash yet. Preserve first read-only P17 results.

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
