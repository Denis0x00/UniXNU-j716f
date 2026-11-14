// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "../UfsObservation.h"
#include <cassert>
#include <cstdio>
#include <map>
#include <vector>
#ifdef NDEBUG
#error This host test requires assertions enabled.
#endif
using namespace shengufs;

struct Fixture {
  std::map<uint64_t, uint32_t> registers;
  std::vector<uint64_t> reads;
  uint64_t fault = UINT64_MAX;
  Range ranges[3] = {{0x177000, 0x1000}, {0x1d84000, 0x3000}, {0x15000020, 12}};
  ReadGrant grant{ranges, 3, true};
  PlatformIdentity identity{text("qcom,sm8250"), text("qcom,ufshc"),
                            {0x1d84000, 0x3000}, {0x15000000, 0x100000}};
  Fixture()
  {
    // Independent expected-address fixture; unexpected reads abort the test.
    for (unsigned off : {0x77004U, 0x77000U, 0x77010U, 0x77018U, 0x77064U,
                         0x7709cU, 0x77020U, 0x770b8U, 0x7701cU, 0x7705cU, 0x770ccU})
      registers[0x100000 + off] = 1;
    registers[0x177004] = 0x80000000;
    registers[0x177000] = 0;
    for (unsigned off : {0U, 8U, 0x20U, 0x24U, 0x30U, 0x34U, 0x50U, 0x54U,
                         0x58U, 0x60U, 0x70U, 0x74U, 0x78U, 0x80U})
      registers[0x1d84000 + off] = 0;
    registers[0x15000020] = 0x48000080; // S1TS, stream matching, 128 groups
    registers[0x15000024] = 0x60000046; // 4K, 128 register pages, 70 banks
    registers[0x15000028] = 0x5111;
  }
  static bool read(void *ctx, uint64_t address, uint32_t *value)
  {
    auto &f = *static_cast<Fixture *>(ctx);
    f.reads.push_back(address);
    assert(f.registers.count(address) == 1);
    if (f.fault == address) { *value = 0xdeadbeef; return false; }
    *value = f.registers.at(address);
    return true;
  }
  UfsObservation run(Platform platform = Platform::Sm8250)
  { return observeUfs(platform, identity, grant, read, this); }
};

