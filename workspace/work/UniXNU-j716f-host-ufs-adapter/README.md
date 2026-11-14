# J716F independent UFS adapter host target

Target: iPadOS 17.7.11 / 21H461 / ordinary arm64 / 4 KiB / one CPU.
Derived from ../UniXNU-j716f-host-dt-panic, retaining its source configuration,
five selected components, eight archive recipes and CONFIG_XNU_DT_PANIC_REGION=y.
UniXNU is pinned to dff276535db3ac394f628cec58a718cd46c20ac2.

The ninth archive recipe builds Sm8250UfsRuntime.cpp, Sm8250UfsHci.cpp,
Sm8250UfsHandoff.cpp and exactly one AppsSmmuOwner.cpp from this target's frozen
source snapshot. ShengUfs core dependencies are headers only; NativeUfs.cpp
and its SM8550 initialization are not compiled. CONFIG_XNU_NATIVE_UFS stays off.
The two additional device-local patches retain the original 58 patches (60 total).
They register the adapter after successful IOKitBSDInit and add a conditional
make dependency for the new force-loaded archive. Initialization only records
availability. It performs no MMIO, DMA allocation, SMMU changes or UFS requests.
Actual hardware operations require a separate caller with validated evidence.
This caller does not discover storage, publish a root device or supply the root
filesystem.

Output root: UniXNU/out-host-ufs-adapter-20261113.
Use scripts/host.py defconfig, fetch, status, tree, kernel.
The kernel action is the official build kernel, with fresh bootstrap compiler
records, all nine archive recipes, native link and prelink. No existing SDK,
objects or compile records may be copied into this output root.
After copying final source/header inputs, run scripts/freeze-inputs.py to record
the snapshot. The recipe verifies docs/ufs-adapter-inputs.json before and after
compilation. It records compiler stack usage and rejects dynamic global
constructors in all four objects. Seven runtime entry points and all 13 owner
exports must have exactly one definition in the archive.
Source identity is recorded by that manifest; build results and native/prelink
artifacts are recorded separately under analysis/host-ufs-adapter-20261113.
Source preparation alone is not a successful build or hardware test.

Board resources are checked against TB_J716F_ZUI_15.0.485 boot.img/DTBO,
DTB0/DTBO1. See ../../analysis/ufs-adapter-20261113/stock-reference.md.
The ROM provides hardware descriptions, not a current firmware/TZ handoff.

No QEMU, firmware/image assembly, root/Data preparation or device operation is
included here. Preserve the main port target and every earlier artifact.
host-source-manifest.json records this target's initial preparation inputs;
ufs-adapter-inputs.json records the later complete frozen source snapshot.
Inherited source-provenance.json describes earlier SoC source changes and does
not qualify this independent target's build or runtime.
