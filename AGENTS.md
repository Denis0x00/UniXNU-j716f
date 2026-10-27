# J716F device repository guide

## Current baseline

Read `docs/V23P21_VM_SWITCH.md` and `research/v23p21-vm-switch.json`.
P21M is the latest bounded device milestone: guarded console bridge, original
TTBR1/TTBR0 setter returns, and original TLBI/DSB/ISB stop0xfffffff0073f5e0c.
Full arm_vm_init has NOT returned; borrowed bootstrap console L2/L3 lifetime is
qualified only through this immediate stop, not later allocation/reuse/pmap.
All six slots L1/R1; nine CRCs independently match fresh ID1/ARM145911AB.
ENTRY/NOW VALID next=size0x380 logical/raw14/14 retained over one video-observed
reboot; first reader ID2/query0/armed0. Terminal SCTLR0x3454593d/M1.
Terminal requests SYSTEM_RESET; reset source independently unresolved. No direct
TTBR/TCR/table dump, complete mapping/cache-coherence, IRQ or scheduler claim.
Next audit original continuation and remaining arm_vm_init return, preserving
probe/stack/console mappings and especially borrowed-subtable lifetime before
adding grouped checkpoints. No repeat P21 needed; preserve P20 and older evidence.
P21 host checks26 kernel +5 dependency +31 reader remain bounded mocked checks.
TCR-aware P20 walker correction preserved: T1SZ26 =>38-bit VA/eight-bit L1 index;
historical high probe/stack gap was a host bug; console bridge added only empty
new low-root L1 slot2, borrowing bootstrap console-only L2/L3.
Never rerun build-only recorders over results or reuse consumed ARM.

P13–P21 are bounded diagnostics on a frozen source-built/prelinked kernel.
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
