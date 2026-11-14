// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "AppsSmmuOwner.h"

namespace {
enum Publication { Unpublished, Partial, Published };
struct Authority { bool used; AppsSmmuHandoff value; };
struct Resource {
  bool used, leased, quarantined, queue_stopped, dma_possible;
  Publication publication;
  uint32_t quarantine_reason;
  uint64_t last_lease;
  AppsSmmuClaim claim;
};

// These objects have one link identity in this TU. There is no lazy allocation
// whose failure can accidentally turn acquisition into success.
unsigned metadata_lock = 0;
uint64_t transaction_lock = 0; // Held from acquisition until release/quarantine.
uint64_t next_lease = 1;
Authority authorities[APPS_SMMU_MAX_CLAIMS] = {};
Resource resources[APPS_SMMU_MAX_CLAIMS] = {};

struct MetadataGuard {
  bool held;
  MetadataGuard()
  {
    unsigned empty = 0;
    held = __atomic_compare_exchange_n(&metadata_lock, &empty, 1, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
  }
  ~MetadataGuard() { if (held) __atomic_store_n(&metadata_lock, 0, __ATOMIC_RELEASE); }
};

bool validClaim(const AppsSmmuClaim &c)
{
  if (!c.client_id || !c.handoff_epoch || !c.controller_base || (c.controller_base & 4095) ||
      c.smmu_base != 0x15000000ULL || !c.sid_count || c.sid_count > APPS_SMMU_MAX_STREAMS ||
      c.slot_count != c.sid_count || c.bank >= 128 || !c.dma_bytes ||
      (c.dma_base & 4095) || (c.dma_bytes & 4095) || c.dma_bytes - 1 > UINT64_MAX - c.dma_base)
    return false;
  for (unsigned i = 0; i < c.sid_count; ++i) {
    if (c.slots[i] >= 128) return false;
    for (unsigned j = 0; j < i; ++j)
      if (c.sids[i] == c.sids[j] || c.slots[i] == c.slots[j]) return false;
  }
  for (unsigned i = c.sid_count; i < APPS_SMMU_MAX_STREAMS; ++i)
    if (c.sids[i] || c.slots[i]) return false;
  return true;
}

bool sameClaim(const AppsSmmuClaim &a, const AppsSmmuClaim &b)
{
  if (a.client_id != b.client_id || a.handoff_epoch != b.handoff_epoch ||
      a.controller_base != b.controller_base || a.smmu_base != b.smmu_base ||
      a.sid_count != b.sid_count || a.slot_count != b.slot_count || a.bank != b.bank ||
      a.dma_base != b.dma_base || a.dma_bytes != b.dma_bytes) return false;
  for (unsigned i = 0; i < APPS_SMMU_MAX_STREAMS; ++i)
    if (a.sids[i] != b.sids[i] || a.slots[i] != b.slots[i]) return false;
  return true;
}

bool conflicts(const AppsSmmuClaim &a, const AppsSmmuClaim &b)
{
  if (a.controller_base == b.controller_base || a.bank == b.bank) return true;
  for (unsigned i = 0; i < a.sid_count; ++i)
    for (unsigned j = 0; j < b.sid_count; ++j)
      if (a.sids[i] == b.sids[j] || a.slots[i] == b.slots[j]) return true;
  // Valid inclusive extents avoid overflow at UINT64_MAX.
  return a.dma_base <= b.dma_base + (b.dma_bytes - 1) &&
         b.dma_base <= a.dma_base + (a.dma_bytes - 1);
}

AppsSmmuResult findLease(const AppsSmmuToken *t, Resource **found)
{
  if (!t) return APPS_SMMU_INVALID;
  if (!t->lease_id || !t->client_id || !t->handoff_epoch) return APPS_SMMU_STALE;
  for (auto &r : resources) {
    if (!r.used || r.last_lease != t->lease_id || r.claim.client_id != t->client_id ||
        r.claim.handoff_epoch != t->handoff_epoch) continue;
    if (r.quarantined) return APPS_SMMU_QUARANTINED;
    if (!r.leased || __atomic_load_n(&transaction_lock, __ATOMIC_RELAXED) != t->lease_id)
      return APPS_SMMU_STALE;
    *found = &r;
    return APPS_SMMU_OK;
  }
  return APPS_SMMU_STALE;
}

AppsSmmuResult ready(const Resource &r)
{
  if (r.dma_possible) return APPS_SMMU_NOT_QUIESCENT;
  if (r.publication != Published || r.queue_stopped) return APPS_SMMU_WRONG_STATE;
  return APPS_SMMU_OK;
}

AppsSmmuResult validate(const AppsSmmuToken *token, const AppsSmmuClaim *claim, bool require_ready)
{
  if (!claim || !validClaim(*claim)) return APPS_SMMU_INVALID;
  MetadataGuard guard;
  if (!guard.held) return APPS_SMMU_BUSY;
  Resource *r = nullptr;
  const auto status = findLease(token, &r);
  if (status != APPS_SMMU_OK) return status;
  if (!sameClaim(r->claim, *claim)) return APPS_SMMU_CONFLICT;
  return require_ready ? ready(*r) : APPS_SMMU_OK;
}

enum Action { BeginPublication, FinishPublication, StartQueue, PossibleDma, CompleteDma, Stopped, Unpublish };
AppsSmmuResult transition(const AppsSmmuToken *token, Action action)
{
  MetadataGuard guard;
  if (!guard.held) return APPS_SMMU_BUSY;
  Resource *r = nullptr;
  const auto status = findLease(token, &r);
  if (status != APPS_SMMU_OK) return status;
  switch (action) {
    case BeginPublication:
      if (r->dma_possible || !r->queue_stopped) return APPS_SMMU_NOT_QUIESCENT;
      if (r->publication != Unpublished) return APPS_SMMU_WRONG_STATE;
      r->publication = Partial;
      break;
    case FinishPublication:
      if (r->dma_possible || !r->queue_stopped) return APPS_SMMU_NOT_QUIESCENT;
      if (r->publication != Partial) return APPS_SMMU_WRONG_STATE;
      r->publication = Published;
      break;
    case StartQueue:
      if (r->dma_possible) return APPS_SMMU_NOT_QUIESCENT;
      if (r->publication != Published || !r->queue_stopped) return APPS_SMMU_WRONG_STATE;
      r->queue_stopped = false;
      break;
    case PossibleDma: {
      const auto result = ready(*r);
      if (result != APPS_SMMU_OK) return result;
      r->dma_possible = true;
      break;
    }
    case CompleteDma:
      if (!r->dma_possible || r->queue_stopped || r->publication != Published) return APPS_SMMU_WRONG_STATE;
      r->dma_possible = false;
      break;
    case Stopped:
      if (r->dma_possible) return APPS_SMMU_NOT_QUIESCENT;
      if (r->publication == Partial) return APPS_SMMU_WRONG_STATE;
      r->queue_stopped = true;
      break;
    case Unpublish:
      if (r->dma_possible || !r->queue_stopped) return APPS_SMMU_NOT_QUIESCENT;
      if (r->publication != Published) return APPS_SMMU_WRONG_STATE;
      r->publication = Unpublished;
      break;
  }
  return APPS_SMMU_OK;
}
} // namespace

extern "C" AppsSmmuResult apps_smmu_owner_install_handoff(const AppsSmmuHandoff *h)
{
  if (!h || !h->authorization_id || h->flags != APPS_SMMU_HANDOFF_REQUIRED || !validClaim(h->claim))
    return APPS_SMMU_INVALID;
  MetadataGuard guard;
  if (!guard.held || __atomic_load_n(&transaction_lock, __ATOMIC_RELAXED)) return APPS_SMMU_BUSY;
  Authority *slot = nullptr;
  for (auto &a : authorities) {
    if (a.used && a.value.claim.client_id == h->claim.client_id) {
      if (h->claim.handoff_epoch <= a.value.claim.handoff_epoch) return APPS_SMMU_STALE;
      slot = &a;
      break;
    }
    if (!a.used && !slot) slot = &a;
  }
  // New paperwork cannot revoke an existing mapping or quarantined resource.
  for (const auto &r : resources)
    if (r.used && r.claim.client_id == h->claim.client_id) return APPS_SMMU_CONFLICT;
  if (!slot) return APPS_SMMU_EXHAUSTED;
  slot->value = *h; slot->used = true;
  return APPS_SMMU_OK;
}

extern "C" AppsSmmuResult apps_smmu_owner_try_acquire(const AppsSmmuClaim *claim, AppsSmmuToken *token)
{
  if (!token) return APPS_SMMU_INVALID;
  *token = {};
  if (!claim || !validClaim(*claim)) return APPS_SMMU_INVALID;
  MetadataGuard guard;
  if (!guard.held || __atomic_load_n(&transaction_lock, __ATOMIC_RELAXED)) return APPS_SMMU_BUSY;
  bool authorized = false;
  for (const auto &a : authorities)
    if (a.used && sameClaim(a.value.claim, *claim)) authorized = true;
  if (!authorized) return APPS_SMMU_NO_HANDOFF;
  Resource *selected = nullptr;
  for (auto &r : resources) {
    if (!r.used) { if (!selected) selected = &r; continue; }
    if (sameClaim(r.claim, *claim)) {
      if (r.quarantined) return APPS_SMMU_QUARANTINED;
      selected = &r;
    } else if (conflicts(r.claim, *claim)) return APPS_SMMU_CONFLICT;
  }
  if (!selected || next_lease == UINT64_MAX) return APPS_SMMU_EXHAUSTED;
  const uint64_t lease = next_lease;
  uint64_t empty = 0;
  if (!__atomic_compare_exchange_n(&transaction_lock, &empty, lease, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
    return APPS_SMMU_BUSY;
  ++next_lease;
  if (!selected->used) {
    *selected = {};
    selected->used = true; selected->queue_stopped = true; selected->claim = *claim;
  }
  selected->leased = true; selected->last_lease = lease;
  token->lease_id = lease; token->client_id = claim->client_id; token->handoff_epoch = claim->handoff_epoch;
  return APPS_SMMU_OK;
}

extern "C" AppsSmmuResult apps_smmu_owner_validate(const AppsSmmuToken *token, const AppsSmmuClaim *claim)
{ return validate(token, claim, false); }
extern "C" AppsSmmuResult apps_smmu_owner_validate_ready(const AppsSmmuToken *token, const AppsSmmuClaim *claim)
{ return validate(token, claim, true); }
extern "C" AppsSmmuResult apps_smmu_owner_publication_begin(const AppsSmmuToken *token)
{ return transition(token, BeginPublication); }
extern "C" AppsSmmuResult apps_smmu_owner_publication_complete(const AppsSmmuToken *token)
{ return transition(token, FinishPublication); }
extern "C" AppsSmmuResult apps_smmu_owner_queue_started(const AppsSmmuToken *token)
{ return transition(token, StartQueue); }
extern "C" AppsSmmuResult apps_smmu_owner_mark_dma_possible(const AppsSmmuToken *token)
{ return transition(token, PossibleDma); }
extern "C" AppsSmmuResult apps_smmu_owner_complete_dma(const AppsSmmuToken *token)
{ return transition(token, CompleteDma); }
extern "C" AppsSmmuResult apps_smmu_owner_confirm_stopped(const AppsSmmuToken *token)
{ return transition(token, Stopped); }
extern "C" AppsSmmuResult apps_smmu_owner_confirm_unpublished(const AppsSmmuToken *token)
{ return transition(token, Unpublish); }

extern "C" AppsSmmuResult apps_smmu_owner_quarantine(const AppsSmmuToken *token, uint32_t reason)
{
  if (!reason) return APPS_SMMU_INVALID;
  MetadataGuard guard;
  if (!guard.held) return APPS_SMMU_BUSY;
  Resource *r = nullptr;
  const auto status = findLease(token, &r);
  if (status != APPS_SMMU_OK) return status;
  r->quarantined = true; r->quarantine_reason = reason; r->leased = false;
  // No claim, DMA allocation or mapping is freed. Only the CPU transaction
  // lease opens so unrelated, non-conflicting resources remain usable.
  __atomic_store_n(&transaction_lock, 0, __ATOMIC_RELEASE);
  return APPS_SMMU_OK;
}

extern "C" AppsSmmuResult apps_smmu_owner_release(const AppsSmmuToken *token)
{
  MetadataGuard guard;
  if (!guard.held) return APPS_SMMU_BUSY;
  Resource *r = nullptr;
  const auto status = findLease(token, &r);
  if (status != APPS_SMMU_OK) return status;
  if (r->dma_possible || !r->queue_stopped) return APPS_SMMU_NOT_QUIESCENT;
  if (r->publication == Partial) return APPS_SMMU_WRONG_STATE;
  r->leased = false;
  if (r->publication == Unpublished) *r = {};
  __atomic_store_n(&transaction_lock, 0, __ATOMIC_RELEASE);
  return APPS_SMMU_OK;
}
