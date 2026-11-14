// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <cassert>
#include <cstdio>
#include <cstring>
#include "../SmmuGroupPlan.h"
#ifdef NDEBUG
#error This host test requires assertions enabled.
#endif

using namespace shengufs::smmu_group;

struct Fixture {
  Request request;
  Snapshot snapshot;
  OwnerGrant grant;
  Fixture()
  {
    request.owner_id = 0x8250; request.generation = 7;
    request.controller_base = 0x01d84000;
    request.sid_count = 2; request.sids[0] = 0xe0; request.sids[1] = 0x4e0;
    request.dma_base = 0x180000000ULL; request.dma_bytes = 0x22000;
    auto &g = snapshot.geometry;
    g.validated = true; g.register_base = 0x15000000; g.register_bytes = 0x8000;
    g.register_page_bytes = 4096;
    g.idr0 = 0x48001e08; g.idr1 = 0x10000004; g.idr2 = 0x1111; // S1, SMS, 15 SID bits.
    g.groups = 8; g.contexts = 4; g.global_pages = 4;
    snapshot.control_captured = true;
    snapshot.stream_count = 8; snapshot.context_count = 4;
    for (unsigned i = 0; i < snapshot.stream_count; ++i) snapshot.streams[i].captured = true;
    for (unsigned i = 0; i < snapshot.context_count; ++i) snapshot.contexts[i].captured = true;
    grant.validated_by_owner = grant.exclusive_handoff = grant.queue_quiesced = true;
    grant.owner_id = request.owner_id; grant.generation = request.generation;
    grant.controller_base = request.controller_base; grant.smmu_base = g.register_base;
    grant.sid_count = 2; grant.sids[0] = 0xe0; grant.sids[1] = 0x4e0;
    grant.dma_base = request.dma_base; grant.dma_bytes = 0x100000;
    grant.slots[6] = grant.slots[7] = true; grant.banks[3] = true;
  }
  Plan run() const
  {
    const Plan returned = plan(request, snapshot, grant);
    Plan inplace;
    const Error error = planInto(inplace, request, snapshot, grant);
    assert(error == returned.error && inplace.error == returned.error);
    assert(inplace.bank == returned.bank && inplace.sctlr == returned.sctlr &&
           inplace.cbar == returned.cbar && inplace.cba2r == returned.cba2r);
    for (unsigned i = 0; i < kMaxStreams; ++i)
      assert(inplace.slots[i] == returned.slots[i] && inplace.reuse[i] == returned.reuse[i]);
    assert(sameGrant(inplace.grant, returned.grant) && sameSnapshot(inplace.before, returned.before));
    assert(inplace.request.owner_id == returned.request.owner_id &&
           inplace.request.generation == returned.request.generation &&
           inplace.request.controller_base == returned.request.controller_base &&
           inplace.request.sid_count == returned.request.sid_count &&
           inplace.request.dma_base == returned.request.dma_base && inplace.request.dma_bytes == returned.request.dma_bytes);
    for (unsigned i = 0; i < kMaxStreams; ++i) assert(inplace.request.sids[i] == returned.request.sids[i]);
    return returned;
  }
  void route(unsigned slot, uint16_t sid, unsigned bank)
  {
    snapshot.streams[slot].smr = 0x80000000U | sid;
    snapshot.streams[slot].s2cr = bank;
  }
  void reuse()
  {
    route(6, 0xe0, 3); route(7, 0x4e0, 3);
    snapshot.contexts[3].cbar = 0x10000; snapshot.contexts[3].cba2r = 1;
  }
};

