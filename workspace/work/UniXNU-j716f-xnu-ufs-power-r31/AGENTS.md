# Independent r31 UFS Power diagnostic image

Keep the frozen r30/r30s targets and artifacts unchanged. Package the independent
r31 official host kernel and original 21H461 trust cache. Retain five manual
entries, default XNU, timeout=-1, CPU0/4KiB, 80MiB payload, 256MiB window, r22 FD
and hardware ADT. Verify the actual rebuilt executable, never old call addresses.
The only new hardware scope is a bounded read-only GCC UFS Power diagnostic;
no HCI/SMMU access, DMA, UFS commands, root mount, disk writes or handoff grants.
Keep the inittodr after-return six-second reset for recovery. Runtime observations
remain separate from host acceptance. Device deployment is the established
boot_b-only workflow when the image is verified and the user is ready.
