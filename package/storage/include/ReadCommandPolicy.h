// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef SHENGUFS_READ_COMMAND_POLICY_H
#define SHENGUFS_READ_COMMAND_POLICY_H

#include <stddef.h>
#include <stdint.h>

// Packet validation for the proposed single-slot, bounded read bring-up path.
// This header performs no MMIO, DMA, allocation, cache maintenance or retries.
// Its records describe inputs; they do not prove DMA ownership or authorize a
// submission. The transport must own/map the allocation, retain an immutable
// request/context snapshot, and call this gate BEFORE touching descriptors,
// acknowledging completion or ringing a doorbell. The existing RootCommands.h
// path is not changed or protected merely by including this header.
namespace shengufs {
namespace read_policy {

enum class Direction : uint8_t { None = 0, ToDevice = 1, FromDevice = 2 };
enum class Command : uint8_t { None, Nop, PowerMode, TestUnitReady, ReadCapacity10, Read10 };
enum class Error : uint8_t {
  None, InvalidLun, InvalidLayout, WrongTag, UnsupportedCommand,
  NoncanonicalRequest, InvalidDirection, InvalidDataLength, InvalidPrd,
  CapacityRequired, InvalidCapacity, InvalidAllowlist, BlockNotAllowed,
  InvalidAccepted, InvalidResponseLength, InvalidOcs, NoncanonicalResponse,
  ScsiStatus, PowerNotReady
};

struct Range { uint64_t address; uint64_t bytes; };
struct OwnedDmaLayout {
  Range allocation, utrd, request, response, prdt, data;
};
struct Capacity {
  bool measured;
  uint8_t lun;
  uint32_t block_bytes;
  uint64_t blocks;
};
struct KnownBlock { uint8_t lun; uint64_t lba; };
enum : unsigned { kMaxKnownBlocks = 8 };
struct Context {
  uint8_t selected_lun;
  uint8_t expected_tag;
  OwnedDmaLayout dma;
  Capacity capacity;
  KnownBlock known_blocks[kMaxKnownBlocks];
  unsigned known_block_count;
};
struct Request {
  uint8_t upiu[32];
  Direction direction;
  uint32_t data_bytes;
  uint8_t prd[16]; // One final little-endian PRD, not a host-side description.
  unsigned prd_count;
};
struct Accepted {
  bool valid;
  Command command;
  uint8_t lun;
  uint8_t tag;
  uint32_t data_bytes;
  uint64_t lba;
  uint64_t data_address;
};
enum class ResponseStatus : uint8_t { Rejected, Completed, Retryable, PowerNotReady };
struct ResponseResult {
  ResponseStatus status;
  Error error;
  uint8_t scsi_status;
  uint32_t power_mode;
};

inline uint32_t loadBe32(const uint8_t *p)
{
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
         (uint32_t(p[2]) << 8) | p[3];
}
inline uint32_t loadLe32(const uint8_t *p)
{
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) |
         (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
inline bool zeroBytes(const uint8_t *p, size_t n)
{
  for (size_t i = 0; i < n; ++i) if (p[i]) return false;
  return true;
}
inline bool ordinaryLun(uint8_t lun) { return (lun & 0x80U) == 0; }
inline bool boundedRange(const Range &r)
{
  return r.bytes && r.address <= UINT64_MAX - r.bytes;
}
inline bool contains(const Range &outer, const Range &inner)
{
  return boundedRange(outer) && boundedRange(inner) &&
         inner.address >= outer.address &&
         inner.address - outer.address <= outer.bytes &&
         inner.bytes <= outer.bytes - (inner.address - outer.address);
}
inline bool overlaps(const Range &a, const Range &b)
{
  return a.address < b.address + b.bytes && b.address < a.address + a.bytes;
}

// Exact slot-0 geometry from RootTransportLayout.h, deliberately limited to
// kDmaBytesOneSlot. Larger production multi-slot allocations are out of scope.
inline Error validateLayout(const OwnedDmaLayout &d)
{
  if (!d.allocation.address || (d.allocation.address & 4095U) ||
      d.allocation.bytes != 139264 || !boundedRange(d.allocation))
    return Error::InvalidLayout;
  const Range parts[] = {d.utrd, d.request, d.response, d.prdt, d.data};
  const uint64_t offsets[] = {0, 4096, 4608, 5120, 8192};
  const uint64_t lengths[] = {32, 32, 512, 16, 131072};
  for (unsigned i = 0; i < 5; ++i) {
    if (parts[i].address != d.allocation.address + offsets[i] ||
        parts[i].bytes != lengths[i] || !contains(d.allocation, parts[i]))
      return Error::InvalidLayout;
    for (unsigned j = 0; j < i; ++j)
      if (overlaps(parts[i], parts[j])) return Error::InvalidLayout;
  }
  return Error::None;
}

inline Error validateReadBlock(const Context &c, uint64_t lba)
{
  if (!c.capacity.measured) return Error::CapacityRequired;
  // READ CAPACITY(10)'s 0xffffffff sentinel is deliberately unsupported.
  if (c.capacity.lun != c.selected_lun || c.capacity.block_bytes != 4096 ||
      !c.capacity.blocks || c.capacity.blocks > UINT64_C(0xffffffff))
    return Error::InvalidCapacity;
  if (!c.known_block_count || c.known_block_count > kMaxKnownBlocks)
    return Error::InvalidAllowlist;
  bool found = false;
  for (unsigned i = 0; i < c.known_block_count; ++i) {
    const KnownBlock &b = c.known_blocks[i];
    if (b.lun != c.selected_lun || b.lba >= c.capacity.blocks)
      return Error::InvalidAllowlist;
    for (unsigned j = 0; j < i; ++j)
      if (c.known_blocks[j].lba == b.lba) return Error::InvalidAllowlist;
    if (b.lba == lba) found = true;
  }
  return lba < c.capacity.blocks && found ? Error::None : Error::BlockNotAllowed;
}

inline Error validateRequest(const Request &r, const Context &c, Accepted &out)
{
  out = Accepted{};
  if (!ordinaryLun(c.selected_lun)) return Error::InvalidLun;
  Error error = validateLayout(c.dma);
  if (error != Error::None) return error;
  if (r.upiu[3] != c.expected_tag) return Error::WrongTag;
  if (r.direction != Direction::None && r.direction != Direction::FromDevice)
    return Error::InvalidDirection;

  uint8_t expected[32] = {};
  expected[3] = c.expected_tag;
  Command command = Command::None;
  uint32_t bytes = 0;
  uint64_t lba = 0;
  if (r.upiu[0] == 0) {
    command = Command::Nop;
  } else if (r.upiu[0] == 0x16) {
    command = Command::PowerMode;
    expected[0] = 0x16; expected[5] = 1;
    expected[12] = 3; expected[13] = 2;
  } else if (r.upiu[0] == 1) {
    if (r.upiu[2] != c.selected_lun || !ordinaryLun(r.upiu[2]))
      return Error::InvalidLun;
    expected[0] = 1; expected[2] = c.selected_lun;
    expected[16] = r.upiu[16];
    switch (r.upiu[16]) {
      case 0: command = Command::TestUnitReady; break;
      case 0x25: command = Command::ReadCapacity10; bytes = 8; break;
      case 0x28:
        command = Command::Read10; bytes = 4096;
        lba = loadBe32(r.upiu + 18);
        error = validateReadBlock(c, lba);
        if (error != Error::None) return error;
        for (unsigned i = 18; i < 22; ++i) expected[i] = r.upiu[i];
        expected[24] = 1; // Transfer length at CDB bytes 7..8, exactly 1.
        break;
      default: return Error::UnsupportedCommand;
    }
    if (bytes) {
      expected[1] = 0x40;
      expected[12] = uint8_t(bytes >> 24); expected[13] = uint8_t(bytes >> 16);
      expected[14] = uint8_t(bytes >> 8); expected[15] = uint8_t(bytes);
    }
  } else {
    return Error::UnsupportedCommand;
  }
  for (unsigned i = 0; i < sizeof(expected); ++i)
    if (r.upiu[i] != expected[i]) return Error::NoncanonicalRequest;
  if (r.direction != (bytes ? Direction::FromDevice : Direction::None))
    return Error::InvalidDirection;
  if (r.data_bytes != bytes) return Error::InvalidDataLength;

  uint64_t address = 0;
  if (bytes) {
    address = uint64_t(loadLe32(r.prd)) | (uint64_t(loadLe32(r.prd + 4)) << 32);
    const Range transfer{address, bytes};
    if (r.prd_count != 1 || (address & 3U) || loadLe32(r.prd + 8) ||
        loadLe32(r.prd + 12) != bytes - 1 || !contains(c.dma.data, transfer))
      return Error::InvalidPrd;
    const Range descriptors[] = {c.dma.utrd, c.dma.request, c.dma.response, c.dma.prdt};
    for (unsigned i = 0; i < 4; ++i)
      if (overlaps(transfer, descriptors[i])) return Error::InvalidPrd;
  } else if (r.prd_count || !zeroBytes(r.prd, sizeof(r.prd))) {
    return Error::InvalidPrd;
  }
  out = Accepted{true, command, r.upiu[2], r.upiu[3], bytes, lba, address};
  return Error::None;
}

// This checks consistency, not provenance: Accepted must be retained privately
// by the transport after validateRequest succeeds. A forged plain C++ record
// cannot be made into an ownership token by another consistency check.
inline Error validateAccepted(const Accepted &a, const Context &c)
{
  if (!a.valid || !ordinaryLun(c.selected_lun) ||
      validateLayout(c.dma) != Error::None || a.tag != c.expected_tag)
    return Error::InvalidAccepted;
  uint32_t bytes = 0;
  uint8_t lun = c.selected_lun;
  switch (a.command) {
    case Command::Nop: case Command::PowerMode: lun = 0; break;
    case Command::TestUnitReady: break;
    case Command::ReadCapacity10: bytes = 8; break;
    case Command::Read10:
      bytes = 4096;
      if (validateReadBlock(c, a.lba) != Error::None) return Error::InvalidAccepted;
      break;
    default: return Error::InvalidAccepted;
  }
  if (a.lun != lun || a.data_bytes != bytes ||
      (a.command != Command::Read10 && a.lba) ||
      (bytes ? ((a.data_address & 3U) || !contains(c.dma.data, Range{a.data_address, bytes}))
             : a.data_address != 0))
    return Error::InvalidAccepted;
  return Error::None;
}

inline Error rejectResponse(ResponseResult &out, Error error)
{
  out = ResponseResult{ResponseStatus::Rejected, error, 0, 0};
  return error;
}

// len is the available, precleared response-area snapshot, not a wire length.
// The transport must establish actual completion and synchronize DMA first.
// SCSI data-segment length may be 0, or 2 for its zero sense-length field.
// EHS, residual, exception flags and all sense bytes must be zero in this
// deliberately narrow initial policy. No CHECK CONDITION recovery or writes.
inline Error validateResponse(const Accepted &a, const Context &c, uint32_t ocs,
                              const uint8_t *rsp, size_t len, ResponseResult &out)
{
  out = ResponseResult{};
  if (validateAccepted(a, c) != Error::None)
    return rejectResponse(out, Error::InvalidAccepted);
  const bool scsi = a.command == Command::TestUnitReady ||
                    a.command == Command::ReadCapacity10 || a.command == Command::Read10;
  if (!rsp || len < (scsi ? 52U : 32U) || len > c.dma.response.bytes)
    return rejectResponse(out, Error::InvalidResponseLength);
  if (ocs != 0) return rejectResponse(out, Error::InvalidOcs);
  uint8_t expected[32] = {};
  expected[0] = scsi ? 0x21 : (a.command == Command::Nop ? 0x20 : 0x36);
  expected[2] = a.lun; expected[3] = a.tag;
  if (scsi) {
    expected[7] = rsp[7];
    if (rsp[10] == 0 && rsp[11] == 2) expected[11] = 2;
  } else if (a.command == Command::PowerMode) {
    expected[5] = 1; expected[12] = 3; expected[13] = 2;
    for (unsigned i = 20; i < 24; ++i) expected[i] = rsp[i];
  }
  for (unsigned i = 0; i < sizeof(expected); ++i)
    if (rsp[i] != expected[i]) return rejectResponse(out, Error::NoncanonicalResponse);
  if (!zeroBytes(rsp + 32, len - 32))
    return rejectResponse(out, Error::NoncanonicalResponse);
  if (scsi) {
    if (rsp[7] != 0 && rsp[7] != 8 && rsp[7] != 0x28)
      return rejectResponse(out, Error::ScsiStatus);
    out = ResponseResult{rsp[7] ? ResponseStatus::Retryable : ResponseStatus::Completed,
                         Error::None, rsp[7], 0};
  } else if (a.command == Command::PowerMode) {
    const uint32_t mode = loadBe32(rsp + 20);
    if (mode != 0x11) {
      out = ResponseResult{ResponseStatus::PowerNotReady, Error::PowerNotReady, 0, mode};
      return Error::PowerNotReady;
    }
    out = ResponseResult{ResponseStatus::Completed, Error::None, 0, mode};
  } else {
    out = ResponseResult{ResponseStatus::Completed, Error::None, 0, 0};
  }
  return Error::None;
}

// Only call after a successful ReadCapacity10 completion; decoding bytes alone
// does not establish that hardware supplied them or that they remain current.
inline Error decodeCapacity(const uint8_t *data, size_t len, uint8_t selected_lun,
                            Capacity &out)
{
  out = Capacity{};
  if (!ordinaryLun(selected_lun)) return Error::InvalidLun;
  if (!data || len != 8) return Error::InvalidCapacity;
  const uint32_t last = loadBe32(data);
  const uint32_t bytes = loadBe32(data + 4);
  if (last == 0xffffffffU || bytes != 4096) return Error::InvalidCapacity;
  out = Capacity{true, selected_lun, bytes, uint64_t(last) + 1};
  return Error::None;
}

} // namespace read_policy
} // namespace shengufs
#endif