static void geometryAndCompleteness()
{
  Fixture f; assert(f.run().error == Error::None);
  f.snapshot.geometry.validated = false; assert(f.run().error == Error::UnvalidatedGeometry);
  f = Fixture(); f.snapshot.geometry.idr0 &= ~(1U << 30); assert(f.run().error == Error::UnsupportedGeometry);
  f = Fixture(); f.snapshot.geometry.idr0 &= ~(1U << 27); assert(f.run().error == Error::UnsupportedGeometry);
  f = Fixture(); f.snapshot.geometry.idr0 &= ~(15U << 9); assert(f.run().error == Error::UnsupportedGeometry);
  f = Fixture(); f.snapshot.geometry.idr0 |= 1U << 8; assert(f.run().error == Error::UnsupportedGeometry);
  f = Fixture(); f.snapshot.scr0 |= 1U << 3; assert(f.run().error == Error::UnsupportedGeometry);
  f = Fixture(); f.snapshot.geometry.idr2 &= ~(1U << 12); assert(f.run().error == Error::UnsupportedGeometry);
  f = Fixture(); f.snapshot.geometry.idr1 |= 0x80000000; assert(f.run().error == Error::UnsupportedGeometry);
  f = Fixture(); f.snapshot.geometry.register_page_bytes = 65536; assert(f.run().error == Error::UnsupportedGeometry);
  f = Fixture(); f.snapshot.geometry.register_bytes = 0x5000; assert(f.run().error == Error::UnsupportedGeometry);
  f = Fixture(); f.snapshot.geometry.groups = 0; assert(f.run().error == Error::UnsupportedGeometry);
  f = Fixture(); f.snapshot.geometry.groups = 129; f.snapshot.geometry.idr0 = 0x48001e81;
  assert(f.run().error == Error::UnsupportedGeometry); // Never clamp unknown extra groups.
  f = Fixture(); f.snapshot.geometry.contexts = 129; assert(f.run().error == Error::UnsupportedGeometry);
  f = Fixture(); f.snapshot.geometry.idr1 |= 5U << 16; assert(f.run().error == Error::UnsupportedGeometry);
  f = Fixture(); f.snapshot.geometry.global_pages = 2; assert(f.run().error == Error::UnsupportedGeometry);
  f = Fixture(); f.snapshot.geometry.global_pages = 2; f.snapshot.geometry.idr1 &= ~(7U << 28);
  assert(f.run().error == Error::UnsupportedGeometry); // More CBs than the context register region can hold.
  f = Fixture(); f.snapshot.geometry.register_base |= 1; assert(f.run().error == Error::UnsupportedGeometry);
  f = Fixture(); f.snapshot.control_captured = false; assert(f.run().error == Error::IncompleteSnapshot);
  f = Fixture(); f.snapshot.stream_count--; assert(f.run().error == Error::IncompleteSnapshot);
  f = Fixture(); f.snapshot.context_count--; assert(f.run().error == Error::IncompleteSnapshot);
  for (unsigned i = 0; i < 8; ++i) {
    f = Fixture(); f.snapshot.streams[i].captured = false;
    assert(f.run().error == Error::IncompleteSnapshot); // Invalid SMRs also need a snapshot.
  }
  for (unsigned i = 0; i < 4; ++i) {
    f = Fixture(); f.snapshot.contexts[i].captured = false;
    assert(f.run().error == Error::IncompleteSnapshot);
  }
  // Upper supported bounds must not index past either fixed-size array.
  f = Fixture(); auto &s = f.snapshot;
  s.geometry.groups = s.stream_count = 128; s.geometry.idr0 = 0x48001e80;
  s.geometry.contexts = s.context_count = 128; s.geometry.idr1 = 0x60000080;
  s.geometry.global_pages = 128; s.geometry.register_bytes = 256 * 4096;
  for (unsigned i = 0; i < 128; ++i) {
    s.streams[i].captured = s.contexts[i].captured = true;
    f.grant.slots[i] = f.grant.banks[i] = false;
  }
  f.grant.slots[126] = f.grant.slots[127] = f.grant.banks[127] = true;
  auto p = f.run(); assert(p.error == Error::None && p.bank == 127 && p.slots[0] == 126 && p.slots[1] == 127);
}

static void explicitOwnerOnly()
{
  Fixture f;
  f.grant.validated_by_owner = false; assert(f.run().error == Error::MissingOwnerGrant);
  f = Fixture(); f.grant.exclusive_handoff = false; assert(f.run().error == Error::MissingOwnerGrant);
  f = Fixture(); f.grant.queue_quiesced = false; assert(f.run().error == Error::MissingOwnerGrant);
  f = Fixture(); f.grant.owner_id++; assert(f.run().error == Error::OwnerMismatch);
  f = Fixture(); f.grant.generation++; assert(f.run().error == Error::OwnerMismatch);
  f = Fixture(); f.grant.controller_base++; assert(f.run().error == Error::OwnerMismatch);
  f = Fixture(); f.grant.smmu_base++; assert(f.run().error == Error::OwnerMismatch);
  f = Fixture(); f.grant.sids[1] = 0x60; assert(f.run().error == Error::OwnerMismatch);
  f = Fixture(); f.grant.sid_count = 1; assert(f.run().error == Error::OwnerMismatch);
  f = Fixture(); f.grant.banks[3] = false; assert(f.run().error == Error::NoAvailableBank);
  f = Fixture(); f.grant.slots[7] = false; assert(f.run().error == Error::NoAvailableSlots);
  // Unused banks/slots being visibly empty cannot create permission.
  f = Fixture(); f.reuse(); f.grant.banks[3] = false; assert(f.run().error == Error::BankNotGranted);
  f = Fixture(); f.reuse(); f.grant.slots[6] = false; assert(f.run().error == Error::SlotNotGranted);
  f = Fixture(); f.request.sid_count = 0; assert(f.run().error == Error::InvalidRequest);
  f = Fixture(); f.request.sid_count = 3; assert(f.run().error == Error::InvalidRequest);
  f = Fixture(); f.request.sids[1] = f.request.sids[0]; assert(f.run().error == Error::DuplicateSid);
  f = Fixture(); f.request.owner_id = 0; assert(f.run().error == Error::InvalidRequest);
  f = Fixture(); f.request.generation = 0; assert(f.run().error == Error::InvalidRequest);
}

