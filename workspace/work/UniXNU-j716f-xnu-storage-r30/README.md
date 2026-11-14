# TB-J716F r30s storage-input candidate

Independent host-only successor of the accepted r30 diagnostic. The new source
kernel is produced by `../UniXNU-j716f-host-storage-r30`; this target packages it
through the retained official r23 ADT generator and payload/bootimg CLI.

The image adds the original 21H461 trust-cache bundle and storage registration
logs. The OS and SystemOS raw images are separate host inputs. No Data seed,
root device, storage authority or installation layout is supplied by this target.

The checkpoint remains the unique `_bsd_init -> _inittodr` return, before root
lookup and mounting. Only then does the six-second counter delay begin. The
new executable must be independently parsed and its expectation frozen; old
kernel addresses and old r30 kernel hashes are not accepted.

Retained policy: CPU0, 4 KiB, 256 MiB owned-memory request, 80 MiB payload,
`-v serial=3 j716f-earlylog=1`, unchanged r22 FD/EFI and stock normalized DTB/shim.
ADT hardware properties match the r29/r30 baseline; only random-seed and the
empty TrustCache handoff placeholder may differ. The five-entry menu defaults
to Start XNU and requires manual confirmation. No automatic boot or RAM log
test entries are added.

Pending physical observation, the useful expected early-log sequence is:

```text
[J716F boot] IOKitBSDInit returned result=0x0
[J716F storage] ... stage=registered ... next=power-read-authority
[J716F boot] BSD pre-root reached; inittodr next; root lookup not reached
```

These markers establish startup/registration reachability only. The bridge
does not call its observe/allocate/activate/read APIs automatically. Actual
powered MMIO access, firmware/TZ exclusion, DMA/SMMU handoff, capacity and known
block reads remain unqualified. A six-second return does not prove timer IRQ,
root mount, completed BSD initialization or iPadOS userspace.

The build and verification entry points and reports are under
`analysis/r30-storage-inputs-20261114`. Preserve this target and its actual
artifact identity once accepted; make later changes in another explicit target.
