// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "../AppsSmmuOwner.h"
#include <cassert>
#include <atomic>
#include <cstdio>
#include <thread>
#ifdef NDEBUG
#error This test requires assertions enabled.
#endif

extern "C" AppsSmmuResult owner_test_peer_acquire(const AppsSmmuClaim *, AppsSmmuToken *);
extern "C" AppsSmmuResult owner_test_peer_validate(const AppsSmmuToken *, const AppsSmmuClaim *);

static AppsSmmuClaim makeClaim(uint64_t client, uint64_t epoch = 1)
{
  // Synthetic board handoff scopes, not measured device grants.
  AppsSmmuClaim c{};
  c.client_id = client; c.handoff_epoch = epoch;
  c.controller_base = 0x01d84000 + client * 0x10000; c.smmu_base = 0x15000000;
  c.sid_count = c.slot_count = 2;
  c.sids[0] = static_cast<uint16_t>(0x100 + client * 0x10); c.sids[1] = c.sids[0] + 1;
  c.slots[0] = static_cast<uint16_t>(2 * client); c.slots[1] = c.slots[0] + 1;
  c.bank = static_cast<uint32_t>(client * 2 + 4);
  c.dma_base = 0x200000000ULL + client * 0x100000; c.dma_bytes = 0x22000;
  return c;
}

static AppsSmmuResult authorize(const AppsSmmuClaim &c)
{
  AppsSmmuHandoff h{100 + c.handoff_epoch, APPS_SMMU_HANDOFF_REQUIRED, c};
  return apps_smmu_owner_install_handoff(&h);
}

template <typename F> static AppsSmmuResult retry(F operation)
{
  // Tests retry BUSY, not the nonblocking implementation. This is bounded and
  // used only for test thread contention, never to retry uncertain DMA.
  for (unsigned i = 0; i < 1000000; ++i) {
    auto result = operation();
    if (result != APPS_SMMU_BUSY) return result;
    std::this_thread::yield();
  }
  assert(false && "bounded host contention retry exhausted");
  return APPS_SMMU_BUSY;
}

static void invalidInputs(const AppsSmmuClaim &a)
{
  AppsSmmuToken t{99, 99, 99};
  assert(apps_smmu_owner_try_acquire(&a, &t) == APPS_SMMU_NO_HANDOFF);
  assert(!t.lease_id && !t.client_id && !t.handoff_epoch);
  assert(apps_smmu_owner_install_handoff(nullptr) == APPS_SMMU_INVALID);
  assert(apps_smmu_owner_try_acquire(nullptr, &t) == APPS_SMMU_INVALID);
  assert(apps_smmu_owner_try_acquire(&a, nullptr) == APPS_SMMU_INVALID);
  assert(apps_smmu_owner_validate(nullptr, &a) == APPS_SMMU_INVALID);
  assert(apps_smmu_owner_validate(&t, nullptr) == APPS_SMMU_INVALID);
  assert(apps_smmu_owner_validate(&t, &a) == APPS_SMMU_STALE);
  for (unsigned i = 0; i < 14; ++i) {
    AppsSmmuHandoff h{101, APPS_SMMU_HANDOFF_REQUIRED, a};
    if (i == 0) h.authorization_id = 0;
    if (i == 1) h.flags &= ~APPS_SMMU_HANDOFF_VALID;
    if (i == 2) h.flags &= ~APPS_SMMU_HANDOFF_EXCLUSIVE;
    if (i == 3) h.flags &= ~APPS_SMMU_HANDOFF_QUEUE_STOPPED;
    if (i == 4) h.flags &= ~APPS_SMMU_HANDOFF_DMA_IDLE;
    if (i == 5) h.flags |= 16;
    if (i == 6) h.claim.sids[1] = h.claim.sids[0];
    if (i == 7) h.claim.slots[1] = h.claim.slots[0];
    if (i == 8) h.claim.bank = 128;
    if (i == 9) h.claim.sid_count = 3;
    if (i == 10) h.claim.dma_bytes = 0;
    if (i == 11) h.claim.dma_base = UINT64_MAX - 4095;
    if (i == 12) h.claim.smmu_base = 0xdead0000;
    if (i == 13) h.claim.slot_count = 1;
    assert(apps_smmu_owner_install_handoff(&h) == APPS_SMMU_INVALID);
  }
}