static void matchingAndConflicts()
{
  Fixture f; const auto before = f.snapshot; const auto grant = f.grant;
  auto p = f.run(); assert(p.error == Error::None && p.bank == 3 && p.slots[0] == 6 && p.slots[1] == 7);
  assert(!p.reuse[0] && !p.reuse[1] && sameSnapshot(before, f.snapshot) && sameGrant(grant, f.grant));
  f.reuse(); p = f.run(); assert(p.error == Error::None && p.reuse[0] && p.reuse[1]);
  f.snapshot.contexts[3].cba2r = 0; p = f.run(); assert(p.error == Error::None && p.cba2r == 1);
  f = Fixture(); f.route(6, 0xe0, 3); f.snapshot.contexts[3].cbar = 0x10000;
  p = f.run(); assert(p.error == Error::None && p.reuse[0] && !p.reuse[1]);
  f = Fixture(); f.reuse(); f.route(5, 0xe0, 3); assert(f.run().error == Error::DuplicateMatch);
  f = Fixture(); f.route(0, 0xe0, 3); f.snapshot.streams[0].smr |= 0x04000000;
  assert(f.run().error == Error::MaskedOverlap); // The mask spans e0 and 4e0.
  f = Fixture(); f.route(0, 0xe1, 3); f.snapshot.streams[0].smr |= 0x00010000;
  assert(f.run().error == Error::MaskedOverlap);
  f = Fixture(); f.reuse(); f.snapshot.streams[7].s2cr = 2; assert(f.run().error == Error::DifferentBanks);
  f = Fixture(); f.reuse(); f.snapshot.streams[6].s2cr = 0x10003; assert(f.run().error == Error::InvalidStreamMapping);
  f = Fixture(); f.reuse(); f.snapshot.streams[6].s2cr = 0x10000003; assert(f.run().error == Error::InvalidStreamMapping);
  f = Fixture(); f.route(0, 0x1401, 3); assert(f.run().error == Error::ForeignBankUser);
  f = Fixture(); f.route(0, 0x40e0, 1); assert(f.run().error == Error::None); // Do not alias proved high SID bits.
  f = Fixture(); f.route(0, 0x40e0, 3); assert(f.run().error == Error::ForeignBankUser);
  f = Fixture(); f.route(0, 0x80e0, 1); assert(f.run().error == Error::InvalidStreamMapping);
  f = Fixture(); f.route(0, 0x1401, 4); assert(f.run().error == Error::InvalidStreamMapping);
  f = Fixture(); f.route(0, 0x1401, 0x30003); assert(f.run().error == Error::InvalidStreamMapping);
  f = Fixture(); f.snapshot.contexts[3].sctlr = 1; assert(f.run().error == Error::ContextNotBypass);
  f = Fixture(); f.snapshot.contexts[3].sctlr = 2; assert(f.run().error == Error::ContextNotBypass);
  f = Fixture(); f.snapshot.contexts[3].cbar = 0x20000; assert(f.run().error == Error::ContextNotBypass);
  f = Fixture(); f.reuse(); f.snapshot.contexts[3].cba2r = 0x10001; assert(f.run().error == Error::ContextNotBypass);
  f = Fixture(); f.reuse(); f.snapshot.contexts[3].cbar = 0; assert(f.run().error == Error::ContextNotBypass);
  f = Fixture(); f.snapshot.geometry.idr1 |= 4U << 16; assert(f.run().error == Error::ContextNotStage1);
  f = Fixture(); f.snapshot.geometry.idr1 |= 3U << 16; assert(f.run().error == Error::None);
  f = Fixture(); f.snapshot.geometry.idr1 |= 2U << 16;
  f.grant.banks[3] = false; f.grant.banks[1] = true;
  assert(f.run().error == Error::ContextNotStage1);
  // Only an explicitly granted alternative is considered, and never when one
  // expected SID is already bound to a different bank.
  f = Fixture(); f.route(0, 0x1401, 2); f.grant.banks[2] = true;
  p = f.run(); assert(p.error == Error::None && p.bank == 3);
  f = Fixture(); f.request.sid_count = f.grant.sid_count = 1;
  f.request.sids[1] = f.grant.sids[1] = 0;
  p = f.run(); assert(p.error == Error::None && p.slots[1] == kNoIndex);
}

