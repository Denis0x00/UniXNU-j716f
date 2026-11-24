// SPDX-License-Identifier: BSD-2-Clause-Patent
// TB-J716F (SM8250-AC, Adreno 650 v2.0) Phase 1 staged GPU power/clock/SMMU/GMU probe.
// Strictly opt-in via boot-arg j716f-gpu=<mode> (default 0 = dormant):
//   1  read-only probe of GCC GPU branches, GPUCC, KGSL SMMU, and RBBM_STATUS (gated by prerequisites)
//   2  mode 1 + enable gcc_gpu_cfg_ahb_clk if clear
//   3  mode 2 + power gpu_cx_gdsc and enable KGSL SMMU vote clocks (never touches GX or SMMU tables)
//   5  mode 3 + read AOP cmd-db ARC/BCM resources (gfx.lvl, mx.lvl, cx.lvl, SH0..CN0) + GMU/RSCC state if clocked
//   6  mode 5 + bring up GMU 200 MHz clocks first (GMU itself remains in reset; no GMU/GPU register writes)

#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <kern/thread.h>
#include <pexpert/pexpert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

extern "C" {
#include "J716fGpuPowerBoard.h"

enum {
    kRowSummary = 0,
    kRowGcc,
    kRowGpucc,
    kRowSmmu,
    kRowClk,
    kRowClk2,
    kRowGmu,
    kRowGmu2,
    kRowDbArc,
    kRowDbBcm,
    kRowLvlGfx,
    kRowLvlMx,
    kRowLvlCx,
    kGpuRows,
};

char j716f_gpu_rows[kGpuRows][120] = {};
volatile uint32_t j716f_gpu_step = 0;
struct gpb_result j716f_gpu_result = {};
struct gpb_gmu_clock_on j716f_gpu_clocks = {};
struct gpb_gmu_state j716f_gpu_gmu_state = {};
}

