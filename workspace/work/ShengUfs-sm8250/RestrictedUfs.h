// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef SHENG_UFS_RESTRICTED_H
#define SHENG_UFS_RESTRICTED_H
#include "ReadCommandPolicy.h"
#include "UfsPlatform.h"
#include "AppsSmmuOwner.h"

namespace shengufs { namespace restricted {
namespace policy = read_policy;
enum class Phase : unsigned {
  Cold, NeedNop, NeedPower, NeedTur, NeedCapacity, ReadReady,
  Running, Halted, Stopped, Quarantined
};
enum class Error : unsigned {
  None, Busy, Unconfigured, InvalidConfiguration, PolicyRejected, WrongPhase,
  InvalidBudget, InvalidPort, Ownership, ProbeFailed, HostNotReady,
  UnexpectedQueue, Fatal, PrepareFailed, AckFailed, RingFailed, PollFailed,
  TimeReversed, Timeout, ResponseFailed, Protocol, PowerNotReady,
  InvalidCapacity, RetryLimit, StopNotProven
};
struct Budget {
  uint64_t timeout_ticks;
  uint32_t max_polls;
  unsigned retries; // At most two, and only for a trusted BUSY/TASK SET FULL response.
};
struct Probe {
  uint64_t ticks, request_list;
  uint32_t hce, hcs, interrupt_enable, interrupt_status;
  uint32_t doorbell, task_doorbell, run_stop;
};
struct Poll {
  uint64_t ticks;
  uint32_t doorbell, task_doorbell, interrupt_status, ocs;
};
struct Completion {
  uint8_t response[512];
  size_t response_bytes;
  uint8_t capacity[8];
  size_t capacity_bytes;
};
// These are explicit adapter obligations, not supplied hardware functions.
// prepare touches ONLY the owned CPU descriptor/data allocation and performs
// the required cache maintenance/barriers. It never submits a command.
// collect performs invalidate-only completion reads; no retry/reset/free.
// ring_slot_zero rings ONLY UTRL bit0; ack acknowledges ONLY prior completion.
// No callback may invoke an old NativeUfs submission, UIC, IRQ, zero-copy,
// persistence or panic path. No callback may block indefinitely.
struct Port {
  void *context;
  bool (*probe)(void *, Probe *);
  bool (*prepare)(void *, const policy::Request &, const policy::Context &);
  bool (*ack_completion)(void *);
  bool (*ring_slot_zero)(void *);
  bool (*poll)(void *, Poll *);
  bool (*collect)(void *, Completion *);
};
struct Result {
  Error error = Error::None;
  policy::Error policy_error = policy::Error::None;
  AppsSmmuResult owner_error = APPS_SMMU_OK;
  unsigned attempts = 0, polls = 0;
  uint32_t last_doorbell = 0, last_interrupt = 0, ocs = 15;
  policy::Capacity capacity = {};
};

class SingleSlot {
  // Neither session state nor a token is copyable into a second submission lane.
  unsigned busy_ = 0;
  unsigned phase_ = static_cast<unsigned>(Phase::Cold);
  policy::Context context_ = {};
  AppsSmmuClaim claim_ = {};
  static constexpr uint32_t fatal_mask_ = 0x71880;
  void phase(Phase value) { __atomic_store_n(&phase_, static_cast<unsigned>(value), __ATOMIC_RELEASE); }
  struct Guard {
    unsigned &busy;
    bool acquired;
    explicit Guard(unsigned &b) : busy(b), acquired(__atomic_exchange_n(&b, 1, __ATOMIC_ACQUIRE) == 0) {}
    ~Guard() { if (acquired) __atomic_store_n(&busy, 0, __ATOMIC_RELEASE); }
  };
  static bool permits(Phase phase, policy::Command command)
  {
    return (phase == Phase::NeedNop && command == policy::Command::Nop) ||
      (phase == Phase::NeedPower && command == policy::Command::PowerMode) ||
      (phase == Phase::NeedTur && command == policy::Command::TestUnitReady) ||
      (phase == Phase::NeedCapacity && command == policy::Command::ReadCapacity10) ||
      (phase == Phase::ReadReady && command == policy::Command::Read10);
  }
  Result quarantine(Result result, Error error, const AppsSmmuToken &token)
  {
    phase(Phase::Quarantined);
    result.error = error;
    // If the metadata gate is busy or the token is stale, nothing is freed:
    // the local terminal state and existing owner claim remain fail-closed.
    result.owner_error = apps_smmu_owner_quarantine(&token, static_cast<uint32_t>(error));
    return result;
  }
public:
  SingleSlot() = default;
  SingleSlot(const SingleSlot &) = delete;
  SingleSlot &operator=(const SingleSlot &) = delete;
  Phase phase() const { return static_cast<Phase>(__atomic_load_n(&phase_, __ATOMIC_ACQUIRE)); }

