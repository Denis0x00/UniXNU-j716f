# ShengUfs

SM8550 UFS transport and sheng persistent storage.

The historical native UFS root qualification applies to the iOS 17 path.

Build and interface details: [documentation](https://unixnu.com/drivers/sheng-ufs). Component sources and dependencies: [catalog](https://unixnu.com/reference/components).

UniXNU's `scripts/build-sheng-source-drivers.py --with-storage-family`
compiles this driver and public IOStorageFamily 337.100.1 against XNU 12377
for arm64e iOS 26. Verified-root read policy remains enabled. These archives
still require family/APFS linkage and device qualification; they do not establish
an iPadOS 26 root boot. See the [migration procedure](https://unixnu.com/porting/migration#source-drivers).

The arm64e `build-arm-driver.py ShengUfs` entry now selects
`build-sheng-source-storage.py`. It builds a deferred public-source
IOBlockStorageDevice adapter and the board's SourceStorage runtime. The runtime
loads the original IOStorageFamily and APFS, checks the original base class
size, authenticates the 23F84 policies, registers the source classes, and starts
NativeUfs on the platform root. Its runtime metadata has a separate name and
inherits the original storage metaclass; its C++ class name is preserved for
compiler-generated arm64e pointer authentication.

`native-source-storage=1 native-ufs-root=1` selects this path. The root policy
tables travel in `kernel.macho.vroot`; only the owner public anchor is linked
into the kernel. On sheng it serves the iPadOS 27.0.1 (24A446) root and Data.

## Command path

Up to 32 commands run at once, one per command slot. The request list sits on
a write-combined page of its own, so slot s uses list entry s; without that page
16 slots use the even entries of the DMA region, so no two entries share a cache
line on this non-coherent stream. Region A holds slots 0 to 15 (slot 0 keeps the
original layout), region B slots 16 to 31. The last slot serves the panic path;
a full pool hands a freed slot to its oldest waiter.

Completion uses the controller interrupt (GIC SPI 265) with UTRCE and the UTRD
interrupt bit, as Linux ufshcd. The handler wakes the waiters of the commands
whose doorbell bits cleared. A wait polls for its first 200 microseconds
without blocking, one WFE between two polls, and sleeps on the interrupt after
that or as soon as a preemption is pending on its CPU. Bring-up and the panic
path poll with `IODelay`.

| Boot argument | Effect |
| --- | --- |
| `native-ufs-wait=1` | Default: spin for the budget, then sleep |
| `native-ufs-wait=0` | Pause with `IODelay(2)` for the budget, then sleep (the earlier behaviour) |
| `native-ufs-wait=2` | Sleep on the interrupt from the first pause |
| `native-ufs-spin-us=N` | Spin budget in microseconds (default 200) |

A command takes about 100 us on sheng, and a blocked thread takes 30 to 37 us
on average to run again after a timer or the completion interrupt. `IODelay(2)`
blocks on a timer on these cores (they register no idle exit latency), so the
paced mode blocks two to three times per command.

Page-aligned buffers are read and written by DMA straight into their pages
through a PRDT of up to 64 entries and 1 MiB per command; other buffers use the
slot's 128 KiB data area. Data is cleaned to memory before the doorbell; after
the command only data the device wrote is invalidated, so the source of a write
stays as the CPU has it. A command answered with BUSY or TASK SET FULL is sent
again after a pause. Root window reads each take their own bounce buffer.

A window device is read-only and reports a solid state medium (`Device
Characteristics`, `Medium Type`), which XNU reads through DKIOCISSOLIDSTATE for
its I/O throttle periods, read-ahead and page-in cluster sizes.

## Verified-root tables

Every window block is checked against its SHA-256 in the owner-signed table
before it is served. A header-only policy keeps its table in the root image
object `<window>.vroot`. At bring-up the whole table is read once and checked
against the digest in the signed header; only a SHA-256 of each 4096-byte table
page stays in memory, read-only (32 bytes per 512 KiB of image). A block's table
page then comes from a wired cache shared by all windows, or is read from the
object on LUN 0 through the transport (never through a window), checked against
its kept page hash and cached. A table page that does not match is not cached and
every block it covers is refused. The cache is allocated before the windows are
published, so a page-in under memory pressure fills it without allocating.

| Boot argument | Effect |
| --- | --- |
| `native-vroot-cache=N` | Table page cache in MiB (default 8: 2048 pages, 1 GiB of image) |
| `native-vroot-cache=0` | Keep every window's whole table wired, no cache |

`native_ufs_dme_get` reads UniPro attributes. The sheng link runs HS-G5 Rate A
on two lanes in FAST mode, as the boot firmware leaves it.

## Statistics

`native_ufs_stats_text` fills one row for the board's log:

| Field | Meaning |
| --- | --- |
| `direct`, `staged` | Data commands on the caller's pages, and through the slot's data area |
| `busy` | Commands answered with BUSY or TASK SET FULL and sent again |
| `long k/n` | The longest store call: `r` read, `w` write, `s` cache flush, and its blocks |
| `pauses n/us` | `IODelay(2)` poll pauses of completion waits and their total time |
| `spins n/us` | Spinning poll pauses (one WFE each) and their total time |
| `yields` | Spins that ended in a sleep because a preemption was pending |
| `done a/b/c/d/e` | Waits whose command completed under 50, 100, 200, 400 us, and later |
| `old ms/op/slot` | The oldest command in flight now: age, opcode (hex), slot; 0 when none |
| `takers` | Threads waiting for a free command slot now |
| `poison p/r/op` | 1 after a timeout or fatal status (later commands are refused), with its IOReturn and opcode |
| `irqs`, `sleeps` | Completion interrupts, and waits that slept for one |
| `wakes n/us`, `max` | Sleeps the interrupt ended, total and longest time until the waiter ran |
| `timeouts` | Sleeps that ended on their 1 ms timeout |
| `win n/MiB/ms` | Root window requests, their bytes and their time |
| `wait n/ms` | Root window requests that waited for a bounce buffer |
| `vb` | Integrity blocks verified |
| `vt h/m/r/e/w/f` | Table cache hits, misses, table page reads, evictions, waits for a page or entry being filled, refused table pages |
| `vtc n/KiB` | Table cache pages (0 with `native-vroot-cache=0`) and wired verified-root memory in KiB |

A paced pause is `IODelay(2)`. XNU spins for it only on a CPU registered with
an idle exit latency above the delay; otherwise the thread blocks on a timer,
and `pauses` shows the cost. Every command wait ends by its deadline (1 s read,
2 s write, 10 s cache flush); one that runs out poisons the host.

## Tests

### SM8250 observation and grouped planning (host preparation)

`UfsPlatform.h` and `UfsObservation.h` add an independent pure core for the
SM8250 port. The caller selects `Platform::Sm8250`, supplies exact SoC/controller
identity and HCI/SMMU ranges, and supplies an established read-access grant.
The board adapter checks the identity, but established power/access authority
must still come from platform evidence. The core does not discover or authorize
hardware access itself.

`observeUfs` preflights its complete fixed address list, then reads at most 28
32-bit GCC/HCI/SMMU-ID registers through a fallible callback. Power/reset/bus
clock failures stop before HCI access; enabled hardware-gated idle clocks are
handled separately from stopped clocks. Partial observations retain successful
read counts and the failed address. `Captured` means only the fixed snapshot was
collected, not that the link, DMA or storage is ready. Expected SIDs `0xe0/0x4e0`
come from Linux SM8250 sources, not measured firmware ownership. No stream or
context table, PHY/ICE/TCU, SM8550-only GCC resource or firmware list pointer is
read. There is no write callback, UIC command, DMA allocation or panic path.

`SmmuGroupPlan.h` separately plans both SIDs from a complete validated snapshot
and an explicit owner grant. It rejects overlapping/duplicate mappings, foreign
bank users, incompatible contexts and DMA extents, and revalidates every captured
field before any future commit. Its records cannot create hardware ownership.
It does not implement shared locking, MMIO publication, rollback or quarantine;
the board owner and handoff transaction implement those responsibilities
separately, without rollback. `planInto` writes into caller-retained storage to
avoid a full `Plan` temporary on the small kernel stack. It rejects overlapping
inputs before changing the output; callers must check its returned error.
The board transaction retains separate initial and revalidation plans. The
value-returning `plan` and the `revalidate` helper, which uses a full local plan,
remain available for host consumers and are not used by this transaction.

The current `NativeUfs.cpp`, SM8550 defaults and official component/archive
names are unchanged. These helpers have no new CLI flags and are not enabled in
the flashed TB-J716F boot path. The independent `j716f-host-ufs-adapter` target
now includes the board adapter in a completed official kernel build and static
verification, as recorded below. The ordinary
archive build alone does not compile unused header APIs; the earlier separate
bridge exercised those APIs with the prepared kernel compiler record.

Host fixtures (no hardware or QEMU):

```sh
clang++ -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined -g tests/PlatformObservation.cpp -o /tmp/ufs-observation
/tmp/ufs-observation
clang++ -std=c++17 -Wall -Wextra -Werror -fsanitize=address,undefined -g tests/SmmuGroupPlan.cpp -o /tmp/ufs-group-plan
/tmp/ufs-group-plan
```

### SM8250 restricted commands and shared ownership (host preparation)

`ReadCommandPolicy.h` validates the final UPIU/CDB and raw PRD before any
submission. It permits only NOP, power-mode READ ATTRIBUTE, TEST UNIT READY,
READ CAPACITY(10), and one 4096-byte READ(10) of an explicitly allowed block.
The capacity must be measured on the selected ordinary LUN. Data-out, other
commands, noncanonical packet fields and DMA outside the exact 139264-byte
owned slot allocation are refused. Responses must match the command, tag and
LUN, with successful OCS and consistent residual/sense fields.

`RestrictedUfs.h` implements a non-reentrant single-slot session, ordered as
NOP -> power -> TUR -> capacity -> known-block reads. It discards caller-supplied
capacity, validates actual owner readiness and HCI preflight, and marks DMA
possible before preparation. Ticks and poll counts both bound the wait; at most
two retries follow a trustworthy completed BUSY/TASK SET FULL response within
the same total budget. An uncertain submission, completion or timeout quarantines
the session and retains its resources. A software halt is not proof of stopped
hardware; `confirmStopped` checks controller readback before confirming it to
the owner. Metadata contention can retain the lease as well as the resources.

The board-side `Sm8250Platform/AppsSmmuOwner.h` and `.cpp` provide a single
allocation-free owner implementation with 13 C entry points. A transaction lease
and short nonblocking metadata lock protect explicit handoff grants, resource
claims, generations, publication and DMA state across translation units.
Quarantine retains the resource claim; stopped release retains published
mappings. An unused bank or this software interface alone cannot establish
firmware/TrustZone ownership. The existing `AppsSmmuLock.h` and legacy clients
have not been migrated to this interface.

The new core calls the actual owner API. The board adapter below now implements
its `Port` callbacks for MMIO, cache maintenance and owned-buffer preparation.
`NativeUfs.cpp`, its original command path, production archive recipe, root
publication and persistent storage path are unchanged. The observer/planner
above still perform no ownership transaction themselves.

Host fixtures `tests/ReadCommandPolicy.cpp`, `tests/RestrictedUfs.cpp` and the
board owner's two test translation units cover packet refusal, callback faults,
sequencing, bounded retries, stop proof, reentry and shared-owner contention.
The workspace runners `analysis/tools/check_shengufs_restricted.py` and
`analysis/tools/build_shengufs_restricted.py` pin inputs, rerun prior checks,
build the official archive separately, and compile/link the new core and one
owner translation unit against the prepared ordinary ARM64 ABI. This relocatable
link is not an integrated kernel link or a device storage result.

### SM8250 board adapter (host model verified)

The device repository's `socs/sm8250/kits/Sm8250Platform` now supplies
`Sm8250UfsHandoff`, `Sm8250UfsHci` and `Sm8250UfsRuntime`. The handoff transaction
requires explicit evidence for register access, complete visible SMMU state,
exclusive authority without external writers, drained old transactions and
owned identity-mapped DMA. It captures and revalidates the complete visible
stream/context set, publishes only the granted dual-SID routes, and leaves the
already stopped host queue stopped. It does not stop unknown firmware queues,
perform cold initialization or program PHY/ICE/TCU, UIC or global SMMU controls.

The HCI adapter installs one owned request list and implements slot-0 submission,
bounded polling, descriptor and response checks, and cache maintenance around
the restricted command chain. The Runtime supplies the IOKit contiguous
allocation, register mappings and ARM64 cache/barrier operations. Its startup
entry only marks registration: allocation and activation require separate
trusted platform calls and actual handoff evidence. Published or quarantined
DMA and mappings remain retained, including after a confirmed queue stop.

The TB-J716F ZUI 15.0.485 DTB0/DTBO1 resources were independently checked against
the stock images. The expected SIDs and GCC branch offsets remain Linux source
references; neither source establishes the current hardware ownership or state.

The 2026-11-13 ASan/UBSan host run passed 28 HCI scenarios plus 148 injected
callback failures, 67 handoff scenarios plus 500 callback failures, and 11
integrated pipeline scenarios, with the preceding regressions also passing.
The pipeline uses the real handoff, HCI, command-state and owner modules against
modeled MMIO and separate CPU/device DMA views; it does not synthesize owner-ready
transitions. `analysis/tools/check_sm8250_ufs_adapter.py` records pinned inputs
and results in `analysis/ufs-adapter-20261113/host-result.json` for this run.
It does not execute the IOKit Runtime or access hardware, and no QEMU run was used.

The independent target completed the official kernel bootstrap, nine archive
recipes, native kernel link and ordinary ARM64 prelink. The successful build
and static acceptance are recorded in
`analysis/host-ufs-adapter-20261113/kernel-03.json` and `verification-03.json`.
All 20 owner/Runtime exports are unique in the native and prelinked outputs,
with their code preserved. Machine-code checks confirm that the startup entry
only stores the registration flag, with no MMIO or allocator calls. The analyzed
maximum internal call chain uses 5376 bytes of stack, including conservative
internal callback sets; this excludes external XNU/IOKit frames and interrupt
nesting and is not a bound on the complete running kernel stack.

These are build and static results. The adapter is not enabled on the tablet,
and actual register access, DMA, known-block reads, IOStorage/root integration
and iPadOS userland remain unqualified on TB-J716F. All 14 device porting areas,
including storage, remain `planned` pending the corresponding hardware evidence.

The existing transport and root fixtures below remain applicable.

`tests/CompletionWait.cpp` drives the completion wait policy through the
production poll loop. `tests/RootCommands.cpp` runs the production command code against a host model:
per-slot entries, descriptors, tags and data, the pool, busy retries, the panic
slot and the one-slot fallback. `tests/check_arm64_root_commands.py` runs it on
QEMU's UFS controller at EL1 with real cache maintenance, including eight
outstanding commands and a 512 KiB zero-copy read.
`tests/VerifiedTableCache.cpp` checks the table cache against `vr_verify_block`
on the whole table, refusal of tampered and unreadable table pages, LRU eviction
and concurrent fills; `tests/check_root_path_host.py --verified-root
<UniXNU>/kits/VerifiedRoot` builds and runs it.

## Wi-Fi DMA window

The optional WCN7850 window maps 128 MiB at IOVA 0x10000000 using 64 two-MiB
blocks in its own SMMU context bank. The Wi-Fi allocator reports current use,
peak use and failures. sheng device checks reach interface readiness and
network traffic with zero DMA allocation failures.

The consuming kernel build compiles the driver. The board's
`scripts/check-wifi-dma.py` checks window capacity and allocation boundaries.
