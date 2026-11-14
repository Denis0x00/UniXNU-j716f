// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef SM8250_UFS_HANDOFF_H
#define SM8250_UFS_HANDOFF_H
#include "AppsSmmuOwner.h"
#include "Sm8250UfsIo.h"
#include "SmmuGroupPlan.h"
#include "UfsObservation.h"

namespace sm8250_ufs {
enum HandoffFact : uint32_t {
  CompleteVisibleSmmu = 1U << 0,
  NoExternalWriters = 1U << 1,
  NoPendingSmmuTransactions = 1U << 2,
  OwnedIdentityDma = 1U << 3,
  RequiredHandoffFacts = 15,
};
// These are trusted board attestations, not facts discovered by this adapter.
// register_access_authorization covers the exact SM8250 GCC/HCI/SMMU ranges.
// visibility_authorization proves IDR counts cover every relevant stream/CB,
// with no hidden, secure or trapped aliases outside the readable register set.
// NoExternalWriters excludes firmware/TZ and other OS agents throughout use of
// the mapping, including quarantine; our CPU lease cannot exclude those agents.
// NoPendingSmmuTransactions proves old UFS transactions are drained, not merely
// DB=0. DMA is one retained, owned, contiguous identity-mapped allocation, with
// no overlapping foreign CPU/DMA aliases. All references must be nonzero.
struct HandoffGrant {
  AppsSmmuHandoff owner = {};
  shengufs::PlatformIdentity identity = {};
  uint64_t register_access_authorization = 0;
  uint64_t visibility_authorization = 0;
  uint64_t dma_authorization = 0;
  uint32_t facts = 0;
};
struct HandoffBudget {
  uint64_t timeout_ticks;
  uint32_t max_operations; // Includes every read/write/barrier; <= 100000.
  uint32_t max_readback_polls; // Per changed register; <= 1024.
};
enum class HandoffError : unsigned {
  None, Busy, AlreadyRun, InvalidGrant, InvalidIo, InvalidDma, InvalidBudget,
  Owner, Clock, TimeReversed, Timeout, OperationLimit, Read, Barrier, Observation,
  HostNotQuiescent, HostUnsupported, Geometry, GlobalControl, Plan,
  ClaimMismatch, SnapshotChanged, Write, Readback, PostPublicationChanged,
};
struct HandoffResult {
  HandoffError error = HandoffError::None;
  AppsSmmuResult owner_error = APPS_SMMU_OK;
  AppsSmmuResult cleanup_error = APPS_SMMU_OK;
  shengufs::smmu_group::Error planner_error = shengufs::smmu_group::Error::None;
  uint64_t failed_address = 0;
  uint32_t operations = 0, reads = 0, writes = 0, readback_polls = 0;
  bool published = false, quarantined = false, lease_retained = false;
  AppsSmmuToken token = {};
  AppsSmmuClaim claim = {};
};

// One-shot, caller-retained transaction workspace. Keep it on typed heap/static
// storage, not the small kernel stack. Success publishes ONLY the SMMU routes:
// the queue remains stopped, the lease stays held, and HciPort::bind/setupQueue
// is a separate caller action. There is no implicit destructor unmap/free.
// No cold init, PHY/UIC, global SMMU/TLB writes, firmware-pointer dereference,
// active-context disable, queue stop/start or request-list replacement occurs.
class HandoffTransaction {
public:
  HandoffTransaction() = default;
  HandoffTransaction(const HandoffTransaction &) = delete;
  HandoffTransaction &operator=(const HandoffTransaction &) = delete;
  HandoffResult run(const HandoffGrant &, const Io &, const DmaView &, const HandoffBudget &);
private:
  unsigned busy_ = 0;
  bool used_ = false, acquired_ = false, publication_begun_ = false;
  bool safe_to_release_ = false, clock_started_ = false;
  uint64_t started_ = 0, last_ = 0;
  Io io_ = {};
  DmaView dma_ = {};
  HandoffGrant grant_ = {};
  HandoffBudget budget_ = {};
  HandoffResult result_ = {};
  shengufs::UfsObservation observation_ = {};
  shengufs::UfsObservation original_host_ = {};
  shengufs::smmu_group::Snapshot current_ = {};
  shengufs::smmu_group::Plan plan_ = {};
  shengufs::smmu_group::Plan replanned_ = {};
  shengufs::smmu_group::OwnerGrant planner_grant_ = {};
  bool step();
  bool read(uint64_t, uint32_t &);
  bool barrier();
  bool observe();
  bool quiescent();
  bool stableHost() const;
  bool capture(shengufs::smmu_group::Snapshot &);
  bool writeAndConfirm(uint64_t, uint32_t);
  bool confirmPublished();
  HandoffResult fail(HandoffError);
  static bool readThunk(void *, uint64_t, uint32_t *);
};
} // namespace sm8250_ufs
#endif
