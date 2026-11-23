// SPDX-License-Identifier: BSD-2-Clause-Patent
// TB-J716F (SM8250-AC / Kona) EPSS CPU frequency and L3 scaling driver.
// Verified against stock TB-J716F DTB (qcom,cpufreq-hw-epss + qcom,devfreq-l3):
//   Domain 0 (4x Cortex-A55 Silver, CPUs 0..3): 0x18591000 (max 1804.8 MHz)
//   Domain 1 (3x Cortex-A77 Gold,   CPUs 4..6): 0x18592000 (max 2419.2 MHz)
//   Domain 2 (1x Cortex-A77 Prime,  CPU 7):     0x18593000 (max 3187.2 MHz)
//   L3 cache (qcom,devfreq-l3):                 0x18590000 (vote offset 0x320)
// Note: SM8250 DTB carries qcom,skip-enable-check, so a domain is treated as
// ready when either REG_ENABLE bit 0 is set or LUT entry 0 is non-zero.
// Boot-args:
//   cpufreq=1  read-only probe + log
//   cpufreq=3  vote highest non-turbo frequency on all 3 domains + L3
//   cpufreq=4  L3 max + 16 ms dynamic governor with touch/button boost
//   cpufreq-cap=<MHz>  optional upper cap
//   cpufreq-min=<MHz>  optional lower floor
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOService.h>
#include <kern/thread.h>
#include <mach/mach_time.h>
#include <pexpert/pexpert.h>

extern "C" {
extern uint64_t sheng_cpu_idle_mach(int cpu) __attribute__((weak_import));
extern volatile uint32_t j716f_hid_events[8] __attribute__((weak_import));
extern volatile uint32_t armcpu_smp_running __attribute__((weak_import));
extern volatile uint32_t armcpu_cpu_init_completed __attribute__((weak_import));
extern volatile uint32_t armcpu_smp_step[8] __attribute__((weak_import));
extern volatile uint32_t armcpu_smp_fail[8] __attribute__((weak_import));

volatile uint32_t j716f_cpufreq_stage = 0;
volatile uint32_t j716f_cpufreq_mhz[4] = {};  // d0, d1, d2, l3 voted MHz
}

