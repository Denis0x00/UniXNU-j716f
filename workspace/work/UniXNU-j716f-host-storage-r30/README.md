# Independent r30 storage integration host build

This target extends `UniXNU-j716f-host-iokit-cpu0` without changing the accepted
r29 source kernel or the existing r30 diagnostic image. It retains the 60 selected
XNU patches, CPU0 repairs, early RAM log, text console and 21H461 prelink profile.
The storage addition is an explicit registration and state-reporting step. It
does not provide controller handoff evidence, enable native SM8550 UFS, or mount
root/Data. Current hardware progress remains the separately recorded r29 test.

Run `python3 scripts/host.py defconfig`, `fetch`, `status`, `tree`, then `kernel`
on the prepared Linux host. The wrapper invokes the official UniXNU CLI, with
independent output `UniXNU/out-host-storage-r30-20261114`. Source caches can be
reused; archived kernels, old objects and compile records cannot seed this build.

`docs/parent-iokit-cpu0/` records the historical accepted parent, not this target's
current input manifest. New source inputs and build evidence are recorded under
`analysis/r30-storage-inputs-20261114/kernel-target`. Device execution is pending.

The wrapper passes the official `--local-module` option for
`driver:J716fStorageBringup=../J716fStorageBringup-r30`; a feed entry alone would
be pruned because this new local component is not yet in the shared catalog.
The ninth recipe compiles its two source files and
five unchanged SM8250 adapter files into `libJ716fStorageBringup.a`, using this
target's fresh `IOPlatformExpert.cpo.json`. It rejects global constructors,
duplicate owner exports, mismatched frozen sources and incorrect archive members.
The init patch calls registration only after `IOKitBSDInit` succeeds. A separate
pre-root message is before `inittodr(0)`, leaving that function and the existing
r30 after-return boundary available for the independently reparsed checkpoint.

`docs/storage-inputs.json` pins the recipe and local sources at build time.
`docs/host-source-manifest.json` pins the target and selected build dependencies.
These manifests do not establish runtime access authority or root readiness.
