# Upstream and peer review, 2026-10-26

## Scope and revisions

Read-only upstream/source review after the P11D observation. No new image build,
device command, dependency-pin change, Git commit or push.

| Source | Reviewed commit |
| --- | --- |
| UniXNU/UniXNU | `ae687009348296e97e49ce6e925795044f363ce6` |
| FFT-infra/UniXNU-sm8250-common | `6bb9b025704764a662728dce1a30557a35e082d8` |
| reAAAq/UniXNU-umi | `3eb72578597d2ca16471b172d45eddc096a34a04` |
| GFander/UniXNU-kebab | `a68c238ba768b3836c1ae2e67d5ade4a9025a0df` |
| xVanTuring/UniXNU-pipa | `cdb3d62771e31d48d832853ee0c90964346d78da` |
| wh1stl3/UniXNU-enuma | `fafe89073a778438a10b6e0b8e891a2e967dc4d2` |
| RigoLigoRLC/unixnu-sm-t733 | `7ec704db1f60d99ea8606aabada919ed2dcbae1b` |

Main has 24 commits beyond our pinned source-build baseline `42bffe8`.
The previously fetched remote main was `759eb61`; only the device-record refresh
`ae68700` is newer than that fetched tip. Do not present all 24 as newly discovered
fixes. The SM8250 common scaffold and umi tip are unchanged from the prior review.

## Upstream changes relevant to this port

