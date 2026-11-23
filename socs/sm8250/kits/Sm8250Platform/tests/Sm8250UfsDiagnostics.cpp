// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "Sm8250UfsDiagnostics.h"
#include <assert.h>
#include <stdio.h>
#include <map>
#include <set>
#include <type_traits>
#include <utility>
#include <vector>

using namespace sm8250_ufs;
namespace {
unsigned scenarios = 0, read_failures = 0, map_failures = 0;
const auto &profile = *shengufs::platformProfile(shengufs::Platform::Sm8250);
constexpr unsigned offsets[] = {0, 8, 0x20, 0x24, 0x30, 0x34, 0x50, 0x54,
                                 0x58, 0x60, 0x70, 0x74, 0x78, 0x80};
template<class T, class = void> struct HasWrite : std::false_type {};
template<class T> struct HasWrite<T, std::void_t<decltype(std::declval<T>().write32)>> : std::true_type {};
template<class T, class = void> struct HasDma : std::false_type {};
template<class T> struct HasDma<T, std::void_t<decltype(std::declval<T>().allocate)>> : std::true_type {};
static_assert(!HasWrite<DiagnosticIo>::value, "Read-only boundary must have no MMIO writer");
static_assert(!HasDma<DiagnosticIo>::value, "Read-only boundary must have no DMA allocator");

DiagnosticRequest request(DiagnosticStage stage)
{
  DiagnosticRequest r = {};
  r.stage = stage;
  r.access.version = 1; r.access.board = 0x4a373136;
  r.access.authorization = 91; r.access.facts = RequiredDiagnosticFacts;
  r.access.range_count = static_cast<unsigned>(stage);
  for (unsigned i = 0; i < r.access.range_count && i < 3; ++i) r.access.ranges[i] = diagnosticRange(i);
  return r;
}
std::vector<uint64_t> expected(DiagnosticStage stage)
{
  std::vector<uint64_t> values = {profile.gcc + profile.gdsc, profile.gcc + profile.reset};
  for (const auto &clock : profile.clocks) values.push_back(profile.gcc + clock.offset);
  if (stage != DiagnosticStage::Power)
    for (const auto offset : offsets) values.push_back(profile.host.base + offset);
  if (stage == DiagnosticStage::Geometry)
    for (unsigned i = 0; i < 3; ++i) values.push_back(profile.smmu.base + 0x20 + i * 4);
  return values;
}
struct Model {
  std::map<uint64_t, uint32_t> registers;
  std::vector<uint64_t> reads, maps, releases;
  bool active[3] = {};
  unsigned map_attempts = 0, fail_map = 0, fail_read = 0;
  DiagnosticRequest *mutate_request = nullptr;
  Model()
  {
    registers[profile.gcc + profile.gdsc] = 0x80000000;
    registers[profile.gcc + profile.reset] = 0;
    for (const auto &clock : profile.clocks) registers[profile.gcc + clock.offset] = 1;
    for (unsigned i = 0; i < shengufs::HostCount; ++i)
      registers[profile.host.base + offsets[i]] = 0x12340000U + i;
    // Deliberately arbitrary firmware list pointers: these must remain values.
    registers[profile.host.base + 0x50] = 0xfffff000;
    registers[profile.host.base + 0x54] = 0xdeadbeef;
    registers[profile.host.base + 0x70] = 0x12340000;
    registers[profile.host.base + 0x74] = 0xfedcba98;
    registers[profile.smmu.base + 0x20] = (1U << 30) | (1U << 27) | 8;
    registers[profile.smmu.base + 0x24] = (2U << 28) | 4; // 8 register pages, 4 CBs.
    registers[profile.smmu.base + 0x28] = (1U << 12) | 0x444;
  }
  void resetTrace()
  {
    for (bool live : active) assert(!live);
    reads.clear(); maps.clear(); releases.clear(); map_attempts = 0;
  }
  static bool map(void *context, uint64_t base, uint64_t bytes)
  {
    auto &m = *static_cast<Model *>(context);
    ++m.map_attempts;
    if (m.fail_map == m.map_attempts) return false;
    unsigned index = 0;
    while (index < 3 && diagnosticRange(index).base != base) ++index;
    assert(index < 3 && bytes == 4096 && !m.active[index]);
    m.active[index] = true; m.maps.push_back(base);
    return true;
  }
  static void unmap(void *context, uint64_t base, uint64_t bytes)
  {
    auto &m = *static_cast<Model *>(context);
    unsigned index = 0;
    while (index < 3 && diagnosticRange(index).base != base) ++index;
    assert(index < 3 && bytes == 4096 && m.active[index]);
    m.active[index] = false; m.releases.push_back(base);
  }
  static bool read(void *context, uint64_t address, uint32_t *out)
  {
    auto &m = *static_cast<Model *>(context);
    assert(out && m.registers.count(address));
    unsigned index = 0;
    while (index < 3 && !shengufs::contains32(diagnosticRange(index), address)) ++index;
    assert(index < 3 && m.active[index]);
    m.reads.push_back(address);
    // A callback cannot turn a copied Power request into a wider observation.
    if (m.mutate_request) { *m.mutate_request = request(DiagnosticStage::Geometry); m.mutate_request = nullptr; }
    *out = 0xbadbadba; // A failing callback must not leak this into raw results.
    if (m.fail_read == m.reads.size()) return false;
    *out = m.registers.at(address);
    return true;
  }
  static bool legacyRead(void *context, uint64_t address, uint32_t *out)
  {
    auto &m = *static_cast<Model *>(context);
    assert(m.registers.count(address));
    *out = m.registers.at(address);
    return true;
  }
  DiagnosticIo io() { return {this, map, unmap, read}; }
  void clean(const DiagnosticReport &r) const
  {
    for (bool live : active) assert(!live);
    assert(r.attempted_reads == reads.size());
    assert(r.mapped_pages == maps.size() && r.released_pages == releases.size());
    assert(maps.size() == releases.size());
    for (size_t i = 0; i < maps.size(); ++i) assert(maps[i] == releases[maps.size() - 1 - i]);
    assert(std::set<uint64_t>(reads.begin(), reads.end()).size() == reads.size());
    assert(!r.observation.assignmentsObserved);
  }
};
void verifyRaw(const shengufs::UfsObservation &a, const shengufs::UfsObservation &b)
{
  assert(a.status == b.status && a.gdsc == b.gdsc && a.reset == b.reset);
  for (unsigned i = 0; i < shengufs::ClockCount; ++i) assert(a.clocks[i] == b.clocks[i]);
  for (unsigned i = 0; i < shengufs::HostCount; ++i) assert(a.host[i] == b.host[i]);
  for (unsigned i = 0; i < 3; ++i) assert(a.idr[i] == b.idr[i]);
  assert(a.gccReads == b.gccReads && a.hostReads == b.hostReads && a.smmuReads == b.smmuReads);
  assert(a.failedAddress == b.failedAddress && a.streamGroups == b.streamGroups);
  assert(a.contextBanks == b.contextBanks && a.stage2OnlyBanks == b.stage2OnlyBanks);
  assert(a.registerPages == b.registerPages && a.assignmentsObserved == b.assignmentsObserved);
  for (unsigned i = 0; i < 2; ++i) assert(a.expectedStreams[i] == b.expectedStreams[i]);
}
void geometryCase(unsigned index, uint32_t value, DiagnosticError error)
{
  Model m;
  m.registers[profile.smmu.base + 0x20 + index * 4] = value;
  auto req = request(DiagnosticStage::Geometry);
  const auto result = observeDiagnostics(req, m.io());
  m.clean(result);
  assert(result.error == error && m.reads == expected(req.stage));
  assert(result.completed_stage == (error == DiagnosticError::None ? req.stage : DiagnosticStage::None));
  const shengufs::PlatformIdentity identity = {profile.soc, profile.controller, profile.host, profile.smmu};
  const shengufs::ReadGrant grant = {req.access.ranges, 3, true};
  const auto legacy = shengufs::observeUfs(profile.platform, identity, grant, Model::legacyRead, &m);
  verifyRaw(result.observation, legacy);
  ++scenarios;
}
void invalid(const DiagnosticRequest &req)
{
  Model m;
  const auto result = observeDiagnostics(req, m.io());
  assert(result.error == DiagnosticError::InvalidRequest && result.completed_stage == DiagnosticStage::None);
  assert(m.map_attempts == 0 && m.reads.empty());
  m.clean(result); ++scenarios;
}
void powerCase(DiagnosticStage stage, uint64_t address, uint32_t value,
               DiagnosticError error, unsigned reads)
{
  Model m; m.registers[address] = value;
  const auto result = observeDiagnostics(request(stage), m.io());
  assert(result.error == error && m.reads.size() == reads && m.maps.size() == 1);
  assert(result.completed_stage == DiagnosticStage::None && result.observation.hostReads == 0 &&
         result.observation.smmuReads == 0);
  m.clean(result); ++scenarios;
}
} // namespace

