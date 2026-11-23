// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once

#include <stdint.h>

namespace shengufs {
namespace smmu_group {

// Pure planning only: no MMIO callbacks, allocation, lock or commit operation.
// An eventual adapter must obtain its grant from the real platform owner and
// capture/revalidate under that owner's shared lock with the queue quiesced.
// These plain records describe that contract; they cannot prove HW ownership.
constexpr unsigned kMaxStreams = 2;
constexpr unsigned kMaxGroups = 128;
constexpr unsigned kMaxContexts = 128;
constexpr unsigned kNoIndex = ~0U;

enum class Error {
  None,
  Unplanned,
  InvalidRequest,
  UnvalidatedGeometry,
  UnsupportedGeometry,
  IncompleteSnapshot,
  MissingOwnerGrant,
  OwnerMismatch,
  DuplicateSid,
  DmaOutsideGrant,
  DmaAddressWidth,
  SidAddressWidth,
  MaskedOverlap,
  DuplicateMatch,
  InvalidStreamMapping,
  DifferentBanks,
  SlotNotGranted,
  BankNotGranted,
  NoAvailableSlots,
  NoAvailableBank,
  ForeignBankUser,
  ContextNotStage1,
  ContextNotBypass,
  InvalidPlan,
  SnapshotChanged,
  GrantChanged,
  AliasedInput,
};

struct Geometry {
  bool validated = false;
  uint64_t register_base = 0;
  uint32_t register_bytes = 0;
  uint32_t register_page_bytes = 0;
  uint32_t idr0 = 0, idr1 = 0, idr2 = 0;
  unsigned groups = 0, contexts = 0, global_pages = 0;
};

struct StreamRegisters {
  bool captured = false;
  uint32_t smr = 0, s2cr = 0;
};

struct ContextRegisters {
  bool captured = false;
  uint32_t sctlr = 0, cbar = 0, cba2r = 0;
};

// Complete means every entry in the validated visible group/context set was
// captured, including invalid SMRs. It is not a full dump of every CB register.
// A producer that cannot exclude hidden groups must not mark geometry validated.
struct Snapshot {
  Geometry geometry;
  bool control_captured = false;
  uint32_t scr0 = 0;
  unsigned stream_count = 0, context_count = 0;
  StreamRegisters streams[kMaxGroups] = {};
  ContextRegisters contexts[kMaxContexts] = {};
};

struct Request {
  uint64_t owner_id = 0, generation = 0;
  uint64_t controller_base = 0;
  unsigned sid_count = 0;
  uint16_t sids[kMaxStreams] = {};
  uint64_t dma_base = 0, dma_bytes = 0;
};

// This grant must come from a separately established owner/handoff contract.
// An invalid SMR or disabled context never grants itself to the caller.
struct OwnerGrant {
  bool validated_by_owner = false;
  bool exclusive_handoff = false;
  bool queue_quiesced = false;
  uint64_t owner_id = 0, generation = 0;
  uint64_t controller_base = 0, smmu_base = 0;
  unsigned sid_count = 0;
  uint16_t sids[kMaxStreams] = {};
  uint64_t dma_base = 0, dma_bytes = 0;
  bool slots[kMaxGroups] = {};
  bool banks[kMaxContexts] = {};
};

struct Plan {
  Error error = Error::Unplanned;
  unsigned bank = kNoIndex;
  unsigned slots[kMaxStreams] = {kNoIndex, kNoIndex};
  bool reuse[kMaxStreams] = {};
  // Desired common disabled-translation context. No writes are performed here.
  uint32_t sctlr = 0, cbar = 0x10000, cba2r = 1;
  Request request;
  OwnerGrant grant;
  Snapshot before;
};
// Plan owns a full baseline snapshot for exact comparison. A future kernel
// adapter must budget these objects outside its small kernel stack; this host
// core does not claim a stack-qualified runtime integration.

inline unsigned addressBits(unsigned encoding)
{
  switch (encoding) {
    case 0: return 32; case 1: return 36; case 2: return 40;
    case 3: return 42; case 4: return 44; case 5: return 48;
    default: return 0;
  }
}

inline bool validExtent(uint64_t base, uint64_t bytes)
{
  return bytes && bytes - 1 <= UINT64_MAX - base;
}

inline bool contains(uint64_t base, uint64_t bytes, uint64_t inner, uint64_t length)
{
  return validExtent(base, bytes) && validExtent(inner, length) &&
         inner >= base && inner - base < bytes && length <= bytes - (inner - base);
}

inline Error validateSnapshot(const Snapshot &s)
{
  const Geometry &g = s.geometry;
  if (!g.validated) return Error::UnvalidatedGeometry;
  // No truncation/clamp of an unobserved group set. This narrow version supports
  // the architectural 4 KiB register format with at most 128 visible groups.
  if (!g.register_base || (g.register_base & 4095) ||
      g.register_page_bytes != 4096 || (g.register_bytes & 4095) ||
      !validExtent(g.register_base, g.register_bytes) ||
      !(g.idr0 & (1U << 30)) || !(g.idr0 & (1U << 27)) ||
      !(g.idr2 & (1U << 12)) || (g.idr1 >> 31) ||
      !g.groups || g.groups > kMaxGroups || g.groups != (g.idr0 & 255) ||
      !g.contexts || g.contexts > kMaxContexts || g.contexts != (g.idr1 & 255) ||
      ((g.idr1 >> 16) & 255) > g.contexts ||
      g.global_pages != (1U << (((g.idr1 >> 28) & 7) + 1)) ||
      g.global_pages > 128 || g.contexts > g.global_pages ||
      (g.global_pages + g.contexts) * 4096 > g.register_bytes)
    return Error::UnsupportedGeometry;
  // Register fields: Linux v6.12 drivers/iommu/arm/arm-smmu/arm-smmu.h;
  // NUMSIDB/NUMS2CB consumption: arm-smmu.c arm_smmu_device_cfg_probe().
  // Narrow legacy SMR-valid format only. EXIDENABLE moves the valid bit to
  // S2CR[10]; EXIDS-capable configurations need a separately reviewed adapter.
  // NUMSIDB gives the proven legacy SID width, never a presumed 16-bit width.
  if ((g.idr0 & (1U << 8)) || !((g.idr0 >> 9) & 15)) return Error::UnsupportedGeometry;
  for (unsigned shift = 0; shift <= 8; shift += 4)
    if (!addressBits((g.idr2 >> shift) & 15)) return Error::UnsupportedGeometry;
  if (!s.control_captured || s.stream_count != g.groups || s.context_count != g.contexts)
    return Error::IncompleteSnapshot;
  if (s.scr0 & (1U << 3)) return Error::UnsupportedGeometry;
  for (unsigned i = 0; i < g.groups; ++i)
    if (!s.streams[i].captured) return Error::IncompleteSnapshot;
  for (unsigned i = 0; i < g.contexts; ++i)
    if (!s.contexts[i].captured) return Error::IncompleteSnapshot;
  return Error::None;
}

inline bool sameGeometry(const Geometry &a, const Geometry &b)
{
  return a.validated == b.validated && a.register_base == b.register_base &&
         a.register_bytes == b.register_bytes && a.register_page_bytes == b.register_page_bytes &&
         a.idr0 == b.idr0 && a.idr1 == b.idr1 && a.idr2 == b.idr2 &&
         a.groups == b.groups && a.contexts == b.contexts && a.global_pages == b.global_pages;
}

inline bool sameSnapshot(const Snapshot &a, const Snapshot &b)
{
  if (!sameGeometry(a.geometry, b.geometry) || a.control_captured != b.control_captured ||
      a.scr0 != b.scr0 || a.stream_count != b.stream_count || a.context_count != b.context_count ||
      a.stream_count > kMaxGroups || a.context_count > kMaxContexts) return false;
  for (unsigned i = 0; i < a.stream_count; ++i)
    if (a.streams[i].captured != b.streams[i].captured ||
        a.streams[i].smr != b.streams[i].smr || a.streams[i].s2cr != b.streams[i].s2cr) return false;
  for (unsigned i = 0; i < a.context_count; ++i)
    if (a.contexts[i].captured != b.contexts[i].captured || a.contexts[i].sctlr != b.contexts[i].sctlr ||
        a.contexts[i].cbar != b.contexts[i].cbar || a.contexts[i].cba2r != b.contexts[i].cba2r) return false;
  return true;
}

inline bool sameGrant(const OwnerGrant &a, const OwnerGrant &b)
{
  if (a.validated_by_owner != b.validated_by_owner || a.exclusive_handoff != b.exclusive_handoff ||
      a.queue_quiesced != b.queue_quiesced || a.owner_id != b.owner_id || a.generation != b.generation ||
      a.controller_base != b.controller_base || a.smmu_base != b.smmu_base || a.sid_count != b.sid_count ||
      a.dma_base != b.dma_base || a.dma_bytes != b.dma_bytes) return false;
  for (unsigned i = 0; i < kMaxStreams; ++i) if (a.sids[i] != b.sids[i]) return false;
  for (unsigned i = 0; i < kMaxGroups; ++i) if (a.slots[i] != b.slots[i]) return false;
  for (unsigned i = 0; i < kMaxContexts; ++i) if (a.banks[i] != b.banks[i]) return false;
  return true;
}

inline Error inspectBank(const Snapshot &s, unsigned bank, const unsigned *matchedSlots,
                         unsigned count, bool reuse)
{
  if (bank >= s.context_count || bank >= kMaxContexts || count > kMaxStreams || !matchedSlots)
    return Error::InvalidRequest;
  if (bank < ((s.geometry.idr1 >> 16) & 255)) return Error::ContextNotStage1;
  for (unsigned i = 0; i < s.stream_count; ++i) {
    const StreamRegisters &r = s.streams[i];
    if (!(r.smr & 0x80000000U) || ((r.s2cr >> 16) & 3) != 0 || (r.s2cr & 255) != bank) continue;
    bool expected = false;
    for (unsigned j = 0; j < count; ++j) if (matchedSlots[j] == i) expected = true;
    if (!expected) return Error::ForeignBankUser;
  }
  const ContextRegisters &c = s.contexts[bank];
  // Even with a grant, reject enabled/unknown context formats instead of
  // repairing a foreign VMID or translating context. Empty format is accepted
  // only for a new, explicitly granted bank, never for a reused stream.
  const bool bypass = c.cbar == 0x10000 && !(c.cba2r & ~1U);
  const bool empty = !c.cbar && !c.cba2r;
  if (c.sctlr || !(bypass || (!reuse && empty))) return Error::ContextNotBypass;
  return Error::None;
}

inline bool overlapsPlanInput(const Plan &out, const void *input, unsigned bytes)
{
  const uintptr_t a = reinterpret_cast<uintptr_t>(&out);
  const uintptr_t b = reinterpret_cast<uintptr_t>(input);
  return a <= b ? b - a < sizeof(out) : a - b < bytes;
}

// In-place entry for a caller-retained workspace. Reject aliases before touching
// output: e.g. planInto(p, p.request, p.before, p.grant) would otherwise erase its
// own inputs. On AliasedInput the entire output is unchanged; use the returned
// Error, not a previous p.error. Other outcomes reset and fully replace output.
// No full Plan/Snapshot temporary lives on this function's stack.
inline Error planInto(Plan &p, const Request &r, const Snapshot &s, const OwnerGrant &grant)
{
  if (overlapsPlanInput(p, &r, sizeof(r)) || overlapsPlanInput(p, &s, sizeof(s)) ||
      overlapsPlanInput(p, &grant, sizeof(grant))) return Error::AliasedInput;
  p.error = Error::Unplanned; p.bank = kNoIndex;
  for (unsigned i = 0; i < kMaxStreams; ++i) { p.slots[i] = kNoIndex; p.reuse[i] = false; }
  p.sctlr = 0; p.cbar = 0x10000; p.cba2r = 1;
  p.request = {}; p.grant = {};
  p.before.geometry = {}; p.before.control_captured = false; p.before.scr0 = 0;
  p.before.stream_count = p.before.context_count = 0;
  for (auto &stream : p.before.streams) stream = {};
  for (auto &context : p.before.contexts) context = {};
  auto fail = [&p](Error e) { p.error = e; return e; };
  if (!r.owner_id || !r.generation || !r.controller_base || !r.sid_count || r.sid_count > kMaxStreams ||
      !validExtent(r.dma_base, r.dma_bytes) || (r.dma_base & 4095) || (r.dma_bytes & 4095))
    return fail(Error::InvalidRequest);
  for (unsigned i = 0; i < r.sid_count; ++i)
    for (unsigned j = 0; j < i; ++j) if (r.sids[i] == r.sids[j]) return fail(Error::DuplicateSid);
  const Error valid = validateSnapshot(s);
  if (valid != Error::None) return fail(valid);
  const uint32_t sidMask = (1U << ((s.geometry.idr0 >> 9) & 15)) - 1;
  for (unsigned i = 0; i < r.sid_count; ++i)
    if (r.sids[i] & ~sidMask) return fail(Error::SidAddressWidth);
  if (!grant.validated_by_owner || !grant.exclusive_handoff || !grant.queue_quiesced)
    return fail(Error::MissingOwnerGrant);
  if (grant.owner_id != r.owner_id || grant.generation != r.generation ||
      grant.controller_base != r.controller_base || grant.smmu_base != s.geometry.register_base ||
      grant.sid_count != r.sid_count) return fail(Error::OwnerMismatch);
  // The grant binds the complete ordered SID set, not an arbitrary subset.
  for (unsigned i = 0; i < r.sid_count; ++i)
    if (grant.sids[i] != r.sids[i]) return fail(Error::OwnerMismatch);
  if (!contains(grant.dma_base, grant.dma_bytes, r.dma_base, r.dma_bytes)) return fail(Error::DmaOutsideGrant);
  for (unsigned shift = 0; shift <= 8; shift += 4) {
    const uint64_t limit = 1ULL << addressBits((s.geometry.idr2 >> shift) & 15);
    if (r.dma_base >= limit || r.dma_bytes > limit - r.dma_base) return fail(Error::DmaAddressWidth);
  }

  unsigned matched[kMaxStreams] = {kNoIndex, kNoIndex};
  unsigned existingBank = kNoIndex;
  for (unsigned i = 0; i < s.stream_count; ++i) {
    const StreamRegisters &v = s.streams[i];
    if (!(v.smr & 0x80000000U)) continue;
    const unsigned type = (v.s2cr >> 16) & 3;
    if (type == 3 || (!type && (v.s2cr & 255) >= s.context_count)) return fail(Error::InvalidStreamMapping);
    const uint32_t mask = (v.smr >> 16) & 0x7fff;
    if (((v.smr & 0xffffU) | mask) & ~sidMask) return fail(Error::InvalidStreamMapping);
    for (unsigned j = 0; j < r.sid_count; ++j) {
      if (((v.smr ^ r.sids[j]) & 0xffffU & ~mask) != 0) continue;
      if (mask) return fail(Error::MaskedOverlap);
      if (matched[j] != kNoIndex) return fail(Error::DuplicateMatch);
      // Only an exact translating-stream route to a disabled context is a
      // reusable bypass route. Unknown S2CR attributes are not normalized.
      if (type || v.s2cr != (v.s2cr & 255)) return fail(Error::InvalidStreamMapping);
      matched[j] = i;
      const unsigned bank = v.s2cr & 255;
      if (existingBank != kNoIndex && existingBank != bank) return fail(Error::DifferentBanks);
      existingBank = bank;
    }
  }
  for (unsigned i = 0; i < r.sid_count; ++i) {
    if (matched[i] == kNoIndex) continue;
    if (!grant.slots[matched[i]]) return fail(Error::SlotNotGranted);
    p.slots[i] = matched[i]; p.reuse[i] = true;
  }
  if (existingBank != kNoIndex) {
    if (!grant.banks[existingBank]) return fail(Error::BankNotGranted);
    const Error e = inspectBank(s, existingBank, matched, r.sid_count, true);
    if (e != Error::None) return fail(e);
    p.bank = existingBank;
  } else {
    Error refused = Error::NoAvailableBank;
    for (unsigned bank = 0; bank < s.context_count; ++bank) {
      if (!grant.banks[bank]) continue;
      const Error e = inspectBank(s, bank, matched, r.sid_count, false);
      if (e == Error::None) { p.bank = bank; break; }
      if (refused == Error::NoAvailableBank) refused = e;
    }
    if (p.bank == kNoIndex) return fail(refused);
  }
  for (unsigned j = 0; j < r.sid_count; ++j) {
    if (p.slots[j] != kNoIndex) continue;
    for (unsigned i = 0; i < s.stream_count; ++i) {
      if (!grant.slots[i] || (s.streams[i].smr & 0x80000000U)) continue;
      bool taken = false;
      for (unsigned k = 0; k < r.sid_count; ++k) if (p.slots[k] == i) taken = true;
      if (!taken) { p.slots[j] = i; break; }
    }
    if (p.slots[j] == kNoIndex) return fail(Error::NoAvailableSlots);
  }
  p.request = r; p.grant = grant; p.before = s;
  p.error = Error::None;
  return p.error;
}

// Compatibility value-returning wrapper for existing host/planner consumers.
// Runtime adapters with a small kernel stack should call planInto directly.
inline Plan plan(const Request &r, const Snapshot &s, const OwnerGrant &grant)
{
  Plan p;
  (void)planInto(p, r, s, grant);
  return p;
}

// Pure check immediately before a future commit, while the real owner's lock
// remains held. No digest collision or struct-padding comparison: all captured
// fields (including unrelated routes/contexts) must still match. Success is not
// a commit, a shared lock, or proof that another firmware agent is quiescent.
inline Error revalidate(const Plan &p, const Snapshot &current, const OwnerGrant &grant)
{
  if (p.error != Error::None) return Error::InvalidPlan;
  const Plan expected = plan(p.request, p.before, p.grant);
  if (expected.error != Error::None || expected.bank != p.bank ||
      expected.sctlr != p.sctlr || expected.cbar != p.cbar || expected.cba2r != p.cba2r)
    return Error::InvalidPlan;
  for (unsigned i = 0; i < kMaxStreams; ++i)
    if (expected.slots[i] != p.slots[i] || expected.reuse[i] != p.reuse[i]) return Error::InvalidPlan;
  if (!sameGrant(p.grant, grant)) return Error::GrantChanged;
  if (!sameSnapshot(p.before, current)) return Error::SnapshotChanged;
  return Error::None;
}

} // namespace smmu_group
} // namespace shengufs
