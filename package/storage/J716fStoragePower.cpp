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
constexpr unsigned hci_offsets[HciRegisterCount] = {
  0x00, 0x08, 0x34, 0x30, 0x20, 0x24, 0x50, 0x54, 0x58, 0x60, 0x70, 0x74, 0x78, 0x80,
  0x34, 0x30, 0x50, 0x54, 0x58, 0x60, 0x70, 0x74, 0x78, 0x80,
};
const char *const hci_names[HciRegisterCount] = {
  "CAP", "VER", "HCE", "HCS", "IS", "IE", "UTRLBA", "UTRLBAU", "UTRLDBR", "UTRLRSR",
  "UTMRLBA", "UTMRLBAU", "UTMRLDBR", "UTMRLRSR",
  "HCE", "HCS", "UTRLBA", "UTRLBAU", "UTRLDBR", "UTRLRSR",
  "UTMRLBA", "UTMRLBAU", "UTMRLDBR", "UTMRLRSR",
};
constexpr unsigned repeat_primary[HciRepeatCount] = {2, 3, 6, 7, 8, 9, 10, 11, 12, 13};
constexpr unsigned smmu_offsets[SmmuHeaderCount] = {0x20, 0x24, 0x28, 0x00};
const char *const smmu_names[SmmuHeaderCount] = {"ID0", "ID1", "ID2", "sCR0"};
static_assert(HciRegisterCount < 32, "HCI validity mask must fit unsigned");
static_assert(MaximumLoads == 295, "Experiment load bound changed");
static_assert(0xc00 + 4 * (SmmuMaxGroups - 1) + 4 <= SmmuPageBytes, "S2CR exceeds fixed page");
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
Result contextResult(const Context &c) {
  if (!c.thread_present || c.interrupt_context || !c.interrupts_enabled ||
      c.preemption_level != 0 || !c.kernel_task_present)
    return Result::ContextRejected;
  if (!c.registered) return Result::RegistrationRequired;
  if (!c.runtime_cold) return Result::RuntimeNotCold;
  return Result::Captured;
}
void clearReport(Report &r) {
  // Reset the retained workspace in place: no full Report temporary on the
  // small kernel stack after the SMMU arrays were added.
  r.result = Result::InvalidBackend; r.phase = Phase::Gcc; r.context = {};
  for (auto &v : r.values) v = 0;
  for (auto &v : r.hci_values) v = 0;
  for (auto &v : r.smmu_header) v = 0;
  for (auto &g : r.smmu_groups) { g[0] = 0; g[1] = 0; }
  r.valid_mask = r.reads_attempted = r.reads_completed = 0;
  r.hci_valid_mask = r.hci_reads_attempted = r.hci_reads_completed = 0;
  r.repeat_compared_mask = r.repeat_changed_mask = 0;
  r.smmu_header_valid_mask = r.smmu_reads_attempted = r.smmu_reads_completed = 0;
  r.smmu_group_words_completed = r.smmu_group_count = r.smmu_context_count = 0;
  r.smmu_stage2_count = r.smmu_global_pages = r.smmu_sid_bits = 0;
  r.smmu_gate_passed = false; r.maps = r.unmaps = 0;
  r.power_on = r.reset_asserted = r.axi_readable_hint = r.ahb_readable_hint = r.hci_gate_passed = false;
}
bool supportedSmmu(Report &r) {
  // These geometry fields restrict the reviewed legacy register format. They
  // are NOT owner validation and do not authorize access to any context bank.
  const uint32_t id0 = r.smmu_header[0], id1 = r.smmu_header[1], id2 = r.smmu_header[2];
  r.smmu_group_count = id0 & 255;
  r.smmu_sid_bits = (id0 >> 9) & 15;
  r.smmu_context_count = id1 & 255;
  r.smmu_stage2_count = (id1 >> 16) & 255;
  r.smmu_global_pages = 1U << (((id1 >> 28) & 7) + 1);
  bool widths_known = true;
  for (unsigned shift = 0; shift <= 8; shift += 4)
    if (((id2 >> shift) & 15) > 5) widths_known = false;
  return (id0 & (1U << 30)) && (id0 & (1U << 27)) && !(id0 & (1U << 8)) &&
    r.smmu_sid_bits && !(id1 & (1U << 31)) && (id2 & (1U << 12)) &&
    r.smmu_group_count && r.smmu_group_count <= SmmuMaxGroups &&
    r.smmu_context_count && r.smmu_context_count <= 128 &&
    r.smmu_stage2_count <= r.smmu_context_count && r.smmu_global_pages <= 128 &&
    r.smmu_context_count <= r.smmu_global_pages &&
    (r.smmu_global_pages + r.smmu_context_count) * 4096 <= 0x100000 &&
    widths_known && !(r.smmu_header[3] & (1U << 3));
}
}
uint64_t address(unsigned index) { return index < RegisterCount ? Page + offsets[index] : 0; }
const char *registerName(unsigned index) { return index < RegisterCount ? names[index] : "invalid"; }
uint64_t hciAddress(unsigned index) { return index < HciRegisterCount ? HciPage + hci_offsets[index] : 0; }
const char *hciRegisterName(unsigned index) { return index < HciRegisterCount ? hci_names[index] : "invalid"; }
const char *hciSampleName(unsigned index) {
  return index < HciPrimaryCount ? "hci-a" : index < HciRegisterCount ? "hci-b" : "invalid";
}
unsigned hciRepeatPrimaryIndex(unsigned index) {
  return index < HciRepeatCount ? repeat_primary[index] : HciRegisterCount;
}
uint64_t smmuAddress(unsigned index) { return index < SmmuHeaderCount ? SmmuPage + smmu_offsets[index] : 0; }
const char *smmuRegisterName(unsigned index) { return index < SmmuHeaderCount ? smmu_names[index] : "invalid"; }
uint64_t smmuGroupAddress(unsigned group, unsigned word) {
  return group < SmmuMaxGroups && word < 2 ? SmmuPage + (word ? 0xc00 : 0x800) + 4 * group : 0;
}
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
    case Result::PowerNotReady: return "power-not-ready";
    case Result::ResetAsserted: return "reset-asserted";
    case Result::ClockNotReady: return "clock-not-ready";
    case Result::HciMappingFailed: return "hci-mapping-failed";
    case Result::HciReadFailed: return "hci-read-failed";
    case Result::SmmuMappingFailed: return "smmu-mapping-failed";
    case Result::SmmuReadFailed: return "smmu-read-failed";
    case Result::SmmuFormatRejected: return "smmu-format-rejected";
  }
  return "invalid-result";
}
Result run(State &state, const Backend &b, Report &out) {
  Guard guard(state);
  // A reentrant callback may not overwrite the outer caller's report.
  if (!guard.held) return Result::Busy;
  clearReport(out);
  if (!complete(b)) return finish(b, out, Result::InvalidBackend);
  if (state.attempted) return finish(b, out, Result::AlreadyAttempted);
  state.attempted = true;
  b.log(b.opaque, Event::Begin, RegisterCount, out);
  out.context = b.context(b.opaque);
  b.log(b.opaque, Event::Context, RegisterCount, out);
  const Result initial_context = contextResult(out.context);
  if (initial_context != Result::Captured) return finish(b, out, initial_context);
  b.log(b.opaque, Event::MapBefore, RegisterCount, out);
  if (!b.map(b.opaque, Page, PageBytes)) return finish(b, out, Result::MappingFailed);
  out.maps = 1;
  b.log(b.opaque, Event::MapAfter, RegisterCount, out);
  Result result = Result::Captured;
  // Observe all GCC entries before evaluating the current-boot HCI gates.
  // UFS power-off does not itself prohibit these remaining GCC reads.
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
  if (result != Result::Captured) return finish(b, out, result);
  out.power_on = (out.values[0] & 0x80000000U) != 0;
  out.reset_asserted = (out.values[1] & 1U) != 0;
  out.axi_readable_hint = shengufs::branchState(out.values[2], true).readable;
  out.ahb_readable_hint = shengufs::branchState(out.values[3], true).readable;
  out.hci_gate_passed = out.power_on && !out.reset_asserted &&
                        out.axi_readable_hint && out.ahb_readable_hint;
  b.log(b.opaque, Event::Gate, RegisterCount, out);
  if (!out.power_on) return finish(b, out, Result::PowerNotReady);
  if (out.reset_asserted) return finish(b, out, Result::ResetAsserted);
  if (!out.axi_readable_hint || !out.ahb_readable_hint)
    return finish(b, out, Result::ClockNotReady);

  // A new first-access experiment, not the old Host14 observer/grant. No read
  // from a prior boot authorizes this mapping. Recheck live software context;
  // neither this nor the GCC snapshot excludes firmware or TZ activity.
  out.phase = Phase::Hci;
  out.context = b.context(b.opaque);
  b.log(b.opaque, Event::Context, HciRegisterCount, out);
  const Result hci_context = contextResult(out.context);
  if (hci_context != Result::Captured) return finish(b, out, hci_context);
  b.log(b.opaque, Event::MapBefore, HciRegisterCount, out);
  if (!b.map(b.opaque, HciPage, HciPageBytes))
    return finish(b, out, Result::HciMappingFailed);
  ++out.maps;
  b.log(b.opaque, Event::MapAfter, HciRegisterCount, out);
  for (unsigned i = 0; i < HciRegisterCount; ++i) {
    ++out.hci_reads_attempted;
    b.log(b.opaque, Event::ReadBefore, i, out);
    uint32_t value = 0;
    if (!b.read32(b.opaque, hciAddress(i), &value)) { result = Result::HciReadFailed; break; }
    out.hci_values[i] = value;
    out.hci_valid_mask |= 1U << i;
    ++out.hci_reads_completed;
    if (i >= HciPrimaryCount) {
      const unsigned repeat = i - HciPrimaryCount;
      // Primary capture completed before the repeat pass. Defensively require
      // both raw entries; never compare an unavailable word with default zero.
      const unsigned primary = hciRepeatPrimaryIndex(repeat);
      if (out.hci_valid_mask & (1U << primary)) {
        out.repeat_compared_mask |= 1U << repeat;
        if (out.hci_values[primary] != value) out.repeat_changed_mask |= 1U << repeat;
      }
    }
    b.log(b.opaque, Event::ReadAfter, i, out);
  }
  b.log(b.opaque, Event::UnmapBefore, HciRegisterCount, out);
  b.unmap(b.opaque);
  ++out.unmaps;
  b.log(b.opaque, Event::UnmapAfter, HciRegisterCount, out);
  if (result != Result::Captured) return finish(b, out, result);

  // HCI A/B share their original map. Only after it is released do we attempt
  // the fixed SMMU page; no HCI queue address or firmware pointer is followed.
  out.phase = Phase::Smmu;
  out.context = b.context(b.opaque);
  b.log(b.opaque, Event::Context, SmmuHeaderCount, out);
  const Result smmu_context = contextResult(out.context);
  if (smmu_context != Result::Captured) return finish(b, out, smmu_context);
  b.log(b.opaque, Event::MapBefore, SmmuHeaderCount, out);
  if (!b.map(b.opaque, SmmuPage, SmmuPageBytes))
    return finish(b, out, Result::SmmuMappingFailed);
  ++out.maps;
  b.log(b.opaque, Event::MapAfter, SmmuHeaderCount, out);
  for (unsigned i = 0; i < SmmuHeaderCount; ++i) {
    ++out.smmu_reads_attempted;
    b.log(b.opaque, Event::ReadBefore, i, out);
    uint32_t value = 0;
    if (!b.read32(b.opaque, smmuAddress(i), &value)) { result = Result::SmmuReadFailed; break; }
    out.smmu_header[i] = value;
    out.smmu_header_valid_mask |= 1U << i;
    ++out.smmu_reads_completed;
    b.log(b.opaque, Event::ReadAfter, i, out);
  }
  if (result == Result::Captured) {
    out.smmu_gate_passed = supportedSmmu(out);
    b.log(b.opaque, Event::Gate, SmmuHeaderCount, out);
    if (!out.smmu_gate_passed) result = Result::SmmuFormatRejected;
  }
  // Complete visible legacy set or no table read; never clamp >128 to fit.
  // Log once per <=4 pairs to fit retained RAM. An external abort can therefore
  // locate at most an eight-word batch, not the exact unreturned word.
  for (unsigned first = 0; result == Result::Captured && first < out.smmu_group_count;
       first += SmmuBatchGroups) {
    const unsigned remaining = out.smmu_group_count - first;
    const unsigned count = remaining < SmmuBatchGroups ? remaining : SmmuBatchGroups;
    b.log(b.opaque, Event::GroupBefore, first, out);
    for (unsigned relative = 0; relative < count * 2; ++relative) {
      const unsigned group = first + relative / 2, word = relative % 2;
      ++out.smmu_reads_attempted;
      uint32_t value = 0;
      if (!b.read32(b.opaque, smmuGroupAddress(group, word), &value)) {
        result = Result::SmmuReadFailed; break;
      }
      out.smmu_groups[group][word] = value;
      ++out.smmu_reads_completed;
      ++out.smmu_group_words_completed;
    }
    b.log(b.opaque, Event::GroupAfter, first, out);
  }
  b.log(b.opaque, Event::UnmapBefore, SmmuHeaderCount, out);
  b.unmap(b.opaque);
  ++out.unmaps;
  b.log(b.opaque, Event::UnmapAfter, SmmuHeaderCount, out);
  return finish(b, out, result);
}
}
