# Local SM8250 transition source

2026-11-13 follow-up: Sm8250UfsDiagnostics and the explicit sm8250_ufs_observe runtime entry provide bounded Power/Host/Geometry snapshots. The independent host-ufs-observation kernel completes official build/prelink and static verification; see [the observation record](../../docs/ufs-observation.md). No automatic hardware access or handoff grant is added.

Copied from `Denis0x00/UniXNU-j716f`, commit
`bf2d0f32f706adbe414551e452f1426ed7c4bd19`, subdirectory `socs/sm8250`.
The source is explicitly selected by `local = "socs/sm8250"` in feeds.toml.
This is a local preparation snapshot, not a separately qualified shared SoC repository.

Current hardware evidence is Android runtime FDT from batch `20261111-231708`,
**DTB0 / DTBO1**, with eight CPU affinities `0x000..0x700`, 19.2 MHz timer,
GICD `0x17a00000/0x10000`, GICR `0x17a60000/0x100000`, stride `0x20000`.
The original `soc.toml` claim `DTBO8` does not describe this boot.

Local changes: source metadata updated; the generic timer node is renamed from
`timer@17a00000` to `timer`, the fabricated GIC-range timer `reg` is removed,
and the four PPI tuples from the runtime FDT are restored. Generic timer routing
still requires review against the selected ArmCpu ABI before actual boot.
Original and current per-file hashes are in `../../docs/source-provenance.json`.

`configs/sm8250.config` remains a placeholder. `kits/Sm8250Platform` contains
legacy UART/register and SMMU helpers; nothing in the current minimal selection
links those helpers. The static inline SMMU lock deserves review for whether
all translation units truly share one lock before reuse.

`patches/xnu/series` and its SMP/Edge patches are retained for review, but the
board's active patch_series intentionally excludes this SoC series. It also
references an upstream power patch. Qualify patch applicability, per-core
interrupt/timer behavior and the 4+3+1 hardware topology before enabling it.
No SMP, UART, power, SMMU, or runtime verification is claimed by this copy.

2026-11-13 UFS preparation: `kits/Sm8250Platform/AppsSmmuOwner.h/.cpp`
provides one allocation-free software owner implementation, independent of the
unchanged legacy `AppsSmmuLock.h`. The earlier two-translation-unit test passed
1,000 threaded leases under ASan/UBSan; separate ordinary ARM64 compilation and
relocatable linking confirmed all 13 owner exports uniquely. Those historical
results remain in [restricted UFS host preparation](../../docs/ufs-restricted-host.md).

The subsequent `Sm8250UfsHandoff`, `Sm8250UfsHci` and `Sm8250UfsRuntime` sources
implement an explicit handoff transaction, a single-slot MMIO/cache adapter and
the XNU allocation/mapping boundary. Host tests exercise the real handoff/HCI
code with modeled registers and separate CPU/device DMA buffers. Firmware/TZ
exclusion, complete SMMU visibility and drained prior transactions remain
external facts that no register scan or unused context bank can establish.
The stock resource reference is `TB_J716F_ZUI_15.0.485`, DTB0/DTBO1; Linux-derived
SIDs are recorded separately from stock properties and live ownership evidence.

An independent `UniXNU-j716f-host-ufs-adapter` target adds the four implementation
objects to an official kernel build. Its initialization call only records
availability after successful `IOKitBSDInit`; it performs no MMIO or allocation
and does not activate storage. Its official native kernel link and ordinary
ARM64 prelink now pass, followed by all ten groups in the final static verifier.
The actual native/prelinked code retains 20 unique exports and the registration
call; the adapter's internal stack estimate is 5,376 bytes, excluding external
XNU/IOKit frames and interrupt nesting. These are host results, not device or
runtime ABI qualification. The main minimal target and frozen r6 artifacts are
unchanged. See [UFS board adapter](../../docs/ufs-board-adapter.md) for the exact
implementation, build evidence, retained failed attempts and required device
evidence.