- [018c8f3](https://github.com/UniXNU/UniXNU/commit/018c8f3439f1093b7ccd9965db0894516aace274)
  restores omitted public XNU headers after a fresh patched-source clone. The
  exact two-line change already exists in our build-driver backport 0004; this
  is a build fix, not an explanation of the hardware reset.
- [71ccc68](https://github.com/UniXNU/UniXNU/commit/71ccc68c1e9b5d3e996d8623c7375d1a381cf5b2)
  translates per-CPU DTS `unixnu,cluster-type` E/P into the ADT integer property.
  Useful after CPU/timer bring-up; J716F topology and scheduler assumptions still
  require their own qualification. Do not enable more CPUs during the P12 control.
- New device discovery and record rendering reads fixed repository snapshots.
  The [J716F page](https://github.com/UniXNU/UniXNU/blob/ae687009348296e97e49ce6e925795044f363ce6/docs/zh/devices/j716f.md)
  uses device commit `f7f9f4a315e2f467d5a9dfe7fdf70c34c917fc87`.
  This is republication of our earlier evidence, not independent hardware
  confirmation or coverage of current local P11 results.
- GPU/display/SMMU revisions and Safari/work-interval changes concern later
  execution. None of the reviewed changes supplies a demonstrated J716F
  BCM/store/barrier reset fix. The architectural-timer-entry patch is unchanged
  between `42bffe8` and this upstream tip.

## Peer implementations

### Same SoC: kebab

[Bring-up record](https://github.com/GFander/UniXNU-kebab/blob/a68c238ba768b3836c1ae2e67d5ade4a9025a0df/docs/BRING-UP-STATE.md)
reports that a hard-coded physical framebuffer breadcrumb faulted after
`arm_vm_init` disabled TTBR0. It looked like a kernel hang. Runtime addresses and
mapping lifetime must be audited before adding post-MMU probes on J716F.
Kebab uses a different kernel/OS and 16 KiB pages; do not copy its KVA, offsets or
page-table fix into the current J716F binary probes.

Its [architectural timer patch](https://github.com/GFander/UniXNU-kebab/blob/a68c238ba768b3836c1ae2e67d5ade4a9025a0df/patches/xnu-8796-kebab/0008-architectural-timer-entry.patch)
guards the BCM2837 route with `!XNU_NATIVE_ARCH_TIMER`. This matches the upstream
design. Its userspace watchdogd policy runs after Data mounts and does not
disable or identify a pre-MMU hardware watchdog.

### Same SoC: umi and pipa

[Umi writer](https://github.com/reAAAq/UniXNU-umi/blob/3eb72578597d2ca16471b172d45eddc096a34a04/scripts/umi-early-trace.inc)
uses no stack or firmware service, with explicitly documented scratch registers.
Its [entry patch](https://github.com/reAAAq/UniXNU-umi/blob/3eb72578597d2ca16471b172d45eddc096a34a04/patches/umi-xnu/early-entry-trace.patch)
adds an uncached TTBR0 console map and pre/post-SCTLR witnesses; later C code uses
`ml_io_map`. The writer still contains DSBs. It is not evidence that removing a
barrier solves J716F. Its 2 MiB console is not our `0xb00be000+0x40000` console.

[Pipa README](https://github.com/xVanTuring/UniXNU-pipa/blob/cdb3d62771e31d48d832853ee0c90964346d78da/README.md)
contains newer October 24 desktop/GPU results below the older October 23
storage-blocked introduction; the introduction alone understates its reported
progress. These are peer-reported results, not independent tests by this review.
Umi and pipa select ArmCpu/ArmDTPlatform/native architectural timer together.
J716F's source qualification config currently disables those paths; the research
config expresses intent but does not establish a built or working integration.

Pipa's [reader init](https://github.com/xVanTuring/UniXNU-pipa/blob/cdb3d62771e31d48d832853ee0c90964346d78da/package/linux-log/init.c)
copies pstore early into RAM `LOG.BIN` and exports a read-only USB mass-storage
image. [BootRamoops.c](https://github.com/xVanTuring/UniXNU-pipa/blob/cdb3d62771e31d48d832853ee0c90964346d78da/package/BootRamoops.c)
implements later kernel recording and a board-specific warm return to fastboot.
Useful architecture for future automation. Its PMIC/SPMI/PS_HOLD sequence,
console layout, stock kernel/DTB and pmsg self-test writes are not J716F inputs.
Current immediate menu-to-reader retention does not prove retention through
fastboot and a separate reader boot.

### SM-T733 and enuma

[SM-T733 MMU markers](https://github.com/RigoLigoRLC/unixnu-sm-t733/blob/7ec704db1f60d99ea8606aabada919ed2dcbae1b/patches/t733-xnu/early-entry/zzzzz-early-mmu-marker.patch)
explicitly map BootVideo while preserving existing L2 tables and bounding table
allocation. Its [EarlyEntry README](https://github.com/RigoLigoRLC/unixnu-sm-t733/blob/7ec704db1f60d99ea8606aabada919ed2dcbae1b/kits/EarlyEntry/README.md)
says both current defconfigs disable these framebuffer markers; working USB
logging is a separate later facility. Source presence is not current execution.

[Enuma status](https://github.com/wh1stl3/UniXNU-enuma/blob/fafe89073a778438a10b6e0b8e891a2e967dc4d2/docs/STATUS.md)
is still pre-menu DXE diagnostics. Its structured journal separates a recorded
status from later screen replay and separates writer reachability from reset
retention. It offers methodology, not a solved equivalent XNU/MMU failure.

## Consequence for P12

1. Keep P11 evidence and boot_b-only scope. Independent non-overwriting witnesses
   must have qualified ownership, a fresh attempt cookie and CRC/commit semantics.
   A barrier/store that is not observable after reset is not an exact stopping PC.
2. Add a matched original-BCM-STR versus skipped-STR comparison using the same
   minimal witness layout. Historical P5's NOP used a different display/reset
   diagnostic; it does not replace this retained-record comparison. Keep the
   first-barrier factor separate from the BCM factor.
3. Independently audit the live register contract, stack use and MMU/cache state
   at each hook. Before pre/post-MMU tests, qualify console mapping lifetime,
   bootstrap page ownership and memory attributes using J716F's actual layout.
4. Source-based ArmCpu/ArmDTPlatform/architectural timer integration is the target
   path after the controlled diagnostic. Skipping one BCM STR does not qualify
   the whole timer/GIC/CPU path. Keep build fixes separate from device experiments.
5. Defer unattended reset/reader automation until one complete J716F retention,
   return-to-fastboot and early export cycle is demonstrated manually.