namespace {

unsigned gMode = 0;
constexpr uint64_t kPhys[GPB_WINDOWS] = {
    GPB_GCC_GPU_PAGE,
    GPB_GCC_VOTE_PAGE,
    GPB_GPUCC_BASE,
    GPB_SMMU_BASE,
    GPB_GPU_BASE,
    GPB_GMU_BASE,
    GPB_RSCC_BASE,
};
constexpr uint64_t kLen[GPB_WINDOWS] = {
    0x1000ULL,
    0x1000ULL,
    GPB_GPUCC_SIZE,
    GPB_SMMU_GR0_SIZE,
    GPB_GPU_WINDOW,
    GPB_GMU_SIZE,
    GPB_RSCC_SIZE,
};
IOMemoryMap *gMaps[GPB_WINDOWS] = {};
uintptr_t gVa[GPB_WINDOWS] = {};
alignas(8) uint8_t gCmdDb[GPB_CMD_DB_BYTES] = {};

bool mapWindow(unsigned w) {
    IODeviceMemory *memory = IODeviceMemory::withRange(kPhys[w], kLen[w]);
    if (!memory) return false;
    gMaps[w] = memory->map(kIOMapAnywhere | kIOMapInhibitCache);
    memory->release();
    gVa[w] = gMaps[w] ? static_cast<uintptr_t>(gMaps[w]->getVirtualAddress()) : 0;
    return gVa[w] != 0;
}

void delayUs(void *, unsigned us) { IODelay(us); }

int rd(void *, unsigned w, uint32_t off, uint32_t *out) {
    if (w >= GPB_WINDOWS || !gVa[w] || off >= kLen[w] || (off & 3u)) return -1;
    *out = *reinterpret_cast<volatile uint32_t *>(gVa[w] + off);
    return 0;
}

bool writable(unsigned w, uint32_t off) {
    if (w == GPB_WIN_GCC_GPU && off == GPB_GCC_GPU_CFG_AHB) return gMode >= 2u;
    if (w == GPB_WIN_GPUCC &&
        (off == GPB_CC_CX_GDSCR || off == GPB_CC_AHB_CLK || off == GPB_CC_SMMU_VOTE_CLK)) {
        return gMode >= 3u;
    }
    if (gMode < 6u) return false;
    if (w == GPB_WIN_GCC_VOTE) return off == GPB_GCC_PLL_VOTE;
    if (w == GPB_WIN_GCC_GPU) return off == GPB_GCC_DDRSS_GPU_AXI || off == GPB_GCC_GPU_MEMNOC_GFX;
    if (w == GPB_WIN_GPUCC) {
        return off == GPB_CC_GMU_RCGR || off == GPB_CC_GMU_RCGR + 4u ||
               off == GPB_CC_CX_GMU_CLK || off == GPB_CC_CXO_CLK;
    }
    return false;
}

int wr(void *, unsigned w, uint32_t off, uint32_t value) {
    if (w >= GPB_WINDOWS || !gVa[w] || off >= kLen[w] || (off & 3u) || !writable(w, off)) return -1;
    *reinterpret_cast<volatile uint32_t *>(gVa[w] + off) = value;
    __asm__ volatile("dsb sy" ::: "memory");
    return 0;
}

void probeCmdDb() {
    j716f_gpu_step = 50u;
    IODeviceMemory *memory = IODeviceMemory::withRange(GPB_CMD_DB_BASE, GPB_CMD_DB_BYTES);
    IOMemoryMap *map = memory ? memory->map(kIOMapAnywhere | kIOMapInhibitCache | kIOMapReadOnly) : nullptr;
    if (memory) memory->release();
    if (!map) {
        snprintf(j716f_gpu_rows[kRowDbArc], sizeof(j716f_gpu_rows[0]), "CMDDB nomap");
        return;
    }
    const volatile uint32_t *src = reinterpret_cast<const volatile uint32_t *>(map->getVirtualAddress());
    for (uint32_t i = 0; i < GPB_CMD_DB_BYTES / 4u; ++i) {
        const uint32_t v = src[i];
        memcpy(gCmdDb + 4u * i, &v, 4u);
    }
    map->release();

    static const char *const kNames[8] = {"gfx.lvl", "mx.lvl", "cx.lvl", "gmxc.lvl", "SH0", "MC0", "ACV", "CN0"};
    gpb_cmd_db_entry found[8] = {};
    int rc[8] = {};
    for (unsigned i = 0; i < 8u; ++i) {
        rc[i] = gpb_cmd_db_find(gCmdDb, sizeof(gCmdDb), kNames[i], &found[i]);
    }
    for (unsigned half = 0; half < 2u; ++half) {
        char *row = j716f_gpu_rows[kRowDbArc + half];
        const size_t cap = sizeof(j716f_gpu_rows[0]);
        int n = half ? snprintf(row, cap, "CMDDB bcm")
                     : snprintf(row, cap, "CMDDB v%08x magic %08x", gpb_le32(gCmdDb), gpb_le32(gCmdDb + 4));
        for (unsigned i = 4u * half; i < 4u * half + 4u && n > 0 && static_cast<size_t>(n) < cap; ++i) {
            if (rc[i]) {
                n += snprintf(row + n, cap - static_cast<size_t>(n), " %s:%d", kNames[i], rc[i]);
            } else {
                n += snprintf(row + n, cap - static_cast<size_t>(n), " %s:%x/%u",
                              kNames[i], found[i].address, found[i].aux_bytes);
            }
        }
    }
    for (unsigned i = 0; i < 3u; ++i) {
        char *row = j716f_gpu_rows[kRowLvlGfx + i];
        const size_t cap = sizeof(j716f_gpu_rows[0]);
        int n = snprintf(row, cap, "LVL %s", kNames[i]);
        for (unsigned k = 0; !rc[i] && k + 1u < found[i].aux_bytes && k < 32u && n > 0 && static_cast<size_t>(n) < cap; k += 2u) {
            n += snprintf(row + n, cap - static_cast<size_t>(n), " %u", gpb_le16(found[i].aux + k));
        }
    }
}

void probeThread(void *, wait_result_t) {
    // Wait briefly so Display, USB Debug, and multi-core log ring are online first.
    IOSleep(4000);
    j716f_gpu_step = 10u;

    unsigned have = 0u;
    const unsigned maxWin = gMode >= 5u ? GPB_WINDOWS : GPB_WIN_GMU;
    for (unsigned w = 0; w < maxWin; ++w) {
        if (mapWindow(w)) have |= (1u << w);
    }
    const gpb_io io = {nullptr, rd, wr, delayUs};

    j716f_gpu_step = 20u;
    gpb_run(&io, gMode, have, &j716f_gpu_result);
    const gpb_result &r = j716f_gpu_result;
    snprintf(j716f_gpu_rows[kRowSummary], sizeof(j716f_gpu_rows[0]),
             "GPU m%u gcc=%s cc=%s smmu=%s gpu=%s io=%d cx=%u clk=%u ahb=%u",
             r.mode, gpb_state_text(r.gcc), gpb_state_text(r.cc), gpb_state_text(r.smmu),
             gpb_state_text(r.gpu), r.io_error, r.powered_cx, r.powered_clocks, r.enabled_ahb);
    snprintf(j716f_gpu_rows[kRowGcc], sizeof(j716f_gpu_rows[0]),
             "GCC cfg_ahb=%08x>%08x memnoc=%08x dvm=%08x misc=%08x gpll0=%08x",
             r.cfg_ahb, r.cfg_ahb_after, r.memnoc, r.dvm, r.misc, r.pll_vote);
    snprintf(j716f_gpu_rows[kRowGpucc], sizeof(j716f_gpu_rows[0]),
             "GPUCC cx=%08x>%08x hw=%08x gx=%08x ahb=%08x smmu=%08x>%08x gmu=%08x",
             r.cx_before, r.cx_gdscr, r.cx_hwctrl, r.gx_gdscr, r.cc_ahb, r.cc_smmu_before, r.cc_smmu, r.cc_gmu);
    snprintf(j716f_gpu_rows[kRowSmmu], sizeof(j716f_gpu_rows[0]),
             "SMMU cr0=%08x id=%08x/%08x/%08x gfsr=%08x smr=%u first=%d(%08x/%08x) rbbm=%08x",
             r.scr0, r.id0, r.id1, r.id2, r.gfsr, r.smr_valid,
             r.smr_first == 0xFFFFFFFFu ? -1 : static_cast<int>(r.smr_first),
             r.smr_first_val, r.s2cr_first_val, r.rbbm_status);

    if (gMode >= 5u) {
        probeCmdDb();
        uint32_t cxGmu = r.cc_gmu;
        uint32_t ccAhb = r.cc_ahb;
        if (gMode >= 6u) {
            j716f_gpu_step = 55u;
            gpb_gmu_clocks_on(&io, &j716f_gpu_result, &j716f_gpu_clocks);
            const gpb_gmu_clock_on &o = j716f_gpu_clocks;
            snprintf(j716f_gpu_rows[kRowClk], sizeof(j716f_gpu_rows[0]),
                     "GMUCLK step=%u fail=%d rcg=%u br=%u vote=%08x cmd=%08x cfg=%08x",
                     o.step, o.failed, o.rcg_polls, o.branch_polls, o.vote, o.gmu_cmd, o.gmu_cfg);
            snprintf(j716f_gpu_rows[kRowClk2], sizeof(j716f_gpu_rows[0]),
                     "GMUCLK cx_gmu=%08x ahb=%08x cxo=%08x axi=%08x memnoc=%08x pll1=%08x",
                     o.cx_gmu, o.ahb, o.cxo, o.axi, o.memnoc, o.pll1_mode);
            cxGmu = o.cx_gmu;
            ccAhb = o.ahb;
        }
        if ((have & (1u << GPB_WIN_GMU)) && (have & (1u << GPB_WIN_RSCC)) &&
            gpb_gmu_regs_readable(&j716f_gpu_result, cxGmu, ccAhb)) {
            j716f_gpu_step = 60u;
            gpb_read_gmu_state(&io, &j716f_gpu_result, &j716f_gpu_gmu_state);
            const gpb_gmu_state &s = j716f_gpu_gmu_state;
            snprintf(j716f_gpu_rows[kRowGmu], sizeof(j716f_gpu_rows[0]),
                     "GMU sysrst=%08x fw=%08x gx=%08x hfi=%08x gen7=%08x",
                     s.sysreset, s.fw_init, s.gx_status, s.hfi_status, s.general7);
            snprintf(j716f_gpu_rows[kRowGmu2], sizeof(j716f_gpu_rows[0]),
                     "GMU ao_irq=%08x fence=%08x rscc_busy=%08x rscc_sts=%08x",
                     s.ao_irq, s.fence, s.rscc_busy, s.rscc_status);
        } else {
            snprintf(j716f_gpu_rows[kRowGmu], sizeof(j716f_gpu_rows[0]),
                     "GMU not readable (cx_gmu=%08x ahb=%08x)", cxGmu, ccAhb);
        }
    }

    j716f_gpu_step = 100u;
    for (unsigned i = 0; i < kGpuRows; ++i) {
        if (j716f_gpu_rows[i][0]) {
            IOLog("[J716F gpu] %s\n", j716f_gpu_rows[i]);
        }
    }
}

} // namespace

extern "C" void j716f_gpu_snapshot(char *out, size_t cap) {
    if (!out || cap == 0) return;
    int n = snprintf(out, cap, "[usb] GPU: mode=%u step=%u\r\n", gMode, j716f_gpu_step);
    for (unsigned i = 0; i < kGpuRows && n > 0 && static_cast<size_t>(n) < cap; ++i) {
        if (!j716f_gpu_rows[i][0]) continue;
        n += snprintf(out + n, cap - static_cast<size_t>(n), "[usb] GPU[%u]: %s\r\n", i, j716f_gpu_rows[i]);
    }
}

extern "C" void j716f_gpu_probe(void) {
    static int started = 0;
    unsigned mode = 0;
    if (!PE_parse_boot_argn("j716f-gpu", &mode, sizeof(mode)) || !mode || mode > 6u) {
        return;
    }
    if (__atomic_exchange_n(&started, 1, __ATOMIC_ACQ_REL)) return;
    gMode = mode;
    j716f_gpu_step = 1u;
    thread_t thread = nullptr;
    if (kernel_thread_start(probeThread, nullptr, &thread) == KERN_SUCCESS && thread) {
        thread_deallocate(thread);
    }
}
