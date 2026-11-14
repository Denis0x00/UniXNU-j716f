# J716F r31 fixed GCC read experiment

This independent successor retains the three r30 bridge files byte-for-byte and
adds `J716fStoragePower.cpp` (bounded orchestration) and
`J716fStoragePowerKernel.cpp` (actual IOKit/context/MMIO bindings). Compile those
four translation units with the same five frozen SM8250 adapter units: nine
total, exactly one AppsSmmuOwner. The parent's fresh official XNU build supplies
real headers, native link and prelink; model tests cannot substitute for it.

After successful IOKitBSDInit, call `j716f_storage_bringup_init()` then
`unsigned j716f_storage_power_probe(void)`. There are no user-selected addresses,
boot-argument access grants or runtime configuration knobs. Return values are
the `j716f_power::Result` enumeration. There is one attempt per boot, no retry.

This is an explicitly experimental first-access path. It does **not** call
`j716f_storage_bringup_observe`, fabricate its RequiredDiagnosticFacts, change
the old Registered state, authorize HCI, or infer firmware/TZ/SMMU ownership.
The old APIs and their stronger preconditions remain unchanged.

## Actual access and limitations

The kernel binding checks a live current thread, non-interrupt context, enabled
interrupts, zero preemption level, kernel_task, bridge Registered and Runtime
Cold state. Its fixed call site is BSD initialization, before inittodr/root
lookup. These checks describe the measured execution context, not a universal
proof that arbitrary callers hold no locks. Concurrent direct raw Runtime use
is outside this one-shot boot integration contract.

`IODeviceMemory::withRange(0x177000,4096)` and
`map(kIOMapAnywhere | kIOMapReadOnly | kIOMapInhibitCache)` create a real
read-only, uncached mapping. Address alignment, exact length, read-only and
cache options are checked before access. Descriptor acquisition/release,
mapping rejection/release, map-before/after and unmap-before/after are logged.
The mapping is released on normal return and callback failures.

Mapping success does **not** establish bus readability. The actual ARM64 XNU
`osfmk/arm64/loose_ends.c` implements ml_probe_read and ml_probe_read_64 as panic
stubs. ml_nofault_copy validates kernel-managed RAM translations; it is not an
MMIO external-abort recovery mechanism. This reader therefore uses one volatile
32-bit load between DSB barriers per fixed address and truthfully logs
`fault-contained=0`. A synchronous external abort or stalled bus transaction
can prevent return and cleanup; the software loop is bounded, bus latency is
not. Recover the retained early log after the normal manual reboot procedure.
A read-before without read-after means incomplete observation, not success.

## Register provenance and meaning

Stock **TB_J716F_ZUI_15.0.485** DTB0 identifies GCC at 0x100000/0x1f0000 and
`qcom,gdsc@177004` as ufs_phy_gdsc. The pinned Linux v6.12 gcc-sm8250.c provides
the remaining offsets and clock semantics. See the parent's independent
`analysis/r31-ufs-power-20261114/board-review.md` and actual-XNU
`ml-probe-read-source.txt`.

| Index | Register | Physical address |
| --- | --- | --- |
| 0 | GDSCR | 0x177004 |
| 1 | BCR | 0x177000 |
| 2 | AXI | 0x177010 |
| 3 | AHB | 0x177018 |
| 4 | ICE | 0x177064 |
| 5 | AUX | 0x17709c |
| 6 | RX0 | 0x177020 |
| 7 | RX1 | 0x1770b8 |
| 8 | TX0 | 0x17701c |
| 9 | UNIPRO | 0x17705c |
| 10 | AGGRE_AXI | 0x1770cc |

Every read is preceded by index/name/physical-address logging and followed by
the returned raw value. All 11 belong to GCC; a UFS domain-off indication does
not by itself prohibit observing other GCC registers. No HCI or SMMU page is
mapped. Post-capture GDSC/reset/AXI/AHB interpretations are explicitly hints.
RX/TX use BRANCH_HALT_DELAY; their raw values are not converted into lane clock
readiness. Even a complete all-zero/all-one capture is merely returned data,
not proof of powered hardware; review the actual values and provenance.

## Expected result

The successful software path ends with:

```text
[J716F power] result=captured code=0 attempted=11 completed=11 valid=0x7ff maps=1 unmaps=1
[J716F power] snapshot-only storage-stage-unchanged hci=0 smmu=0 dma=0 writes=0 next=review-power-snapshot
```

This demonstrates only that the listed loads and normal mapping release
returned on that boot. Review power/reset/clock values before designing a
subsequent Host observation. No capacity, storage command, block device, root
mount, firmware exclusion, DMA ownership, IRQ or iPadOS userspace is qualified.
The parent image retains the after-inittodr six-second reset boundary.

Run the pure model suite on the Linux build host:

```sh
python3 work/J716fStorageBringup-r31/tests/run.py --root "$PWD" --output "$PWD/analysis/r31-ufs-power-20261114/power-model-01"
```

Tests cover exact addresses/order, all partial-read failures, every context
gate, no callbacks on rejected input, map failure, release accounting,
one-shot refusal, reentrancy and power/clock interpretation limits. No modeled
callback test establishes real fault containment or executes IOKit.
