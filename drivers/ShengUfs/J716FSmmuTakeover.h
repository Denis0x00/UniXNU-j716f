// SPDX-License-Identifier: BSD-2-Clause-Patent
// SM8250 APPS SMMU dual-stream bypass takeover for J716F UFS controller.
// Only the last, unused/disabled context bank is used for bypass.
#pragma once
#include <stdint.h>

struct J716fSmmuPlan {
  unsigned bank = 0, pages = 0, count = 0;
  unsigned slot[2] = {~0U, ~0U};
  bool reuse[2] = {};
  unsigned error = 0;
};
static constexpr uint16_t kJ716fUfsStreams[2] = {0x0e0, 0x4e0};

template<class Read>
static J716fSmmuPlan planJ716fSmmu(uint64_t dma, uint64_t bytes, Read read)
{
  J716fSmmuPlan p;
  uint32_t id0 = read(0x20), id1 = read(0x24), id2 = read(0x28);
  p.count = id0 & 255;
  p.pages = 1U << (((id1 >> 28) & 7) + 1);
  unsigned banks = id1 & 255;
  if (!(id0 & (1U << 30)) || !(id0 & (1U << 27)) || !(id2 & (1U << 12)) ||
      !p.count || p.count > 128 || (id1 >> 31) || p.pages > 128 ||
      banks < 2 || banks > 128 || (p.pages + banks) * 4096 > 0xd0000) {
    p.error = 1; return p;
  }
  const unsigned bits[] = {32, 36, 40, 42, 44, 48};
  for (unsigned shift = 0; shift <= 8; shift += 4) {
    unsigned code = (id2 >> shift) & 15;
    if (code >= 6 || !bytes || dma >= (1ULL << bits[code]) ||
        bytes > (1ULL << bits[code]) - dma) { p.error = 2; return p; }
  }
  p.bank = banks - 1;
  unsigned freeSlots[128], freeCount = 0;
  for (unsigned i = 0; i < p.count; ++i) {
    uint32_t smr = read(0x800 + 4 * i);
    if (!(smr & 0x80000000U)) { freeSlots[freeCount++] = i; continue; }
    uint32_t s2cr = read(0xc00 + 4 * i), mask = (smr >> 16) & 0x7fff;
    bool ownBank = ((s2cr >> 16) & 3) == 0 && (s2cr & 255) == p.bank;
    bool owned = false;
    for (unsigned s = 0; s < 2; ++s) {
      if (((smr ^ kJ716fUfsStreams[s]) & 0xffff & ~mask) != 0) continue;
      if (p.reuse[s] || mask || !ownBank) { p.error = 3; return p; }
      p.reuse[s] = true; p.slot[s] = i; owned = true;
    }
    if (ownBank && !owned) { p.error = 4; return p; }
  }
  uint32_t sctlr = read((p.pages + p.bank) * 4096);
  if ((sctlr & 1) ||
      ((p.reuse[0] || p.reuse[1]) &&
       (sctlr || read(0x1000 + 4 * p.bank) != 0x10000 ||
        (read(0x1800 + 4 * p.bank) & ~1U)))) { p.error = 5; return p; }
  for (unsigned s = 0; s < 2; ++s) if (!p.reuse[s]) {
    if (!freeCount) { p.error = 6; return p; }
    p.slot[s] = freeSlots[--freeCount];
  }
  return p;
}

template<class Read, class Write>
static unsigned configureJ716fSmmu(const J716fSmmuPlan &p, Read read, Write write)
{
  if (p.error) return p.error;
  unsigned cb = (p.pages + p.bank) * 4096;
  for (unsigned s = 0; s < 2; ++s) {
    uint32_t smr = read(0x800 + 4 * p.slot[s]);
    if (p.reuse[s] ? (smr != (0x80000000U | kJ716fUfsStreams[s]) ||
         read(0xc00 + 4 * p.slot[s]) != p.bank) : (smr & 0x80000000U)) return 7;
  }
  if (read(cb) & 1) return 7;
  if (!p.reuse[0] && !p.reuse[1]) write(cb, 0);
  if (read(cb) != 0) return 8;
  if (read(0x1800 + 4 * p.bank) != 1) write(0x1800 + 4 * p.bank, 1);
  if (!p.reuse[0] && !p.reuse[1]) write(0x1000 + 4 * p.bank, 0x10000);
  if (read(0x1800 + 4 * p.bank) != 1 || read(0x1000 + 4 * p.bank) != 0x10000 ||
     read(cb) != 0) return 9;
  for (unsigned s = 0; s < 2; ++s) if (!p.reuse[s]) {
    write(0xc00 + 4 * p.slot[s], p.bank);
    if (read(0xc00 + 4 * p.slot[s]) != p.bank) return 10;
  }
  for (unsigned s = 0; s < 2; ++s) if (!p.reuse[s]) {
    write(0x800 + 4 * p.slot[s], 0x80000000U | kJ716fUfsStreams[s]);
    if (read(0x800 + 4 * p.slot[s]) != (0x80000000U | kJ716fUfsStreams[s]) ||
       read(0xc00 + 4 * p.slot[s]) != p.bank) return 11;
  }
  return 0;
}