  Error configure(const policy::Context &context, const AppsSmmuClaim &claim)
  {
    Guard guard(busy_);
    if (!guard.acquired) return Error::Busy;
    if (phase() != Phase::Cold) return Error::WrongPhase;
    policy::Context retained = context;
    retained.capacity = {}; // A caller cannot skip the measured-capacity stage.
    policy::Request nop = {};
    nop.upiu[3] = retained.expected_tag;
    policy::Accepted accepted = {};
    const auto *profile = platformProfile(Platform::Sm8250);
    if (policy::validateRequest(nop, retained, accepted) != policy::Error::None ||
        !claim.client_id || !claim.handoff_epoch || claim.controller_base != profile->host.base ||
        claim.smmu_base != profile->smmu.base || claim.sid_count != 2 || claim.slot_count != 2 ||
        claim.sids[0] != profile->expectedStreams[0] || claim.sids[1] != profile->expectedStreams[1] ||
        claim.slots[0] >= 128 || claim.slots[1] >= 128 || claim.slots[0] == claim.slots[1] ||
        claim.bank >= 128 || claim.dma_base != retained.dma.allocation.address ||
        claim.dma_bytes != retained.dma.allocation.bytes)
      return Error::InvalidConfiguration;
    context_ = retained;
    claim_ = claim;
    phase(Phase::NeedNop);
    return Error::None;
  }

