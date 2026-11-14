# J716F independent DT panic-region host target

Target: iPadOS 17.7.11 / 21H461 / ordinary arm64, one CPU.
Derived from ../UniXNU-j716f-host; old outputs and targets remain unchanged.
The sole effective kernel configuration change is CONFIG_XNU_DT_PANIC_REGION=y.
Keep 5 components, 58 patches and 8 archive recipes. Use synchronized UniXNU
dff276535db3ac394f628cec58a718cd46c20ac2 and the pinned original kernelcache.

This is a new kernel/prelink-only host build, not a new firmware or image.
Output root: UniXNU/out-host-dt-panic-20261113.
Use scripts/host.py defconfig, fetch, status, tree, kernel.
The kernel action calls the official build kernel, including prelink.
No QEMU, image assembly, root/Data or device actions belong to this target.

The original kernel used XNU_DT_PANIC_REGION=0 and fixed BCM2837 panic memory
0x3c0fc000 outside J716F DRAM. With the official flag and no ADT /pram,
the corresponding branch returns before mapping; persistent panic logging
is not provided. This is a known incompatible configuration, not proof of the
exact r4 stall. r5 user timing supports reaching KPRINTF initialization return.

Reparse symbols/addresses after building. Do not reuse original-kernel probe
addresses or its verifier constants. Check effective config, pe_init.o compile
record, new PE_create_console flow, native/prelink structure and artifacts.
Current status: prepared source only, build and runtime unverified.

Hardware and source provenance documents copied below are historical inputs;
old target build results are deliberately not inherited as this target result.
See ../../analysis/xnu-mid-init-20261113/checkpoint-review.md.
