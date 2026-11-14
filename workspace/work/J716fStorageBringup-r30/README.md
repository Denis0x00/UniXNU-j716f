# J716F r30 storage bring-up bridge

TB-J716F / SM8250, iPadOS 17.7.11 / 21H461, ordinary arm64, CPU0 and 4 KiB.
This new component connects the already implemented restricted UFS transport to
an explicit staged board entry. It does not change frozen r30 or claim hardware
storage support. Runtime is blocked pending real board access/ownership evidence.

## Integration

Compile these **two** translation units with the new target's fresh XNU C++
compiler record and ordinary arm64 ABI:

- `J716fStorageBringup.cpp`: bounded orchestration and state/report APIs.
- `J716fStorageKernel.cpp`: real Runtime bindings and `printf` log emission.

Add the existing **five** adapter translation units from the frozen
`work/UniXNU-j716f-host-ufs-observation/socs/sm8250/kits/Sm8250Platform`:
`Sm8250UfsRuntime.cpp`, `Sm8250UfsHci.cpp`, `Sm8250UfsHandoff.cpp`,
`Sm8250UfsDiagnostics.cpp`, `AppsSmmuOwner.cpp`.
Include that directory, the frozen target's `drivers/ShengUfs`, and this directory.
Exactly one owner TU must reach the final native/prelinked kernel. `NativeUfs.cpp`
is neither compiled nor initialized. `CONFIG_XNU_NATIVE_UFS` remains off.

The new independent target is `work/UniXNU-j716f-host-storage-r30`; its recipe,
XNU init patch and archive dependency are maintained separately. At the successful
`IOKitBSDInit` return branch, call only `j716f_storage_bringup_init()`.
It calls the existing registration flag setter and emits:

```text
[J716F storage] seq=1 event=0 stage=registered err=0 backend=0 next=power-read-authority
```

The real `printf` path feeds the existing early XNU log. Recovering this line will
prove bridge registration reached; it does not prove registers, ownership, DMA,
capacity or a block device. Log event numbers are the `Event` enum in the header.
Each explicit operation has enter/return markers; a missing return helps localize
a fault but cannot distinguish a fault from a dropped uncommitted log tail.

## Explicit APIs

All APIs are trusted-kernel-only, synchronous, bounded/non-reentrant at the bridge
gate; output/state/request/reference buffers must be kernel-owned and nonaliasing.
The one static state and underlying Runtime live for the entire boot. Concurrent
direct callers of the raw Runtime APIs are not part of this integration contract.

| Export | Action and prerequisite |
| --- | --- |
| `j716f_storage_bringup_init()` | Register and log only; no mapping/allocation/commands |
| `..._status(Report *)` | Copy current stage, next gate, detailed backend results |
| `..._observe(DiagnosticRequest *, Report *)` | Power, then Host, then Geometry; exact externally authorized read-only pages |
| `..._allocate(RuntimeAccess *, Report *)` | Only after geometry snapshot; actual memory ownership, MMIO access and sleepable context |
| `..._activate(RuntimeStart *, Report *)` | Exact allocation and full live handoff; original transaction and NOP → power → TUR → READ CAPACITY(10) |
| `..._read_known(lba, reference, output, 4096, Report *)` | Actual one-block allowlisted READ(10), then byte comparison to independent immutable reference |
| `..._stop(Report *)` | Explicit original queue stop/readback and owner release; retains published DMA/mappings |
| `..._discard_unused(Report *)` | Only allocated, never activated; original unused release; terminal one-shot state |

No request generates access authority. Geometry is a 28-register snapshot, not a
complete SMMU capture or handoff. No observer automatically allocates or advances
to DMA. Allocation does not activate transport. A successful activation preserves
the actual measured capacity in `Report.capacity`; later command reports may not
carry capacity. Only a matched known-block read sets `reference_matched`.

The bridge preserves original errors and quarantine state. Backend-contract
inconsistencies stop the bridge in `Failed` and retain existing resources; this
does not claim that the original owner entered its own quarantined state. There
is no automatic release/retry/reset after uncertain DMA, no write command and no
IOStorage/IOMedia registration. `TransportReady` describes the limited transport
only, and must not be presented as root-ready or a published block device.

## Board evidence and remaining gates

The frozen resources were independently checked against **TB_J716F_ZUI_15.0.485**,
DTB0/DTBO1: HCI `0x01d84000/0x3000`, APPS SMMU `0x15000000/0x100000`, GCC
`0x00100000/0x1f0000`, SPI 265 / INTID 297. The diagnostic pages are GCC
`0x177000`, HCI `0x1d84000`, SMMU IDs `0x15000000`, one 4096-byte page each.
The expected SID pair `0xe0/0x4e0` and clock offsets come from separately pinned
Linux SM8250 sources, not measured ownership in the ROM.

The stock evidence does not establish current powered MMIO readability, firmware
and TZ exclusion, complete relevant SMMU visibility, old transaction drain or
this boot's owned DMA allocation. Stock secure request `0x800a` only adjusts EBS
variable-service policy; `0x800b` synchronization return does not propagate the
underlying storage write result. Aggregate SCM EBS return does not prove both
listeners `0x2000` and `0x2001` exited. No grants are manufactured from these facts.

References: `analysis/ufs-adapter-20261113/stock-reference.md`,
`analysis/stock-ufs-contract-20261113/README.md`, and the official
`UniXNU/docs/porting/ufs.md`. No PHY/ICE/TCU, UIC, SMC or runtime firmware address
is accessed by this bridge. Synchronous external aborts remain uncaught in the
underlying explicitly authorized MMIO reader.

## Validation

On the Linux host, using a fresh evidence directory:

```sh
python3 work/J716fStorageBringup-r30/tests/run.py \
  --root "$PWD" \
  --output "$PWD/analysis/r30-storage-inputs-20261114/storage/host-01"
```

The new core tests cover registration without I/O, stage/order and authority
rejection, precise handoff/allocation association, entry/return logs, callback
failure, quarantine retention, allowlist refusal, reference mismatch/match,
stop/discard lifetime and callback reentry. They use a modeled Runtime backend.
The same runner separately executes the existing **real** diagnostics and
Handoff/HCI/SingleSlot/owner pipeline against instrumented register/cache models.
Neither is a hardware/MMIO/DMA or IOKit Runtime execution result. The independent
official target supplies actual XNU-header compilation, native link and prelink.
No QEMU or device operation is included.
