# J716F independent UFS observation host target

Target: iPadOS 17.7.11 / 21H461, ordinary arm64, 4 KiB, CPU0.
Derived from the frozen ../UniXNU-j716f-host-ufs-adapter source target.
UniXNU stays at dff276535db3ac394f628cec58a718cd46c20ac2. The same five
components, nine archive recipes, 60 patches and DT panic correction remain.
The final archive adds Sm8250UfsDiagnostics.cpp to Runtime, Hci, Handoff and
one AppsSmmuOwner.cpp. It exposes 8 runtime and 13 owner C APIs.
CONFIG_XNU_NATIVE_UFS remains off; NativeUfs.cpp is not compiled.

sm8250_ufs_adapter_init still only marks availability after IOKitBSDInit.
sm8250_ufs_observe is an explicit trusted-platform entry with no automatic
caller or boot flag. Power, Host and Geometry requests use only their fixed
read ranges and temporary inhibited-cache mappings. No DMA buffer allocation,
MMIO writes, UIC commands, pointer following, queue changes or ownership grants
are produced by observation. A mapped page or nonzero evidence reference does
not prove hardware accessibility, register power or firmware/TZ exclusion.
Synchronous external aborts are not caught. Snapshots are non-atomic and cannot
qualify ownership, complete SMMU visibility, drain, DMA or storage.

Hardware descriptions reference TB_J716F_ZUI_15.0.485, selected DTB0/DTBO1.
The pinned Linux v6.12 clock/SID expectations remain separate from ROM facts.
See ../../analysis/ufs-observation-20261113 for current source/host evidence
and ../../analysis/ufs-adapter-20261113/stock-reference.md for retained resources.

Use scripts/host.py defconfig, fetch, status, tree, kernel. Kernel invokes the
unmodified official build kernel, producing new bootstrap records, archives,
native link and ordinary ARM64 prelink in UniXNU/out-host-ufs-observation-20261113.
Final immutable inputs are in docs/ufs-observation-inputs.json. Preparation
identity is separately in docs/host-source-manifest.json. Results live in
analysis/host-ufs-observation-20261113, not inferred from source preparation.

No firmware/payload/boot image assembly, tablet operation, QEMU or root/Data
preparation is included. Physical progress remains early XNU Stage 5: r5 is
the last positive checkpoint and deployed r6 awaits onsite observation.