static void acquireAndLifecycle(const AppsSmmuClaim &a)
{
  assert(authorize(a) == APPS_SMMU_OK);
  assert(authorize(a) == APPS_SMMU_STALE);
  AppsSmmuToken t{}, other{};
  assert(owner_test_peer_acquire(&a, &t) == APPS_SMMU_OK);
  assert(apps_smmu_owner_try_acquire(&a, &other) == APPS_SMMU_BUSY);
  auto b = makeClaim(2);
  assert(authorize(b) == APPS_SMMU_BUSY);
  assert(owner_test_peer_validate(&t, &a) == APPS_SMMU_OK);
  auto changed = a; ++changed.bank;
  assert(apps_smmu_owner_validate(&t, &changed) == APPS_SMMU_CONFLICT);
  auto fake = t; ++fake.lease_id;
  assert(apps_smmu_owner_release(&fake) == APPS_SMMU_STALE);
  assert(apps_smmu_owner_validate_ready(&t, &a) == APPS_SMMU_WRONG_STATE);
  assert(apps_smmu_owner_mark_dma_possible(&t) == APPS_SMMU_WRONG_STATE);
  assert(apps_smmu_owner_queue_started(&t) == APPS_SMMU_WRONG_STATE);
  assert(apps_smmu_owner_release(&t) == APPS_SMMU_OK);
  assert(apps_smmu_owner_validate(&t, &a) == APPS_SMMU_STALE);
  const auto old = t;
  assert(apps_smmu_owner_try_acquire(&a, &t) == APPS_SMMU_OK && t.lease_id > old.lease_id);
  assert(apps_smmu_owner_release(&old) == APPS_SMMU_STALE);
  assert(apps_smmu_owner_validate(&t, &a) == APPS_SMMU_OK);
  assert(apps_smmu_owner_publication_begin(&t) == APPS_SMMU_OK);
  assert(apps_smmu_owner_publication_begin(&t) == APPS_SMMU_WRONG_STATE);
  assert(apps_smmu_owner_release(&t) == APPS_SMMU_WRONG_STATE);
  assert(apps_smmu_owner_confirm_stopped(&t) == APPS_SMMU_WRONG_STATE);
  assert(apps_smmu_owner_publication_complete(&t) == APPS_SMMU_OK);
  assert(apps_smmu_owner_publication_complete(&t) == APPS_SMMU_WRONG_STATE);
  assert(apps_smmu_owner_queue_started(&t) == APPS_SMMU_OK);
  assert(apps_smmu_owner_validate_ready(&t, &a) == APPS_SMMU_OK);
  assert(apps_smmu_owner_complete_dma(&t) == APPS_SMMU_WRONG_STATE);
  assert(apps_smmu_owner_mark_dma_possible(&t) == APPS_SMMU_OK);
  assert(apps_smmu_owner_mark_dma_possible(&t) == APPS_SMMU_NOT_QUIESCENT);
  assert(apps_smmu_owner_validate_ready(&t, &a) == APPS_SMMU_NOT_QUIESCENT);
  assert(apps_smmu_owner_release(&t) == APPS_SMMU_NOT_QUIESCENT);
  assert(apps_smmu_owner_confirm_stopped(&t) == APPS_SMMU_NOT_QUIESCENT);
  assert(apps_smmu_owner_complete_dma(&t) == APPS_SMMU_OK);
  assert(apps_smmu_owner_release(&t) == APPS_SMMU_NOT_QUIESCENT); // Queue still started.
  assert(apps_smmu_owner_confirm_stopped(&t) == APPS_SMMU_OK);
  assert(apps_smmu_owner_validate_ready(&t, &a) == APPS_SMMU_WRONG_STATE);
  assert(apps_smmu_owner_release(&t) == APPS_SMMU_OK); // Mapping claim remains reserved.
  assert(apps_smmu_owner_validate(&t, &a) == APPS_SMMU_STALE);
  changed = a; ++changed.handoff_epoch;
  assert(authorize(changed) == APPS_SMMU_CONFLICT); // Cannot revoke a retained claim.
}

static AppsSmmuClaim retainedConflicts(const AppsSmmuClaim &a)
{
  uint64_t epoch = 1;
  for (unsigned kind = 0; kind < 5; ++kind, ++epoch) {
    auto b = makeClaim(2, epoch);
    if (kind == 0) b.sids[0] = a.sids[0];
    if (kind == 1) b.slots[0] = a.slots[0];
    if (kind == 2) b.bank = a.bank;
    if (kind == 3) b.controller_base = a.controller_base;
    if (kind == 4) b.dma_base = a.dma_base + a.dma_bytes - 4096;
    assert(authorize(b) == APPS_SMMU_OK);
    AppsSmmuToken t{};
    assert(apps_smmu_owner_try_acquire(&b, &t) == APPS_SMMU_CONFLICT && !t.lease_id);
  }
  auto b = makeClaim(2, epoch++);
  b.dma_base = a.dma_base + a.dma_bytes; // Adjacent is not overlap.
  assert(authorize(b) == APPS_SMMU_OK);
  AppsSmmuToken t{};
  assert(apps_smmu_owner_try_acquire(&b, &t) == APPS_SMMU_OK);
  assert(apps_smmu_owner_release(&t) == APPS_SMMU_OK);
  b = makeClaim(2, epoch);
  assert(authorize(b) == APPS_SMMU_OK);
  return b;
}

