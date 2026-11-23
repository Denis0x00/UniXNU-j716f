// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef J716F_STORAGE_DOMAIN_H
#define J716F_STORAGE_DOMAIN_H

namespace j716f_storage_domain {
enum class Kind : unsigned { Runtime, Probe, Bootstrap };
enum class State : unsigned { Idle, Runtime, Probe, Bootstrap, Parked };

// CPU software exclusion only. This does not exclude firmware/TZ producers,
// acquire SMMU ownership, drain DMA or establish safe MMIO access.
// A Domain must outlive its guards. There is intentionally no reset/unseal API.
class Domain final {
 public:
  constexpr Domain() noexcept = default;
  Domain(const Domain &) = delete;
  Domain &operator=(const Domain &) = delete;
  Domain(Domain &&) = delete;
  Domain &operator=(Domain &&) = delete;
  State state() const noexcept;
  // This is an observation, not proof that the current caller owns a guard.
  bool bootstrapHeld() const noexcept { return state() == State::Bootstrap; }

 private:
  friend class Guard;
  bool acquire(Kind kind) noexcept;
  void release(Kind kind) noexcept;
  alignas(unsigned) unsigned state_ = static_cast<unsigned>(State::Idle);
};

class Guard final {
 public:
  Guard(Domain &domain, Kind kind) noexcept
      : domain_(domain), kind_(kind), held_(domain.acquire(kind)) {}
  ~Guard() { if (held_) domain_.release(kind_); }
  Guard(const Guard &) = delete;
  Guard &operator=(const Guard &) = delete;
  Guard(Guard &&) = delete;
  Guard &operator=(Guard &&) = delete;
  bool held() const noexcept { return held_; }

 private:
  Domain &domain_;
  const Kind kind_;
  const bool held_;
};

// Exactly one definition is linked into the kernel. Tests may also use isolated
// Domain objects, but production entry points must all use this instance.
Domain &shared() noexcept;
} // namespace j716f_storage_domain
#endif