int main()
{
  Fixture f;
  auto s = f.run();
  assert(s.status == ObservationStatus::Captured && f.reads.size() == 28);
  assert(s.gccReads == 11 && s.hostReads == 14 && s.smmuReads == 3);
  assert(s.streamGroups == 128 && s.contextBanks == 70 && s.registerPages == 128);
  assert(s.expectedStreams[0] == 0xe0 && s.expectedStreams[1] == 0x4e0);
  assert(!s.assignmentsObserved); // Expected SIDs are not measured assignment.
  const auto order = f.reads;
  for (Platform p : {Platform::None, static_cast<Platform>(255)}) {
    Fixture bad; assert(bad.run(p).status == ObservationStatus::Unsupported && bad.reads.empty());
  }
  for (Text soc : {text("qcom,sm8550"), text("xqcom,sm8250"), text("qcom,sm8250-extra"),
                   Text{nullptr, 11}, Text{"qcom,sm8250", 0}}) {
    Fixture bad; bad.identity.soc = soc;
    assert(bad.run().status == ObservationStatus::Unsupported && bad.reads.empty());
  }
  for (Text host : {text("qcom,sm8550-ufshc"), text("qcom,ufshc-extra"), Text{nullptr, 0}}) {
    Fixture bad; bad.identity.controller = host;
    assert(bad.run().status == ObservationStatus::Unsupported && bad.reads.empty());
  }
  for (unsigned field = 0; field < 4; ++field) {
    Fixture bad;
    uint64_t *fields[] = {&bad.identity.host.base, &bad.identity.host.bytes,
                          &bad.identity.smmu.base, &bad.identity.smmu.bytes};
    ++*fields[field];
    assert(bad.run().status == ObservationStatus::Unsupported && bad.reads.empty());
  }
  for (unsigned which = 0; which < 6; ++which) {
    Fixture bad;
    if (which == 0) bad.grant.accessEstablished = false;
    if (which == 1) bad.grant.ranges = nullptr;
    if (which == 2) bad.grant.count = 0;
    if (which == 3) bad.grant.count = 17;
    if (which == 4) bad.ranges[2] = {UINT64_MAX - 3, 8};
    if (which == 5) bad.ranges[2].base++;
    assert(bad.run().status == ObservationStatus::InvalidGrant && bad.reads.empty());
  }
  Fixture noReader;
  assert(observeUfs(Platform::Sm8250, noReader.identity, noReader.grant, nullptr, nullptr).status ==
         ObservationStatus::InvalidGrant && noReader.reads.empty());
  for (unsigned i = 0; i < 3; ++i) {
    Fixture bad; bad.ranges[i].bytes = 4;
    assert(bad.run().status == ObservationStatus::Unreadable && bad.reads.empty());
  }
  assert(contains32({UINT64_MAX - 3, 4}, UINT64_MAX - 3));
  assert(!contains32({UINT64_MAX - 3, 8}, UINT64_MAX - 3));
  assert(!contains32({0, 4}, 1) && !contains32({0, 3}, 0));
  assert(!contains32({0x1000, 4}, 0xffc) && !contains32({0x1000, 4}, 0x1004));
  assert(!contains32({0, 0}, 0));

  // Failure at each actual register stops exactly there, including a callback
  // that corrupts its output before reporting failure. No retry/scan follows.
  for (size_t i = 0; i < order.size(); ++i) {
    Fixture bad; bad.fault = order[i];
    auto r = bad.run();
    assert(r.status == ObservationStatus::Unreadable && r.failedAddress == order[i]);
    assert(bad.reads.size() == i + 1 && r.gccReads + r.hostReads + r.smmuReads == i);
  }
  for (unsigned kind = 0; kind < 2; ++kind) {
    Fixture bad; bad.registers[kind ? 0x177000 : 0x177004] = kind ? 1 : 0;
    auto r = bad.run();
    assert(r.status == (kind ? ObservationStatus::ResetAsserted : ObservationStatus::PowerNotReady));
    assert(bad.reads.size() == 2 && !r.hostReads && !r.smmuReads);
  }
  for (uint64_t clock : {0x177010ULL, 0x177018ULL}) {
    for (uint32_t value : {0U, 2U, 0x80000001U, 0x80000002U, 0x80000000U}) {
      Fixture bad; bad.registers[clock] = value;
      auto r = bad.run();
      assert(r.status == ObservationStatus::ClockNotReady && bad.reads.size() == 11);
    }
    for (uint32_t value : {1U, 3U, 0x80000003U}) {
      Fixture good; good.registers[clock] = value;
      assert(good.run().status == ObservationStatus::Captured);
    }
  }
  assert(!branchState(0x80000003, false).readable); // bit1 is not universal HWCG.
  Fixture absentLink;
  absentLink.registers[0x177020] = 0; // Observation is not link-ready certification.
  assert(absentLink.run().status == ObservationStatus::Captured);
  for (unsigned which = 0; which < 14; ++which) {
    Fixture bad;
    if (which == 0) bad.registers[0x15000020] &= ~(1U << 30);
    if (which == 1) bad.registers[0x15000020] &= ~(1U << 27);
    if (which == 2) bad.registers[0x15000028] &= ~(1U << 12);
    if (which == 3) bad.registers[0x15000024] |= 1U << 31;
    if (which == 4) bad.registers[0x15000020] &= ~255U;
    if (which == 5) bad.registers[0x15000020] |= 255;
    if (which == 6) bad.registers[0x15000024] &= ~255U;
    if (which == 7) bad.registers[0x15000024] |= 255;
    if (which == 8) bad.registers[0x15000024] |= 1U << 28;
    if (which == 9) bad.registers[0x15000024] |= 71U << 16;
    if (which == 10) bad.registers[0x15000024] &= ~(7U << 28);
    if (which >= 11) bad.registers[0x15000028] |= 6U << (4 * (which - 11));
    auto r = bad.run();
    assert(r.status == ObservationStatus::GeometryUnknown && bad.reads.size() == 28);
    assert(!r.streamGroups && !r.assignmentsObserved);
  }
  // Broad grants and malicious firmware list bases cannot extend the allowlist.
  Fixture pointers;
  Range broad{0, UINT64_MAX - 3};
  pointers.grant = {&broad, 1, true};
  for (unsigned off : {0x50U, 0x54U, 0x70U, 0x74U}) pointers.registers[0x1d84000 + off] = 0xffffffff;
  auto r = pointers.run();
  assert(r.status == ObservationStatus::Captured && pointers.reads == order);
  assert(r.host[TransferBaseLow] == 0xffffffff && r.host[TaskBaseHigh] == 0xffffffff);
  // No SM8550 GCC+0x9e000, PHY, ICE, TCU, stream/CB or firmware-pointer access.
  for (uint64_t address : order) assert(address != 0x19e000);
  puts("PASS: exact identity, bounded grants, 28 fault sites, power/reset/clock gates, fixed SMMU IDs, no pointer traversal");
}
