// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "J716fStoragePower.h"
#include "UfsPlatform.h"

namespace j716f_power {
namespace {
constexpr unsigned offsets[RegisterCount] = {
  0x04, 0x00, 0x10, 0x18, 0x64, 0x9c, 0x20, 0xb8, 0x1c, 0x5c, 0xcc,
};
const char *const names[RegisterCount] = {
  "GDSCR", "BCR", "AXI", "AHB", "ICE", "AUX", "RX0", "RX1", "TX0", "UNIPRO", "AGGRE_AXI",
};
struct Guard {
  State &state;
  bool held;
  explicit Guard(State &s) : state(s), held(__atomic_exchange_n(&s.busy, 1U, __ATOMIC_ACQUIRE) == 0) {}
  ~Guard() { if (held) __atomic_store_n(&state.busy, 0U, __ATOMIC_RELEASE); }
};
bool complete(const Backend &b) { return b.context && b.map && b.read32 && b.unmap && b.log; }
Result finish(const Backend &b, Report &r, Result result) {
  r.result = result;
  if (b.log) b.log(b.opaque, Event::Summary, RegisterCount, r);
  return result;
}
}
uint64_t address(unsigned index) { return index < RegisterCount ? Page + offsets[index] : 0; }
const char *registerName(unsigned index) { return index < RegisterCount ? names[index] : "invalid"; }
const char *resultName(Result result) {
  switch (result) {
    case Result::Captured: return "captured";
    case Result::Busy: return "busy";
    case Result::AlreadyAttempted: return "already-attempted";
    case Result::InvalidBackend: return "invalid-backend";
    case Result::ContextRejected: return "context-rejected";
    case Result::RegistrationRequired: return "registration-required";
    case Result::RuntimeNotCold: return "runtime-not-cold";
    case Result::MappingFailed: return "mapping-failed";
    case Result::ReadFailed: return "read-failed";
  }
  return "invalid-result";
}
Result run(State &state, const Backend &b, Report &out) {
  Guard guard(state);
  // A reentrant callback may not overwrite the outer caller's report.
  if (!guard.held) return Result::Busy;
  out = {};
  if (!complete(b)) return finish(b, out, Result::InvalidBackend);
  if (state.attempted) return finish(b, out, Result::AlreadyAttempted);
  state.attempted = true;
  b.log(b.opaque, Event::Begin, RegisterCount, out);
  out.context = b.context(b.opaque);
  b.log(b.opaque, Event::Context, RegisterCount, out);
  const Context &c = out.context;
  if (!c.thread_present || c.interrupt_context || !c.interrupts_enabled ||
      c.preemption_level != 0 || !c.kernel_task_present)
    return finish(b, out, Result::ContextRejected);
  if (!c.registered) return finish(b, out, Result::RegistrationRequired);
  if (!c.runtime_cold) return finish(b, out, Result::RuntimeNotCold);
  b.log(b.opaque, Event::MapBefore, RegisterCount, out);
  if (!b.map(b.opaque, Page, PageBytes)) return finish(b, out, Result::MappingFailed);
  out.maps = 1;
  b.log(b.opaque, Event::MapAfter, RegisterCount, out);
  Result result = Result::Captured;
  // Every address is in the GCC controller's page. UFS power-off does not by
  // itself prohibit observing the remaining GCC status registers. No HCI reads.
  for (unsigned i = 0; i < RegisterCount; ++i) {
    ++out.reads_attempted;
    b.log(b.opaque, Event::ReadBefore, i, out);
    uint32_t value = 0;
    if (!b.read32(b.opaque, address(i), &value)) { result = Result::ReadFailed; break; }
    out.values[i] = value;
    out.valid_mask |= 1U << i;
    ++out.reads_completed;
    b.log(b.opaque, Event::ReadAfter, i, out);
  }
  b.log(b.opaque, Event::UnmapBefore, RegisterCount, out);
  b.unmap(b.opaque);
  out.unmaps = 1;
  b.log(b.opaque, Event::UnmapAfter, RegisterCount, out);
  if (result == Result::Captured) {
    out.power_on = (out.values[0] & 0x80000000U) != 0;
    out.reset_asserted = (out.values[1] & 1U) != 0;
    out.axi_readable_hint = shengufs::branchState(out.values[2], true).readable;
    out.ahb_readable_hint = shengufs::branchState(out.values[3], true).readable;
  }
  return finish(b, out, result);
}
}
