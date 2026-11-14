# Independent r31 Power-only diagnostic host target

This target extends the accepted r30 storage target in a separate directory.
It preserves the original 60 patches/eight archive recipes, CPU0 repair, early
RAM log, text console, DT panic setting and iPadOS 17.7.11/21H461 ABI. The storage
archive now resolves the explicit `J716fStorageBringup-r31` local component.

After IOKitBSDInit returns zero, startup registers the storage interface and
calls `j716f_storage_power_probe()`. This is a fixed, bounded Power-register read
experiment with logs before and after each read and an explicit marker if
external-abort containment is absent. Successful IOMap is not proof of read
success; no old runtime authority bit is asserted by this new path. This
target does not advance to Host/Geometry, transfer ownership, issue DMA/storage
writes, publish a block device or mount root.

Run `python3 scripts/host.py defconfig`, `fetch`, `status`, `tree`, then `kernel`
on the prepared Linux host. This wrapper invokes the official UniXNU CLI with
independent output `UniXNU/out-host-ufs-power-r31-20261114`. Source caches may be
reused; r30 objects and compile records must not substitute for the fresh build.

The new executable must be reparsed before retaining the diagnostic boundary
after inittodr returns, with six-second reset before root lookup. Host tests,
tablet execution, storage reads and userland startup remain separate evidence.
Historical manifests in `docs/parent-storage-r30/` describe the frozen parent;
the current manifest is generated only after the new component is frozen.