  Result execute(const policy::Request &request, const AppsSmmuToken &input_token,
                 const Port &input_port, const Budget &input_budget)
  {
    Result result;
    Guard guard(busy_);
    if (!guard.acquired) { result.error = Error::Busy; return result; }
    const AppsSmmuToken token = input_token;
    const Port port = input_port;
    const Budget budget = input_budget;
    const Phase previous = phase();
    if (previous == Phase::Cold) { result.error = Error::Unconfigured; return result; }
    // Retain the FINAL packet/PRD, not a boolean that a mutable packet passed.
    const policy::Request retained = request;
    policy::Accepted accepted = {};
    result.policy_error = policy::validateRequest(retained, context_, accepted);
    if (result.policy_error != policy::Error::None) { result.error = Error::PolicyRejected; return result; }
    if (!permits(previous, accepted.command)) { result.error = Error::WrongPhase; return result; }
    if (!budget.timeout_ticks || !budget.max_polls || budget.max_polls > 100000 || budget.retries > 2) {
      result.error = Error::InvalidBudget; return result;
    }
    if (!port.probe || !port.prepare || !port.ack_completion || !port.ring_slot_zero || !port.poll || !port.collect) {
      result.error = Error::InvalidPort; return result;
    }
    result.owner_error = apps_smmu_owner_validate_ready(&token, &claim_);
    if (result.owner_error != APPS_SMMU_OK) { result.error = Error::Ownership; return result; }
    uint64_t started = 0, last = 0;
    for (unsigned attempt = 0; attempt <= budget.retries; ++attempt) {
      Probe probe = {};
      if (!port.probe(port.context, &probe)) return quarantine(result, Error::ProbeFailed, token);
      if (!attempt) started = last = probe.ticks;
      if (probe.ticks < last) return quarantine(result, Error::TimeReversed, token);
      last = probe.ticks;
      result.last_doorbell = probe.doorbell;
      result.last_interrupt = probe.interrupt_status;
      if (probe.interrupt_status & fatal_mask_) return quarantine(result, Error::Fatal, token);
      if (probe.doorbell || probe.task_doorbell) return quarantine(result, Error::UnexpectedQueue, token);
      if (probe.ticks - started >= budget.timeout_ticks || result.polls >= budget.max_polls) {
        phase(Phase::Halted); result.error = Error::Timeout; return result; // No command in flight here.
      }
      if (probe.hce != 1 || (probe.hcs & 3) != 3 || probe.interrupt_enable || probe.run_stop != 1 ||
          probe.request_list != context_.dma.utrd.address) {
        phase(Phase::Halted); result.error = Error::HostNotReady; return result;
      }
      // Owner checks publication/queue state again. Once marked, even an
      // ambiguous prepare/ACK/ring failure permanently retains this allocation.
      result.owner_error = apps_smmu_owner_mark_dma_possible(&token);
      if (result.owner_error != APPS_SMMU_OK) { phase(Phase::Halted); result.error = Error::Ownership; return result; }
      phase(Phase::Running);
      ++result.attempts;
      if (!port.prepare(port.context, retained, context_)) return quarantine(result, Error::PrepareFailed, token);
      if (!port.ack_completion(port.context)) return quarantine(result, Error::AckFailed, token);
      if (!port.ring_slot_zero(port.context)) return quarantine(result, Error::RingFailed, token);
      bool completed = false;
      while (result.polls < budget.max_polls) {
        Poll observation = {};
        ++result.polls;
        if (!port.poll(port.context, &observation)) return quarantine(result, Error::PollFailed, token);
        result.last_doorbell = observation.doorbell;
        result.last_interrupt |= observation.interrupt_status;
        result.ocs = observation.ocs;
        if (observation.ticks < last) return quarantine(result, Error::TimeReversed, token);
        last = observation.ticks;
        if (observation.interrupt_status & fatal_mask_) return quarantine(result, Error::Fatal, token);
        if ((observation.doorbell & ~1U) || observation.task_doorbell)
          return quarantine(result, Error::UnexpectedQueue, token);
        if (observation.ticks - started >= budget.timeout_ticks) return quarantine(result, Error::Timeout, token);
        // DB=0 alone is not completion, even when the counter is frozen.
        if (!observation.doorbell && observation.ocs != 15) { completed = true; break; }
      }
      if (!completed) return quarantine(result, Error::Timeout, token);
      Completion completion = {};
      if (!port.collect(port.context, &completion)) return quarantine(result, Error::ResponseFailed, token);
      policy::ResponseResult response = {};
      result.policy_error = policy::validateResponse(accepted, context_, result.ocs,
                          completion.response, completion.response_bytes, response);
      if (response.status == policy::ResponseStatus::Rejected)
        return quarantine(result, Error::Protocol, token);
      result.owner_error = apps_smmu_owner_complete_dma(&token);
      if (result.owner_error != APPS_SMMU_OK) return quarantine(result, Error::Ownership, token);
      if (response.status == policy::ResponseStatus::PowerNotReady) {
        phase(Phase::Halted); result.error = Error::PowerNotReady; return result;
      }
      if (response.status == policy::ResponseStatus::Retryable) {
        if (attempt == budget.retries) { phase(Phase::Halted); result.error = Error::RetryLimit; return result; }
        continue; // Only a trusted completed BUSY/TASK SET FULL can get here.
      }
      if (accepted.command == policy::Command::ReadCapacity10) {
        result.policy_error = policy::decodeCapacity(completion.capacity, completion.capacity_bytes,
                                                     context_.selected_lun, result.capacity);
        if (result.policy_error != policy::Error::None) {
          phase(Phase::Halted); result.error = Error::InvalidCapacity; return result;
        }
        context_.capacity = result.capacity;
      }
      phase(previous == Phase::NeedNop ? Phase::NeedPower : previous == Phase::NeedPower ? Phase::NeedTur :
            previous == Phase::NeedTur ? Phase::NeedCapacity : Phase::ReadReady);
      return result;
    }
    return quarantine(result, Error::Protocol, token); // Exhaustive fail-closed fallback.
  }

  // The future adapter stops the hardware queue separately. This method reads
  // the actual stopped state; it never treats software Halted as DMA proof and
  // never unmaps/frees resources. Quarantine cannot be recovered through it.
  Result confirmStopped(const AppsSmmuToken &token, const Port &port)
  {
    Result result;
    Guard guard(busy_);
    if (!guard.acquired) { result.error = Error::Busy; return result; }
    if (phase() == Phase::Cold || phase() == Phase::Running || phase() == Phase::Quarantined) {
      result.error = Error::WrongPhase; return result;
    }
    result.owner_error = apps_smmu_owner_validate(&token, &claim_);
    if (result.owner_error != APPS_SMMU_OK) { result.error = Error::Ownership; return result; }
    Probe probe = {};
    if (!port.probe || !port.probe(port.context, &probe)) return quarantine(result, Error::ProbeFailed, token);
    if (probe.doorbell || probe.task_doorbell || (probe.interrupt_status & fatal_mask_))
      return quarantine(result, Error::StopNotProven, token);
    if (probe.hce != 1 || probe.interrupt_enable || probe.run_stop || probe.request_list != context_.dma.utrd.address) {
      result.error = Error::StopNotProven; return result;
    }
    result.owner_error = apps_smmu_owner_confirm_stopped(&token);
    if (result.owner_error != APPS_SMMU_OK) { result.error = Error::Ownership; return result; }
    phase(Phase::Stopped);
    return result;
  }
};
}} // namespace shengufs::restricted
#endif
