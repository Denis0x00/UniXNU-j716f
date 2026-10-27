# J716F device repository guide

## Current baseline

Read `docs/V23P20_VM_INIT.md` and `research/v23p20-vm-init.json`.
P20M is the latest bounded device milestone: original arm_vm_init entry, early
root/upper-table allocation, segment/page-size layout and arm_vm_prot_init return.
Stop0xfffffff0073f5c04 precedes new TCR/TTBR activation; full arm_vm_init not returned.
All six slots L1/R1; terminal SCTLR0x3454593d/M1. Nine CRCs independently match
fresh ID1/ARM8DD10BB5; VALID header0x2c45f logical/raw13/13 retained over one
continuous-video-observed reboot; first reader ID2/query0/armed0.
Terminal intentionally requests SYSTEM_RESET; reset source independently unresolved.
Candidate console/probe/stack gaps are host-model findings, not device table dumps.
Next audit original continuation, completed candidate mappings/permissions and
TCR/TTBR order, including executable probe, live stack and retained console lifetime.
No repeat P20 needed; preserve P19 and all earlier version-specific evidence.
27 kernel +5 dependency +31 linked-reader host cases remain bounded mocked checks.
Never rerun build-only recorders over results or reuse consumed ARM.

P13–P20 are bounded diagnostics on a frozen source-built/prelinked kernel.
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
