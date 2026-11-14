# Independent r30 storage-input image

Host-only successor of frozen r30. Use the independently rebuilt storage kernel
and official original 21H461 trust-cache. Retain five manual entries, CPU0/4KiB,
80MiB payload, 256MiB window, unchanged r22 FD and r30 hardware ADT.
Use the official payload patcher at unique bsd_init -> inittodr after-return
checkpoint, six seconds, with independent new machine-code verification.
No device operations, QEMU, grants, block writes, root mount or userland claims.
Keep frozen r30 and other targets unchanged.
