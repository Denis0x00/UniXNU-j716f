# J716F current-kernel display reservation metadata

This independent package connects the accepted resource-only provider to one
real platform caller. It does not implement a framebuffer, scanout handoff,
cache mapping, pixel writes, display service, GPU, DPU, or panel driver.
Results belong in `analysis/display-handoff-chain-20261113/README.md`; this
source package is frozen before official host compilation.

The patch is for the exact resolved iPadOS 17.7.11 / 21H461 source at XNU
`d8b80295118ef25ac3a784134bcf95cd8e88109f`. It replaces the tail of
`IOPlatformExpert::start`: only after the complete virtual `configure()`
(including ArmDTPlatform's later work) succeeds does it call the new function.
Failure of optional resource validation never changes platform start's result.
`j716f-gop-resource=1` requests checks; it is not evidence or a grant. The
unselected path allocates nothing and changes no diagnostic counters.

The caller reads current XNU scalar globals, not old boot pointers or serialized
tokens. It accepts only the high-span branch `gPhysBase >= 0x9e400000` and checks
4 KiB alignment, nonoverflowing `gPhysBase + gPhysSize`, and:

```
gPhysBase <= first_avail_phys <= avail_start <= avail_end <= gPhysBase + gPhysSize
vm_first_phys == gPhysBase
vm_last_phys == avail_end
```

The exact source chain shows `first_avail_phys` captures `topOfKernelData`;
the qualified early dynamic allocator grows upward from there; pmap's boot page
enumerator and VM page release use the same high managed span. KTRR/CTRR static
page allocation configurations fail compilation. The conservative rule rejects
even a nonoverlapping span below GOP and does not implement internal holes.
All 9,216 pages in `[0x9c000000,0x9e400000)` must additionally return false from
the real `pmap_valid_page`, twice. This is current allocator exclusion, not
physical presence, firmware quiescence, cache compatibility, or scanout proof.

The same dedicated-target patch also prevents later adoption of this range:
`vm_page_create` checks its complete half-open physical page interval before
any page is grabbed or released, and panics if it intersects the reservation.
The policy is unconditional and independent of the resource request flag.
It does not silently skip pages (which would corrupt reclaim callers' counts)
or partially release a request before discovering a collision. An in-function
runtime check requires 4 KiB pages before any address shift, because ordinary
ARM64 XNU's `PAGE_SHIFT_CONST` is actually a runtime int. This closes the path whereby
`ml_static_mfree` or another caller could later add pages outside the original
pmap span. It is a core VM exclusion policy, not protection against arbitrary
kernel code mapping/writing physical memory or corrupting free-list internals.

After checking the current PE console, a plain IOService owns one full-range
IODeviceMemory descriptor. Init/range/attach/start failures unwind; the caller
rechecks bounds, console and platform identity/activity before registration.
The accepted provider's registration function retains the current owner and
descriptor; this caller separately keeps its owner reference until reboot, even
if later provider validation fails. No resource is registered as a discoverable
display service. No hot-unplug, suspend, revocation, or automatic retry is
implemented. Concurrency uses a nonspinning gate and one attempt per boot.

Only allocator metadata authorization is supplied. The frozen provider still
requires its ordinary official early/late entry conditions, current console,
descriptor consistency and IOBSD. Its display and scanout capabilities remain
zero. This caller does not select the official graphics-load/client probes.
The same existing caveats about their reachability and side effects remain.

```
python3 tests/run_host_tests.py --cxx clang++ --out /absolute/new-test-output
python3 tests/run_adoption_tests.py --cc clang --out /absolute/new-adoption-test-output --xnu-source /absolute/resolved/tree/.cache/xnu-work/osfmk/vm/vm_resident.c
python3 scripts/build.py --tree /absolute/resolved/tree --source /absolute/J716fDisplayHandoff-20261113
```

The builder first verifies all 16 fixed resolved source hashes in
`package/source-contract.json`, including the new platform patch. It invokes
the unchanged accepted provider builder, then compiles the new source from the
same actual IOPlatformExpert C++ compilation record. Both objects occupy the
one official `libNativeLegacyDisplay.late.a` slot. It records both object
audits and all inputs; no synthetic object or header SDK is substituted.

The host test includes the actual caller plus frozen provider `.cpp`, with a
modeled narrow IOKit API. It exercises every GOP page as unexpectedly managed,
invalid early/managed bounds, second-pass evidence changes, all allocation and
lifecycle failures, duplicate/busy calls, and provider early/late integration.
ASan/UBSan and official ARM64/link results are external evidence, not tablet
qualification. No new boot image or hardware action is part of this package.

The adoption test extracts the exact patched XNU `vm_page_create` after checking
its complete file hash. Its host stubs count grabs, releases, locks and page
accounting. Boundary, enclosing and partial-overlap requests must panic before
any counts change; runtime page shifts 0 through 64 except 12 must be rejected
before any shift or page release. The caller's model also uses a runtime int.

Adding any other physical-memory adoption mechanism or changing XNU/loader
sources requires a new source-chain audit. This proof is not permission for
another driver to map or allocate the stock reservation.
