# J716F device repository guide

## Current baseline

Read `docs/V23P18_BOOTSTRAP_CLOCK.md` and `research/v23p18-bootstrap-clock.json`.
P18M is the latest device milestone: original boot CPU publication/validation,
current-thread setter and rtclock_early_init return. Actual terminal
SCTLR0x3454593d/M1 at0xfffffff0079e1b18; all9 CRCs match fresh ID1/ARM209C5114,
header0x380 logical/raw14/14 retained over one video-observed reboot.
cpu_bootstrap is a RET stub; reset source independently unresolved.

P19M is built/host verified, flashed only to boot_b; reboot command succeeded. Read `docs/V23P19_STARTUP_INIT.md` and
`research/v23p19-startup-init.json`. Original debug/startup (849 callbacks in host
model), timer_call_init/cpu_init/processor_bootstrap return group; terminal
0xfffffff0079e1b34 before later arm_init/arm_vm_init. 57 kernel +6 dependency
+31 linked-reader checks. MIDR/cache/debug/DAIF/counter, SP banks, MMU aliases,
barriers, SMC and UEFI are modeled; no P19 kernel result. Next user photo of
P19 initial read-only reader FIRST, then reviewed fresh Query/ARM and single attempt.

P13–P19 are bounded diagnostics on a frozen source-built/prelinked kernel.
Timer IRQ, real WFE wait, ArmCpu/GIC, scheduler/storage/userspace remain unverified.
Separate native platform source candidate compiled/exported in CI at816e3c55;
it is not the diagnostic kernel and has no hardware IRQ qualification. Read
`docs/NATIVE_PLATFORM_SOURCE_CANDIDATE.md`. Preserve all earlier version results.

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
