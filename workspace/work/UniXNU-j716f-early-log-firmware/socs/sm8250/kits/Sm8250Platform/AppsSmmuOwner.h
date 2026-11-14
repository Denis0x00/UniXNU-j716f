// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef SM8250_APPS_SMMU_OWNER_H
#define SM8250_APPS_SMMU_OWNER_H
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Fixed, allocation-free software authority for one APPS SMMU instance.
// Exactly one AppsSmmuOwner.cpp must be linked, not one copy per driver.
// Board handoff evidence is an external prerequisite: none of these functions
// establishes firmware/TZ ownership, reads MMIO, or proves DMA quiescence.
enum {
  APPS_SMMU_MAX_CLAIMS = 4,
  APPS_SMMU_MAX_STREAMS = 2,
  APPS_SMMU_HANDOFF_VALID = 1U << 0,
  APPS_SMMU_HANDOFF_EXCLUSIVE = 1U << 1,
  APPS_SMMU_HANDOFF_QUEUE_STOPPED = 1U << 2,
  APPS_SMMU_HANDOFF_DMA_IDLE = 1U << 3,
  APPS_SMMU_HANDOFF_REQUIRED = 15,
};

typedef enum AppsSmmuResult {
  APPS_SMMU_OK = 0,
  APPS_SMMU_INVALID,
  APPS_SMMU_NO_HANDOFF,
  APPS_SMMU_BUSY,
  APPS_SMMU_STALE,
  APPS_SMMU_CONFLICT,
  APPS_SMMU_NOT_QUIESCENT,
  APPS_SMMU_QUARANTINED,
  APPS_SMMU_EXHAUSTED,
  APPS_SMMU_WRONG_STATE,
} AppsSmmuResult;

typedef struct AppsSmmuClaim {
  uint64_t client_id, handoff_epoch;
  uint64_t controller_base, smmu_base;
  uint32_t sid_count, slot_count;
  uint16_t sids[APPS_SMMU_MAX_STREAMS], slots[APPS_SMMU_MAX_STREAMS];
  uint32_t bank;
  uint64_t dma_base, dma_bytes;
} AppsSmmuClaim;

typedef struct AppsSmmuHandoff {
  uint64_t authorization_id; // Nonzero reference to board-provided evidence.
  uint32_t flags;           // All HANDOFF_REQUIRED facts must be attested.
  AppsSmmuClaim claim;      // Exact scope, never inferred from an unused CB.
} AppsSmmuHandoff;

typedef struct AppsSmmuToken {
  uint64_t lease_id, client_id, handoff_epoch;
} AppsSmmuToken;

// Every call is bounded and nonblocking. BUSY leaves state unchanged. Tokens
// are software capabilities for trusted kernel callers, not cryptographic or
// thread identities; a token must not be shared concurrently by its holder.
AppsSmmuResult apps_smmu_owner_install_handoff(const AppsSmmuHandoff *handoff);
AppsSmmuResult apps_smmu_owner_try_acquire(const AppsSmmuClaim *claim, AppsSmmuToken *token);
AppsSmmuResult apps_smmu_owner_validate(const AppsSmmuToken *token, const AppsSmmuClaim *claim);
// Also requires complete publication, a started queue and no possible DMA.
AppsSmmuResult apps_smmu_owner_validate_ready(const AppsSmmuToken *token, const AppsSmmuClaim *claim);

AppsSmmuResult apps_smmu_owner_publication_begin(const AppsSmmuToken *token);
AppsSmmuResult apps_smmu_owner_publication_complete(const AppsSmmuToken *token);
AppsSmmuResult apps_smmu_owner_queue_started(const AppsSmmuToken *token);
AppsSmmuResult apps_smmu_owner_mark_dma_possible(const AppsSmmuToken *token);
// Caller invokes this only after its trusted completion/response checks.
// Doorbell=0 by itself, a timeout, or a protocol error is not completion proof.
AppsSmmuResult apps_smmu_owner_complete_dma(const AppsSmmuToken *token);
// Explicit adapter attestations after the corresponding hardware readback.
AppsSmmuResult apps_smmu_owner_confirm_stopped(const AppsSmmuToken *token);
AppsSmmuResult apps_smmu_owner_confirm_unpublished(const AppsSmmuToken *token);

// Releases the CPU transaction lease but permanently retains the resources.
// No reset/recovery API exists; reboot/platform shutdown is the recovery bound.
AppsSmmuResult apps_smmu_owner_quarantine(const AppsSmmuToken *token, uint32_t reason);
// Requires stopped queue, known-idle DMA, and no partial publication. A complete
// mapping retains its resource claim for the same tuple's later reacquisition;
// only an unpublished claim is removed. Old tokens are invalid after release.
AppsSmmuResult apps_smmu_owner_release(const AppsSmmuToken *token);

#ifdef __cplusplus
}
#endif
#endif