int main()
{
  const DiagnosticStage stages[] = {DiagnosticStage::Power, DiagnosticStage::Host, DiagnosticStage::Geometry};
  for (auto stage : stages) {
    Model m;
    const auto req = request(stage);
    const auto result = observeDiagnostics(req, m.io());
    m.clean(result);
    assert(result.error == DiagnosticError::None && result.completed_stage == stage && result.requested_stage == stage);
    assert(m.reads == expected(stage) && m.maps.size() == static_cast<unsigned>(stage));
    assert(result.observation.gccReads == 11);
    assert(result.observation.hostReads == (stage == DiagnosticStage::Power ? 0U : 14U));
    assert(result.observation.smmuReads == (stage == DiagnosticStage::Geometry ? 3U : 0U));
    assert(result.observation.expectedStreams[0] == 0xe0 && result.observation.expectedStreams[1] == 0x4e0);
    assert(result.observation.streamGroups == (stage == DiagnosticStage::Geometry ? 8U : 0U));
    if (stage == DiagnosticStage::Geometry) {
      assert(result.observation.contextBanks == 4 && result.observation.registerPages == 8);
      assert(result.observation.stage2OnlyBanks == 0);
    }
    ++scenarios;

    const auto allowed = expected(stage);
    for (unsigned page = 0; page < 3; ++page) {
      for (unsigned offset = 0; offset < 4096; offset += 4) {
        const uint64_t address = diagnosticRange(page).base + offset;
        bool found = false;
        for (auto fixed : allowed) if (fixed == address) found = true;
        assert(diagnosticAddressAllowed(stage, address) == found);
        assert(!diagnosticAddressAllowed(stage, address + 1));
      }
    }
    for (uint64_t excluded : {0x1d87000ULL, 0x1d90000ULL, 0x15182000ULL,
                              0x15000800ULL, 0x15000c00ULL, 0x15001000ULL,
                              0x15008000ULL, 0xfffffffffffffffCULL})
      assert(!diagnosticAddressAllowed(stage, excluded));
    ++scenarios;

    for (unsigned at = 1; at <= allowed.size(); ++at) {
      Model failure; failure.fail_read = at;
      const auto r = observeDiagnostics(req, failure.io());
      failure.clean(r);
      assert(r.error == DiagnosticError::Read && r.completed_stage == DiagnosticStage::None);
      assert(r.observation.failedAddress == allowed[at - 1] && r.attempted_reads == at);
      assert(r.observation.gccReads + r.observation.hostReads + r.observation.smmuReads == at - 1);
      assert(failure.reads == std::vector<uint64_t>(allowed.begin(), allowed.begin() + at));
      assert(r.observation.gdsc != 0xbadbadba && r.observation.reset != 0xbadbadba);
      for (auto raw : r.observation.clocks) assert(raw != 0xbadbadba);
      for (auto raw : r.observation.host) assert(raw != 0xbadbadba);
      for (auto raw : r.observation.idr) assert(raw != 0xbadbadba);
      ++read_failures;
    }
    for (unsigned at = 1; at <= static_cast<unsigned>(stage); ++at) {
      Model failure; failure.fail_map = at;
      const auto r = observeDiagnostics(req, failure.io());
      failure.clean(r);
      assert(r.error == DiagnosticError::Mapping && r.completed_stage == DiagnosticStage::None);
      assert(r.failed_mapping == diagnosticRange(at - 1).base && failure.map_attempts == at);
      assert(r.mapped_pages == at - 1 && r.attempted_reads == (at == 1 ? 0U : at == 2 ? 11U : 25U));
      ++map_failures;
    }
    powerCase(stage, profile.gcc + profile.gdsc, 0, DiagnosticError::PowerNotReady, 2);
    powerCase(stage, profile.gcc + profile.reset, 1, DiagnosticError::ResetAsserted, 2);
    powerCase(stage, profile.gcc + profile.clocks[shengufs::Axi].offset, 0, DiagnosticError::ClockNotReady, 11);
    powerCase(stage, profile.gcc + profile.clocks[shengufs::Ahb].offset, 0x80000001, DiagnosticError::ClockNotReady, 11);
    Model gated;
    for (const auto &clock : profile.clocks) gated.registers[profile.gcc + clock.offset] = 0x80000000;
    gated.registers[profile.gcc + profile.clocks[shengufs::Axi].offset] = 0x80000003;
    gated.registers[profile.gcc + profile.clocks[shengufs::Ahb].offset] = 0x80000003;
    const auto r = observeDiagnostics(req, gated.io());
    assert(r.error == DiagnosticError::None && gated.reads == allowed);
    gated.clean(r); ++scenarios;
  }

  auto req = request(DiagnosticStage::Geometry);
  for (unsigned value : {0U, 4U, 0xffffffffU}) { auto r = req; r.stage = static_cast<DiagnosticStage>(value); invalid(r); }
  for (unsigned value : {0U, 2U, 0xffffffffU}) { auto r = req; r.access.version = value; invalid(r); }
  { auto r = req; r.access.board = 0x8550; invalid(r); }
  { auto r = req; r.access.authorization = 0; invalid(r); }
  for (unsigned value : {0U, 1U, 3U, 6U, 15U, 0xffffffffU}) { auto r = req; r.access.facts = value; invalid(r); }
  for (unsigned value : {0U, 1U, 2U, 4U, 0xffffffffU}) { auto r = req; r.access.range_count = value; invalid(r); }
  for (unsigned i = 0; i < 3; ++i) {
    auto r = req; r.access.ranges[i].base += 4; invalid(r);
    r = req; r.access.ranges[i].bytes += 4096; invalid(r);
    r = req; r.access.ranges[i].bytes = 4; invalid(r);
    r = req; r.access.ranges[i] = {UINT64_MAX - 3, 4096}; invalid(r);
  }
  { auto r = req; std::swap(r.access.ranges[0], r.access.ranges[1]); invalid(r); }
  { auto r = req; r.stage = DiagnosticStage::Power; invalid(r); } // A broad grant is not implicit escalation.
  { auto r = request(DiagnosticStage::Power); r.access.ranges[1] = diagnosticRange(1); invalid(r); }
  for (unsigned i = 0; i < 3; ++i) {
    Model m; auto io = m.io();
    if (i == 0) io.mapReadOnly = nullptr;
    if (i == 1) io.unmap = nullptr;
    if (i == 2) io.read32 = nullptr;
    const auto r = observeDiagnostics(req, io);
    assert(r.error == DiagnosticError::InvalidIo && m.map_attempts == 0 && m.reads.empty());
    m.clean(r); ++scenarios;
  }
  {
    Model m; auto r = request(DiagnosticStage::Power); m.mutate_request = &r;
    const auto result = observeDiagnostics(r, m.io());
    assert(r.stage == DiagnosticStage::Geometry && result.requested_stage == DiagnosticStage::Power);
    assert(result.error == DiagnosticError::None && m.reads == expected(DiagnosticStage::Power));
    m.clean(result); ++scenarios;
  }

  const uint32_t idr0 = (1U << 30) | (1U << 27) | 8;
  const uint32_t idr1 = (2U << 28) | 4;
  const uint32_t idr2 = (1U << 12) | 0x444;
  geometryCase(0, idr0, DiagnosticError::None);
  geometryCase(0, idr0 & ~(1U << 30), DiagnosticError::GeometryUnknown);
  geometryCase(0, idr0 & ~(1U << 27), DiagnosticError::GeometryUnknown);
  geometryCase(0, idr0 & ~255U, DiagnosticError::GeometryUnknown);
  geometryCase(0, (idr0 & ~255U) | 129, DiagnosticError::GeometryUnknown);
  geometryCase(0, (idr0 & ~255U) | 128, DiagnosticError::None);
  geometryCase(1, idr1 | (1U << 31), DiagnosticError::GeometryUnknown);
  geometryCase(1, idr1 & ~255U, DiagnosticError::GeometryUnknown);
  geometryCase(1, (idr1 & ~255U) | 129, DiagnosticError::GeometryUnknown);
  geometryCase(1, (idr1 & ~255U) | 9, DiagnosticError::GeometryUnknown);
  geometryCase(1, idr1 | (5U << 16), DiagnosticError::GeometryUnknown);
  geometryCase(1, idr1 | (4U << 16), DiagnosticError::None);
  geometryCase(1, (7U << 28) | 4, DiagnosticError::GeometryUnknown);
  geometryCase(1, (6U << 28) | 128, DiagnosticError::None);
  geometryCase(2, idr2 & ~(1U << 12), DiagnosticError::GeometryUnknown);
  for (unsigned shift = 0; shift <= 8; shift += 4) {
    geometryCase(2, (idr2 & ~(15U << shift)) | (6U << shift), DiagnosticError::GeometryUnknown);
    geometryCase(2, (idr2 & ~(15U << shift)) | (5U << shift), DiagnosticError::None);
  }
  {
    Model m;
    auto first = observeDiagnostics(request(DiagnosticStage::Power), m.io()); m.clean(first);
    assert(first.error == DiagnosticError::None);
    m.resetTrace(); m.registers[profile.gcc + profile.reset] = 1;
    auto second = observeDiagnostics(request(DiagnosticStage::Host), m.io()); m.clean(second);
    assert(second.error == DiagnosticError::ResetAsserted && second.attempted_reads == 2 && second.observation.hostReads == 0);
    m.resetTrace(); m.registers[profile.gcc + profile.reset] = 0;
    auto third = observeDiagnostics(request(DiagnosticStage::Geometry), m.io()); m.clean(third);
    assert(third.error == DiagnosticError::None && third.attempted_reads == 28);
    ++scenarios;
  }
  assert(!diagnosticAddressAllowed(DiagnosticStage::None, 0x177004));
  assert(diagnosticRange(3).bytes == 0 && diagnosticRange(0xffffffffU).bytes == 0);
  printf("Sm8250UfsDiagnostics: %u staged scope/grant/power/geometry/fresh-snapshot scenarios, "
         "%u individual read failures and %u mapping failures passed; "
         "fixed read-only callbacks, no DMA/writer, complete mapping release\n",
         scenarios, read_failures, map_failures);
}
