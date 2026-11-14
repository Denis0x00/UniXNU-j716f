// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host orchestration tests. Synthetic access records are MODEL INPUTS ONLY.
#include "J716fStorageBringup.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
using namespace j716f_storage;
using namespace sm8250_ufs;
static unsigned checks = 0;
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr, "FAIL line %u: %s\n", __LINE__, #x); std::exit(1); } } while (0)
struct Mock {
  unsigned init = 0, observations = 0, allocations = 0, activations = 0, reads = 0, stops = 0, discards = 0;
  unsigned failure = 0;
  bool wrong_contract = false, quarantine = false, mismatch = false, reenter = false;
  Event events[128] = {};
  unsigned event_count = 0;
  State *state = nullptr;
  Error reentry_result = Error::None;
};
static Mock mock;
static RuntimeReport modelRuntime(RuntimePhase phase) {
  RuntimeReport r;
  r.phase = phase;
  r.allocation = {1, 0x90000000, 139264};
  r.command.capacity = {true, 0, 4096, 128};
  return r;
}
static void init() { ++mock.init; }
static unsigned diag(const DiagnosticRequest *in, DiagnosticReport *out) {
  ++mock.observations;
  *out = {};
  out->requested_stage = in->stage;
  out->completed_stage = mock.wrong_contract ? DiagnosticStage::None : in->stage;
  out->attempted_reads = 28; out->mapped_pages = out->released_pages = static_cast<unsigned>(in->stage);
  out->error = mock.failure ? DiagnosticError::Read : DiagnosticError::None;
  return mock.failure;
}
static unsigned runtimeResult(RuntimeReport *out, RuntimePhase phase) {
  *out = modelRuntime(phase);
  if (mock.failure) {
    out->error = RuntimeError::Command;
    if (mock.quarantine) out->phase = RuntimePhase::Quarantined;
  }
  if (mock.wrong_contract) out->phase = RuntimePhase::Cold;
  return mock.failure;
}
static unsigned alloc(const RuntimeAccess *, RuntimeReport *out) { ++mock.allocations; return runtimeResult(out, RuntimePhase::Allocated); }
static unsigned activateBackend(const RuntimeStart *, RuntimeReport *out) { ++mock.activations; return runtimeResult(out, RuntimePhase::Active); }
static unsigned read(uint64_t lba, uint8_t *out, size_t bytes, RuntimeReport *report) {
  ++mock.reads;
  if (lba != 7) {
    *report = modelRuntime(RuntimePhase::Active); report->error = RuntimeError::Command;
    report->command.error = shengufs::restricted::Error::PolicyRejected;
    return static_cast<unsigned>(RuntimeError::Command);
  }
  for (size_t i = 0; i < bytes; ++i) out[i] = static_cast<uint8_t>(i);
  if (mock.mismatch) out[4095] ^= 1;
  return runtimeResult(report, RuntimePhase::Active);
}
static unsigned stopBackend(RuntimeReport *out) { ++mock.stops; return runtimeResult(out, RuntimePhase::Stopped); }
static unsigned discard(RuntimeReport *out) { ++mock.discards; return runtimeResult(out, RuntimePhase::Failed); }
static void log(Event e, const Report &) {
  CHECK(mock.event_count < 128);
  mock.events[mock.event_count++] = e;
  if (mock.reenter) {
    Report output;
    mock.reentry_result = status(*mock.state, &output);
  }
}
static const Backend backend = {init, diag, alloc, activateBackend, read, stopBackend, discard, log};
static DiagnosticRequest request(DiagnosticStage stage) {
  DiagnosticRequest r = {};
  r.stage = stage;
  r.access = {1, 0x4a373136, 123, RequiredDiagnosticFacts, static_cast<unsigned>(stage), {}};
  for (unsigned i = 0; i < r.access.range_count; ++i) r.access.ranges[i] = diagnosticRange(i);
  return r;
}
static RuntimeAccess access() { return {1, 0x4a373136, 321, RequiredAccessFacts}; }
static RuntimeStart start() {
  RuntimeStart r = {};
  r.allocation_id = 1;
  r.handoff.register_access_authorization = 321;
  r.handoff.visibility_authorization = 456;
  r.handoff.dma_authorization = 789;
  r.handoff.facts = RequiredHandoffFacts;
  r.handoff.owner.authorization_id = 987;
  r.handoff.owner.flags = APPS_SMMU_HANDOFF_REQUIRED;
  r.handoff.owner.claim.dma_base = 0x90000000;
  r.handoff.owner.claim.dma_bytes = 139264;
  r.lun = 0;
  r.known_block_count = 1;
  r.known_blocks[0] = {0, 7};
  return r;
}
static void registered(State &s) {
  mock = {};
  CHECK(initialize(s, backend, nullptr) == Error::None);
  CHECK(mock.init == 1 && !mock.observations && !mock.allocations && !mock.activations);
  CHECK(s.report.gate == Gate::PowerReadAuthority);
}
static void geometry(State &s) {
  registered(s);
  Report out;
  for (auto stage : {DiagnosticStage::Power, DiagnosticStage::Host, DiagnosticStage::Geometry}) {
    const auto r = request(stage);
    CHECK(observe(s, backend, &r, &out) == Error::None);
  }
  CHECK(s.report.stage == Stage::GeometryObserved && mock.observations == 3);
}
static void allocated(State &s) {
  geometry(s);
  Report out;
  const auto a = access();
  CHECK(allocate(s, backend, &a, &out) == Error::None);
  CHECK(out.gate == Gate::ExclusiveHandoff && mock.allocations == 1);
}
static void ready(State &s) {
  allocated(s);
  Report out;
  const auto r = start();
  CHECK(activate(s, backend, &r, &out) == Error::None);
  CHECK(out.stage == Stage::TransportReady && out.runtime.command.capacity.measured);
}
int main() {
  {
    State s; Report out;
    CHECK(status(s, &out) == Error::NotRegistered);
    auto r = request(DiagnosticStage::Power);
    CHECK(observe(s, backend, &r, &out) == Error::NotRegistered);
    registered(s);
    CHECK(initialize(s, backend, &out) == Error::WrongStage && mock.init == 1);
    CHECK(allocate(s, backend, nullptr, &out) == Error::InvalidArgument && mock.allocations == 0);
    r = request(DiagnosticStage::Geometry);
    CHECK(observe(s, backend, &r, &out) == Error::WrongStage && mock.observations == 0);
  }
  for (unsigned mutation = 0; mutation < 10; ++mutation) {
    State s; Report out; registered(s);
    auto r = request(DiagnosticStage::Power);
    switch (mutation) {
      case 0: r.access.version = 2; break;
      case 1: r.access.board = 0; break;
      case 2: r.access.authorization = 0; break;
      case 3: r.access.facts &= ~1U; break;
      case 4: r.access.facts &= ~2U; break;
      case 5: r.access.facts &= ~4U; break;
      case 6: ++r.access.range_count; break;
      case 7: ++r.access.ranges[0].base; break;
      case 8: ++r.access.ranges[0].bytes; break;
      case 9: r.access.ranges[1].base = 1; break;
    }
    CHECK(observe(s, backend, &r, &out) == Error::DiagnosticAccessRequired);
    CHECK(mock.observations == 0 && s.report.stage == Stage::Registered);
  }
  {
    State s; Report out; registered(s);
    auto r = request(DiagnosticStage::Power);
    mock.failure = 9;
    CHECK(observe(s, backend, &r, &out) == Error::BackendFailure && out.backend_result == 9);
    CHECK(s.report.stage == Stage::Registered);
    mock.failure = 0;
    CHECK(observe(s, backend, &r, &out) == Error::None);
    CHECK(mock.events[mock.event_count-2] == Event::ObserveEnter && mock.events[mock.event_count-1] == Event::ObserveReturn);
  }
  for (unsigned bit = 0; bit < 4; ++bit) {
    State s; Report out; geometry(s);
    auto a = access(); a.facts &= ~(1U << bit);
    CHECK(allocate(s, backend, &a, &out) == Error::RuntimeAccessRequired && mock.allocations == 0);
  }
  for (unsigned mutation = 0; mutation < 13; ++mutation) {
    State s; Report out; allocated(s);
    auto r = start();
    if (mutation < 4) r.handoff.facts &= ~(1U << mutation);
    else if (mutation < 8) r.handoff.owner.flags &= ~(1U << (mutation - 4));
    else switch (mutation) {
      case 8: r.handoff.visibility_authorization = 0; break;
      case 9: r.handoff.dma_authorization = 0; break;
      case 10: r.handoff.register_access_authorization = 0; break;
      case 11: ++r.handoff.owner.claim.dma_base; break;
      case 12: ++r.allocation_id; break;
    }
    CHECK(activate(s, backend, &r, &out) == Error::HandoffRequired);
    CHECK(mock.activations == 0 && s.report.stage == Stage::Allocated);
  }
  {
    State s; Report out; allocated(s);
    mock.failure = 9; mock.quarantine = true;
    auto r = start();
    CHECK(activate(s, backend, &r, &out) == Error::BackendFailure);
    CHECK(out.stage == Stage::Quarantined && out.gate == Gate::Terminal);
    CHECK(discardUnused(s, backend, &out) == Error::WrongStage && mock.discards == 0);
    CHECK(initialize(s, backend, &out) == Error::WrongStage);
  }
  {
    State s; Report out; ready(s);
    uint8_t reference[4096], output[4096];
    for (unsigned i = 0; i < 4096; ++i) reference[i] = static_cast<uint8_t>(i);
    CHECK(readKnown(s, backend, 7, reference, reference, 4096, &out) == Error::InvalidArgument && !mock.reads);
    CHECK(readKnown(s, backend, 7, reference, output, 4095, &out) == Error::InvalidArgument && !mock.reads);
    CHECK(readKnown(s, backend, 8, reference, output, 4096, &out) == Error::BackendFailure);
    CHECK(out.stage == Stage::TransportReady);
    mock.mismatch = true;
    CHECK(readKnown(s, backend, 7, reference, output, 4096, &out) == Error::KnownBlockMismatch);
    CHECK(!out.reference_matched && out.compared_bytes == 4096 && out.stage == Stage::TransportReady);
    mock.mismatch = false;
    CHECK(readKnown(s, backend, 7, reference, output, 4096, &out) == Error::None);
    CHECK(out.reference_matched && out.stage == Stage::KnownBlockRead && out.gate == Gate::StopProof);
    CHECK(out.capacity.measured && out.capacity.blocks == 128 && out.capacity.block_bytes == 4096);
    CHECK(stop(s, backend, &out) == Error::None && out.stage == Stage::Stopped);
    CHECK(discardUnused(s, backend, &out) == Error::WrongStage && !mock.discards);
    CHECK(readKnown(s, backend, 7, reference, output, 4096, &out) == Error::WrongStage);
  }
  {
    State s; Report out; allocated(s);
    CHECK(discardUnused(s, backend, &out) == Error::None && mock.discards == 1);
    CHECK(out.stage == Stage::Failed && out.gate == Gate::Terminal);
    CHECK(initialize(s, backend, &out) == Error::WrongStage);
  }
  {
    State s; Report out; registered(s);
    mock.state = &s; mock.reenter = true;
    auto r = request(DiagnosticStage::Power);
    CHECK(observe(s, backend, &r, &out) == Error::None && mock.reentry_result == Error::Busy);
    CHECK(status(s, &out) == Error::None);
    s.busy = 1;
    CHECK(status(s, &out) == Error::Busy);
    CHECK(observe(s, backend, &r, &out) == Error::Busy);
    s.busy = 0;
  }
  {
    State s; Report out; registered(s); mock.wrong_contract = true;
    auto r = request(DiagnosticStage::Power);
    CHECK(observe(s, backend, &r, &out) == Error::BackendContract);
    CHECK(out.stage == Stage::Failed);
  }
  std::printf("PASS %u checks: storage orchestration MODEL only; no MMIO/DMA/device access\n", checks);
}