static void threads(const AppsSmmuClaim &a, const AppsSmmuClaim &b)
{
  std::atomic<unsigned> simultaneous{0}, completed{0};
  uint64_t leases[1000] = {};
  std::thread workers[4];
  for (unsigned k = 0; k < 4; ++k) {
    workers[k] = std::thread([&, k] {
      const auto &claim = (k & 1) ? a : b;
      for (unsigned iteration = 0; iteration < 250; ++iteration) {
        AppsSmmuToken t{};
        assert(retry([&] { return owner_test_peer_acquire(&claim, &t); }) == APPS_SMMU_OK);
        assert(simultaneous.fetch_add(1) == 0);
        assert(retry([&] { return apps_smmu_owner_validate(&t, &claim); }) == APPS_SMMU_OK);
        assert(retry([&] { return owner_test_peer_validate(&t, &claim); }) == APPS_SMMU_OK);
        const auto index = completed.fetch_add(1);
        assert(index < 1000); leases[index] = t.lease_id;
        assert(simultaneous.fetch_sub(1) == 1);
        assert(retry([&] { return apps_smmu_owner_release(&t); }) == APPS_SMMU_OK);
      }
    });
  }
  for (auto &worker : workers) worker.join();
  assert(completed == 1000 && simultaneous == 0);
  for (unsigned i = 0; i < 1000; ++i) {
    assert(leases[i]);
    for (unsigned j = 0; j < i; ++j) assert(leases[i] != leases[j]);
  }
}

static void quarantineAndUnrelatedProgress(const AppsSmmuClaim &a, const AppsSmmuClaim &b)
{
  AppsSmmuToken t{};
  assert(apps_smmu_owner_try_acquire(&a, &t) == APPS_SMMU_OK);
  assert(apps_smmu_owner_confirm_unpublished(&t) == APPS_SMMU_OK);
  assert(apps_smmu_owner_publication_begin(&t) == APPS_SMMU_OK);
  assert(apps_smmu_owner_release(&t) == APPS_SMMU_WRONG_STATE);
  const auto partial = t;
  assert(apps_smmu_owner_quarantine(&t, 1) == APPS_SMMU_OK);
  assert(apps_smmu_owner_validate(&partial, &a) == APPS_SMMU_QUARANTINED);
  assert(apps_smmu_owner_release(&partial) == APPS_SMMU_QUARANTINED);
  assert(apps_smmu_owner_try_acquire(&a, &t) == APPS_SMMU_QUARANTINED);
  auto changed = a; ++changed.handoff_epoch;
  assert(authorize(changed) == APPS_SMMU_CONFLICT);

  // Quarantine opens the CPU lease, not the quarantined resource claim.
  assert(owner_test_peer_acquire(&b, &t) == APPS_SMMU_OK);
  assert(apps_smmu_owner_publication_begin(&t) == APPS_SMMU_OK);
  assert(apps_smmu_owner_publication_complete(&t) == APPS_SMMU_OK);
  assert(apps_smmu_owner_queue_started(&t) == APPS_SMMU_OK);
  assert(apps_smmu_owner_mark_dma_possible(&t) == APPS_SMMU_OK);
  assert(apps_smmu_owner_confirm_stopped(&t) == APPS_SMMU_NOT_QUIESCENT);
  assert(apps_smmu_owner_confirm_unpublished(&t) == APPS_SMMU_NOT_QUIESCENT);
  assert(apps_smmu_owner_release(&t) == APPS_SMMU_NOT_QUIESCENT);
  assert(apps_smmu_owner_quarantine(&t, 0) == APPS_SMMU_INVALID);
  assert(apps_smmu_owner_quarantine(&t, 2) == APPS_SMMU_OK);
  assert(apps_smmu_owner_complete_dma(&t) == APPS_SMMU_QUARANTINED);
  assert(apps_smmu_owner_try_acquire(&b, &t) == APPS_SMMU_QUARANTINED);

  auto c = makeClaim(3); c.bank = b.bank;
  assert(authorize(c) == APPS_SMMU_OK);
  assert(apps_smmu_owner_try_acquire(&c, &t) == APPS_SMMU_CONFLICT);
  c = makeClaim(3, 2);
  assert(authorize(c) == APPS_SMMU_OK);
  assert(apps_smmu_owner_try_acquire(&c, &t) == APPS_SMMU_OK);
  assert(apps_smmu_owner_release(&t) == APPS_SMMU_OK);
  auto d = makeClaim(4);
  assert(authorize(d) == APPS_SMMU_OK);
  assert(apps_smmu_owner_try_acquire(&d, &t) == APPS_SMMU_OK);
  assert(apps_smmu_owner_publication_begin(&t) == APPS_SMMU_OK);
  assert(apps_smmu_owner_publication_complete(&t) == APPS_SMMU_OK);
  assert(apps_smmu_owner_confirm_unpublished(&t) == APPS_SMMU_OK);
  assert(apps_smmu_owner_release(&t) == APPS_SMMU_OK);
  assert(authorize(makeClaim(5)) == APPS_SMMU_EXHAUSTED);
}

int main()
{
  const auto a = makeClaim(1);
  invalidInputs(a);
  acquireAndLifecycle(a);
  const auto b = retainedConflicts(a);
  threads(a, b);
  quarantineAndUnrelatedProgress(a, b);
  std::puts("PASS: single-TU owner across two test TUs, 1000 threaded leases, explicit handoff, stale tokens, resource conflicts, DMA/publication quarantine and unrelated progress");
}