static void dmaExtents()
{
  Fixture f;
  f.request.dma_bytes = 0; assert(f.run().error == Error::InvalidRequest);
  f = Fixture(); f.request.dma_base++; assert(f.run().error == Error::InvalidRequest);
  f = Fixture(); f.request.dma_bytes--; assert(f.run().error == Error::InvalidRequest);
  f = Fixture(); f.request.dma_base = UINT64_MAX - 4095; assert(f.run().error == Error::InvalidRequest);
  f = Fixture(); f.grant.dma_bytes = 0; assert(f.run().error == Error::DmaOutsideGrant);
  f = Fixture(); f.grant.dma_base++; assert(f.run().error == Error::DmaOutsideGrant);
  f = Fixture(); f.grant.dma_bytes = 0x21000; assert(f.run().error == Error::DmaOutsideGrant);
  f = Fixture(); f.grant.dma_bytes = UINT64_MAX; assert(f.run().error == Error::DmaOutsideGrant);
  for (unsigned shift = 0; shift <= 8; shift += 4) {
    f = Fixture(); f.snapshot.geometry.idr2 &= ~(15U << shift);
    assert(f.run().error == Error::DmaAddressWidth); // Each independent 32-bit limit.
    f.snapshot.geometry.idr2 |= 6U << shift;
    assert(f.run().error == Error::UnsupportedGeometry);
  }
  f = Fixture(); f.request.dma_base = f.grant.dma_base = (1ULL << 36) - 4096;
  f.request.dma_bytes = f.grant.dma_bytes = 4096; assert(f.run().error == Error::None);
  f.request.dma_bytes = f.grant.dma_bytes = 8192; assert(f.run().error == Error::DmaAddressWidth);
  f = Fixture(); f.request.dma_base = f.grant.dma_base = 0x1000000000ULL;
  assert(f.run().error == Error::DmaAddressWidth);
}

static void legacySidWidths()
{
  Fixture f;
  // e0 fits 8 bits, but the complete expected set also contains 4e0 (11 bits).
  f.snapshot.geometry.idr0 = (f.snapshot.geometry.idr0 & ~(15U << 9)) | (10U << 9);
  assert(f.run().error == Error::SidAddressWidth);
  f.snapshot.geometry.idr0 = (f.snapshot.geometry.idr0 & ~(15U << 9)) | (11U << 9);
  assert(f.run().error == Error::None);
  f.route(0, 0x800, 1); assert(f.run().error == Error::InvalidStreamMapping);
  f = Fixture(); f.snapshot.geometry.idr0 = (f.snapshot.geometry.idr0 & ~(15U << 9)) | (11U << 9);
  f.route(0, 0x100, 1); f.snapshot.streams[0].smr |= 0x08000000;
  assert(f.run().error == Error::InvalidStreamMapping); // Unknown high mask bits cannot be ignored.
  f = Fixture(); f.request.sids[1] = f.grant.sids[1] = 0x8000;
  assert(f.run().error == Error::SidAddressWidth);
}

