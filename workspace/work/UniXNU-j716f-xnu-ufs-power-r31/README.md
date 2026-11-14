# J716F r31: UFS Power diagnostic

Independent successor to accepted r30s. Uses the new official host kernel with
the board Power-only probe, original 21H461 trust cache and unchanged early RAM
log / menu drawing fixes. Diagnostic records, not the six-second reset alone,
determine whether GCC reads completed and whether UFS power/clocks are ready.
The checkpoint remains after inittodr and before root lookup. Root storage,
APFS mounting, writable Data and iPadOS userspace are not yet qualified.
