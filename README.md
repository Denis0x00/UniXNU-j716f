# UniXNU for Lenovo TB-J716F — current bring-up sources

This repository records the current **TB-J716F / Snapdragon 870 (SM8250) /
iPadOS 17.7.11 (21H461), ordinary arm64** bring-up. It contains source and
reconstruction instructions, not a ready-to-install iPadOS image.

The measured r30s path reaches BSD initialization after `IOKitBSDInit`, registers
the storage interface, and stops at the diagnostic boundary before root lookup.
The firmware menu, volume/power navigation, QR logger, retained early XNU logs,
and CPU0 initialization have tablet evidence. This does **not** establish UFS
I/O, APFS root mounting, writable Data, launchd, SpringBoard, touch/HID, graphics
acceleration, or a working iPadOS desktop.

**r31 is an opt-in experiment**, adding eleven fixed read-only GCC power/clock
register observations to the storage path. Fresh host compilation exposed an
XNU declaration hidden by `MACH_KERNEL_PRIVATE`; the source now declares its
verified `get_preemption_level` C ABI without enabling that global flag. The
nine-TU compile, official kernel/prelink build and host verification have passed.
The r31 image passed the Linux and Windows verifiers, including seven rejection
cases on each host, and deployment wrote only `boot_b`. The user reports that
selecting Start XNU returned to the r31 menu after approximately six seconds.
The retained XNU log now reports `captured`, eleven attempted/completed GCC
reads, `valid=0x7ff`, and one map/unmap. Ten raw register values are recoverable.
The retention importer reports three corrupt records and missing sequences
13, 51 and 72; this is an internal retained-record integrity gap, not merely a
missing QR page. Sequence 72 contains the unavailable AHB raw result, which
must not be reconstructed from the `ahb-readable=1` summary hint. The surviving
values show the GDSC power-on bit set, reset deasserted and AXI enabled; lane
clock values do not establish a working UFS link. This is GCC observation
evidence, not verification of all eleven raw values or HCI/SMMU access, DMA,
UFS block I/O or root mounting. The probe has no external abort containment.
The raw device log remains private and is not included in this source export.

## Source organization

The original workspace paths are intentional: frozen recipes, component feeds,
and source manifests use them. This repository root is an export container,
**not** itself an official `-D` device target.

- `workspace/work/UniXNU-j716f-host-storage-r30`: measured registration baseline.
- `workspace/work/UniXNU-j716f-host-ufs-power-r31`: experimental r31 kernel target.
- `workspace/work/UniXNU-j716f-xnu-{storage-r30,ufs-power-r31}`: image definitions.
- `workspace/work/J716fStorageBringup-*`, `ShengUfs-sm8250`, and the targets'
  `socs/sm8250`: storage interfaces, command policy, slot state, MMIO/DMA and
  shared ownership work. Many operations still require unprovided live grants.
- `workspace/work/J716f{GeniBackend,Goodix,TouchTransaction,GopAdapter,DisplayHandoff}-*`:
  offline research and synthetic host tests. These are **not enabled** by the
  current storage target. Historical host targets remain independent experiments.
- `workspace/work/ArmCpu-j716f-iokit-cpu0-20261114`: CPU0 source override.
- `workspace/work/J716fEarlyLog-20261114`: public retention protocol and decoder.
- `workspace/analysis`: authored builders/verifiers and synthetic tests only;
  historical runtime evidence and private analysis outputs are intentionally absent.
- `pins/`, `overlays/`: source hashes, exact upstream core revisions and local
  source changes, including the IOKit `AAPL,phandle` correction.

The Data-only macOS workflow is independent of the Linux kernel/firmware build.
Creating an APFS Data seed on a runner does not mount it on the tablet and does
not qualify the board's storage driver. Full macOS kernel/image migration is not
claimed by this source update.

See the [Data runner guide](README-data-runner.md) for its manual/tag trigger,
fixed Apple inputs, APFS validation, output preservation and installation UUID
contract. The repaired macOS [run 43597879267](https://github.com/Denis0x00/UniXNU-j716f/actions/runs/43597879267)
successfully created and validated a fresh Data seed. Its retained artifact
passed independent byte/provenance verification on Windows and the Linux VM;
those host checks did not themselves execute APFS. The same seed is preserved
for subsequent builds. Kernel UUID binding, payload integration and a successful
device mount are still pending.

## Reconstruct a build workspace

Python 3.11+ and Git are needed to export sources. Choose a **new** directory:

```sh
python3 scripts/export_workspace.py --destination /absolute/new/j716f-workspace --fetch-core
cd /absolute/new/j716f-workspace
```

The helper preserves `work/` and `analysis/`, verifies the source manifest, checks
out both pinned official cores, applies their separate patches and adds their
source tests. It refuses to overwrite an existing destination and never flashes
a device. Fetches need network access and sufficient disk space. Without
`--fetch-core`, it exports local source only for inspection.

For r31 on the prepared Linux host with its toolchain in `/opt/unixnu-linux`, use
the official CLI wrapper from the **exported workspace root**:

```sh
python3 work/UniXNU-j716f-host-ufs-power-r31/scripts/host.py defconfig
python3 work/UniXNU-j716f-host-ufs-power-r31/scripts/host.py fetch
python3 work/UniXNU-j716f-host-ufs-power-r31/scripts/host.py status
python3 work/UniXNU-j716f-host-ufs-power-r31/scripts/host.py tree
python3 work/UniXNU-j716f-host-ufs-power-r31/scripts/host.py kernel
python3 work/UniXNU-j716f-host-ufs-power-r31/scripts/host.py prelink
```

These are build entry points, **not a claim that the export is independently
build-qualified**. Follow [build prerequisites](docs/reconstruction.md) first.
The kernel/prelink steps require the user-provided original 21H461 kernelcache
at `UniXNU/out/ipsw/21H461/kernelcache.decompressed`. The wrapper checks its hash.
Apple sources/headers and the Linux cross toolchain are separate dependencies.

Image packaging additionally needs the locally built early-log UEFI/BootShim,
the user's normalized stock DTB, original trust cache, private identity input,
and a newly reviewed kernel expectation. They are not included here. Historical
analysis wrappers retain their original paths and evidence requirements; never
fabricate a missing receipt or reuse an older kernel's addresses to satisfy them.

Read [status and source provenance](docs/status-and-provenance.md) and
[reconstruction limitations](docs/reconstruction.md). Upstream workflow and
device definitions remain the reference:
[UniXNU](https://github.com/UniXNU/UniXNU),
[building](https://github.com/UniXNU/UniXNU/blob/main/docs/building.md),
[device porting](https://github.com/UniXNU/UniXNU/tree/main/docs/porting).

## License

Retain the original notices and per-file SPDX identifiers. The root `LICENSE`
is the upstream UniXNU BSD-2-Clause-Patent license; it does not replace licenses
of third-party dependencies or relabel the BSD-2-Clause touch/GENI sources.
See [license provenance](docs/licenses.md). Proprietary ROM, IPSW, kernelcache,
precompiled firmware, system images and private logs are not distributed here.
