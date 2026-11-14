// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "J716fStoragePower.h"
#include "UfsPlatform.h"
#include <assert.h>
#include <stdio.h>
#include <vector>

using namespace j716f_power;
struct Seen { Event event; unsigned index; Report report; };
struct Model {
  Context facts{true, false, true, 0, true, true, true};
  unsigned contexts = 0, maps = 0, unmaps = 0, reads = 0;
  bool map_ok = true, mapped = false, reenter = false;
  unsigned fail_read = RegisterCount;
  uint32_t values[RegisterCount] = {0x80000000, 0, 1, 1, 2, 2, 0, 0, 0, 2, 2};
  std::vector<uint64_t> addresses;
  std::vector<Seen> events;
  State *state = nullptr;
  Backend *backend = nullptr;
};
Context context(void *opaque) {
  auto &m = *static_cast<Model *>(opaque); ++m.contexts; return m.facts;
}
bool map(void *opaque, uint64_t page, uint64_t bytes) {
  auto &m = *static_cast<Model *>(opaque);
  assert(page == 0x177000 && bytes == 4096 && !m.mapped);
  ++m.maps;
  m.mapped = m.map_ok;
  return m.map_ok;
}
bool read(void *opaque, uint64_t physical, uint32_t *out) {
  auto &m = *static_cast<Model *>(opaque);
  assert(m.mapped && m.reads < RegisterCount && physical == address(m.reads));
  assert(physical >= Page && physical + 4 <= Page + PageBytes && !(physical & 3));
  m.addresses.push_back(physical);
  const unsigned index = m.reads++;
  if (index == m.fail_read) return false;
  *out = m.values[index];
  return true;
}
void unmap(void *opaque) {
  auto &m = *static_cast<Model *>(opaque); assert(m.mapped); ++m.unmaps; m.mapped = false;
}
void log(void *opaque, Event event, unsigned index, const Report &report) {
  auto &m = *static_cast<Model *>(opaque);
  m.events.push_back({event, index, report});
  if (m.reenter) {
    Report nested;
    nested.valid_mask = 0x555;
    assert(run(*m.state, *m.backend, nested) == Result::Busy);
    assert(nested.valid_mask == 0x555);
  }
}
Backend backend(Model &m) { return {&m, context, map, read, unmap, log}; }
void no_io(const Model &m) { assert(m.maps == 0 && m.reads == 0 && m.unmaps == 0); }

int main() {
  unsigned cases = 0;
  // The single page and order match the pinned SM8250 profile exactly.
  const auto &p = *shengufs::platformProfile(shengufs::Platform::Sm8250);
  assert(address(0) == p.gcc + p.gdsc && address(1) == p.gcc + p.reset);
  for (unsigned i = 0; i < 9; ++i) assert(address(2 + i) == p.gcc + p.clocks[i].offset);
  assert(address(11) == 0 && address(~0U) == 0);
  ++cases;
  {
    State s; Model m; Backend b = backend(m); Report r;
    assert(run(s, b, r) == Result::Captured);
    assert(r.reads_attempted == 11 && r.reads_completed == 11 && r.valid_mask == 0x7ff);
    assert(r.maps == 1 && r.unmaps == 1 && !m.mapped);
    assert(r.power_on && !r.reset_asserted && r.axi_readable_hint && r.ahb_readable_hint);
    assert(m.events.size() == 29);
    for (unsigned i = 0; i < 11; ++i) {
      const auto &before = m.events[4 + 2 * i], &after = m.events[5 + 2 * i];
      assert(before.event == Event::ReadBefore && before.index == i && before.report.reads_completed == i);
      assert(after.event == Event::ReadAfter && after.index == i && after.report.values[i] == m.values[i]);
    }
    assert(m.events[26].event == Event::UnmapBefore && m.events[27].event == Event::UnmapAfter);
    assert(m.events[28].event == Event::Summary && m.events[28].report.unmaps == 1);
    assert(run(s, b, r) == Result::AlreadyAttempted);
    assert(m.reads == 11 && m.maps == 1 && m.unmaps == 1);
    ++cases;
  }
  // Every partial read failure is explicit, bounded and releases its map once.
  for (unsigned fail = 0; fail < RegisterCount; ++fail) {
    State s; Model m; Backend b = backend(m); Report r; m.fail_read = fail;
    assert(run(s, b, r) == Result::ReadFailed);
    assert(r.reads_attempted == fail + 1 && r.reads_completed == fail);
    assert(r.valid_mask == (1U << fail) - 1 && r.maps == 1 && r.unmaps == 1 && !m.mapped);
    assert(m.reads == fail + 1 && m.events.back().report.result == Result::ReadFailed);
    assert(!r.power_on && !r.axi_readable_hint);
    ++cases;
  }
  for (unsigned bad = 0; bad < 7; ++bad) {
    State s; Model m; Backend b = backend(m); Report r;
    switch (bad) {
      case 0: m.facts.thread_present = false; break;
      case 1: m.facts.interrupt_context = true; break;
      case 2: m.facts.interrupts_enabled = false; break;
      case 3: m.facts.preemption_level = 1; break;
      case 4: m.facts.kernel_task_present = false; break;
      case 5: m.facts.registered = false; break;
      case 6: m.facts.runtime_cold = false; break;
    }
    const Result expected = bad == 5 ? Result::RegistrationRequired :
                            bad == 6 ? Result::RuntimeNotCold : Result::ContextRejected;
    assert(run(s, b, r) == expected); no_io(m);
    assert(run(s, b, r) == Result::AlreadyAttempted); no_io(m);
    ++cases;
  }
  {
    State s; Model m; Backend b = backend(m); Report r; m.map_ok = false;
    assert(run(s, b, r) == Result::MappingFailed);
    assert(m.maps == 1 && !m.mapped && m.reads == 0 && m.unmaps == 0);
    assert(r.maps == 0 && r.unmaps == 0 && r.reads_attempted == 0);
    ++cases;
  }
  for (unsigned absent = 0; absent < 5; ++absent) {
    State s; Model m; Backend b = backend(m); Report r;
    switch (absent) {
      case 0: b.context = nullptr; break;
      case 1: b.map = nullptr; break;
      case 2: b.read32 = nullptr; break;
      case 3: b.unmap = nullptr; break;
      case 4: b.log = nullptr; break;
    }
    assert(run(s, b, r) == Result::InvalidBackend); no_io(m);
    assert(m.contexts == 0 && !s.attempted);
    ++cases;
  }
  {
    State s; Model m; Backend b = backend(m); Report r;
    m.reenter = true; m.state = &s; m.backend = &b;
    assert(run(s, b, r) == Result::Captured && r.reads_completed == 11);
    ++cases;
  }
  // Power/reset/clock hints do not stop GCC capture or become an HCI grant.
  for (unsigned pattern = 0; pattern < 3; ++pattern) {
    State s; Model m; Backend b = backend(m); Report r;
    for (auto &value : m.values) value = pattern == 0 ? 0 : 0xffffffff;
    if (pattern == 2) { m.values[0] = 0x80000000; m.values[1] = 0; m.values[2] = 0x80000001; }
    assert(run(s, b, r) == Result::Captured && r.reads_completed == 11);
    assert(r.valid_mask == 0x7ff && r.unmaps == 1);
    if (pattern == 0) assert(!r.power_on && !r.reset_asserted);
    if (pattern == 1) assert(r.power_on && r.reset_asserted);
    if (pattern == 2) assert(!r.axi_readable_hint);
    ++cases;
  }
  printf("StoragePower: %u cases passed; modeled callbacks only, no hardware or abort containment\n", cases);
}
