// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef SM8250_UFS_RUNTIME_H
#define SM8250_UFS_RUNTIME_H
#include "Sm8250UfsHandoff.h"
#include "Sm8250UfsHci.h"

namespace sm8250_ufs {
enum AccessFact : uint32_t {
  BoardIdentityVerified = 1U << 0, RegisterAccessEstablished = 1U << 1,
  KernelMemoryOwned = 1U << 2, SleepableIoContext = 1U << 3,
  RequiredAccessFacts = 15,
};
// Kernel-platform caller only. These references must name actual board handoff
// evidence, not boot arguments. No user-facing control or automatic grant exists.
struct RuntimeAccess {
  uint32_t version;
  uint32_t board; // 0x4a373136 = TB-J716F, whose stock resource profile is fixed.
  uint64_t authorization;
  uint32_t facts;
};
struct RuntimeAllocation { uint64_t id, physical, bytes; };
struct RuntimeStart {
  uint64_t allocation_id;
  HandoffGrant handoff;
  HandoffBudget handoff_budget;
  shengufs::restricted::Budget command_budget;
  uint8_t lun, tag;
  unsigned known_block_count;
  shengufs::read_policy::KnownBlock known_blocks[8];
};
enum class RuntimePhase : unsigned { Cold, Allocated, Active, Stopped, Failed, Quarantined };
enum class RuntimeError : unsigned {
  None, Busy, NotInitialized, WrongPhase, InvalidAccess, CacheGeometry,
  Allocation, Mapping, InvalidStart, Handoff, Queue, Command, Read, Stop, Owner,
};
struct RuntimeReport {
  RuntimeError error = RuntimeError::None;
  RuntimePhase phase = RuntimePhase::Cold;
  RuntimeAllocation allocation = {};
  HandoffResult handoff = {};
  shengufs::restricted::Result command = {};
  AppsSmmuResult owner_error = APPS_SMMU_OK;
};
}

// init is called after successful IOKitBSDInit in the independent host target.
// It only marks registration; it maps nothing, allocates nothing and issues no
// register access. Later APIs require a sleepable trusted platform caller and
// actual handoff evidence. Outputs must be kernel-owned writable buffers.
extern "C" void sm8250_ufs_adapter_init(void);
extern "C" unsigned sm8250_ufs_allocate(const sm8250_ufs::RuntimeAccess *, sm8250_ufs::RuntimeReport *);
extern "C" unsigned sm8250_ufs_activate(const sm8250_ufs::RuntimeStart *, sm8250_ufs::RuntimeReport *);
extern "C" unsigned sm8250_ufs_read_known(uint64_t lba, uint8_t *output, size_t bytes, sm8250_ufs::RuntimeReport *);
extern "C" unsigned sm8250_ufs_stop(sm8250_ufs::RuntimeReport *);
// Only unused allocations can be discarded. Published/quarantined buffers and
// mappings remain retained even after a confirmed stop; there is no reset API.
extern "C" unsigned sm8250_ufs_discard_unused(sm8250_ufs::RuntimeReport *);
extern "C" unsigned sm8250_ufs_status(sm8250_ufs::RuntimeReport *);
#endif