static void exactRevalidation()
{
  Fixture f; const Plan p = f.run(); assert(revalidate(p, f.snapshot, f.grant) == Error::None);
  assert(revalidate(Plan(), f.snapshot, f.grant) == Error::InvalidPlan);
  Plan bad = p; bad.bank = 2; assert(revalidate(bad, f.snapshot, f.grant) == Error::InvalidPlan);
  bad = p; bad.slots[1] = bad.slots[0]; assert(revalidate(bad, f.snapshot, f.grant) == Error::InvalidPlan);
  bad = p; bad.reuse[0] = true; assert(revalidate(bad, f.snapshot, f.grant) == Error::InvalidPlan);
  bad = p; bad.cba2r = 0; assert(revalidate(bad, f.snapshot, f.grant) == Error::InvalidPlan);
  auto g = f.grant; g.generation++; assert(revalidate(p, f.snapshot, g) == Error::GrantChanged);
  g = f.grant; g.slots[0] = true; assert(revalidate(p, f.snapshot, g) == Error::GrantChanged);
  g = f.grant; g.banks[0] = true; assert(revalidate(p, f.snapshot, g) == Error::GrantChanged);
  g = f.grant; g.queue_quiesced = false; assert(revalidate(p, f.snapshot, g) == Error::GrantChanged);
  for (unsigned i = 0; i < f.snapshot.stream_count; ++i) {
    auto s = f.snapshot; s.streams[i].smr ^= 1;
    assert(revalidate(p, s, f.grant) == Error::SnapshotChanged);
    s = f.snapshot; s.streams[i].s2cr ^= 1;
    assert(revalidate(p, s, f.grant) == Error::SnapshotChanged);
    s = f.snapshot; s.streams[i].captured = false;
    assert(revalidate(p, s, f.grant) == Error::SnapshotChanged);
  }
  for (unsigned i = 0; i < f.snapshot.context_count; ++i) {
    auto s = f.snapshot; s.contexts[i].sctlr ^= 1;
    assert(revalidate(p, s, f.grant) == Error::SnapshotChanged);
    s = f.snapshot; s.contexts[i].cbar ^= 1;
    assert(revalidate(p, s, f.grant) == Error::SnapshotChanged);
    s = f.snapshot; s.contexts[i].cba2r ^= 1;
    assert(revalidate(p, s, f.grant) == Error::SnapshotChanged);
  }
  auto s = f.snapshot; s.geometry.idr0 ^= 1; assert(revalidate(p, s, f.grant) == Error::SnapshotChanged);
  s = f.snapshot; s.geometry.idr1 ^= 1; assert(revalidate(p, s, f.grant) == Error::SnapshotChanged);
  s = f.snapshot; s.geometry.idr2 ^= 1; assert(revalidate(p, s, f.grant) == Error::SnapshotChanged);
  s = f.snapshot; s.scr0 ^= 1; assert(revalidate(p, s, f.grant) == Error::SnapshotChanged);
  s = f.snapshot; s.stream_count--; assert(revalidate(p, s, f.grant) == Error::SnapshotChanged);
  // A late foreign user invalidates the snapshot even if it does not collide
  // with our selected slots, so revalidation cannot silently bless stale plans.
  f.route(0, 0x1401, 3); assert(revalidate(p, f.snapshot, f.grant) == Error::SnapshotChanged);
  f = Fixture(); f.reuse(); const auto reused = f.run();
  assert(revalidate(reused, f.snapshot, f.grant) == Error::None);
}

static void inPlaceWorkspace()
{
  Fixture f;
  Plan output = f.run();
  unsigned char before[sizeof(output)];
  std::memcpy(before, &output, sizeof(output));
  assert(planInto(output, output.request, f.snapshot, f.grant) == Error::AliasedInput);
  assert(std::memcmp(before, &output, sizeof(output)) == 0);
  assert(planInto(output, f.request, output.before, f.grant) == Error::AliasedInput);
  assert(std::memcmp(before, &output, sizeof(output)) == 0);
  assert(planInto(output, f.request, f.snapshot, output.grant) == Error::AliasedInput);
  assert(std::memcmp(before, &output, sizeof(output)) == 0);
  assert(planInto(output, output.request, output.before, output.grant) == Error::AliasedInput);
  assert(std::memcmp(before, &output, sizeof(output)) == 0);
  auto invalid = f.request; invalid.owner_id = 0;
  assert(planInto(output, invalid, f.snapshot, f.grant) == Error::InvalidRequest);
  assert(output.error == Error::InvalidRequest && output.bank == kNoIndex && !output.request.owner_id);
  assert(!output.grant.validated_by_owner && !output.before.control_captured);
  for (unsigned i = 0; i < kMaxStreams; ++i) assert(output.slots[i] == kNoIndex && !output.reuse[i]);
  for (const auto &v : output.before.streams) assert(!v.captured && !v.smr && !v.s2cr);
  for (const auto &v : output.before.contexts) assert(!v.captured && !v.sctlr && !v.cbar && !v.cba2r);
  // Reuse an ordinary output workspace after both a refusal and a success.
  assert(planInto(output, f.request, f.snapshot, f.grant) == Error::None);
  f.reuse();assert(planInto(output, f.request, f.snapshot, f.grant) == Error::None);
  assert(output.reuse[0] && output.reuse[1] && output.bank == 3);
}

int main()
{
  geometryAndCompleteness();
  explicitOwnerOnly();
  matchingAndConflicts();
  dmaExtents();
  legacySidWidths();
  exactRevalidation();
  inPlaceWorkspace();
  std::puts("PASS: grouped SMMU pure planning: explicit grants, complete geometry/snapshots, dual SID conflicts, DMA bounds, exact precommit revalidation, in-place equivalence and unchanged alias rejection; no MMIO/commit");
}
