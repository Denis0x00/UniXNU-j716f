# J716F device repository guide

## Current baseline

Read `docs/V23P19_STARTUP_INIT.md` and `research/v23p19-startup-init.json`.
P19M is the latest bounded device milestone: original debug/startup,
timer_call_init, cpu_init and processor_bootstrap return. All six slots L1/R1,
actual terminal SCTLR0x3454593d/M1 at0xfffffff0079e1b34. Nine CRCs independently
match fresh ID1/ARM17BBA853; header0x2cef8 logical/raw13/13 retained over one
continuous-video-observed reboot; first reader ID2/query0/armed0.
Terminal intentionally requests SYSTEM_RESET; reset source independently unresolved.
Host849 callbacks/27 functions are not device callback measurements.
No arm_vm_init yet. Next audit remaining arm_init and VM mapping/stack lifetime.
P18 evidence remains in its version-specific document/manifest. No repeat P19 test.

P20M built/host qualified; flashed only to boot_b, B active/bootable verified,
reboot command succeeded. P20 initial reader confirmed EL1/VALID next=size0x2c11f, logical/raw0,
ID1/query0/armed0; 81621-byte tail needs no new session. Next Query/ARM once each
and photograph ID/CRC/templates before single P20M. No P20 kernel result. Read `docs/V23P20_VM_INIT.md` and
`research/v23p20-vm-init.json`. 27 kernel +5 dependency +31 linked-reader cases.
Original arm_vm_init early allocation/layout/protection returns to stop0xfffffff0073f5c04,
before TCR/TTBR replacement. Full-segment DarwinBoot placement and AT/PAR table walks
are modeled; candidate roots lack console and complete probe/stack mappings here.
No full VM or new hardware claim. Next P20 reader FIRST, fresh Query/ARM and one
recorded P20M kernel attempt after reviewed initial/ARM photos. Never rerun P19 recorders or repurpose its ARM.

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
