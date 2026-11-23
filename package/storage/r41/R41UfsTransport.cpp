// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "R41UfsTransport.h"
#include "R41Sha256.h"
#include "ReadCommandPolicy.h"
#include "SmmuGroupPlan.h"

namespace j716f_r41 {
namespace {
namespace rp = shengufs::read_policy;
namespace sg = shengufs::smmu_group;

constexpr uint64_t kGccBase = 0x00100000ULL;
constexpr uint64_t kHciBase = R35_HCI_BASE;
constexpr uint64_t kSmmuBase = R35_SMMU_BASE;
constexpr uint32_t kDisplaySlot = 4;
constexpr uint32_t kDisplaySmr = 0x84020820U;
constexpr uint32_t kDisplayS2cr = 0x00000004U;
constexpr uint32_t kUfsSlot4e0 = 2;
constexpr uint32_t kUfsSlotE0 = 3;
constexpr uint32_t kUfsSmr4e0 = 0x800004e0U;
constexpr uint32_t kUfsSmrE0 = 0x800000e0U;
constexpr uint32_t kHciFatalMask = 0x71880U;
constexpr uint32_t kHciInitFatalMask = 0x71884U;

struct BusyGuard {
  unsigned &flag;
  bool held;
  explicit BusyGuard(unsigned &f)
      : flag(f), held(__atomic_exchange_n(&f, 1U, __ATOMIC_ACQUIRE) == 0) {}
  ~BusyGuard() {
    if (held) __atomic_store_n(&flag, 0U, __ATOMIC_RELEASE);
  }
};

struct LockGuard {
  const Platform &p;
  explicit LockGuard(const Platform &plat) : p(plat) {
    if (p.lock) p.lock(p.context);
  }
  ~LockGuard() {
    if (p.unlock) p.unlock(p.context);
  }
};

void clearBytes(uint8_t *p, size_t n) {
  for (size_t i = 0; i < n; ++i) p[i] = 0;
}

void copyBytes(uint8_t *d, const uint8_t *s, size_t n) {
  for (size_t i = 0; i < n; ++i) d[i] = s[i];
}

bool bytesEqual(const uint8_t *a, const uint8_t *b, size_t n) {
  uint8_t diff = 0;
  for (size_t i = 0; i < n; ++i) diff |= static_cast<uint8_t>(a[i] ^ b[i]);
  return diff == 0;
}

void storeLe32(uint8_t *p, uint32_t v) {
  for (unsigned i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
}

uint32_t loadLe32(const uint8_t *p) {
  return static_cast<uint32_t>(p[0]) |
         (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}

uint64_t loadLe64(const uint8_t *p) {
  return static_cast<uint64_t>(loadLe32(p)) |
         (static_cast<uint64_t>(loadLe32(p + 4)) << 32);
}

uint32_t loadBe32(const uint8_t *p) {
  return (static_cast<uint32_t>(p[0]) << 24) |
         (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) |
         static_cast<uint32_t>(p[3]);
}

bool rangesOverlap(uint64_t a_base, uint64_t a_len, uint64_t b_base, uint64_t b_len) {
  return a_base < b_base + b_len && b_base < a_base + a_len;
}

bool isZeroGuid(const uint8_t guid[16]) {
  for (unsigned i = 0; i < 16; ++i) {
    if (guid[i] != 0) return false;
  }
  return true;
}

bool asciiEqual(const char *a, const char *b) {
  while (*a && *b) {
    if (*a != *b) return false;
    ++a;
    ++b;
  }
  return *a == *b;
}

void decodeUtf16LeAscii(const uint8_t *src, unsigned code_units, char *dst, size_t dst_cap) {
  size_t out = 0;
  for (unsigned i = 0; i < code_units && out + 1 < dst_cap; ++i) {
    const uint16_t ch = static_cast<uint16_t>(src[2 * i]) |
                        (static_cast<uint16_t>(src[2 * i + 1]) << 8);
    if (ch == 0) break;
    dst[out++] = (ch >= 0x20 && ch <= 0x7E) ? static_cast<char>(ch) : '?';
  }
  dst[out] = '\0';
}

bool allowReadOnlyUpiu(const uint8_t upiu[32], uint32_t data_bytes, bool from_device) {
  const uint8_t tc = upiu[0];
  if (tc == 0x00) {
    return data_bytes == 0 && !from_device;
  }
  if (tc == 0x16) {
    return data_bytes == 0 && !from_device &&
           upiu[5] == 1 && upiu[12] == 3 && upiu[13] == 2;
  }
  if (tc == 0x01) {
    const uint8_t cdb0 = upiu[16];
    if (cdb0 == 0x00) {
      return data_bytes == 0 && !from_device;
    }
    if (cdb0 == 0x25) {
      return data_bytes == 8 && from_device;
    }
    if (cdb0 == 0x28) {
      const uint32_t blocks = (static_cast<uint32_t>(upiu[23]) << 8) | upiu[24];
      return from_device && blocks >= 1 && blocks <= kMaxChunkBlocks &&
             data_bytes == blocks * kBlockBytes;
    }
  }
  return false;
}

struct WindowSliceCtx {
  uint64_t leading_skip;
  uint64_t total_bytes;
  bool (*user_sink)(void *, uint64_t, const uint8_t *, size_t);
  void *user_ctx;
  uint64_t copied;
};

bool windowSliceSink(void *ctx, uint64_t byte_off, const uint8_t *src, size_t bytes) {
  auto &s = *static_cast<WindowSliceCtx *>(ctx);
  const uint64_t chunk_end = byte_off + bytes;
  const uint64_t want_start = s.leading_skip;
  const uint64_t want_end = s.leading_skip + s.total_bytes;
  if (chunk_end <= want_start || byte_off >= want_end) return true;
  const uint64_t ov_start = byte_off > want_start ? byte_off : want_start;
  const uint64_t ov_end = chunk_end < want_end ? chunk_end : want_end;
  const size_t copy_len = static_cast<size_t>(ov_end - ov_start);
  const uint64_t src_off = ov_start - byte_off;
  const uint64_t dst_off = ov_start - want_start;
  if (!s.user_sink(s.user_ctx, dst_off, src + src_off, copy_len)) return false;
  s.copied += copy_len;
  return true;
}

uint8_t g_init_block[kBlockBytes] = {};

}  // namespace

const char *phaseName(Phase p) {
  switch (p) {
    case Phase::Waiting: return "waiting";
    case Phase::ReadRecord: return "read-record";
    case Phase::VerifyHardware: return "verify-hardware";
    case Phase::AllocateDma: return "allocate-dma";
    case Phase::InstallOwner: return "install-owner";
    case Phase::SetupQueue: return "setup-queue";
    case Phase::CommandNop: return "cmd-nop";
    case Phase::CommandPowerMode: return "cmd-power";
    case Phase::Lun4SelfTest: return "lun4-self-test";
    case Phase::Lun0Capacity: return "lun0-capacity";
    case Phase::Lun0GptScan: return "lun0-gpt-scan";
    case Phase::Online: return "online";
    case Phase::Quarantined: return "quarantined";
  }
  return "unknown";
}

const char *errorName(Error e) {
  switch (e) {
    case Error::None: return "none";
    case Error::Busy: return "busy";
    case Error::AlreadyInitialized: return "already-init";
    case Error::InvalidPlatform: return "invalid-platform";
    case Error::InvalidBudget: return "invalid-budget";
    case Error::RamSlotReadFailed: return "ram-slot-read";
    case Error::InvalidOwnerTail: return "invalid-owner-tail";
    case Error::InvalidRecord: return "invalid-record";
    case Error::RecordEpochMismatch: return "record-epoch-mismatch";
    case Error::RecordNotMaintained: return "record-not-maintained";
    case Error::RecordUnsupportedClaim: return "record-unsupported-claim";
    case Error::GccObservationFailed: return "gcc-observation";
    case Error::HciReadFailed: return "hci-read";
    case Error::HciStateMismatch: return "hci-state-mismatch";
    case Error::SmmuReadFailed: return "smmu-read";
    case Error::SmmuHeaderMismatch: return "smmu-header-mismatch";
    case Error::SmmuStreamMismatch: return "smmu-stream-mismatch";
    case Error::SmmuContextMismatch: return "smmu-context-mismatch";
    case Error::HciDrift: return "hci-drift";
    case Error::DmaAllocFailed: return "dma-alloc";
    case Error::DmaRangeInvalid: return "dma-range";
    case Error::OwnerFailed: return "owner";
    case Error::QueueSetupFailed: return "queue-setup";
    case Error::NopFailed: return "nop";
    case Error::PowerModeFailed: return "power-mode";
    case Error::Lun4TurFailed: return "lun4-tur";
    case Error::Lun4CapacityFailed: return "lun4-capacity";
    case Error::Lun4ReadFailed: return "lun4-read";
    case Error::Lun4Mismatch: return "lun4-mismatch";
    case Error::Lun0TurFailed: return "lun0-tur";
    case Error::Lun0CapacityFailed: return "lun0-capacity";
    case Error::Lun0GptReadFailed: return "lun0-gpt-read";
    case Error::Lun0GptInvalid: return "lun0-gpt-invalid";
    case Error::NotOnline: return "not-online";
    case Error::ReadOutOfRange: return "read-out-of-range";
    case Error::ReadTransferFailed: return "read-transfer-failed";
    case Error::PostCheckFailed: return "post-check-failed";
  }
  return "unknown";
}

uint32_t crc32Step(uint32_t crc, const uint8_t *data, size_t bytes) {
  static constexpr uint32_t kNibble[16] = {
      0x00000000U, 0x1DB71064U, 0x3B6E20C8U, 0x26D930ACU,
      0x76DC4190U, 0x6B6B51F4U, 0x4DB26158U, 0x5005713CU,
      0xEDB88320U, 0xF00F9344U, 0xD6D6A3E8U, 0xCB61B38CU,
      0x9B64C2B0U, 0x86D3D2D4U, 0xA00AE278U, 0xBDBDF21CU,
  };
  for (size_t i = 0; i < bytes; ++i) {
    crc ^= data[i];
    crc = (crc >> 4) ^ kNibble[crc & 0x0FU];
    crc = (crc >> 4) ^ kNibble[crc & 0x0FU];
  }
  return crc;
}

uint32_t ieeeCrc32(const uint8_t *data, size_t bytes) {
  return crc32Step(0xFFFFFFFFU, data, bytes) ^ 0xFFFFFFFFU;
}

bool Transport::failInit(Error e, uint64_t addr) {
  if (report_.error == Error::None) {
    report_.error = e;
    report_.failed_address = addr;
  }
  return false;
}

void Transport::quarantine(uint32_t reason) {
  __atomic_store_n(&online_, 0U, __ATOMIC_RELEASE);
  __atomic_store_n(&quarantined_, 1U, __ATOMIC_RELEASE);
  report_.phase = Phase::Quarantined;
  if (token_.lease_id) {
    (void)apps_smmu_owner_quarantine(&token_, 0x52343100U | (reason & 0xFFU));
  }
}

bool Transport::read32(uint64_t pa, uint32_t &out) {
  return platform_.read32 && platform_.read32(platform_.context, pa, &out);
}

bool Transport::read64(uint64_t pa, uint64_t &out) {
  return platform_.read64 && platform_.read64(platform_.context, pa, &out);
}

bool Transport::writeHci32(uint32_t offset, uint32_t val) {
  if (!((offset == 0x20 && val == 1) ||
        (offset == 0x58 && val == 1) ||
        (offset == 0x60 && val <= 1) ||
        (offset == 0x50 && val == static_cast<uint32_t>(dma_.physical)) ||
        (offset == 0x54 && val == static_cast<uint32_t>(dma_.physical >> 32)))) {
    return false;
  }
  return platform_.barrier(platform_.context) &&
         platform_.write32(platform_.context, kHciBase + offset, val) &&
         platform_.barrier(platform_.context);
}

bool Transport::cacheOp(sm8250_ufs::CacheOp op, size_t off, size_t bytes) {
  if (!bytes || off >= dma_.bytes || bytes > dma_.bytes - off) return false;
  const size_t first = off & ~size_t(63);
  const size_t end = (off + bytes + 63) & ~size_t(63);
  if (end > dma_.bytes || end <= first) return false;
  return platform_.barrier(platform_.context) &&
         platform_.cache(platform_.context, op, first, end - first) &&
         platform_.barrier(platform_.context);
}

bool Transport::observeReadThunk(void *ctx, uint64_t pa, uint32_t *out) {
  auto &self = *static_cast<Transport *>(ctx);
  return out && self.read32(pa, *out);
}

bool Transport::verifyRecord() {
  report_.phase = Phase::ReadRecord;
  record_ = {};
  uint32_t jown_magic = 0, jown_seq = 0;
  if (!platform_.read_ram_slot(platform_.context, &record_, &jown_magic, &jown_seq))
    return failInit(Error::RamSlotReadFailed, kRamSlotPhysical);
  report_.jown_sequence = jown_seq;
  if (jown_magic != kOwnerMagic || jown_seq == 0)
    return failInit(Error::InvalidOwnerTail, kOwnerPagePhysical + kOwnerOffsetInPage);

  report_.record_sequence = record_.boot_sequence;
  report_.record_crc32 = record_.crc32;

  if (!r35_record_validate(&record_))
    return failInit(Error::InvalidRecord, kRamSlotPhysical);
  if (record_.tz_exclusion_proven || record_.dma_ownership_granted ||
      record_.global_dma_drain_proven || record_.root_ready)
    return failInit(Error::RecordUnsupportedClaim, kRamSlotPhysical);
  if (record_.boot_sequence != jown_seq)
    return failInit(Error::RecordEpochMismatch, kRamSlotPhysical);
  if (record_.status != R35Maintained || record_.phase != R35Done ||
      !record_.finished || !record_.host_stopped ||
      record_.disabled_mask != 0xCU || record_.synchronized_mask != 0xCU ||
      !record_.routes_unchanged || !record_.display_config_unchanged ||
      !record_.old_namespace_captured || !record_.post_detacher_recorded ||
      !record_.post_detacher_preserved || !record_.maintenance_record_valid ||
      record_.selected_slots[0] != kUfsSlot4e0 || record_.selected_slots[1] != kUfsSlotE0 ||
      record_.selected_banks[0] != 2 || record_.selected_banks[1] != 3)
    return failInit(Error::RecordNotMaintained, kRamSlotPhysical);

  report_.record_valid = true;
  return true;
}

bool Transport::verifyHardware() {
  report_.phase = Phase::VerifyHardware;
  if (!platform_.barrier(platform_.context))
    return failInit(Error::HciReadFailed, kHciBase);

  const auto *profile = shengufs::platformProfile(shengufs::Platform::Sm8250);
  const shengufs::PlatformIdentity identity{profile->soc, profile->controller,
                                            profile->host, profile->smmu};
  const shengufs::Range ranges[] = {
      {kGccBase, 0x80000}, {kHciBase, 0x3000}, {kSmmuBase, 0x100000}};
  const shengufs::ReadGrant grant{ranges, 3, true};
  const auto obs = shengufs::observeUfs(shengufs::Platform::Sm8250, identity,
                                        grant, observeReadThunk, this);
  if (obs.status != shengufs::ObservationStatus::Captured)
    return failInit(Error::GccObservationFailed, obs.failedAddress ? obs.failedAddress : kGccBase);

  uint32_t hci1[R35_HCI_WORDS] = {};
  for (unsigned i = 0; i < R35_HCI_WORDS; ++i) {
    const uint64_t pa = hciAddress(i);
    if (!read32(pa, hci1[i])) return failInit(Error::HciReadFailed, pa);
    if (i != R35Is && hci1[i] != record_.after.hci[i])
      return failInit(Error::HciStateMismatch, pa);
  }
  if (hci1[R35Ver] != 0x300U || (hci1[R35Cap] & (1U << 30)) ||
      hci1[R35Hce] != 1U || (hci1[R35Hcs] & 0x3FU) != 0x0FU ||
      (hci1[R35Is] & kHciInitFatalMask) || hci1[R35Ie] != 0U ||
      hci1[R35TransferDoorbell] != 0U || hci1[R35TaskDoorbell] != 0U ||
      hci1[R35TransferRunStop] != 0U || hci1[R35TaskRunStop] != 0U)
    return failInit(Error::HciStateMismatch, kHciBase);

  static const unsigned hdr_offsets[4] = {0x20, 0x24, 0x28, 0x00};
  for (unsigned i = 0; i < 4; ++i) {
    uint32_t v = 0;
    const uint64_t pa = kSmmuBase + hdr_offsets[i];
    if (!read32(pa, v)) return failInit(Error::SmmuReadFailed, pa);
    if (v != record_.after.headers[i]) return failInit(Error::SmmuHeaderMismatch, pa);
  }

  unsigned match4e0 = 0, matchE0 = 0;
  for (unsigned i = 0; i < R35_GROUPS; ++i) {
    uint32_t smr = 0, s2cr = 0;
    const uint64_t pa_smr = kSmmuBase + 0x800 + 4U * i;
    const uint64_t pa_s2cr = kSmmuBase + 0xC00 + 4U * i;
    if (!read32(pa_smr, smr)) return failInit(Error::SmmuReadFailed, pa_smr);
    if (!read32(pa_s2cr, s2cr)) return failInit(Error::SmmuReadFailed, pa_s2cr);

    if (i == kUfsSlot4e0) {
      if (smr != kUfsSmr4e0 || s2cr != 2U) return failInit(Error::SmmuStreamMismatch, pa_smr);
      ++match4e0;
      continue;
    }
    if (i == kUfsSlotE0) {
      if (smr != kUfsSmrE0 || s2cr != 3U) return failInit(Error::SmmuStreamMismatch, pa_smr);
      ++matchE0;
      continue;
    }
    if (i == kDisplaySlot) {
      if (smr != kDisplaySmr || s2cr != kDisplayS2cr)
        return failInit(Error::SmmuStreamMismatch, pa_smr);
      continue;
    }
    if (!(smr & 0x80000000U)) continue;
    const uint32_t mask = (smr >> 16) & 0x7FFFU;
    const uint32_t type = (s2cr >> 16) & 3U;
    const uint32_t bank = s2cr & 255U;
    if ((((smr ^ 0x4e0U) & 0xFFFFU) & ~mask) == 0 ||
        (((smr ^ 0x0e0U) & 0xFFFFU) & ~mask) == 0)
      return failInit(Error::SmmuStreamMismatch, pa_smr);
    if (type == 0 && (bank == 2 || bank == 3))
      return failInit(Error::SmmuStreamMismatch, pa_s2cr);
  }
  if (match4e0 != 1 || matchE0 != 1)
    return failInit(Error::SmmuStreamMismatch, kSmmuBase + 0x800);

  for (unsigned idx = 0; idx < 2; ++idx) {
    const unsigned bank = idx + 2;
    const R35Context &expected = record_.after.context[idx];
    for (unsigned w = 0; w < R35_CONTEXT_WORDS; ++w) {
      uint32_t val = 0;
      const uint64_t pa = contextAddress(bank, w);
      if (!read32(pa, val)) return failInit(Error::SmmuReadFailed, pa);
      if (val != expected.words[w]) return failInit(Error::SmmuContextMismatch, pa);
    }
    const uint64_t cb_base = kSmmuBase + 0x80000ULL + (static_cast<uint64_t>(bank) << 12);
    for (unsigned t = 0; t < 2; ++t) {
      uint64_t wide = 0;
      const uint64_t pa = cb_base + 0x20ULL + 8ULL * t;
      if (!read64(pa, wide)) return failInit(Error::SmmuReadFailed, pa);
      if (wide != expected.ttbr[t]) return failInit(Error::SmmuContextMismatch, pa);
    }
    const uint32_t sctlr = expected.words[R35Sctlr];
    const uint32_t cbar = expected.words[R35Cbar];
    const uint32_t cba2r = expected.words[R35Cba2r];
    const uint32_t fsr = expected.words[R35Fsr];
    const uint32_t tlbstatus = expected.words[R35Tlbstatus];
    if ((sctlr & 1U) || ((cbar >> 16) & 3U) != 1U || cba2r != 1U ||
        (fsr & ~0x400U) != 0U || tlbstatus != 0U)
      return failInit(Error::SmmuContextMismatch, cb_base);
  }

  if (!platform_.barrier(platform_.context)) return failInit(Error::HciDrift, kHciBase);
  for (unsigned i = 0; i < R35_HCI_WORDS; ++i) {
    uint32_t v = 0;
    const uint64_t pa = hciAddress(i);
    if (!read32(pa, v)) return failInit(Error::HciReadFailed, pa);
    if (v != hci1[i]) return failInit(Error::HciDrift, pa);
  }

  report_.hardware_matched = true;
  return true;
}

bool Transport::verifyDmaRange(const sm8250_ufs::DmaView &dma) {
  if (!dma.cpu || (reinterpret_cast<uintptr_t>(dma.cpu) & 4095U) ||
      dma.bytes != kDmaBytes || !dma.physical || (dma.physical & 4095U) ||
      dma.physical > UINT64_MAX - dma.bytes ||
      reinterpret_cast<uintptr_t>(dma.cpu) > UINTPTR_MAX - dma.bytes)
    return false;
  const rp::OwnedDmaLayout layout{
      {dma.physical, dma.bytes},
      {dma.physical, 32},
      {dma.physical + 4096, 32},
      {dma.physical + 4608, 512},
      {dma.physical + 5120, 16},
      {dma.physical + kDataOffset, kMaxDataBytes}};
  if (rp::validateLayout(layout) != rp::Error::None) return false;
  const uint32_t id2 = record_.after.headers[2];
  for (unsigned shift = 0; shift <= 8; shift += 4) {
    const unsigned bits = sg::addressBits((id2 >> shift) & 15U);
    if (!bits) return false;
    const uint64_t limit = 1ULL << bits;
    if (dma.physical >= limit || dma.bytes > limit - dma.physical) return false;
  }
  if (!(record_.after.hci[R35Cap] & (1U << 24)) &&
      dma.physical + dma.bytes - 1 > UINT32_MAX)
    return false;
  static const uint64_t forbidden[][2] = {
      {0x00100000ULL, 0x00080000ULL},
      {0x01D84000ULL, 0x00014000ULL},
      {0x15000000ULL, 0x00100000ULL},
      {0x9C000000ULL, 0x02400000ULL},
      {0xB0000000ULL, 0x00080000ULL},
  };
  for (const auto &r : forbidden) {
    if (rangesOverlap(dma.physical, dma.bytes, r[0], r[1])) return false;
  }
  return true;
}

bool Transport::setupOwnerAndQueue() {
  report_.phase = Phase::AllocateDma;
  dma_ = {};
  if (!platform_.allocate_dma(platform_.context, &dma_))
    return failInit(Error::DmaAllocFailed, 0);
  report_.dma_physical = dma_.physical;
  if (!verifyDmaRange(dma_)) return failInit(Error::DmaRangeInvalid, dma_.physical);

  report_.phase = Phase::InstallOwner;
  claim_ = {};
  claim_.client_id = 0x4A37313646523431ULL;  // "J716FR41"
  claim_.handoff_epoch = (static_cast<uint64_t>(record_.boot_sequence) << 32) | 1ULL;
  claim_.controller_base = kHciBase;
  claim_.smmu_base = kSmmuBase;
  claim_.sid_count = 2;
  claim_.slot_count = 2;
  claim_.sids[0] = 0x00e0;
  claim_.sids[1] = 0x04e0;
  claim_.slots[0] = kUfsSlotE0;
  claim_.slots[1] = kUfsSlot4e0;
  claim_.bank = 2;
  claim_.dma_base = dma_.physical;
  claim_.dma_bytes = dma_.bytes;

  AppsSmmuHandoff handoff = {};
  handoff.authorization_id =
      record_.boot_id ^ (static_cast<uint64_t>(record_.boot_sequence) << 32) ^ record_.crc32;
  if (handoff.authorization_id == 0) handoff.authorization_id = 1;
  handoff.flags = APPS_SMMU_HANDOFF_REQUIRED;
  handoff.claim = claim_;

  report_.owner_error = apps_smmu_owner_install_handoff(&handoff);
  if (report_.owner_error != APPS_SMMU_OK) return failInit(Error::OwnerFailed, kSmmuBase);
  report_.owner_error = apps_smmu_owner_try_acquire(&claim_, &token_);
  if (report_.owner_error != APPS_SMMU_OK) return failInit(Error::OwnerFailed, kSmmuBase);
  report_.owner_error = apps_smmu_owner_publication_begin(&token_);
  if (report_.owner_error != APPS_SMMU_OK) return failInit(Error::OwnerFailed, kSmmuBase);
  report_.owner_error = apps_smmu_owner_publication_complete(&token_);
  if (report_.owner_error != APPS_SMMU_OK) return failInit(Error::OwnerFailed, kSmmuBase);

  report_.phase = Phase::SetupQueue;
  if (!writeHci32(0x50, static_cast<uint32_t>(dma_.physical)) ||
      !writeHci32(0x54, static_cast<uint32_t>(dma_.physical >> 32)))
    return failInit(Error::QueueSetupFailed, kHciBase + 0x50);

  uint32_t lo = 0, hi = 0, rsr = 0;
  if (!read32(kHciBase + 0x50, lo) || !read32(kHciBase + 0x54, hi) ||
      !read32(kHciBase + 0x60, rsr) ||
      (static_cast<uint64_t>(lo) | (static_cast<uint64_t>(hi) << 32)) != dma_.physical ||
      rsr != 0U)
    return failInit(Error::QueueSetupFailed, kHciBase + 0x50);

  if (!writeHci32(0x60, 1U)) return failInit(Error::QueueSetupFailed, kHciBase + 0x60);

  uint32_t hce = 0, hcs = 0, db = 0, tdb = 0, ie = 0, is = 0;
  if (!read32(kHciBase + 0x34, hce) || !read32(kHciBase + 0x30, hcs) ||
      !read32(kHciBase + 0x60, rsr) || !read32(kHciBase + 0x58, db) ||
      !read32(kHciBase + 0x78, tdb) || !read32(kHciBase + 0x24, ie) ||
      !read32(kHciBase + 0x20, is) ||
      hce != 1U || (hcs & 3U) != 3U || rsr != 1U || db != 0U || tdb != 0U ||
      ie != 0U || (is & kHciFatalMask) != 0U ||
      apps_smmu_owner_queue_started(&token_) != APPS_SMMU_OK) {
    quarantine(static_cast<uint32_t>(Error::QueueSetupFailed));
    return failInit(Error::QueueSetupFailed, kHciBase + 0x60);
  }

  report_.queue_started = true;
  return true;
}

Transport::CmdOutcome Transport::submitLocked(const uint8_t upiu[32], uint32_t data_bytes,
                                              bool from_device) {
  CmdOutcome out = {};
  if (__atomic_load_n(&quarantined_, __ATOMIC_ACQUIRE)) return out;
  if (!allowReadOnlyUpiu(upiu, data_bytes, from_device)) return out;
  if (apps_smmu_owner_validate(&token_, &claim_) != APPS_SMMU_OK) {
    quarantine(static_cast<uint32_t>(Error::OwnerFailed));
    return out;
  }

  uint32_t hce = 0, hcs = 0, ie = 0, is = 0, db = 0, tdb = 0, rsr = 0, lo = 0, hi = 0;
  if (!platform_.barrier(platform_.context) ||
      !read32(kHciBase + 0x34, hce) || !read32(kHciBase + 0x30, hcs) ||
      !read32(kHciBase + 0x24, ie) || !read32(kHciBase + 0x20, is) ||
      !read32(kHciBase + 0x58, db) || !read32(kHciBase + 0x78, tdb) ||
      !read32(kHciBase + 0x60, rsr) || !read32(kHciBase + 0x50, lo) ||
      !read32(kHciBase + 0x54, hi) ||
      hce != 1U || (hcs & 3U) != 3U || ie != 0U || (is & kHciFatalMask) != 0U ||
      db != 0U || tdb != 0U || rsr != 1U ||
      (static_cast<uint64_t>(lo) | (static_cast<uint64_t>(hi) << 32)) != dma_.physical) {
    quarantine(static_cast<uint32_t>(Error::ReadTransferFailed));
    return out;
  }

  clearBytes(dma_.cpu, 64);
  clearBytes(dma_.cpu + 4096, 1088);
  const uint32_t dir_bits = from_device ? (2U << 25) : 0U;
  storeLe32(dma_.cpu + 0, 0x10000000U | dir_bits);
  storeLe32(dma_.cpu + 8, 15U);
  storeLe32(dma_.cpu + 16, static_cast<uint32_t>(dma_.physical + 4096));
  storeLe32(dma_.cpu + 20, static_cast<uint32_t>((dma_.physical + 4096) >> 32));
  storeLe32(dma_.cpu + 24, 128U | (128U << 16));
  storeLe32(dma_.cpu + 28, (data_bytes ? 1U : 0U) | (256U << 16));

  copyBytes(dma_.cpu + 4096, upiu, 32);
  if (data_bytes) {
    const uint64_t data_pa = dma_.physical + kDataOffset;
    storeLe32(dma_.cpu + 5120, static_cast<uint32_t>(data_pa));
    storeLe32(dma_.cpu + 5124, static_cast<uint32_t>(data_pa >> 32));
    storeLe32(dma_.cpu + 5128, 0U);
    storeLe32(dma_.cpu + 5132, data_bytes - 1U);
  }

  if (!cacheOp(sm8250_ufs::CacheOp::CleanInvalidate, 0, 64) ||
      !cacheOp(sm8250_ufs::CacheOp::CleanInvalidate, 4096, 1088)) {
    quarantine(static_cast<uint32_t>(Error::ReadTransferFailed));
    return out;
  }
  if (data_bytes && !cacheOp(sm8250_ufs::CacheOp::Invalidate, kDataOffset, data_bytes)) {
    quarantine(static_cast<uint32_t>(Error::ReadTransferFailed));
    return out;
  }

  if (!writeHci32(0x20, 1U) || !writeHci32(0x58, 1U)) {
    quarantine(static_cast<uint32_t>(Error::ReadTransferFailed));
    return out;
  }

  uint64_t t0 = 0;
  if (!platform_.ticks(platform_.context, &t0)) {
    quarantine(static_cast<uint32_t>(Error::ReadTransferFailed));
    return out;
  }

  bool done = false;
  for (uint32_t poll = 0; poll < budget_.command_max_polls; ++poll) {
    uint64_t now = 0;
    if (!read32(kHciBase + 0x58, db) || !read32(kHciBase + 0x78, tdb) ||
        !read32(kHciBase + 0x20, is) || !read32(kHciBase + 0x60, rsr) ||
        !platform_.ticks(platform_.context, &now) ||
        rsr != 1U || (is & kHciFatalMask) != 0U || now < t0) {
      quarantine(static_cast<uint32_t>(Error::ReadTransferFailed));
      return out;
    }
    if (db == 0U && tdb == 0U) {
      done = true;
      break;
    }
    if (now - t0 >= budget_.command_timeout_ticks) {
      quarantine(static_cast<uint32_t>(Error::ReadTransferFailed));
      return out;
    }
  }
  if (!done) {
    quarantine(static_cast<uint32_t>(Error::ReadTransferFailed));
    return out;
  }

  if (!cacheOp(sm8250_ufs::CacheOp::Invalidate, 0, 64) ||
      !cacheOp(sm8250_ufs::CacheOp::Invalidate, 4608, 512)) {
    quarantine(static_cast<uint32_t>(Error::ReadTransferFailed));
    return out;
  }
  if (data_bytes && !cacheOp(sm8250_ufs::CacheOp::Invalidate, kDataOffset, data_bytes)) {
    quarantine(static_cast<uint32_t>(Error::ReadTransferFailed));
    return out;
  }

  out.ocs = loadLe32(dma_.cpu + 8) & 0xFFU;
  const uint8_t *rsp = dma_.cpu + 4608;
  out.rsp_type = rsp[0] & 0x3FU;
  out.rsp_status = rsp[6];
  out.scsi_status = rsp[7];
  if (out.rsp_type == 0x21 && out.scsi_status == 0x02) {
    const uint16_t dseg = (static_cast<uint16_t>(rsp[10]) << 8) | rsp[11];
    if (dseg >= 5) out.sense_key = rsp[34 + 2] & 0x0FU;
  }
  if (out.ocs != 0 || rsp[3] != upiu[3]) return out;
  out.ok = true;
  return out;
}

bool Transport::cmdNopLocked() {
  uint8_t upiu[32] = {};
  upiu[0] = 0x00;
  upiu[3] = next_tag_++;
  const auto res = submitLocked(upiu, 0, false);
  return res.ok && res.rsp_type == 0x20 && res.rsp_status == 0;
}

bool Transport::cmdPowerModeLocked() {
  uint8_t upiu[32] = {};
  upiu[0] = 0x16;
  upiu[3] = next_tag_++;
  upiu[5] = 1;
  upiu[12] = 3;
  upiu[13] = 2;
  const auto res = submitLocked(upiu, 0, false);
  if (!res.ok || res.rsp_type != 0x36 || res.rsp_status != 0) return false;
  return loadBe32(dma_.cpu + 4608 + 20) == 0x11U;
}

bool Transport::cmdTurLocked(uint8_t lun) {
  for (unsigned attempt = 0; attempt <= budget_.tur_retries; ++attempt) {
    uint8_t upiu[32] = {};
    upiu[0] = 0x01;
    upiu[2] = lun;
    upiu[3] = next_tag_++;
    upiu[16] = 0x00;
    const auto res = submitLocked(upiu, 0, false);
    if (!res.ok || res.rsp_type != 0x21 || res.rsp_status != 0) return false;
    if (res.scsi_status == 0x00) return true;
    if (((res.scsi_status == 0x02 && res.sense_key == 0x06) ||
         res.scsi_status == 0x08 || res.scsi_status == 0x28) &&
        attempt < budget_.tur_retries)
      continue;
    return false;
  }
  return false;
}

bool Transport::cmdReadCapacityLocked(uint8_t lun, LunInfo &out) {
  out = {};
  uint8_t upiu[32] = {};
  upiu[0] = 0x01;
  upiu[1] = 0x40;
  upiu[2] = lun;
  upiu[3] = next_tag_++;
  upiu[15] = 8;
  upiu[16] = 0x25;
  const auto res = submitLocked(upiu, 8, true);
  if (!res.ok || res.rsp_type != 0x21 || res.rsp_status != 0 || res.scsi_status != 0)
    return false;
  const uint8_t *d = dma_.cpu + kDataOffset;
  const uint32_t last_lba = loadBe32(d);
  const uint32_t blk_bytes = loadBe32(d + 4);
  if (blk_bytes != kBlockBytes || last_lba == 0 || last_lba == 0xFFFFFFFFU)
    return false;
  out.ready = true;
  out.blocks = static_cast<uint64_t>(last_lba) + 1ULL;
  out.block_bytes = blk_bytes;
  return true;
}

bool Transport::cmdRead10ChunkLocked(uint8_t lun, uint64_t lba, uint32_t blocks, uint8_t *dst) {
  if (blocks == 0 || blocks > kMaxChunkBlocks || lba > 0xFFFFFFFFULL)
    return false;
  const LunInfo &info = (lun == kUserLun) ? report_.lun0 :
                        (lun == kKnownLun) ? report_.lun4 : LunInfo{};
  if (!info.ready || lba >= info.blocks || blocks > info.blocks - lba)
    return false;

  const uint32_t bytes = blocks * kBlockBytes;
  for (unsigned attempt = 0; attempt <= budget_.tur_retries; ++attempt) {
    uint8_t upiu[32] = {};
    upiu[0] = 0x01;
    upiu[1] = 0x40;
    upiu[2] = lun;
    upiu[3] = next_tag_++;
    upiu[12] = static_cast<uint8_t>(bytes >> 24);
    upiu[13] = static_cast<uint8_t>(bytes >> 16);
    upiu[14] = static_cast<uint8_t>(bytes >> 8);
    upiu[15] = static_cast<uint8_t>(bytes);
    upiu[16] = 0x28;
    upiu[18] = static_cast<uint8_t>(lba >> 24);
    upiu[19] = static_cast<uint8_t>(lba >> 16);
    upiu[20] = static_cast<uint8_t>(lba >> 8);
    upiu[21] = static_cast<uint8_t>(lba);
    upiu[23] = static_cast<uint8_t>(blocks >> 8);
    upiu[24] = static_cast<uint8_t>(blocks);

    const auto res = submitLocked(upiu, bytes, true);
    if (!res.ok || res.rsp_type != 0x21 || res.rsp_status != 0) return false;
    if (res.scsi_status == 0x00) {
      if (dst) copyBytes(dst, dma_.cpu + kDataOffset, bytes);
      __atomic_add_fetch(&read_commands_, 1ULL, __ATOMIC_RELAXED);
      __atomic_add_fetch(&blocks_read_, blocks, __ATOMIC_RELAXED);
      if (blocks > max_chunk_blocks_) max_chunk_blocks_ = blocks;
      return true;
    }
    if ((res.scsi_status == 0x08 || res.scsi_status == 0x28) && attempt < budget_.tur_retries)
      continue;
    return false;
  }
  return false;
}

void Transport::parseShngRootLocked() {
  GptSummary &g = report_.gpt;
  ShngRootManifest &m = g.manifest;
  m = {};
  if (!g.found_userdata || g.userdata_last_lba < g.userdata_first_lba ||
      g.userdata_last_lba >= report_.lun0.blocks) {
    m.status = 1;
    return;
  }
  if (!cmdRead10ChunkLocked(kUserLun, g.userdata_first_lba, 1, g_init_block)) {
    m.status = 1;
    return;
  }
  m.magic0 = loadLe32(g_init_block);
  m.magic1 = loadLe32(g_init_block + 4);
  if (m.magic0 != 0x474E4853U || m.magic1 != 0x544F4F52U) {
    m.status = 1;
    return;
  }

  m.version = loadLe32(g_init_block + 8);
  const uint32_t count = loadLe32(g_init_block + 12);
  const uint64_t image_bytes = loadLe64(g_init_block + 16);
  if (!((m.version == 1 && count >= 1 && count <= kMaxRootWindows) ||
        (m.version == 2 && count >= 1 && count <= 8))) {
    m.status = 2;
    return;
  }

  uint8_t digest[32] = {};
  sha256Compute(g_init_block, 4064, digest);
  if (!bytesEqual(digest, g_init_block + 4064, 32)) {
    m.status = 3;
    return;
  }

  const uint64_t part_bytes = (g.userdata_last_lba - g.userdata_first_lba + 1ULL) * kBlockBytes;
  if (image_bytes < kBlockBytes || (image_bytes & (kBlockBytes - 1)) != 0 ||
      image_bytes > part_bytes) {
    m.status = 4;
    return;
  }

  const uint64_t ud_base_bytes = g.userdata_first_lba * kBlockBytes;
  uint32_t wcount = 0;
  for (uint32_t idx = 0; idx < count; ++idx) {
    const uint8_t *e = g_init_block + 24 + 128 * idx;
    const uint64_t offset = loadLe64(e + 32);
    const uint64_t size = loadLe64(e + 40);
    const uint32_t bsz = loadLe32(e + 48);
    const uint32_t flags = loadLe32(e + 52);
    const bool is_window = (bsz == kLogicalBlockBytes && flags == 1U);
    const bool is_object = (m.version == 2 && bsz == kBlockBytes && flags == 3U);
    if (!is_window && !is_object) {
      m.status = 7;
      return;
    }
    if ((offset & (kBlockBytes - 1)) != 0 || offset < kBlockBytes || size == 0 ||
        (is_window && (size & (kLogicalBlockBytes - 1)) != 0) ||
        offset >= image_bytes || size > image_bytes - offset ||
        ((size + kBlockBytes - 1) & ~uint64_t(kBlockBytes - 1)) > image_bytes - offset) {
      m.status = 4;
      return;
    }
    unsigned nlen = 0;
    while (nlen < 32 && e[nlen] >= 0x20 && e[nlen] <= 0x7E) ++nlen;
    if (nlen == 0 || nlen == 32 || e[nlen] != 0) {
      m.status = 5;
      return;
    }
    if (!is_window) continue;
    if (wcount >= kMaxRootWindows) {
      m.status = 7;
      return;
    }
    const uint64_t padded_size = (size + kBlockBytes - 1) & ~uint64_t(kBlockBytes - 1);
    for (uint32_t prev = 0; prev < wcount; ++prev) {
      if (asciiEqual(reinterpret_cast<const char *>(e), m.windows[prev].name)) {
        m.status = 5;
        return;
      }
      const uint64_t prev_padded =
          (m.windows[prev].size_bytes + kBlockBytes - 1) & ~uint64_t(kBlockBytes - 1);
      if (rangesOverlap(offset, padded_size, m.windows[prev].rel_offset, prev_padded)) {
        m.status = 6;
        return;
      }
    }
    auto &slot = m.windows[wcount];
    copyBytes(reinterpret_cast<uint8_t *>(slot.name), e, nlen);
    slot.name[nlen] = '\0';
    slot.rel_offset = offset;
    slot.abs_offset = ud_base_bytes + offset;
    slot.size_bytes = size;
    copyBytes(slot.sha256, e + 56, 32);
    if (asciiEqual(slot.name, "OS")) m.os_index = static_cast<int>(wcount);
    if (asciiEqual(slot.name, "SystemOS")) m.systemos_index = static_cast<int>(wcount);
    ++wcount;
  }

  if (m.os_index < 0 || m.systemos_index < 0) {
    m.status = 7;
    return;
  }
  m.image_bytes = image_bytes;
  m.window_count = wcount;
  m.status = 0;
  m.valid = true;
}

bool Transport::scanLun0GptLocked() {
  report_.phase = Phase::Lun0GptScan;
  GptSummary &g = report_.gpt;
  g = {};

  if (!cmdRead10ChunkLocked(kUserLun, 1, 1, g_init_block))
    return failInit(Error::Lun0GptReadFailed, 1);

  if (loadLe64(g_init_block) != 0x5452415020494645ULL)
    return failInit(Error::Lun0GptInvalid, 1);

  const uint32_t hdr_size = loadLe32(g_init_block + 12);
  const uint32_t hdr_crc = loadLe32(g_init_block + 16);
  if (hdr_size < 92 || hdr_size > kBlockBytes)
    return failInit(Error::Lun0GptInvalid, 1);

  uint8_t saved_crc[4];
  copyBytes(saved_crc, g_init_block + 16, 4);
  clearBytes(g_init_block + 16, 4);
  const uint32_t calc_hdr_crc = ieeeCrc32(g_init_block, hdr_size);
  copyBytes(g_init_block + 16, saved_crc, 4);
  if (calc_hdr_crc != hdr_crc)
    return failInit(Error::Lun0GptInvalid, 1);

  g.header_valid = true;
  const uint64_t entry_lba = loadLe64(g_init_block + 72);
  const uint32_t total_slots = loadLe32(g_init_block + 80);
  const uint32_t entry_size = loadLe32(g_init_block + 84);
  const uint32_t entries_crc = loadLe32(g_init_block + 88);

  if (entry_size < 128 || entry_size > kBlockBytes ||
      (kBlockBytes % entry_size) != 0 ||
      total_slots == 0 || total_slots > 256 ||
      entry_lba < 2 || entry_lba >= report_.lun0.blocks)
    return failInit(Error::Lun0GptInvalid, entry_lba);

  const uint64_t total_entry_bytes = static_cast<uint64_t>(total_slots) * entry_size;
  const uint64_t entry_blocks = (total_entry_bytes + kBlockBytes - 1) / kBlockBytes;
  if (entry_blocks == 0 || entry_blocks > 32 ||
      entry_blocks > report_.lun0.blocks - entry_lba)
    return failInit(Error::Lun0GptInvalid, entry_lba);

  uint32_t running_crc = 0xFFFFFFFFU;
  uint64_t remaining_bytes = total_entry_bytes;
  uint32_t slot_index = 0;

  for (uint64_t b = 0; b < entry_blocks; ++b) {
    if (!cmdRead10ChunkLocked(kUserLun, entry_lba + b, 1, g_init_block))
      return failInit(Error::Lun0GptReadFailed, entry_lba + b);
    const size_t chunk = remaining_bytes < kBlockBytes
                             ? static_cast<size_t>(remaining_bytes)
                             : kBlockBytes;
    running_crc = crc32Step(running_crc, g_init_block, chunk);
    remaining_bytes -= chunk;

    for (size_t off = 0; off + entry_size <= chunk && slot_index < total_slots;
         off += entry_size, ++slot_index) {
      const uint8_t *e = g_init_block + off;
      if (isZeroGuid(e)) continue;
      ++g.active_partitions;
      char name[24] = {};
      decodeUtf16LeAscii(e + 56, 36, name, sizeof(name));
      if (asciiEqual(name, "userdata")) {
        g.found_userdata = true;
        g.userdata_first_lba = loadLe64(e + 32);
        g.userdata_last_lba = loadLe64(e + 40);
      }
    }
  }

  if ((running_crc ^ 0xFFFFFFFFU) != entries_crc)
    return failInit(Error::Lun0GptInvalid, entry_lba);

  g.entries_valid = true;
  parseShngRootLocked();
  return true;
}

PostCheck Transport::postCheck() {
  PostCheck p = {};
  uint32_t smr4 = 0, s2cr4 = 0;
  const bool ok =
      read32(kSmmuBase + 0x800 + 4U * kDisplaySlot, smr4) &&
      read32(kSmmuBase + 0xC00 + 4U * kDisplaySlot, s2cr4) &&
      read32(contextAddress(2, R35Fsr), p.cb2_fsr) &&
      read32(contextAddress(3, R35Fsr), p.cb3_fsr) &&
      read32(contextAddress(2, R35Tlbstatus), p.cb2_tlbstatus) &&
      read32(contextAddress(3, R35Tlbstatus), p.cb3_tlbstatus) &&
      read32(kHciBase + 0x60, p.utrlrsr) &&
      read32(kHciBase + 0x58, p.utrldbr);
  p.checked = ok;
  p.display_slot4_unchanged = ok && smr4 == kDisplaySmr && s2cr4 == kDisplayS2cr;
  return p;
}

InitReport Transport::initialize(const Platform &platform, const Budget &budget,
                                 const uint8_t (&lun4_reference)[kBlockBytes]) {
  BusyGuard busy(busy_);
  if (!busy.held) {
    InitReport r;
    r.error = Error::Busy;
    return r;
  }
  if (__atomic_exchange_n(&initialized_, 1U, __ATOMIC_ACQ_REL)) {
    InitReport r;
    r.error = Error::AlreadyInitialized;
    return r;
  }
  report_ = {};
  if (!platform.read_ram_slot || !platform.read32 || !platform.read64 ||
      !platform.write32 || !platform.barrier || !platform.ticks ||
      !platform.cache || !platform.allocate_dma) {
    failInit(Error::InvalidPlatform);
    return report_;
  }
  if (!budget.command_timeout_ticks || !budget.command_max_polls ||
      budget.command_max_polls > 1000000 || budget.tur_retries > 4) {
    failInit(Error::InvalidBudget);
    return report_;
  }
  platform_ = platform;
  budget_ = budget;

  if (!verifyRecord() || !verifyHardware() || !setupOwnerAndQueue()) {
    return report_;
  }

  report_.phase = Phase::CommandNop;
  if (!cmdNopLocked()) {
    quarantine(static_cast<uint32_t>(Error::NopFailed));
    failInit(Error::NopFailed, kHciBase + 0x58);
    return report_;
  }
  report_.phase = Phase::CommandPowerMode;
  if (!cmdPowerModeLocked()) {
    quarantine(static_cast<uint32_t>(Error::PowerModeFailed));
    failInit(Error::PowerModeFailed, kHciBase + 0x58);
    return report_;
  }

  report_.phase = Phase::Lun4SelfTest;
  if (!cmdTurLocked(kKnownLun)) {
    quarantine(static_cast<uint32_t>(Error::Lun4TurFailed));
    failInit(Error::Lun4TurFailed, kHciBase + 0x58);
    return report_;
  }
  if (!cmdReadCapacityLocked(kKnownLun, report_.lun4) ||
      report_.lun4.blocks <= kKnownLba) {
    quarantine(static_cast<uint32_t>(Error::Lun4CapacityFailed));
    failInit(Error::Lun4CapacityFailed, kHciBase + 0x58);
    return report_;
  }
  if (!cmdRead10ChunkLocked(kKnownLun, kKnownLba, 1, g_init_block)) {
    quarantine(static_cast<uint32_t>(Error::Lun4ReadFailed));
    failInit(Error::Lun4ReadFailed, kKnownLba);
    return report_;
  }
  report_.lun4_crc32 = ieeeCrc32(g_init_block, kBlockBytes);
  bool matched = true;
  for (size_t i = 0; i < kBlockBytes; ++i) {
    if (g_init_block[i] != lun4_reference[i]) {
      matched = false;
      break;
    }
  }
  report_.lun4_matched = matched;
  if (!matched) {
    quarantine(static_cast<uint32_t>(Error::Lun4Mismatch));
    failInit(Error::Lun4Mismatch, kKnownLba);
    return report_;
  }

  report_.phase = Phase::Lun0Capacity;
  if (!cmdTurLocked(kUserLun)) {
    quarantine(static_cast<uint32_t>(Error::Lun0TurFailed));
    failInit(Error::Lun0TurFailed, kHciBase + 0x58);
    return report_;
  }
  if (!cmdReadCapacityLocked(kUserLun, report_.lun0) || report_.lun0.blocks < 64) {
    quarantine(static_cast<uint32_t>(Error::Lun0CapacityFailed));
    failInit(Error::Lun0CapacityFailed, kHciBase + 0x58);
    return report_;
  }
  if (!scanLun0GptLocked()) {
    quarantine(static_cast<uint32_t>(report_.error));
    return report_;
  }

  report_.post = postCheck();
  if (!report_.post.checked || !report_.post.display_slot4_unchanged ||
      (report_.post.cb2_fsr & ~0x400U) != 0U ||
      (report_.post.cb3_fsr & ~0x400U) != 0U ||
      report_.post.cb2_tlbstatus != 0U || report_.post.cb3_tlbstatus != 0U) {
    quarantine(static_cast<uint32_t>(Error::PostCheckFailed));
    failInit(Error::PostCheckFailed, kSmmuBase);
    return report_;
  }

  report_.phase = Phase::Online;
  __atomic_store_n(&online_, 1U, __ATOMIC_RELEASE);
  return report_;
}

bool Transport::readBlocksWithSink(uint8_t lun, uint64_t lba, uint32_t block_count,
                                   bool (*sink)(void *ctx, uint64_t byte_off,
                                                const uint8_t *src, size_t bytes),
                                   void *sink_ctx) {
  if (!online() || !sink || block_count == 0) {
    __atomic_add_fetch(&read_errors_, 1ULL, __ATOMIC_RELAXED);
    return false;
  }
  const LunInfo &info = (lun == kUserLun) ? report_.lun0 :
                        (lun == kKnownLun) ? report_.lun4 : LunInfo{};
  if (!info.ready || lba >= info.blocks || block_count > info.blocks - lba) {
    __atomic_add_fetch(&read_errors_, 1ULL, __ATOMIC_RELAXED);
    return false;
  }

  LockGuard guard(platform_);
  if (!online()) {
    __atomic_add_fetch(&read_errors_, 1ULL, __ATOMIC_RELAXED);
    return false;
  }
  __atomic_add_fetch(&read_calls_, 1ULL, __ATOMIC_RELAXED);

  uint32_t remaining = block_count;
  uint64_t cur_lba = lba;
  uint64_t byte_off = 0;
  while (remaining > 0) {
    const uint32_t chunk = remaining > kMaxChunkBlocks ? kMaxChunkBlocks : remaining;
    if (!cmdRead10ChunkLocked(lun, cur_lba, chunk, nullptr)) {
      __atomic_add_fetch(&read_errors_, 1ULL, __ATOMIC_RELAXED);
      return false;
    }
    const size_t chunk_bytes = static_cast<size_t>(chunk) * kBlockBytes;
    if (!sink(sink_ctx, byte_off, dma_.cpu + kDataOffset, chunk_bytes)) {
      __atomic_add_fetch(&read_errors_, 1ULL, __ATOMIC_RELAXED);
      return false;
    }
    cur_lba += chunk;
    byte_off += chunk_bytes;
    remaining -= chunk;
  }
  return true;
}

bool Transport::readWindow512WithSink(uint64_t abs_offset_bytes, uint64_t window_bytes,
                                      uint64_t block512, uint64_t count512,
                                      bool (*sink)(void *ctx, uint64_t dst_off,
                                                   const uint8_t *src, size_t bytes),
                                      void *sink_ctx, uint64_t *out_bytes) {
  if (out_bytes) *out_bytes = 0;
  if (!online() || !sink || !report_.lun0.ready || count512 == 0 ||
      (abs_offset_bytes & (kBlockBytes - 1)) != 0 ||
      window_bytes == 0 || (window_bytes & (kLogicalBlockBytes - 1)) != 0) {
    __atomic_add_fetch(&read_errors_, 1ULL, __ATOMIC_RELAXED);
    return false;
  }
  const uint64_t total_blocks512 = window_bytes / kLogicalBlockBytes;
  if (block512 >= total_blocks512 || count512 > total_blocks512 - block512) {
    __atomic_add_fetch(&read_errors_, 1ULL, __ATOMIC_RELAXED);
    return false;
  }
  const uint64_t lun_bytes = report_.lun0.blocks * kBlockBytes;
  const uint64_t padded_window = (window_bytes + kBlockBytes - 1) & ~uint64_t(kBlockBytes - 1);
  if (abs_offset_bytes >= lun_bytes || padded_window > lun_bytes - abs_offset_bytes) {
    __atomic_add_fetch(&read_errors_, 1ULL, __ATOMIC_RELAXED);
    return false;
  }

  const uint64_t abs_start = abs_offset_bytes + block512 * kLogicalBlockBytes;
  const uint64_t total_bytes = count512 * kLogicalBlockBytes;
  const uint64_t first_lba = abs_start / kBlockBytes;
  const uint64_t last_lba = (abs_start + total_bytes - 1ULL) / kBlockBytes;
  const uint64_t lba_count = last_lba - first_lba + 1ULL;
  if (lba_count == 0 || lba_count > UINT32_MAX) {
    __atomic_add_fetch(&read_errors_, 1ULL, __ATOMIC_RELAXED);
    return false;
  }

  WindowSliceCtx slice{abs_start & (kBlockBytes - 1), total_bytes, sink, sink_ctx, 0};
  const bool ok = readBlocksWithSink(
      kUserLun, first_lba, static_cast<uint32_t>(lba_count), windowSliceSink, &slice);
  if (out_bytes) *out_bytes = slice.copied;
  return ok && slice.copied == total_bytes;
}

Stats Transport::stats() const {
  Stats s;
  s.read_calls = __atomic_load_n(&read_calls_, __ATOMIC_RELAXED);
  s.read_commands = __atomic_load_n(&read_commands_, __ATOMIC_RELAXED);
  s.blocks_read = __atomic_load_n(&blocks_read_, __ATOMIC_RELAXED);
  s.read_errors = __atomic_load_n(&read_errors_, __ATOMIC_RELAXED);
  s.max_chunk_blocks = max_chunk_blocks_;
  s.quarantined = __atomic_load_n(&quarantined_, __ATOMIC_ACQUIRE) != 0;
  return s;
}

}  // namespace j716f_r41
