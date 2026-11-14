// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "J716fStorageBringup.h"

namespace j716f_storage {
namespace {
using namespace sm8250_ufs;
struct Guard {
  unsigned &busy;
  bool held;
  explicit Guard(State &s) : busy(s.busy), held(__atomic_exchange_n(&busy, 1U, __ATOMIC_ACQUIRE) == 0) {}
  ~Guard() { if (held) __atomic_store_n(&busy, 0U, __ATOMIC_RELEASE); }
};
Gate gate(Stage stage) {
  switch (stage) {
    case Stage::Unregistered: return Gate::Registration;
    case Stage::Registered: return Gate::PowerReadAuthority;
    case Stage::PowerObserved: return Gate::HostReadAuthority;
    case Stage::HostObserved: return Gate::GeometryReadAuthority;
    case Stage::GeometryObserved: return Gate::AllocationAuthority;
    case Stage::Allocated: return Gate::ExclusiveHandoff;
    case Stage::TransportReady: return Gate::KnownBlockReference;
    case Stage::KnownBlockRead: return Gate::StopProof;
    default: return Gate::Terminal;
  }
}
bool complete(const Backend &b) {
  return b.init && b.observe && b.allocate && b.activate && b.read && b.stop && b.discard && b.log;
}
Error finish(State &s, const Backend &b, Report *out, Error error, Event event) {
  s.report.error = error;
  s.report.gate = gate(s.report.stage);
  ++s.report.sequence;
  if (b.log) b.log(event, s.report);
  if (out) *out = s.report;
  return error;
}
void enter(State &s, const Backend &b, Event event) {
  s.report.error = Error::None;
  s.report.backend_result = 0;
  ++s.report.sequence;
  b.log(event, s.report);
}
Error invalid(State &s, const Backend &b, Report *out, Error error) {
  return finish(s, b, out, error, Event::Refused);
}
bool active(Stage s) { return s == Stage::TransportReady || s == Stage::KnownBlockRead; }
void runtimeFailure(State &s) {
  if (s.report.runtime.phase == RuntimePhase::Quarantined) s.report.stage = Stage::Quarantined;
  else if (s.report.runtime.phase == RuntimePhase::Failed) s.report.stage = Stage::Failed;
}
bool authority(const DiagnosticRequest &r) {
  const auto &a = r.access;
  const unsigned pages = static_cast<unsigned>(r.stage);
  if (!pages || pages > 3 || a.version != 1 || a.board != 0x4a373136 ||
      !a.authorization || a.facts != RequiredDiagnosticFacts || a.range_count != pages) return false;
  for (unsigned i = 0; i < 3; ++i) {
    const auto expected = i < pages ? diagnosticRange(i) : shengufs::Range{};
    if (a.ranges[i].base != expected.base || a.ranges[i].bytes != expected.bytes) return false;
  }
  return true;
}
} // namespace

const char *stageName(Stage s) {
  switch (s) {
    case Stage::Unregistered: return "unregistered";
    case Stage::Registered: return "registered";
    case Stage::PowerObserved: return "power-observed";
    case Stage::HostObserved: return "host-observed";
    case Stage::GeometryObserved: return "geometry-observed";
    case Stage::Allocated: return "allocated";
    case Stage::TransportReady: return "transport-ready";
    case Stage::KnownBlockRead: return "known-block-read";
    case Stage::Stopped: return "stopped";
    case Stage::Failed: return "failed";
    case Stage::Quarantined: return "quarantined";
  }
  return "invalid";
}
const char *gateName(Gate g) {
  switch (g) {
    case Gate::Registration: return "registration";
    case Gate::PowerReadAuthority: return "power-read-authority";
    case Gate::HostReadAuthority: return "host-read-authority";
    case Gate::GeometryReadAuthority: return "geometry-read-authority";
    case Gate::AllocationAuthority: return "allocation-authority";
    case Gate::ExclusiveHandoff: return "exclusive-firmware-tz-dma-handoff";
    case Gate::KnownBlockReference: return "known-block-reference";
    case Gate::StopProof: return "confirmed-stop";
    case Gate::Terminal: return "terminal-retain-resources";
  }
  return "invalid";
}
Error initialize(State &s, const Backend &b, Report *out) {
  Guard guard(s); if (!guard.held) return Error::Busy;
  if (!complete(b)) return invalid(s, b, out, Error::InvalidArgument);
  if (s.report.stage != Stage::Unregistered) return invalid(s, b, out, Error::WrongStage);
  b.init();
  s.report.stage = Stage::Registered;
  return finish(s, b, out, Error::None, Event::Registered);
}
Error status(State &s, Report *out) {
  Guard guard(s); if (!guard.held) return Error::Busy;
  if (!out) return Error::InvalidArgument;
  *out = s.report;
  return s.report.stage == Stage::Unregistered ? Error::NotRegistered : Error::None;
}
Error observe(State &s, const Backend &b, const DiagnosticRequest *request, Report *out) {
  Guard guard(s); if (!guard.held) return Error::Busy;
  if (!complete(b) || !request || !out) return invalid(s, b, out, Error::InvalidArgument);
  if (s.report.stage == Stage::Unregistered) return invalid(s, b, out, Error::NotRegistered);
  const DiagnosticRequest r = *request;
  const Stage required = r.stage == DiagnosticStage::Power ? Stage::Registered :
    r.stage == DiagnosticStage::Host ? Stage::PowerObserved :
    r.stage == DiagnosticStage::Geometry ? Stage::HostObserved : Stage::Unregistered;
  if (required == Stage::Unregistered || s.report.stage != required)
    return invalid(s, b, out, Error::WrongStage);
  if (!authority(r)) return invalid(s, b, out, Error::DiagnosticAccessRequired);
  s.report.diagnostic = {};
  enter(s, b, Event::ObserveEnter);
  s.report.backend_result = b.observe(&r, &s.report.diagnostic);
  if (s.report.backend_result || s.report.diagnostic.error != DiagnosticError::None)
    return finish(s, b, out, Error::BackendFailure, Event::ObserveReturn);
  if (s.report.diagnostic.completed_stage != r.stage || s.report.diagnostic.requested_stage != r.stage) {
    s.report.stage = Stage::Failed;
    return finish(s, b, out, Error::BackendContract, Event::ObserveReturn);
  }
  s.report.stage = r.stage == DiagnosticStage::Power ? Stage::PowerObserved :
    r.stage == DiagnosticStage::Host ? Stage::HostObserved : Stage::GeometryObserved;
  return finish(s, b, out, Error::None, Event::ObserveReturn);
}
Error allocate(State &s, const Backend &b, const RuntimeAccess *access, Report *out) {
  Guard guard(s); if (!guard.held) return Error::Busy;
  if (!complete(b) || !access || !out) return invalid(s, b, out, Error::InvalidArgument);
  if (s.report.stage != Stage::GeometryObserved) return invalid(s, b, out, Error::WrongStage);
  const RuntimeAccess a = *access;
  if (a.version != 1 || a.board != 0x4a373136 || !a.authorization || a.facts != RequiredAccessFacts)
    return invalid(s, b, out, Error::RuntimeAccessRequired);
  enter(s, b, Event::AllocateEnter);
  s.report.backend_result = b.allocate(&a, &s.report.runtime);
  if (s.report.backend_result || s.report.runtime.error != RuntimeError::None) {
    runtimeFailure(s); return finish(s, b, out, Error::BackendFailure, Event::AllocateReturn);
  }
  const auto &allocation = s.report.runtime.allocation;
  if (s.report.runtime.phase != RuntimePhase::Allocated || !allocation.id || !allocation.physical ||
      (allocation.physical & 4095) || allocation.bytes != 139264 || allocation.physical > UINT64_MAX - allocation.bytes) {
    s.report.stage = Stage::Failed;
    return finish(s, b, out, Error::BackendContract, Event::AllocateReturn);
  }
  s.allocation_access = a;
  s.report.stage = Stage::Allocated;
  return finish(s, b, out, Error::None, Event::AllocateReturn);
}
Error activate(State &s, const Backend &b, const RuntimeStart *start, Report *out) {
  Guard guard(s); if (!guard.held) return Error::Busy;
  if (!complete(b) || !start || !out) return invalid(s, b, out, Error::InvalidArgument);
  if (s.report.stage != Stage::Allocated) return invalid(s, b, out, Error::WrongStage);
  const RuntimeStart r = *start;
  const auto &h = r.handoff;
  const auto &a = s.report.runtime.allocation;
  if (r.allocation_id != a.id || h.register_access_authorization != s.allocation_access.authorization ||
      !h.visibility_authorization || !h.dma_authorization || h.facts != RequiredHandoffFacts ||
      !h.owner.authorization_id || h.owner.flags != APPS_SMMU_HANDOFF_REQUIRED ||
      h.owner.claim.dma_base != a.physical || h.owner.claim.dma_bytes != a.bytes)
    return invalid(s, b, out, Error::HandoffRequired);
  enter(s, b, Event::ActivateEnter);
  s.report.backend_result = b.activate(&r, &s.report.runtime);
  if (s.report.backend_result || s.report.runtime.error != RuntimeError::None) {
    runtimeFailure(s); return finish(s, b, out, Error::BackendFailure, Event::ActivateReturn);
  }
  const auto &capacity = s.report.runtime.command.capacity;
  if (s.report.runtime.phase != RuntimePhase::Active || !capacity.measured || capacity.lun != r.lun ||
      capacity.block_bytes != 4096 || !capacity.blocks || capacity.blocks > UINT32_MAX) {
    s.report.stage = Stage::Failed;
    return finish(s, b, out, Error::BackendContract, Event::ActivateReturn);
  }
  s.report.stage = Stage::TransportReady;
  s.report.capacity = capacity;
  return finish(s, b, out, Error::None, Event::ActivateReturn);
}
Error readKnown(State &s, const Backend &b, uint64_t lba, const uint8_t *reference,
                uint8_t *output, size_t bytes, Report *out) {
  Guard guard(s); if (!guard.held) return Error::Busy;
  if (!complete(b) || !reference || !output || !out || bytes != 4096 || lba > UINT32_MAX)
    return invalid(s, b, out, Error::InvalidArgument);
  const uintptr_t rp = reinterpret_cast<uintptr_t>(reference), op = reinterpret_cast<uintptr_t>(output);
  if (rp > UINTPTR_MAX - bytes || op > UINTPTR_MAX - bytes || (rp < op + bytes && op < rp + bytes))
    return invalid(s, b, out, Error::InvalidArgument);
  if (!active(s.report.stage)) return invalid(s, b, out, Error::WrongStage);
  s.report.last_lba = lba; s.report.compared_bytes = 0; s.report.reference_matched = false;
  enter(s, b, Event::ReadEnter);
  s.report.backend_result = b.read(lba, output, bytes, &s.report.runtime);
  if (s.report.backend_result || s.report.runtime.error != RuntimeError::None) {
    runtimeFailure(s); return finish(s, b, out, Error::BackendFailure, Event::ReadReturn);
  }
  if (s.report.runtime.phase != RuntimePhase::Active) {
    s.report.stage = Stage::Failed;
    return finish(s, b, out, Error::BackendContract, Event::ReadReturn);
  }
  unsigned difference = 0;
  for (size_t i = 0; i < bytes; ++i) difference |= reference[i] ^ output[i];
  s.report.compared_bytes = static_cast<unsigned>(bytes);
  s.report.reference_matched = difference == 0;
  if (difference) return finish(s, b, out, Error::KnownBlockMismatch, Event::ReadReturn);
  s.report.stage = Stage::KnownBlockRead;
  return finish(s, b, out, Error::None, Event::ReadReturn);
}
Error stop(State &s, const Backend &b, Report *out) {
  Guard guard(s); if (!guard.held) return Error::Busy;
  if (!complete(b) || !out) return invalid(s, b, out, Error::InvalidArgument);
  if (!active(s.report.stage)) return invalid(s, b, out, Error::WrongStage);
  enter(s, b, Event::StopEnter);
  s.report.backend_result = b.stop(&s.report.runtime);
  if (s.report.backend_result || s.report.runtime.error != RuntimeError::None) {
    runtimeFailure(s); return finish(s, b, out, Error::BackendFailure, Event::StopReturn);
  }
  if (s.report.runtime.phase != RuntimePhase::Stopped) {
    s.report.stage = Stage::Failed;
    return finish(s, b, out, Error::BackendContract, Event::StopReturn);
  }
  s.report.stage = Stage::Stopped;
  return finish(s, b, out, Error::None, Event::StopReturn);
}
Error discardUnused(State &s, const Backend &b, Report *out) {
  Guard guard(s); if (!guard.held) return Error::Busy;
  if (!complete(b) || !out) return invalid(s, b, out, Error::InvalidArgument);
  if (s.report.stage != Stage::Allocated) return invalid(s, b, out, Error::WrongStage);
  enter(s, b, Event::DiscardEnter);
  s.report.backend_result = b.discard(&s.report.runtime);
  if (s.report.backend_result || s.report.runtime.error != RuntimeError::None) {
    runtimeFailure(s); return finish(s, b, out, Error::BackendFailure, Event::DiscardReturn);
  }
  // Runtime is one-shot and uses Failed to denote a discarded unused context.
  s.report.stage = Stage::Failed;
  return finish(s, b, out, Error::None, Event::DiscardReturn);
}
} // namespace j716f_storage
