# TB-J716F early retained RAM log protocol

This implementation provides bounded, freestanding C serialization and frozen
snapshot decoding. It does not map physical memory, change reset registers,
install a kernel hook, create a HOB, export a QR code, or touch a device.
Those are integration tasks with separate acceptance and device evidence.

## Ownership and bounds

The stock/runtime DT reserves `[0xB0000000,0xB0080000)` as `no-map`; the
collected EFI map marks the same 512 KiB Reserved/UC. This proves allocator
exclusion in those captured states, not retention through an ABL/PMIC reset.
The current ADT converter does not propagate DTS `reserved-memory`.

UEFI keeps the existing JLOG header, capacity `0x7FFE0`, and display owner.
After successful ExitBootServices, a serialized caller may replace the JLOG
epoch with JXLG. Begin verifies JLOG magic/length/capacity/nonzero sequence and
the existing JOWN magic/matching sequence. It reads the complete reserved
region, so its pointer must cover 512 KiB, but writes only `[base,base+30848)`.
The JOWN tail `[0xB007FFF4,0xB0080000)` is never modified. After JXLG magic is
published the existing JLOG/owner API rejects the region and cannot append.

On the next firmware boot, SEC must validate and copy the 30848-byte JXLG
snapshot **before** RamLogReset. A static SEC buffer can be copied to a GUID HOB
after InitializeMemory; this is smaller than the UINT16 HOB length limit. DXE
must read that HOB, since the original address is then new-boot JLOG storage.
No pointers in the serialized data are dereferenced. Validated prior sequence
can initialize the next JLOG epoch, with an explicit wrap-to-one policy.

The caller must prove the fixed physical region is outside XNU's owned memory,
provide a single noncacheable mapping, and serialize initialization/appends on
CPU0. Do not use `phystokv` for this address or create a cached alias. Calls to
Begin and the writer state must not overlap a reader or another writer. State
and caller input buffers must not overlap the capture region. Frozen snapshots
can be read with only 30848 accessible bytes; HeaderValidate needs 128.

## Wire format v1

All integers are little endian. Header is 128 bytes, followed by 240 records of
128 bytes, for a total of 30848 bytes. CRC is IEEE CRC32, init/final XOR
`0xFFFFFFFF`, reflected polynomial `0xEDB88320`.

| Header offset | Bytes | Meaning |
|---:|---:|---|
| 0 | 4 | JXLG magic `0x474C584A`, published last |
| 4 | 2 | Version 1 |
| 6 | 2 | Header size 128 |
| 8 | 4 | Record size 128 |
| 12 | 4 | Record count 240 |
| 16 | 4 | Total size 30848 |
| 20 | 4 | Nonzero inherited JLOG boot sequence |
| 24 | 4 | Source 1: UEFI retention probe; 2: XNU |
| 28 | 4 | Zero |
| 32 | 8 | Nonzero caller-supplied boot ID |
| 40 | 8 | Physical base `0xB0000000` |
| 48 | 4 | Region size `0x80000` |
| 52 | 4 | Flags, zero |
| 56 | 32 | 1–31 printable ASCII bytes, NUL and zero padding |
| 88 | 36 | Zero |
| 124 | 4 | CRC32 over bytes 0–123, including expected magic |

| Record offset | Bytes | Meaning |
|---:|---:|---|
| 0 | 8 | Nonzero sequence |
| 8 | 4 | Length 1–104 |
| 12 | 4 | Flags, zero |
| 16 | 104 | Payload followed by zero padding |
| 120 | 4 | CRC32 over bytes 0–119 |
| 124 | 4 | `0x58434D54 XOR low32(seq) XOR high32(seq)` |

Slot index is `(seq-1)%240`. A zero commit means uncommitted. The XOR formula
can produce zero for some nonzero sequences, starting at `0x58434D54`; Append
rejects an entire call that would encounter such a sequence before modifying
RAM. It also rejects an operation that would overflow UINT64 sequence space.
This is preferable to interpreting cleared stale slots as valid records.

Begin invalidates magic, fences, clears every old record commit, writes the new
header, fences, publishes magic, and fences. Each publish clears commit,
fences, writes metadata/payload/padding/CRC, fences, publishes commit, and
fences. Tail payload is merged until 104 bytes; a partial record is republished
after each Append call. The final record of an interrupted append may be lost;
other committed records remain independently decodable. A reused ring slot can
discard its old record before the replacement commits. This protocol does not
promise atomic multi-record append or retain more than 24960 payload bytes.

## API

`include/J716fEarlyLog.h` is a C/C++ compatible static-inline implementation,
with no system headers, allocation or explicit libc calls. Supply a platform
`J716F_EL_BARRIER()` if the default ARM64 `dsb sy` is unsuitable. Host GCC/Clang
uses an atomic fence. The test-only `J716F_EL_WRITE_HOOK(offset,value)` allows
interruption after every byte store and should remain undefined in production.

```c
int J716fEarlyLogHeaderValidate(const volatile J716F_EL_U8 *memory,
    J716F_EL_U64 bytes, J716F_EL_HEADER *optional_header);
int J716fEarlyLogBegin(J716F_EL_STATE *state,
    volatile J716F_EL_U8 *memory, J716F_EL_U64 bytes,
    J716F_EL_U32 source, J716F_EL_U64 boot_id, const char *build_tag);
int J716fEarlyLogAppend(J716F_EL_STATE *state,
    const void *data, J716F_EL_U64 bytes);
int J716fEarlyLogSnapshot(const volatile J716F_EL_U8 *memory,
    J716F_EL_U64 bytes, J716F_EL_EXPORT callback, void *context,
    J716F_EL_REPORT *optional_report);
```

Zero-length Append is a no-op on an active epoch, including a NULL data pointer.
A nonzero NULL input is rejected. Append is capped at UINT32_MAX input bytes
per call, prechecks sequence/commit limits, and rejects stale writer state if
the region's magic/boot ID/boot sequence/source no longer matches. Return zero
means success. Negative enum values distinguish bad arguments, malformed
header/JLOG, inactive state, overflow/unrepresentable sequence, and callback
stop. Output arguments are only valid on success (or the documented partial
report on callback stop); a failed Begin deactivates its state.

Snapshot validates all slots, finds the highest valid sequence, and examines at
most the last 240 sequence numbers. It never loops from 1 to an arbitrary large
sequence. Its callback receives `(context, sequence, payload, length)` with a
temporary payload valid only during that callback; return zero to continue.
NULL callback validates/counts without exporting. Report distinguishes valid,
invalid, uncommitted and missing sequence records. No valid highest sequence
means an empty export, not proof that no bytes were attempted.

## Host validation

Run the independent stdlib decoder fixtures:

```text
python -B -m unittest discover -s tests -p test_decoder.py -v
```

Compile/run C writer tests with a host compiler:

```text
cc -std=c11 -O2 -Wall -Wextra -Werror tests/test_protocol.c -o /tmp/j716f-earlylog-test
/tmp/j716f-earlylog-test /tmp/j716f-earlylog-fixture.bin
python -B tests/decode_early_log.py /tmp/j716f-earlylog-fixture.bin
```

The C suite checks every byte-store interruption in Begin and partial-record
republish, barrier ordering, ring wrap, canaries through the owner tail, stale
epochs, JLOG/JOWN rejection, malformed CRC/padding, callbacks, argument bounds,
UINT64_MAX and zero-commit sequences. Python independently checks layout/CRC,
corruption, semantic metadata, stale slots, wrap and bounded high sequences.
Neither suite establishes RAM retention, cache mappings or actual XNU output.
