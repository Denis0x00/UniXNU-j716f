# J716F device repository guide

## Current baseline

Read `docs/V23P17_CHOSEN_TOPOLOGY.md` and
`research/v23p17-chosen-topology.json` first. P17M reached original chosen/stall,
CPU topology and boot CPU number returns; actual terminal SCTLR0x3454593d/M1.
Nine CRCs bind fresh ID1/ARM6303C7DA; full logical next=size0x380 and all records
survived one video-observed reboot. This deliberately stops at0xfffffff0079e1a98,
before boot CPU publication/max-CPU check. Reset source remains independent.

P13–P18 are bounded diagnostics on a frozen source-built/prelinked kernel.
Timer IRQ, real WFE wait, ArmCpu/GIC, scheduler/storage/userspace remain unverified.
Native platform candidate is separate/single-core; actual configuration/component/
archive and ADT input checks pass; source compile/export CI passed at816e3c55, runtime pending. Read
`docs/NATIVE_PLATFORM_SOURCE_CANDIDATE.md`.

P18M now verified on boot_b: all six witnesses reach boot CPU publication/max-CPU
validation, current-thread setter and original early-clock return. Entry0xc00808/M0,
terminal0x3454593d/M1 at0xfffffff0079e1b18; all9 CRCs independently match fresh
ID1/ARM209C5114, header0x380 logical/raw14/14 retained over one video-observed
reboot. New reader ID2/query0/armed0, read-only. Read
`docs/V23P18_BOOTSTRAP_CLOCK.md` and `research/v23p18-bootstrap-clock.json`.
Preserve P17 and P18 media/source hashes. cpu_bootstrap is a RET stub, no full
cpu_init/timer IRQ claim; reset source independently unresolved. Next review
original kernel_debug_string_early/timer_call_init/cpu_init/processor_bootstrap
boundaries and stack/mapping lifetimes; no successor built/flashed.

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
