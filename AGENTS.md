# J716F device repository guide

## Current mainline (2026-10-27)

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

Selected receive-only baseline: run40448429664/attempt1 at fcf79e5624c2ab2b0d3c090a3f522fe494a89bdc,
locked in configs/native-candidate-lock.json. This artifact contains the
volatile IRQ observer, NOT the new recoverable early logger. Do not describe
it as hardware-ready after prelink alone. A logger rebuild needs its own lock.

Next outcome: exact native provenance through prelink/payload/final image plus
recoverable entry/VM/platform/exception/IRQ evidence. Then verify platform
matching -> CPU registration -> GIC -> real timer IRQ/rtclock/EOI -> single-core
scheduling on the SAME candidate. UFS/root/userspace follow that gate.
No device write/reboot is authorized by a host build or checker.

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
