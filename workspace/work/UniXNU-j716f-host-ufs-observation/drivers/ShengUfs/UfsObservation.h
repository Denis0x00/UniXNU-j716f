// SPDX-License-Identifier: BSD-2-Clause-Patent
// Bounded, zero-write fixed-register observation; no IOKit, DMA or UIC dependency.
#ifndef SHENG_UFS_OBSERVATION_H
#define SHENG_UFS_OBSERVATION_H
#include "UfsPlatform.h"

namespace shengufs {
enum class ObservationStatus {
  Unsupported, InvalidGrant, Unreadable, PowerNotReady, ResetAsserted,
  ClockNotReady, GeometryUnknown, Captured
};
struct ReadGrant {
  const Range *ranges;
  size_t count;
  // Supplied by a future platform adapter only after access is established.
  // These are not inferred from the presence of a DT register window.
  bool accessEstablished;
};
using Read32 = bool (*)(void *context, uint64_t physical, uint32_t *value);
enum HostIndex {
  Cap, Version, InterruptStatus, InterruptEnable, HostStatus, HostEnable,
  TransferBaseLow, TransferBaseHigh, TransferDoorbell, TransferRunStop,
  TaskBaseLow, TaskBaseHigh, TaskDoorbell, TaskRunStop, HostCount
};
struct UfsObservation {
  ObservationStatus status = ObservationStatus::Unsupported;
  uint32_t gdsc = 0, reset = 0, clocks[ClockCount] = {}, host[HostCount] = {}, idr[3] = {};
  unsigned gccReads = 0, hostReads = 0, smmuReads = 0;
  uint64_t failedAddress = 0;
  uint16_t expectedStreams[2] = {}; // SoC expectation only; no stream table was read.
  bool assignmentsObserved = false;
  // Geometry is only a bounded format observation, NOT permission to scan/use it.
  unsigned streamGroups = 0, contextBanks = 0, stage2OnlyBanks = 0, registerPages = 0;
};
inline bool validGrant(const ReadGrant &g)
{
  if (!g.accessEstablished || !g.ranges || !g.count || g.count > 16) return false;
  for (size_t i = 0; i < g.count; ++i) {
    if (!validRange(g.ranges[i]) || (g.ranges[i].base & 3) || (g.ranges[i].bytes & 3))
      return false;
  }
  return true;
}
inline bool permitted(const ReadGrant &g, uint64_t address)
{
  for (size_t i = 0; i < g.count; ++i) if (contains32(g.ranges[i], address)) return true;
  return false;
}
inline UfsObservation observeUfs(Platform platform, const PlatformIdentity &identity,
                                 const ReadGrant &grant, Read32 read, void *context)
{
  UfsObservation result;
  const auto *p = platformProfile(platform);
  if (!p || !matches(*p, identity)) return result; // Zero reads on wrong identity.
  result.expectedStreams[0] = p->expectedStreams[0];
  result.expectedStreams[1] = p->expectedStreams[1];
  if (!read || !validGrant(grant)) { result.status = ObservationStatus::InvalidGrant; return result; }
  const uint32_t hostOffsets[HostCount] = {
    0x00, 0x08, 0x20, 0x24, 0x30, 0x34, 0x50, 0x54, 0x58, 0x60,
    0x70, 0x74, 0x78, 0x80
  };
  // Check every fixed address before the first callback. A large grant cannot
  // widen this allowlist. The hardware pointers read below remain plain values.
  uint64_t addresses[2 + ClockCount + HostCount + 3] = {};
  unsigned n = 0;
  addresses[n++] = p->gcc + p->gdsc;
  addresses[n++] = p->gcc + p->reset;
  for (unsigned i = 0; i < ClockCount; ++i) addresses[n++] = p->gcc + p->clocks[i].offset;
  for (unsigned i = 0; i < HostCount; ++i) addresses[n++] = p->host.base + hostOffsets[i];
  for (unsigned i = 0; i < 3; ++i) addresses[n++] = p->smmu.base + 0x20 + 4 * i;
  for (unsigned i = 0; i < n; ++i) {
    if (!permitted(grant, addresses[i])) {
      result.status = ObservationStatus::Unreadable;
      result.failedAddress = addresses[i];
      return result;
    }
  }
  auto fetch = [&](uint64_t address, uint32_t &value, unsigned &count) {
    uint32_t observed = 0;
    if (!read(context, address, &observed)) {
      result.status = ObservationStatus::Unreadable;
      result.failedAddress = address;
      return false;
    }
    value = observed;
    ++count;
    return true;
  };
  unsigned at = 0;
  if (!fetch(addresses[at++], result.gdsc, result.gccReads) ||
      !fetch(addresses[at++], result.reset, result.gccReads)) return result;
  if (!(result.gdsc & 0x80000000U)) { result.status = ObservationStatus::PowerNotReady; return result; }
  if (result.reset & 1) { result.status = ObservationStatus::ResetAsserted; return result; }
  for (unsigned i = 0; i < ClockCount; ++i)
    if (!fetch(addresses[at++], result.clocks[i], result.gccReads)) return result;
  // Only bus clocks gate HCI reads. Other branches are recorded without
  // claiming that a readable HCI proves PHY/link readiness.
  if (!branchState(result.clocks[Axi], p->clocks[Axi].hardwareGating).readable ||
      !branchState(result.clocks[Ahb], p->clocks[Ahb].hardwareGating).readable) {
    result.status = ObservationStatus::ClockNotReady;
    return result;
  }
  for (unsigned i = 0; i < HostCount; ++i)
    if (!fetch(addresses[at++], result.host[i], result.hostReads)) return result;
  for (unsigned i = 0; i < 3; ++i)
    if (!fetch(addresses[at++], result.idr[i], result.smmuReads)) return result;
  // Narrow SMMUv2 format subset. No clamping, probes, stream/CB reads or
  // inference of ownership. Complete geometry/width checks belong to planning.
  unsigned groups = result.idr[0] & 255, banks = result.idr[1] & 255;
  unsigned stage2 = (result.idr[1] >> 16) & 255;
  unsigned pages = 1U << (((result.idr[1] >> 28) & 7) + 1);
  bool widthsKnown = true;
  for (unsigned shift = 0; shift <= 8; shift += 4)
    if (((result.idr[2] >> shift) & 15) > 5) widthsKnown = false;
  if (!(result.idr[0] & (1U << 30)) || !(result.idr[0] & (1U << 27)) ||
      !(result.idr[2] & (1U << 12)) || (result.idr[1] >> 31) ||
      !groups || groups > 128 || !banks || banks > 128 || pages > 128 ||
      banks > pages || stage2 > banks || !widthsKnown ||
      (uint64_t(pages) + banks) * 4096 > p->smmu.bytes) {
    result.status = ObservationStatus::GeometryUnknown;
    return result;
  }
  result.streamGroups = groups;
  result.contextBanks = banks;
  result.stage2OnlyBanks = stage2;
  result.registerPages = pages;
  result.status = ObservationStatus::Captured;
  return result; // Captured does NOT mean host/link/DMA/storage-ready.
}
} // namespace shengufs
#endif
