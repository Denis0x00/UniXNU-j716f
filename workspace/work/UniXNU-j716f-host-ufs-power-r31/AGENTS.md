# Independent r31 Power-diagnostic host target

User-authorized extension of the accepted r30 storage target. Preserve the first
60 XNU patches, first eight archive recipes, CPU0 repairs, early-log/text-console,
DT panic setting and 21H461 prelink ABI. Use the independent r31 component and
output tree. Do not change frozen r30 targets, manifests, kernels or images.

After IOKitBSDInit returns zero, register the storage interface and call the
explicit experimental Power-only board entry. It may attempt the fixed GCC
read set with per-read logs and an explicit lack-of-fault-containment marker;
successful IOMap alone must not be promoted to a read-success fact. Do not
invent old runtime authority, enter Host/Geometry stages, transfer firmware ownership, issue
DMA/storage writes or mount root. Preserve the after-inittodr six-second boundary.

Use scripts/host.py for the official CLI pipeline. Fresh native/prelink outputs
need a new dynamic machine review before packaging. No VM/device/QEMU action is
part of this preparation task. Host evidence stays separate from tablet results.
