// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "Sm8250UfsHandoff.h"
#include "ReadCommandPolicy.h"

// Register facts (not ownership evidence): Linux v6.12
// drivers/iommu/arm/arm-smmu/arm-smmu.h and arm-smmu.c::arm_smmu_write_sme,
// include/ufs/ufshci.h. Only 4 KiB legacy SMR-valid SMMUv2 is supported.
namespace sm8250_ufs {
namespace sg = shengufs::smmu_group;
namespace rp = shengufs::read_policy;
namespace {
constexpr uint64_t kSmmu = 0x15000000;
constexpr uint32_t kFatal = 0x71884; // Includes UIC error, not just fatal HCI bits.
struct Guard {
  unsigned &gate;
  bool held;
  explicit Guard(unsigned &g) : gate(g), held(__atomic_exchange_n(&g, 1, __ATOMIC_ACQUIRE) == 0) {}
  ~Guard() { if (held) __atomic_store_n(&gate, 0, __ATOMIC_RELEASE); }
};
}

bool HandoffTransaction::step()
{
  if (result_.operations >= budget_.max_operations) {
    result_.error = HandoffError::OperationLimit; return false;
  }
  uint64_t now = 0;
  if (!io_.ticks(io_.context, &now)) { result_.error = HandoffError::Clock; return false; }
  if (!clock_started_) { started_ = last_ = now; clock_started_ = true; }
  if (now < last_) { result_.error = HandoffError::TimeReversed; return false; }
  last_ = now;
  if (now - started_ >= budget_.timeout_ticks) { result_.error = HandoffError::Timeout; return false; }
  ++result_.operations;
  return true;
}
bool HandoffTransaction::read(uint64_t address, uint32_t &value)
{
  if (!step()) return false;
  ++result_.reads;
  if (!io_.read32(io_.context, address, &value)) {
    result_.failed_address = address; result_.error = HandoffError::Read; return false;
  }
  return true;
}
bool HandoffTransaction::readThunk(void *context, uint64_t address, uint32_t *value)
{ return value && static_cast<HandoffTransaction *>(context)->read(address, *value); }
bool HandoffTransaction::barrier()
{
  if (!step()) return false;
  if (!io_.barrier(io_.context)) { result_.error = HandoffError::Barrier; return false; }
  return true;
}
bool HandoffTransaction::observe()
{
  // Read permissions remain a separately established grant. These ranges only
  // bound the existing observer's fixed register allowlist.
  const shengufs::Range ranges[] = {{0x100000, 0x80000}, {0x1d84000, 0x3000}, {kSmmu, 0x100000}};
  const shengufs::ReadGrant access{ranges, 3, true};
  observation_ = shengufs::observeUfs(shengufs::Platform::Sm8250, grant_.identity,
                                      access, readThunk, this);
  if (observation_.status != shengufs::ObservationStatus::Captured) {
    if (result_.error == HandoffError::None) result_.error = HandoffError::Observation;
    return false;
  }
  return true;
}
bool HandoffTransaction::quiescent()
{
  using namespace shengufs;
  const auto &h = observation_.host;
  if (h[HostEnable] != 1 || (h[HostStatus] & 3) != 3 || h[InterruptEnable] ||
      (h[InterruptStatus] & kFatal) || h[TransferDoorbell] || h[TaskDoorbell] ||
      h[TransferRunStop] || h[TaskRunStop]) {
    result_.error = HandoffError::HostNotQuiescent; return false;
  }
  // Narrow legacy single-doorbell HCI. No MCQ or implicit 64-bit DMA support.
  const uint32_t version = h[Version];
  if ((version != 0x200 && version != 0x210 && version != 0x300 && version != 0x310) ||
      (h[Cap] & (1U << 30)) ||
      (!(h[Cap] & (1U << 24)) && dma_.physical + dma_.bytes - 1 > UINT32_MAX)) {
    result_.error = HandoffError::HostUnsupported; return false;
  }
  return true;
}
bool HandoffTransaction::stableHost() const
{
  using namespace shengufs;
  const unsigned stable[] = {Cap, Version, TransferBaseLow, TransferBaseHigh, TaskBaseLow, TaskBaseHigh};
  for (unsigned i : stable) if (observation_.host[i] != original_host_.host[i]) return false;
  for (unsigned i = 0; i < 3; ++i) if (observation_.idr[i] != original_host_.idr[i]) return false;
  return true;
}
bool HandoffTransaction::capture(sg::Snapshot &snapshot)
{
  snapshot = {};
  if (!observe() || !quiescent()) return false;
  const auto *profile = shengufs::platformProfile(shengufs::Platform::Sm8250);
  auto &g = snapshot.geometry;
  g.validated = true; // Requires the explicit, retained visibility attestation.
  g.register_base = profile->smmu.base;
  g.register_bytes = uint32_t(profile->smmu.bytes);
  g.register_page_bytes = 4096;
  g.idr0 = observation_.idr[0]; g.idr1 = observation_.idr[1]; g.idr2 = observation_.idr[2];
  g.groups = observation_.streamGroups; g.contexts = observation_.contextBanks;
  g.global_pages = observation_.registerPages;
  if ((g.idr0 & (1U << 8)) || !((g.idr0 >> 9) & 15)) {
    result_.error = HandoffError::Geometry; return false;
  }
  if (!read(kSmmu, snapshot.scr0)) return false;
  if (snapshot.scr0 & 9U) { result_.error = HandoffError::GlobalControl; return false; }
  snapshot.control_captured = true;
  snapshot.stream_count = g.groups; snapshot.context_count = g.contexts;
  // Counts are bounded by observeUfs before any indexed register access.
  for (unsigned i = 0; i < g.groups; ++i) {
    auto &r = snapshot.streams[i];
    if (!read(kSmmu + 0x800 + 4 * i, r.smr) || !read(kSmmu + 0xc00 + 4 * i, r.s2cr)) return false;
    r.captured = true;
  }
  for (unsigned i = 0; i < g.contexts; ++i) {
    auto &r = snapshot.contexts[i];
    if (!read(kSmmu + (g.global_pages + i) * 4096, r.sctlr) ||
        !read(kSmmu + 0x1000 + 4 * i, r.cbar) || !read(kSmmu + 0x1800 + 4 * i, r.cba2r)) return false;
    r.captured = true;
  }
  result_.planner_error = sg::validateSnapshot(snapshot);
  if (result_.planner_error != sg::Error::None) { result_.error = HandoffError::Geometry; return false; }
  return true;
}
bool HandoffTransaction::writeAndConfirm(uint64_t address, uint32_t desired)
{
  if (!step()) return false;
  // A false write callback may still have touched the hardware. Never retry it
  // or roll it back. The partial owner publication was recorded before entry.
  ++result_.writes;
  if (!io_.write32(io_.context, address, desired)) {
    result_.failed_address = address; result_.error = HandoffError::Write; return false;
  }
  if (!barrier()) return false;
  for (uint32_t i = 0; i < budget_.max_readback_polls; ++i) {
    uint32_t actual = 0;
    ++result_.readback_polls;
    if (!read(address, actual)) return false;
    if (actual == desired) return true;
  }
  result_.failed_address = address; result_.error = HandoffError::Readback; return false;
}
bool HandoffTransaction::confirmPublished()
{
  // Construct the exact expected post-state using the retained plan. Compare
  // every captured route/context, not just our two entries, after publication.
  if (!capture(current_)) return false;
  auto &expected = plan_.before;
  expected.contexts[plan_.bank].sctlr = plan_.sctlr;
  expected.contexts[plan_.bank].cbar = plan_.cbar;
  expected.contexts[plan_.bank].cba2r = plan_.cba2r;
  for (unsigned i = 0; i < 2; ++i) {
    expected.streams[plan_.slots[i]].smr = 0x80000000U | result_.claim.sids[i];
    expected.streams[plan_.slots[i]].s2cr = plan_.bank;
  }
  if (!sg::sameSnapshot(expected, current_)) {
    result_.error = HandoffError::PostPublicationChanged; return false;
  }
  if (!observe() || !quiescent()) return false;
  if (!stableHost()) {
    result_.error = HandoffError::PostPublicationChanged; return false;
  }
  return true;
}
HandoffResult HandoffTransaction::fail(HandoffError error)
{
  result_.error = error;
  if (!acquired_) return result_;
  // Only a fully validated, unchanged, stopped preflight can release normally,
  // and only before publication or any attempted write. All uncertainty retains
  // the resource claim. No rollback/unmap/free is attempted here.
  if (safe_to_release_ && !publication_begun_ && !result_.writes) {
    result_.cleanup_error = apps_smmu_owner_release(&result_.token);
    if (result_.cleanup_error == APPS_SMMU_OK) { result_.lease_retained = false; return result_; }
  }
  result_.cleanup_error = apps_smmu_owner_quarantine(&result_.token, unsigned(error) + 1);
  result_.quarantined = result_.cleanup_error == APPS_SMMU_OK;
  result_.lease_retained = !result_.quarantined;
  return result_;
}
HandoffResult HandoffTransaction::run(const HandoffGrant &grant, const Io &io,
                                     const DmaView &dma, const HandoffBudget &budget)
{
  Guard guard(busy_);
  if (!guard.held) { HandoffResult r; r.error = HandoffError::Busy; return r; }
  if (used_) { HandoffResult r; r.error = HandoffError::AlreadyRun; return r; }
  used_ = true;
  // Retain all scalar contracts before the first callback. Identity strings are
  // validated here and canonicalized so callback mutation cannot change them.
  const auto *profile = shengufs::platformProfile(shengufs::Platform::Sm8250);
  if (!shengufs::matches(*profile, grant.identity) || !grant.owner.authorization_id ||
      grant.owner.flags != APPS_SMMU_HANDOFF_REQUIRED ||
      !grant.register_access_authorization || !grant.visibility_authorization ||
      !grant.dma_authorization || grant.facts != RequiredHandoffFacts)
    return fail(HandoffError::InvalidGrant);
  grant_ = grant; io_ = io; dma_ = dma; budget_ = budget;
  grant_.identity = {profile->soc, profile->controller, profile->host, profile->smmu};
  if (!io_.read32 || !io_.write32 || !io_.barrier || !io_.ticks || !io_.cache)
    return fail(HandoffError::InvalidIo);
  if (!budget_.timeout_ticks || !budget_.max_operations || budget_.max_operations > 100000 ||
      !budget_.max_readback_polls || budget_.max_readback_polls > 1024)
    return fail(HandoffError::InvalidBudget);
  const auto &c = grant_.owner.claim;
  const uint64_t base = dma_.physical;
  const rp::OwnedDmaLayout layout{{base, dma_.bytes}, {base, 32}, {base + 4096, 32},
      {base + 4608, 512}, {base + 5120, 16}, {base + 8192, 131072}};
  if (!dma_.cpu || (reinterpret_cast<uintptr_t>(dma_.cpu) & 4095U) ||
      rp::validateLayout(layout) != rp::Error::None || c.dma_base != base || c.dma_bytes != dma_.bytes ||
      reinterpret_cast<uintptr_t>(dma_.cpu) > UINTPTR_MAX - dma_.bytes)
    return fail(HandoffError::InvalidDma);
  const rp::Range mmio[] = {{profile->gcc, 0x80000}, {profile->host.base, profile->host.bytes},
                            {profile->smmu.base, profile->smmu.bytes}};
  for (const auto &range : mmio) if (rp::overlaps(layout.allocation, range))
    return fail(HandoffError::InvalidDma);
  if (!c.client_id || !c.handoff_epoch || c.controller_base != profile->host.base ||
      c.smmu_base != profile->smmu.base || c.sid_count != 2 || c.slot_count != 2 ||
      c.sids[0] != profile->expectedStreams[0] || c.sids[1] != profile->expectedStreams[1] ||
      c.slots[0] >= 128 || c.slots[1] >= 128 || c.slots[0] == c.slots[1] || c.bank >= 128)
    return fail(HandoffError::InvalidGrant);
  result_.claim = c;
  result_.owner_error = apps_smmu_owner_install_handoff(&grant_.owner);
  if (result_.owner_error != APPS_SMMU_OK) return fail(HandoffError::Owner);
  result_.owner_error = apps_smmu_owner_try_acquire(&c, &result_.token);
  if (result_.owner_error != APPS_SMMU_OK) return fail(HandoffError::Owner);
  acquired_ = result_.lease_retained = true;
  if (!barrier() || !capture(current_)) return fail(result_.error);
  original_host_ = observation_;
  auto &g = planner_grant_;
  g.validated_by_owner = g.exclusive_handoff = g.queue_quiesced = true;
  g.owner_id = c.client_id; g.generation = c.handoff_epoch;
  g.controller_base = c.controller_base; g.smmu_base = c.smmu_base;
  g.sid_count = 2; g.sids[0] = c.sids[0]; g.sids[1] = c.sids[1];
  g.dma_base = c.dma_base; g.dma_bytes = c.dma_bytes;
  g.slots[c.slots[0]] = g.slots[c.slots[1]] = true; g.banks[c.bank] = true;
  const sg::Request request{c.client_id, c.handoff_epoch, c.controller_base, 2,
                           {c.sids[0], c.sids[1]}, c.dma_base, c.dma_bytes};
  result_.planner_error = sg::planInto(plan_, request, current_, g);
  if (result_.planner_error != sg::Error::None) return fail(HandoffError::Plan);
  if (plan_.bank != c.bank || plan_.slots[0] != c.slots[0] || plan_.slots[1] != c.slots[1])
    return fail(HandoffError::ClaimMismatch);
  if (!barrier() || !capture(current_)) return fail(result_.error);
  // Same replan/snapshot/grant/field checks as sg::revalidate, with the second
  // Plan in caller-retained workspace instead of nesting two large stack frames.
  result_.planner_error = sg::planInto(replanned_, plan_.request, plan_.before, plan_.grant);
  if (result_.planner_error != sg::Error::None || replanned_.bank != plan_.bank ||
      replanned_.sctlr != plan_.sctlr || replanned_.cbar != plan_.cbar || replanned_.cba2r != plan_.cba2r) {
    result_.planner_error = sg::Error::InvalidPlan; return fail(HandoffError::SnapshotChanged);
  }
  for (unsigned i = 0; i < 2; ++i)
    if (replanned_.slots[i] != plan_.slots[i] || replanned_.reuse[i] != plan_.reuse[i]) {
      result_.planner_error = sg::Error::InvalidPlan; return fail(HandoffError::SnapshotChanged);
    }
  if (!sg::sameGrant(plan_.grant, g)) {
    result_.planner_error = sg::Error::GrantChanged; return fail(HandoffError::SnapshotChanged);
  }
  if (!sg::sameSnapshot(plan_.before, current_) || !stableHost()) {
    result_.planner_error = sg::Error::SnapshotChanged; return fail(HandoffError::SnapshotChanged);
  }
  // Repeat HCI observation after the indexed capture and immediately before
  // publication. It never dereferences the old firmware request-list values.
  if (!observe() || !quiescent() || !barrier()) return fail(result_.error);
  if (!stableHost()) return fail(HandoffError::SnapshotChanged);
  safe_to_release_ = true;
  result_.owner_error = apps_smmu_owner_validate(&result_.token, &c);
  if (result_.owner_error != APPS_SMMU_OK) return fail(HandoffError::Owner);
  result_.owner_error = apps_smmu_owner_publication_begin(&result_.token);
  if (result_.owner_error != APPS_SMMU_OK) return fail(HandoffError::Owner);
  publication_begun_ = true;
  // SCTLR is already exactly zero; never disable an enabled context. Set VA64
  // and stage-1-bypass context format before making either new stream valid.
  const auto &before = plan_.before.contexts[plan_.bank];
  if (before.cba2r != plan_.cba2r && !writeAndConfirm(kSmmu + 0x1800 + 4 * c.bank, plan_.cba2r))
    return fail(result_.error);
  if (before.cbar != plan_.cbar && !writeAndConfirm(kSmmu + 0x1000 + 4 * c.bank, plan_.cbar))
    return fail(result_.error);
  for (unsigned i = 0; i < 2; ++i) if (!plan_.reuse[i]) {
    if (!writeAndConfirm(kSmmu + 0xc00 + 4 * c.slots[i], c.bank) ||
        !writeAndConfirm(kSmmu + 0x800 + 4 * c.slots[i], 0x80000000U | c.sids[i]))
      return fail(result_.error);
  }
  if (!barrier() || !confirmPublished() || !barrier()) return fail(result_.error);
  result_.owner_error = apps_smmu_owner_publication_complete(&result_.token);
  if (result_.owner_error != APPS_SMMU_OK) return fail(HandoffError::Owner);
  result_.published = true;
  return result_;
}
} // namespace sm8250_ufs
