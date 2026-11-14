// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "Sm8250UfsDiagnostics.h"

namespace sm8250_ufs {
namespace {
constexpr unsigned host_offsets[shengufs::HostCount] = {
  0x00, 0x08, 0x20, 0x24, 0x30, 0x34, 0x50, 0x54, 0x58, 0x60,
  0x70, 0x74, 0x78, 0x80
};
unsigned stagePages(DiagnosticStage stage)
{
  switch (stage) {
    case DiagnosticStage::Power: return 1;
    case DiagnosticStage::Host: return 2;
    case DiagnosticStage::Geometry: return 3;
    default: return 0;
  }
}
bool validRequest(const DiagnosticRequest &r)
{
  const unsigned count = stagePages(r.stage);
  const auto &a = r.access;
  if (!count || a.version != 1 || a.board != 0x4a373136 || !a.authorization ||
      a.facts != RequiredDiagnosticFacts || a.range_count != count) return false;
  for (unsigned i = 0; i < 3; ++i) {
    const shengufs::Range expected = i < count ? diagnosticRange(i) : shengufs::Range{};
    if (a.ranges[i].base != expected.base || a.ranges[i].bytes != expected.bytes) return false;
  }
  return true;
}
DiagnosticError observationError(shengufs::ObservationStatus status)
{
  using S = shengufs::ObservationStatus;
  switch (status) {
    case S::Captured: return DiagnosticError::None;
    case S::PowerNotReady: return DiagnosticError::PowerNotReady;
    case S::ResetAsserted: return DiagnosticError::ResetAsserted;
    case S::ClockNotReady: return DiagnosticError::ClockNotReady;
    case S::GeometryUnknown: return DiagnosticError::GeometryUnknown;
    case S::Unreadable: return DiagnosticError::Read;
    default: return DiagnosticError::InvalidRequest;
  }
}
class Session {
public:
  Session(DiagnosticStage stage, const DiagnosticIo &io, DiagnosticReport &report)
    : stage_(stage), io_(io), report_(report) {}
  ~Session() { release(); }
  Session(const Session &) = delete;
  Session &operator=(const Session &) = delete;
  bool read(uint64_t address, uint32_t *out)
  {
    if (!out || !diagnosticAddressAllowed(stage_, address)) return false;
    unsigned index = 0;
    while (index < stagePages(stage_) && !shengufs::contains32(diagnosticRange(index), address)) ++index;
    if (index == stagePages(stage_)) return false;
    const auto range = diagnosticRange(index);
    if (!mapped_[index]) {
      if (!io_.mapReadOnly(io_.context, range.base, range.bytes)) {
        report_.failed_mapping = range.base;
        report_.error = DiagnosticError::Mapping;
        return false;
      }
      mapped_[index] = true;
      ++report_.mapped_pages;
    }
    ++report_.attempted_reads;
    return io_.read32(io_.context, address, out);
  }
  void release()
  {
    for (unsigned i = 3; i > 0; --i) {
      if (!mapped_[i - 1]) continue;
      const auto range = diagnosticRange(i - 1);
      io_.unmap(io_.context, range.base, range.bytes);
      mapped_[i - 1] = false;
      ++report_.released_pages;
    }
  }
  static bool readThunk(void *context, uint64_t address, uint32_t *out)
  { return static_cast<Session *>(context)->read(address, out); }
private:
  DiagnosticStage stage_;
  DiagnosticIo io_;
  DiagnosticReport &report_;
  bool mapped_[3] = {};
};
void prefix(DiagnosticStage stage, Session &session, shengufs::UfsObservation &o)
{
  const auto &p = *shengufs::platformProfile(shengufs::Platform::Sm8250);
  o.expectedStreams[0] = p.expectedStreams[0];
  o.expectedStreams[1] = p.expectedStreams[1];
  auto fetch = [&](uint64_t address, uint32_t &value, unsigned &count) {
    uint32_t observed = 0;
    if (!session.read(address, &observed)) {
      o.status = shengufs::ObservationStatus::Unreadable;
      o.failedAddress = address;
      return false;
    }
    value = observed;
    ++count;
    return true;
  };
  if (!fetch(p.gcc + p.gdsc, o.gdsc, o.gccReads) ||
      !fetch(p.gcc + p.reset, o.reset, o.gccReads)) return;
  if (!(o.gdsc & 0x80000000U)) { o.status = shengufs::ObservationStatus::PowerNotReady; return; }
  if (o.reset & 1) { o.status = shengufs::ObservationStatus::ResetAsserted; return; }
  for (unsigned i = 0; i < shengufs::ClockCount; ++i)
    if (!fetch(p.gcc + p.clocks[i].offset, o.clocks[i], o.gccReads)) return;
  if (!shengufs::branchState(o.clocks[shengufs::Axi], p.clocks[shengufs::Axi].hardwareGating).readable ||
      !shengufs::branchState(o.clocks[shengufs::Ahb], p.clocks[shengufs::Ahb].hardwareGating).readable) {
    o.status = shengufs::ObservationStatus::ClockNotReady;
    return;
  }
  if (stage == DiagnosticStage::Host)
    for (unsigned i = 0; i < shengufs::HostCount; ++i)
      if (!fetch(p.host.base + host_offsets[i], o.host[i], o.hostReads)) return;
  o.status = shengufs::ObservationStatus::Captured;
}
} // namespace

shengufs::Range diagnosticRange(unsigned index)
{
  switch (index) {
    case 0: return {0x177000, 4096};
    case 1: return {0x1d84000, 4096};
    case 2: return {0x15000000, 4096};
    default: return {};
  }
}
bool diagnosticAddressAllowed(DiagnosticStage stage, uint64_t address)
{
  const unsigned pages = stagePages(stage);
  if (!pages || (address & 3)) return false;
  const auto &p = *shengufs::platformProfile(shengufs::Platform::Sm8250);
  if (address == p.gcc + p.gdsc || address == p.gcc + p.reset) return true;
  for (const auto &clock : p.clocks) if (address == p.gcc + clock.offset) return true;
  if (pages >= 2)
    for (const auto offset : host_offsets) if (address == p.host.base + offset) return true;
  if (pages == 3)
    for (unsigned i = 0; i < 3; ++i) if (address == p.smmu.base + 0x20 + 4 * i) return true;
  return false;
}
DiagnosticReport observeDiagnostics(const DiagnosticRequest &input, const DiagnosticIo &io)
{
  // Copy the small request so no callback can widen a grant/stage mid-capture.
  const DiagnosticRequest request = input;
  DiagnosticReport result = {};
  result.requested_stage = request.stage;
  if (!validRequest(request)) { result.error = DiagnosticError::InvalidRequest; return result; }
  if (!io.mapReadOnly || !io.unmap || !io.read32) {
    result.error = DiagnosticError::InvalidIo;
    return result;
  }
  Session session(request.stage, io, result);
  if (request.stage == DiagnosticStage::Geometry) {
    const auto &p = *shengufs::platformProfile(shengufs::Platform::Sm8250);
    const shengufs::PlatformIdentity identity = {p.soc, p.controller, p.host, p.smmu};
    const shengufs::ReadGrant grant = {request.access.ranges, request.access.range_count, true};
    // One capture only: reuse the original observer's geometry interpretation,
    // including its power/clock prefix. Do not read that prefix separately.
    result.observation = shengufs::observeUfs(p.platform, identity, grant, Session::readThunk, &session);
  } else {
    prefix(request.stage, session, result.observation);
  }
  if (result.error != DiagnosticError::Mapping) result.error = observationError(result.observation.status);
  if (result.error == DiagnosticError::None) result.completed_stage = request.stage;
  session.release(); // Counters must be final before copying the return value.
  return result;
}
} // namespace sm8250_ufs