namespace {
constexpr uint64_t kDomainBase[3] = {0x18591000ULL, 0x18592000ULL, 0x18593000ULL};
constexpr unsigned kDomainCpus[3] = {4U, 3U, 1U};
constexpr int kCpuDomain[8] = {0, 0, 0, 0, 1, 1, 1, 2};
constexpr unsigned kCpus = 8U;

constexpr uint64_t kL3Base = 0x18590000ULL;
constexpr uint64_t kWindowSize = 0x1000ULL;
constexpr uint32_t kRegEnable = 0x000U;
constexpr uint32_t kRegDomainState = 0x020U;
constexpr uint32_t kRegDcvsCtrl = 0x0b0U;
constexpr uint32_t kRegFreqLut = 0x100U;
constexpr uint32_t kRegPerfState = 0x320U;
constexpr uint32_t kRegL3Vote = 0x320U;
constexpr unsigned kLutEntries = 40U;
constexpr uint32_t kXoKHz = 19200U;        // 19.2 MHz XO
constexpr uint32_t kGpllHalfKHz = 300000U; // GPLL0 / 2

struct Window {
  IOMemoryMap *map;
  uintptr_t va;
  int voted;
  uint32_t votedKHz;
  uint32_t preKHz;
};

struct Levels {
  unsigned count;
  uint8_t index[kLutEntries];
  uint32_t khz[kLutEntries];
};

Window gDomain[3] = {};
Window gL3 = {};
Levels gLevels[3] = {};
IOLock *gLock = nullptr;
unsigned gMode = 0;
uint32_t gCapKHz = 0;
uint32_t gFloorKHz = 0;
unsigned gBoostMs = 1000U;

inline uint32_t rd32(uintptr_t base, uint32_t reg) {
  return *reinterpret_cast<volatile uint32_t *>(base + reg);
}

inline void wr32(uintptr_t base, uint32_t reg, uint32_t value) {
  *reinterpret_cast<volatile uint32_t *>(base + reg) = value;
  __asm__ volatile("dsb sy" ::: "memory");
}

inline uint32_t lutKHz(uint32_t row) {
  return (row >> 30) ? kXoKHz * (row & 0xffU) : kGpllHalfKHz;
}

inline bool windowReady(uintptr_t va) {
  if (!va) return false;
  return (rd32(va, kRegEnable) & 1U) != 0U || rd32(va, kRegFreqLut) != 0U;
}

bool mapWindow(Window &w, uint64_t base) {
  IODeviceMemory *memory = IODeviceMemory::withRange(base, kWindowSize);
  w.map = memory ? memory->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
  if (memory) memory->release();
  w.va = w.map ? static_cast<uintptr_t>(w.map->getVirtualAddress()) : 0;
  w.voted = -1;
  w.votedKHz = 0;
  w.preKHz = 0;
  return w.va != 0;
}

void buildLevelsForDomain(unsigned d, bool skipSingleCoreTurbo) {
  Levels &l = gLevels[d];
  l.count = 0;
  uint32_t prev = 0;
  for (unsigned i = 0; i < kLutEntries; ++i) {
    const uint32_t row = rd32(gDomain[d].va, kRegFreqLut + i * 4U);
    if (!row) break;
    const uint32_t khz = lutKHz(row);
    if (i > 0 && khz == prev) break;
    prev = khz;
    if (skipSingleCoreTurbo && ((row >> 16) & 7U) == 1U) continue;
    if (gCapKHz && khz > gCapKHz) continue;
    unsigned at = l.count;
    while (at > 0 && l.khz[at - 1] > khz) {
      l.khz[at] = l.khz[at - 1];
      l.index[at] = l.index[at - 1];
      --at;
    }
    l.khz[at] = khz;
    l.index[at] = static_cast<uint8_t>(i);
    ++l.count;
  }
  unsigned first = 0;
  while (first + 1 < l.count && l.khz[first] < gFloorKHz) ++first;
  if (first > 0) {
    for (unsigned i = first; i < l.count; ++i) {
      l.khz[i - first] = l.khz[i];
      l.index[i - first] = l.index[i];
    }
    l.count -= first;
  }
}

void buildLevels(unsigned d) {
  // Multi-core domains skip single-core-only turbo rows if all-core rows exist;
  // single-core Prime (Domain 2) keeps all valid LUT rows up to 3.19 GHz.
  buildLevelsForDomain(d, kDomainCpus[d] > 1U);
  if (gLevels[d].count == 0) {
    buildLevelsForDomain(d, false);
  }
}

void setDomainLevel(unsigned d, unsigned at) {
  Window &w = gDomain[d];
  const uint32_t level = gLevels[d].index[at];
  wr32(w.va, kRegPerfState, level);
  if (rd32(w.va, kRegDcvsCtrl) & 1U) {
    for (unsigned c = 1; c < kDomainCpus[d]; ++c) {
      wr32(w.va, kRegPerfState + c * 4U, level);
    }
  }
  w.voted = static_cast<int>(level);
  w.votedKHz = gLevels[d].khz[at];
  j716f_cpufreq_mhz[d] = w.votedKHz / 1000U;
}

void voteL3Max() {
  if (!windowReady(gL3.va)) return;
  int best = -1;
  uint32_t bestKHz = 0;
  uint32_t prev = 0;
  for (unsigned i = 0; i < kLutEntries; ++i) {
    const uint32_t row = rd32(gL3.va, kRegFreqLut + i * 4U);
    if (!row) break;
    const uint32_t khz = lutKHz(row);
    if (i > 0 && khz == prev) break;
    prev = khz;
    if (gCapKHz && khz > gCapKHz) continue;
    if (khz >= bestKHz) {
      best = static_cast<int>(i);
      bestKHz = khz;
    }
  }
  if (best < 0) return;
  wr32(gL3.va, kRegL3Vote, static_cast<uint32_t>(best));
  gL3.voted = best;
  gL3.votedKHz = bestKHz;
  j716f_cpufreq_mhz[3] = bestKHz / 1000U;
}

void logSummary(const char *tag) {
  uint32_t curMHz[3] = {};
  uint32_t en[3] = {};
  for (unsigned d = 0; d < 3; ++d) {
    if (gDomain[d].va) {
      en[d] = rd32(gDomain[d].va, kRegEnable) & 1U;
      curMHz[d] = (rd32(gDomain[d].va, kRegDomainState) & 0xffU) * kXoKHz / 1000U;
    }
  }
  const uint32_t smpRun = &armcpu_smp_running ? armcpu_smp_running : 0U;
  const uint32_t smpEdge = &armcpu_cpu_init_completed ? armcpu_cpu_init_completed : 0U;
  IOLog("[J716F cpufreq] %s mode=%u smp=%u/edge=%u d0=e%u/%u->%uMHz(r%u) "
        "d1=e%u/%u->%uMHz(r%u) d2=e%u/%u->%uMHz(r%u) l3=%uMHz\n",
        tag, gMode, smpRun, smpEdge,
        en[0], curMHz[0], gDomain[0].votedKHz / 1000U, gLevels[0].count,
        en[1], curMHz[1], gDomain[1].votedKHz / 1000U, gLevels[1].count,
        en[2], curMHz[2], gDomain[2].votedKHz / 1000U, gLevels[2].count,
        gL3.votedKHz / 1000U);
}

void cpufreqThread(void *, wait_result_t) {
  // Wait briefly for secondary CPUs to bring up clusters 1 & 2, then vote.
  IOSleep(500);

  IOLockLock(gLock);
  for (unsigned d = 0; d < 3; ++d) {
    if (windowReady(gDomain[d].va)) {
      buildLevels(d);
      if (gLevels[d].count > 0 && gMode >= 3) {
        gDomain[d].preKHz = (rd32(gDomain[d].va, kRegDomainState) & 0xffU) * kXoKHz;
        setDomainLevel(d, gLevels[d].count - 1U);
      }
    }
  }
  if (gMode >= 3) voteL3Max();
  j716f_cpufreq_stage = 2;
  logSummary("init");
  IOLockUnlock(gLock);

  if (gMode < 4 || !&sheng_cpu_idle_mach) {
    // Re-check once after 3 s in case late secondary cores brought domains online.
    IOSleep(3000);
    IOLockLock(gLock);
    for (unsigned d = 0; d < 3; ++d) {
      if (gDomain[d].voted < 0 && windowReady(gDomain[d].va)) {
        buildLevels(d);
        if (gLevels[d].count > 0 && gMode >= 3) {
          setDomainLevel(d, gLevels[d].count - 1U);
        }
      }
    }
    if (gMode >= 3 && gL3.voted < 0) voteL3Max();
    logSummary("late");
    IOLockUnlock(gLock);
    return;
  }

  // Mode 4: 16 ms dynamic governor with touch/button boost (matches UniXNU-sheng).
  uint64_t lastIdle[kCpus] = {};
  uint64_t lastT = mach_absolute_time();
  for (unsigned c = 0; c < kCpus; ++c) lastIdle[c] = sheng_cpu_idle_mach(static_cast<int>(c));

  unsigned cur[3] = {};
  unsigned down[3] = {};
  unsigned quiet[3] = {};
  bool ready[3] = {};
  for (unsigned d = 0; d < 3; ++d) {
    if (gLevels[d].count > 0 && gDomain[d].voted >= 0) {
      cur[d] = gLevels[d].count - 1U;
      ready[d] = true;
    }
  }

  uint64_t boostAbs = 0;
  uint64_t boostEnd = 0;
  nanoseconds_to_absolutetime(static_cast<uint64_t>(gBoostMs) * 1000000ULL, &boostAbs);
  uint32_t lastTouch = &j716f_hid_events ? j716f_hid_events[6] : 0U;
  uint32_t lastBtn = &j716f_hid_events ? j716f_hid_events[4] : 0U;
  unsigned ticks = 0;

  for (;;) {
    IOSleep(16);
    ++ticks;
    const uint64_t t = mach_absolute_time();
    const uint64_t dt = t - lastT;
    lastT = t;
    if (!dt) continue;

    if (boostAbs && &j716f_hid_events) {
      const uint32_t nowTouch = j716f_hid_events[6];
      const uint32_t nowBtn = j716f_hid_events[4];
      if (nowTouch != lastTouch || nowBtn != lastBtn) {
        lastTouch = nowTouch;
        lastBtn = nowBtn;
        boostEnd = t + boostAbs;
      }
    }
    const bool boost = t < boostEnd;

    uint64_t busy[3] = {};
    for (unsigned c = 0; c < kCpus; ++c) {
      const uint64_t idle = sheng_cpu_idle_mach(static_cast<int>(c));
      const uint64_t di = idle - lastIdle[c];
      lastIdle[c] = idle;
      const uint64_t b = di >= dt ? 0ULL : (dt - di) * 1000ULL / dt;
      const int d = kCpuDomain[c];
      if (b > busy[d]) busy[d] = b;
    }

    IOLockLock(gLock);
    for (unsigned d = 0; d < 3; ++d) {
      Window &w = gDomain[d];
      if (!windowReady(w.va)) continue;
      Levels &l = gLevels[d];
      if (!ready[d]) {
        buildLevels(d);
        if (!l.count) continue;
        w.preKHz = (rd32(w.va, kRegDomainState) & 0xffU) * kXoKHz;
        cur[d] = l.count - 1U;
        setDomainLevel(d, cur[d]);
        ready[d] = true;
      }
      unsigned want = l.count - 1U;
      if (busy[d] < 900ULL) {
        const uint64_t need = static_cast<uint64_t>(l.khz[cur[d]]) * busy[d] * 125ULL / 100000ULL;
        want = 0;
        while (want + 1 < l.count && l.khz[want] < need) ++want;
      }
      if (d > 0) {
        quiet[d] = busy[d] < 100ULL ? quiet[d] + 1U : 0U;
        if (boost) {
          want = l.count - 1U;
        } else if (quiet[d] < 12U) {
          while (want + 1 < l.count && l.khz[want] * 2U < l.khz[l.count - 1U]) ++want;
        }
      }
      if (want > cur[d]) {
        cur[d] = want;
        down[d] = 0;
        setDomainLevel(d, want);
      } else if (want < cur[d]) {
        if (++down[d] >= 4U) {
          cur[d] = want;
          down[d] = 0;
          setDomainLevel(d, want);
        }
      } else {
        down[d] = 0;
      }
    }
    if (gL3.voted < 0) voteL3Max();
    if (ticks == 180U) {  // ~3 s after start: log settled SMP + governor state once
      logSummary("gov3s");
    }
    IOLockUnlock(gLock);
  }
}
}  // namespace

extern "C" void j716f_cpufreq_probe(void) {
  unsigned mode = 0;
  unsigned capMHz = 0;
  unsigned minMHz = 0;
  unsigned boostMs = 1000U;
  PE_parse_boot_argn("cpufreq", &mode, sizeof(mode));
  PE_parse_boot_argn("cpufreq-cap", &capMHz, sizeof(capMHz));
  PE_parse_boot_argn("cpufreq-min", &minMHz, sizeof(minMHz));
  PE_parse_boot_argn("cpufreq-boost-ms", &boostMs, sizeof(boostMs));
  if (!mode || gLock) return;

  gLock = IOLockAlloc();
  if (!gLock) return;
  gMode = mode;
  gCapKHz = capMHz * 1000U;
  gFloorKHz = minMHz * 1000U;
  gBoostMs = boostMs;
  j716f_cpufreq_stage = 1;

  for (unsigned d = 0; d < 3; ++d) mapWindow(gDomain[d], kDomainBase[d]);
  if (mode >= 3) mapWindow(gL3, kL3Base);

  thread_t thread = nullptr;
  if (kernel_thread_start(cpufreqThread, nullptr, &thread) == KERN_SUCCESS) {
    thread_deallocate(thread);
  }
}
