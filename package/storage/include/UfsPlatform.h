// SPDX-License-Identifier: BSD-2-Clause-Patent
// Pure SoC facts for the new observer. Does not select a runtime driver.
#ifndef SHENG_UFS_PLATFORM_H
#define SHENG_UFS_PLATFORM_H
#include <stdint.h>
#include <stddef.h>

namespace shengufs {
struct Text { const char *data; size_t size; };
template <size_t N> constexpr Text text(const char (&s)[N]) { return {s, N - 1}; }
inline bool equal(Text a, Text b)
{
  if (!a.data || !b.data || a.size != b.size) return false;
  for (size_t i = 0; i < a.size; ++i) if (a.data[i] != b.data[i]) return false;
  return true;
}
struct Range { uint64_t base, bytes; };
inline bool validRange(Range r)
{
  return r.bytes && r.base <= UINT64_MAX - (r.bytes - 1);
}
inline bool contains32(Range r, uint64_t address)
{
  return validRange(r) && !(address & 3) && r.bytes >= 4 &&
         address >= r.base && address - r.base <= r.bytes - 4;
}
enum class Platform { None, Sm8250 };
enum ClockIndex { Axi, Ahb, Ice, Aux, Rx0, Rx1, Tx0, Unipro, AggreAxi, ClockCount };
struct ClockRegister { uint32_t offset; bool hardwareGating; };
struct UfsPlatformProfile {
  Platform platform;
  Text soc, controller;
  Range host, smmu;
  uint64_t gcc;
  uint32_t gdsc, reset;
  ClockRegister clocks[ClockCount];
  uint16_t expectedStreams[2]; // Source expectation, NOT observed assignment/ownership.
  uint32_t interruptId;
};
inline const UfsPlatformProfile *platformProfile(Platform platform)
{
  // Linux v6.12 gcc-sm8250.c and sm8250.dtsi; common HCI/SMMU ranges also
  // match retained TB-J716F stock DTB0/DTBO1. PHY/ICE/TCU are not observed.
  // Board identity, power/read grants and storage policy belong to the caller.
  static const UfsPlatformProfile sm8250 = {
    Platform::Sm8250, text("qcom,sm8250"), text("qcom,ufshc"),
    {0x01d84000, 0x3000}, {0x15000000, 0x100000},
    0x00100000, 0x77004, 0x77000,
    {{0x77010, true}, {0x77018, true}, {0x77064, true}, {0x7709c, true},
     {0x77020, false}, {0x770b8, false}, {0x7701c, false},
     {0x7705c, true}, {0x770cc, true}},
    {0xe0, 0x4e0}, 297
  };
  return platform == Platform::Sm8250 ? &sm8250 : nullptr;
}
struct PlatformIdentity {
  Text soc, controller;
  Range host, smmu;
};
inline bool matches(const UfsPlatformProfile &p, const PlatformIdentity &id)
{
  return equal(p.soc, id.soc) && equal(p.controller, id.controller) &&
    p.host.base == id.host.base && p.host.bytes == id.host.bytes &&
    p.smmu.base == id.smmu.base && p.smmu.bytes == id.smmu.bytes;
}
struct BranchState { bool enabled, hardwareGated, off, readable; };
inline BranchState branchState(uint32_t value, bool supportsHardwareGating)
{
  BranchState s{bool(value & 1), supportsHardwareGating && bool(value & 2),
                bool(value & 0x80000000U), false};
  // CLK_OFF alone does not reject an enabled branch with supported HW gating.
  s.readable = s.enabled && (!s.off || s.hardwareGated);
  return s;
}
} // namespace shengufs
#endif
