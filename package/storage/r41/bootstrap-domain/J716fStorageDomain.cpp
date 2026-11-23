// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "J716fStorageDomain.h"

namespace j716f_storage_domain {
namespace {
Domain shared_domain;
static_assert(__atomic_always_lock_free(sizeof(unsigned), nullptr),
              "Storage domain requires lock-free unsigned atomics");
}
State Domain::state() const noexcept {
  return static_cast<State>(__atomic_load_n(&state_, __ATOMIC_ACQUIRE));
}
bool Domain::acquire(Kind kind) noexcept {
  State desired;
  switch (kind) {
    case Kind::Runtime: desired = State::Runtime; break;
    case Kind::Probe: desired = State::Probe; break;
    case Kind::Bootstrap: desired = State::Bootstrap; break;
    default: return false;
  }
  unsigned expected = static_cast<unsigned>(State::Idle);
  return __atomic_compare_exchange_n(&state_, &expected,
      static_cast<unsigned>(desired), false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
}
void Domain::release(Kind kind) noexcept {
  // Even a bootstrap exit before the first hardware operation is terminal.
  // An interrupted/faulting bootstrap retains Bootstrap, also denying entrants.
  const State next = kind == Kind::Bootstrap ? State::Parked : State::Idle;
  __atomic_store_n(&state_, static_cast<unsigned>(next), __ATOMIC_RELEASE);
}
Domain &shared() noexcept { return shared_domain; }
} // namespace j716f_storage_domain
