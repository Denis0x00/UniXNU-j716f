// SPDX-License-Identifier: BSD-2-Clause-Patent
// TB-J716F (SM8250-AC, Adreno 650 v2.0) staged GPU power/clock/SMMU/GMU/GX probe.
// Strictly opt-in via boot-arg j716f-gpu=<mode> (default 0 = dormant):
//   1  read-only probe of GCC GPU branches, GPUCC, KGSL SMMU, and RBBM_STATUS (gated by prerequisites)
//   2  mode 1 + enable gcc_gpu_cfg_ahb_clk if clear
//   3  mode 2 + power gpu_cx_gdsc and enable KGSL SMMU vote clocks (never touches GX or SMMU tables)
//   5  mode 3 + read AOP cmd-db ARC/BCM resources (gfx.lvl, mx.lvl, cx.lvl, SH0..CN0) + GMU/RSCC state if clocked
//   6  mode 5 + bring up GMU 200 MHz clocks first (GMU itself remains in reset; no GMU/GPU register writes)
//   7  mode 6 + KGSL SMMU ArmSmmu4K GMU context (bank 1 / ASID 1 / slots 2,3 -> SIDs 0x0005,0x0405) + A650GmuColdStart
//   8  mode 7 + A650GmuPowerGx (j716f-gpu-perf=<1..7>, default 7 = 670 MHz) + read-only GX/RBBM/GBIF verification

#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <kern/clock.h>
#include <kern/task.h>
#include <kern/thread.h>
#include <pexpert/pexpert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

extern "C" {
#include "ArmSmmu4KMmio.h"
#include "J716fA650Gmu.h"
#include "J716fA650GmuContext.h"
#include "J716fGpuPowerBoard.h"

extern const uint8_t j716f_gpu_gmu_firmware[];
extern const uint32_t j716f_gpu_gmu_firmware_size;

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
    kRowContext,
    kRowLoad,
    kRowHfi,
    kRowAfter,
    kRowGx,
    kGpuRows,
};

char j716f_gpu_rows[kGpuRows][120] = {};
volatile uint32_t j716f_gpu_step = 0;
struct gpb_result j716f_gpu_result = {};
struct gpb_gmu_clock_on j716f_gpu_clocks = {};
struct gpb_gmu_state j716f_gpu_gmu_state = {};
J716fGmuCtxReport j716f_gpu_gmu_ctx = {};
A650GmuStartupResult j716f_gpu_cold = {};
A650GmuGxResult j716f_gpu_gx = {};
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
struct gpb_cmd_db_entry gDbFound[8] = {};
int gDbRc[8] = {-1, -1, -1, -1, -1, -1, -1, -1};

// Mode 7/8 persistent hardware mappings and DMA backing (retained once SMMU bank is touched).
struct ContigBuffer {
    IOBufferMemoryDescriptor *desc = nullptr;
    IOMemoryMap *wcMap = nullptr;
    uint8_t *cachedVa = nullptr;
    volatile uint32_t *wcVa = nullptr;
    uint64_t phys = 0;
    size_t bytes = 0;
};

IOMemoryMap *gSmmuFullMap = nullptr;
IOMemoryMap *gPdcMap = nullptr;
ArmSmmu4KMmio gSmmuMmio = {};
ArmSmmu4KIo gSmmuIo = {};
J716fGmuCtxState gGmuCtxState = {};
ContigBuffer gGmuBufs[J716F_GMU_BUF_COUNT] = {};
ContigBuffer gGmuTableBuf = {};

volatile uint8_t *gGmuWin[A650GmuWindows] = {};
A650GmuDevice gGmuDev = {};
A650GmuConfig gGmuCfg = {};
AdrenoHfiStartupConfig gGmuStartup = {};
bool gColdOk = false;

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

size_t dcacheLineSize() {
    uint64_t ctr = 0;
    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
    const unsigned dmin = static_cast<unsigned>((ctr >> 16) & 0xFu);
    size_t line = static_cast<size_t>(4u) << dmin;
    return line < 16u ? 64u : line;
}

void cleanCacheToPoc(const void *addr, size_t bytes) {
    const size_t step = dcacheLineSize();
    uintptr_t p = reinterpret_cast<uintptr_t>(addr) & ~(step - 1u);
    const uintptr_t end = reinterpret_cast<uintptr_t>(addr) + bytes;
    for (; p < end; p += step) {
        __asm__ volatile("dc cvac, %0" : : "r"(p) : "memory");
    }
    __asm__ volatile("dsb sy" ::: "memory");
}

void cleanInvalidateCacheToPoc(const void *addr, size_t bytes) {
    const size_t step = dcacheLineSize();
    uintptr_t p = reinterpret_cast<uintptr_t>(addr) & ~(step - 1u);
    const uintptr_t end = reinterpret_cast<uintptr_t>(addr) + bytes;
    for (; p < end; p += step) {
        __asm__ volatile("dc civac, %0" : : "r"(p) : "memory");
    }
    __asm__ volatile("dsb sy" ::: "memory");
}

int publishTablePages(void *, const void *tableCpu, size_t bytes) {
    if (!tableCpu || !bytes) return -1;
    cleanCacheToPoc(tableCpu, bytes);
    return 0;
}

bool allocContigBuffer(size_t bytes, unsigned outputBits, bool mapWc, ContigBuffer *out) {
    if (!out || !bytes || (bytes & 4095u) || outputBits < 32u || outputBits > 48u) return false;
    const uint64_t limit = UINT64_C(1) << outputBits;
    IOBufferMemoryDescriptor *desc = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task,
        static_cast<IOOptionBits>(kIODirectionInOut) | static_cast<IOOptionBits>(kIOMemoryMapperNone) |
            kIOMemoryPhysicallyContiguous,
        bytes,
        (limit - 1ULL) & ~4095ULL);
    if (!desc) return false;
    if (desc->prepare() != kIOReturnSuccess) {
        desc->release();
        return false;
    }
    IOByteCount firstLen = 0;
    IOByteCount lastLen = 0;
    const addr64_t first = desc->getPhysicalSegment(0, &firstLen, kIOMemoryMapperNone);
    const addr64_t last = desc->getPhysicalSegment(bytes - 4096u, &lastLen, kIOMemoryMapperNone);
    uint8_t *cached = static_cast<uint8_t *>(desc->getBytesNoCopy());
    if (!first || (first & 4095ULL) || !cached || (reinterpret_cast<uintptr_t>(cached) & 4095u) ||
        last != first + bytes - 4096ULL || first >= limit || bytes > limit - first ||
        desc->getLength() != bytes) {
        (void)desc->complete();
        desc->release();
        return false;
    }
    memset(cached, 0, bytes);
    cleanInvalidateCacheToPoc(cached, bytes);

    IOMemoryMap *wcMap = nullptr;
    volatile uint32_t *wcVa = nullptr;
    if (mapWc) {
        IODeviceMemory *devMem = IODeviceMemory::withRange(first, bytes);
        if (devMem) {
            wcMap = devMem->map(kIOMapAnywhere | kIOMapWriteCombineCache);
            devMem->release();
        }
        if (!wcMap) {
            (void)desc->complete();
            desc->release();
            return false;
        }
        wcVa = reinterpret_cast<volatile uint32_t *>(wcMap->getVirtualAddress());
    }

    out->desc = desc;
    out->wcMap = wcMap;
    out->cachedVa = cached;
    out->wcVa = wcVa;
    out->phys = first;
    out->bytes = bytes;
    return true;
}

void releaseContigBuffer(ContigBuffer *buf) {
    if (!buf) return;
    if (buf->wcMap) {
        buf->wcMap->release();
        buf->wcMap = nullptr;
    }
    if (buf->desc) {
        (void)buf->desc->complete();
        buf->desc->release();
        buf->desc = nullptr;
    }
    buf->cachedVa = nullptr;
    buf->wcVa = nullptr;
    buf->phys = 0;
    buf->bytes = 0;
}

int gmuRead(void *, unsigned w, size_t at, uint32_t *out) {
    if (w >= A650GmuWindows || !gGmuWin[w] || (at & 3u) || at + 4u > gGmuDev.io.bytes[w]) return 1;
    *out = *reinterpret_cast<volatile uint32_t *>(gGmuWin[w] + at);
    return 0;
}

int gmuWrite(void *, unsigned w, size_t at, uint32_t value) {
    if (w >= A650GmuWindows || !gGmuWin[w] || (at & 3u) || at + 4u > gGmuDev.io.bytes[w]) return 1;
    *reinterpret_cast<volatile uint32_t *>(gGmuWin[w] + at) = value;
    return 0;
}

int gmuBarrier(void *) {
    __asm__ volatile("dsb sy" ::: "memory");
    return 0;
}

int gmuNow(void *, uint64_t *us) {
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
    *us = ns / 1000ULL;
    return 0;
}

int gmuWait(void *, unsigned us) {
    IODelay(us);
    return 0;
}

uint32_t gpuReadDword(uint32_t dword) {
    uint32_t v = 0xDEADBEEFu;
    (void)rd(nullptr, GPB_WIN_GPU, dword * 4u, &v);
    return v;
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
    for (unsigned i = 0; i < 8u; ++i) {
        gDbRc[i] = gpb_cmd_db_find(gCmdDb, sizeof(gCmdDb), kNames[i], &gDbFound[i]);
    }
    for (unsigned half = 0; half < 2u; ++half) {
        char *row = j716f_gpu_rows[kRowDbArc + half];
        const size_t cap = sizeof(j716f_gpu_rows[0]);
        int n = half ? snprintf(row, cap, "CMDDB bcm")
                     : snprintf(row, cap, "CMDDB v%08x magic %08x", gpb_le32(gCmdDb), gpb_le32(gCmdDb + 4));
        for (unsigned i = 4u * half; i < 4u * half + 4u && n > 0 && static_cast<size_t>(n) < cap; ++i) {
            if (gDbRc[i]) {
                n += snprintf(row + n, cap - static_cast<size_t>(n), " %s:%d", kNames[i], gDbRc[i]);
            } else {
                n += snprintf(row + n, cap - static_cast<size_t>(n), " %s:%x/%u",
                              kNames[i], gDbFound[i].address, gDbFound[i].aux_bytes);
            }
        }
    }
    for (unsigned i = 0; i < 3u; ++i) {
        char *row = j716f_gpu_rows[kRowLvlGfx + i];
        const size_t cap = sizeof(j716f_gpu_rows[0]);
        int n = snprintf(row, cap, "LVL %s", kNames[i]);
        for (unsigned k = 0; !gDbRc[i] && k + 1u < gDbFound[i].aux_bytes && k < 32u && n > 0 && static_cast<size_t>(n) < cap; k += 2u) {
            n += snprintf(row + n, cap - static_cast<size_t>(n), " %u", gpb_le16(gDbFound[i].aux + k));
        }
    }
}

void runMode7GmuColdStart(const gpb_result &r, const gpb_gmu_clock_on &o) {
    char *ctxRow = j716f_gpu_rows[kRowContext];
    char *loadRow = j716f_gpu_rows[kRowLoad];
    const size_t cap = sizeof(j716f_gpu_rows[0]);

    j716f_gpu_step = 70u;
    if (j716f_gpu_gmu_firmware_size < 16u) {
        snprintf(ctxRow, cap, "GMUCTX no firmware linked (build with J716F_GPU_FIRMWARE_DIR)");
        return;
    }
    if (o.step != 5u || o.failed || r.smmu != GPB_DONE || r.smr_valid != 0u ||
        gDbRc[0] || gDbRc[1] || gDbRc[2] || gDbRc[4] || gDbRc[5] || gDbRc[6] || gDbRc[7] ||
        j716f_gpu_gmu_state.sysreset != 1u) {
        snprintf(ctxRow, cap, "GMUCTX skip: clk=%u/%d smmu=%s smr=%u db=%d%d%d%d%d%d%d rst=%08x",
                 o.step, o.failed, gpb_state_text(r.smmu), r.smr_valid,
                 gDbRc[0], gDbRc[1], gDbRc[2], gDbRc[4], gDbRc[5], gDbRc[6], gDbRc[7],
                 j716f_gpu_gmu_state.sysreset);
        return;
    }

    IODeviceMemory *smmuMem = IODeviceMemory::withRange(J716F_KGSL_SMMU_BASE, J716F_KGSL_SMMU_SIZE);
    if (smmuMem) {
        gSmmuFullMap = smmuMem->map(kIOMapAnywhere | kIOMapInhibitCache);
        smmuMem->release();
    }
    if (!gSmmuFullMap) {
        snprintf(ctxRow, cap, "GMUCTX nomap kgsl-smmu");
        return;
    }

    gSmmuMmio.mapping = reinterpret_cast<volatile unsigned char *>(gSmmuFullMap->getVirtualAddress());
    gSmmuMmio.bytes = J716F_KGSL_SMMU_SIZE;
    if (ArmSmmu4KBindMmio(&gSmmuMmio, &gSmmuIo) != ArmSmmu4KSuccess) {
        snprintf(ctxRow, cap, "GMUCTX bind mmio failed");
        return;
    }

    uint32_t id2 = 0;
    unsigned outputBits = 0;
    if (gSmmuIo.read32(gSmmuIo.cookie, 0x28u, &id2) ||
        J716fGmuCtxDecodeOas(id2, &outputBits) != ArmSmmu4KSuccess) {
        snprintf(ctxRow, cap, "GMUCTX bad id2=%08x", id2);
        return;
    }

    J716fGmuCtxConfig ctxCfg = {};
    J716fGmuCtxInitDefaultConfig(&ctxCfg);
    unsigned coherentOpt = 0;
    if (PE_parse_boot_argn("j716f-gpu-coherent", &coherentOpt, sizeof(coherentOpt))) {
        ctxCfg.coherent_walk = coherentOpt ? 1u : 0u;
    }

    // Allocate the 5 GMU payloads (with WC CPU views) + 16-page SMMU table pool before touching SMMU HW.
    j716f_gpu_step = 75u;
    for (unsigned i = 0; i < J716F_GMU_BUF_COUNT; ++i) {
        if (!allocContigBuffer(static_cast<size_t>(ctxCfg.regions[i].bytes), outputBits, true, &gGmuBufs[i])) {
            for (unsigned j = 0; j < i; ++j) releaseContigBuffer(&gGmuBufs[j]);
            snprintf(ctxRow, cap, "GMUCTX alloc payload %u (%llu B) failed", i,
                     static_cast<unsigned long long>(ctxCfg.regions[i].bytes));
            return;
        }
        ctxCfg.regions[i].physical = gGmuBufs[i].phys;
    }
    if (!allocContigBuffer(J716F_GMU_TABLE_BYTES, outputBits, false, &gGmuTableBuf)) {
        for (unsigned j = 0; j < J716F_GMU_BUF_COUNT; ++j) releaseContigBuffer(&gGmuBufs[j]);
        snprintf(ctxRow, cap, "GMUCTX alloc tables (%u B) failed", J716F_GMU_TABLE_BYTES);
        return;
    }
    ctxCfg.table_physical = gGmuTableBuf.phys;
    ctxCfg.table_cpu = reinterpret_cast<uint64_t *>(gGmuTableBuf.cachedVa);
    ctxCfg.table_capacity_pages = J716F_GMU_TABLE_PAGES;
    ctxCfg.publish_tables = publishTablePages;

    j716f_gpu_step = 80u;
    const ArmSmmu4KStatus ctxSt = J716fGmuCtxBringup(&gSmmuIo, &ctxCfg, &gGmuCtxState, &j716f_gpu_gmu_ctx);
    const J716fGmuCtxReport &g = j716f_gpu_gmu_ctx;
    snprintf(ctxRow, cap,
             "GMUCTX st=%u step=%u b=%u p=%u/%u/%u r=%u/%u id2=%x oas=%u tbl=%u ttbr0=%llx smr=%08x s2cr=%08x mm=%x",
             static_cast<unsigned>(ctxSt), g.step, g.build_status, g.program_status,
             g.program_readback_valid, g.program_enabled, g.route_status, g.routes_verified,
             g.id2, g.output_bits, g.table_pages, static_cast<unsigned long long>(g.ttbr0),
             g.smr, g.s2cr, g.mismatch_mask);
    if (ctxSt != ArmSmmu4KSuccess) return;

    // Map GMU PDC window and prepare A650GmuColdStart.
    IODeviceMemory *pdcMem = IODeviceMemory::withRange(J716F_GMU_PDC_BASE, J716F_GMU_PDC_SIZE);
    if (pdcMem) {
        gPdcMap = pdcMem->map(kIOMapAnywhere | kIOMapInhibitCache);
        pdcMem->release();
    }
    if (!gPdcMap || !gVa[GPB_WIN_GMU] || !gVa[GPB_WIN_RSCC]) {
        snprintf(loadRow, cap, "LOAD nomap gmu/rscc/pdc");
        return;
    }

    gGmuWin[A650GmuCore] = reinterpret_cast<volatile uint8_t *>(gVa[GPB_WIN_GMU]);
    gGmuWin[A650GmuRscc] = reinterpret_cast<volatile uint8_t *>(gVa[GPB_WIN_RSCC]);
    gGmuWin[A650GmuPdc] = reinterpret_cast<volatile uint8_t *>(gPdcMap->getVirtualAddress());

    memset(&gGmuDev, 0, sizeof(gGmuDev));
    gGmuDev.io.cookie = nullptr;
    gGmuDev.io.bytes[A650GmuCore] = A650_GMU_CORE_BYTES;
    gGmuDev.io.bytes[A650GmuRscc] = A650_GMU_RSCC_BYTES;
    gGmuDev.io.bytes[A650GmuPdc] = A650_GMU_PDC_BYTES;
    gGmuDev.io.read32 = gmuRead;
    gGmuDev.io.write32 = gmuWrite;
    gGmuDev.io.barrier = gmuBarrier;
    gGmuDev.services.cookie = nullptr;
    gGmuDev.services.now_us = gmuNow;
    gGmuDev.services.wait_us = gmuWait;

    gGmuCfg.chip_id = A650_GMU_CHIP_ID;
    gGmuCfg.firmware = j716f_gpu_gmu_firmware;
    gGmuCfg.firmware_bytes = j716f_gpu_gmu_firmware_size;
    gGmuCfg.icache = {gGmuBufs[J716F_GMU_BUF_ICACHE].wcVa, gGmuBufs[J716F_GMU_BUF_ICACHE].bytes, J716F_GMU_ICACHE_IOVA};
    gGmuCfg.dummy = {gGmuBufs[J716F_GMU_BUF_DUMMY].wcVa, gGmuBufs[J716F_GMU_BUF_DUMMY].bytes, J716F_GMU_DUMMY_IOVA};
    gGmuCfg.log = {gGmuBufs[J716F_GMU_BUF_LOG].wcVa, gGmuBufs[J716F_GMU_BUF_LOG].bytes, J716F_GMU_LOG_IOVA};
    gGmuCfg.hfi = {gGmuBufs[J716F_GMU_BUF_HFI].wcVa, gGmuBufs[J716F_GMU_BUF_HFI].bytes, J716F_GMU_HFI_IOVA};
    gGmuCfg.poll_budget = 100u;

    unsigned ddrVote = 0;
    if (PE_parse_boot_argn("j716f-gpu-bw", &ddrVote, sizeof(ddrVote))) {
        A650GmuDdrVote = ddrVote ? 1u : 0u;
    } else {
        A650GmuDdrVote = 0u;
    }

    const AdrenoHfiStartupStatus built = A650GmuBuildStartup(
        gDbFound[0].aux, gDbFound[0].aux_bytes,
        gDbFound[1].aux, gDbFound[1].aux_bytes,
        gDbFound[2].aux, gDbFound[2].aux_bytes,
        gDbFound[5].address, gDbFound[4].address, gDbFound[6].address, gDbFound[7].address,
        &gGmuStartup);
    if (built != AdrenoHfiStartupSuccess) {
        snprintf(loadRow, cap, "LOAD vote table err=%u", static_cast<unsigned>(built));
        return;
    }

    j716f_gpu_step = 90u;
    const A650GmuStartupLimits limits = {1000u, 100u, 50000ULL, 2000000ULL};
    const A650GmuStatus cold = A650GmuColdStart(&gGmuDev, &gGmuCfg, &gGmuStartup, &limits, &j716f_gpu_cold);
    j716f_gpu_step = cold == A650GmuSuccess ? 97u : 98u;

    const A650GmuResult &l = j716f_gpu_cold.load;
    snprintf(loadRow, cap,
             "LOAD st=%u ph=%u blk=%u ver=%08x init=%08x hfi=%08x rdy=%u%u ao=%08x cm3=%08x rst=%u rsc=%u",
             static_cast<unsigned>(cold), l.phase, l.blocks, l.firmware_version, l.firmware_init_result,
             l.hfi_status, l.firmware_ready_observed, l.hfi_ready_observed, l.ao_fault_status,
             l.cm3_fault_status, l.reset_verified, l.rsc_stop_status);

    const AdrenoHfiStartupResult &p = j716f_gpu_cold.protocol;
    const AdrenoHfi &q = gGmuDev.hfi;
    snprintf(j716f_gpu_rows[kRowHfi], cap,
             "HFI st=%u stg=%u ack=%x err=%x xpt=%u sent=%u ms=%llu skip=%u/%x/%x hlth=%u",
             static_cast<unsigned>(j716f_gpu_cold.protocol_status), p.stage, p.acknowledged_stages,
             p.firmware_error, static_cast<unsigned>(p.transport_status), p.published_commands,
             static_cast<unsigned long long>(p.elapsed_us / 1000ULL),
             q.skipped, q.skipped_header, q.skipped_ret, static_cast<unsigned>(j716f_gpu_cold.health_status));

    uint32_t after[5] = {};
    (void)gmuRead(nullptr, A650GmuCore, GPB_GMU_CM3_SYSRESET, &after[0]);
    (void)gmuRead(nullptr, A650GmuCore, GPB_GMU_CM3_FW_INIT_RESULT, &after[1]);
    (void)gmuRead(nullptr, A650GmuCore, GPB_GMU_HFI_CTRL_STATUS, &after[2]);
    (void)gmuRead(nullptr, A650GmuCore, GPB_GMU_SPTPRAC_PWR_CLK_STATUS, &after[3]);
    (void)gmuRead(nullptr, A650GmuCore, GPB_GMU_AO_HOST_INTERRUPT_STATUS, &after[4]);
    snprintf(j716f_gpu_rows[kRowAfter], cap,
             "AFTER sysrst=%08x init=%08x hfi=%08x gx=%08x ao=%08x",
             after[0], after[1], after[2], after[3], after[4]);

    gColdOk = (cold == A650GmuSuccess);
}

void runMode8PowerGx() {
    constexpr uint32_t kRbbmGbifHalt = 0x0016u;
    constexpr uint32_t kRbbmGbifHaltAck = 0x0017u;
    constexpr uint32_t kRbbmStatus = 0x0210u;
    constexpr uint32_t kGbifHalt = 0x3C45u;
    constexpr uint32_t kGbifHaltAck = 0x3C46u;

    unsigned perf = 7u; // 670 MHz top OPP on TB-J716F stock DTB gpu-opp-table_v2
    PE_parse_boot_argn("j716f-gpu-perf", &perf, sizeof(perf));
    if (perf < 1u || perf > 7u) perf = 7u;

    j716f_gpu_step = 100u;
    const A650GmuStatus st = A650GmuPowerGx(&gGmuDev, perf, &j716f_gpu_gx);
    uint32_t gdscr = 0;
    (void)rd(nullptr, GPB_WIN_GPUCC, GPB_CC_GX_GDSCR, &gdscr);

    char *row = j716f_gpu_rows[kRowGx];
    const size_t cap = sizeof(j716f_gpu_rows[0]);
    const A650GmuGxResult &g = j716f_gpu_gx;
    if (!g.gx_on) {
        snprintf(row, cap,
                 "GX off st=%u perf=%u vote=%u/%x oob=%u info=%08x gx=%08x>%08x gdscr=%08x",
                 static_cast<unsigned>(st), perf, g.vote_status, g.vote_error, g.oob_polls,
                 g.oob_info, g.gx_before, g.gx_after, gdscr);
    } else {
        j716f_gpu_step = 101u;
        snprintf(row, cap,
                 "GX on perf=%u vote=%u oob=%u gx=%08x>%08x gdscr=%08x rbbm=%08x gbif=%x/%x rh=%x/%x",
                 perf, g.vote_status, g.oob_polls, g.gx_before, g.gx_after, gdscr,
                 gpuReadDword(kRbbmStatus), gpuReadDword(kGbifHalt), gpuReadDword(kGbifHaltAck),
                 gpuReadDword(kRbbmGbifHalt), gpuReadDword(kRbbmGbifHaltAck));
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

    if (gMode >= 7u) {
        runMode7GmuColdStart(j716f_gpu_result, j716f_gpu_clocks);
    }
    if (gMode >= 8u && gColdOk) {
        runMode8PowerGx();
    }

    if (j716f_gpu_step < 100u && gMode < 7u) {
        j716f_gpu_step = 100u;
    }
    for (unsigned i = 0; i < kGpuRows; ++i) {
        if (j716f_gpu_rows[i][0]) {
            IOLog("[J716F gpu] %s\n", j716f_gpu_rows[i]);
        }
    }
}

} // namespace

extern "C" void j716f_gpu_snapshot(char *out, size_t cap) {
    if (!out || cap == 0) return;
    int n = snprintf(out, cap, "[usb] GPU: mode=%u step=%u fw=%u\r\n",
                     gMode, j716f_gpu_step, j716f_gpu_gmu_firmware_size);
    for (unsigned i = 0; i < kGpuRows && n > 0 && static_cast<size_t>(n) < cap; ++i) {
        if (!j716f_gpu_rows[i][0]) continue;
        n += snprintf(out + n, cap - static_cast<size_t>(n), "[usb] GPU[%u]: %s\r\n", i, j716f_gpu_rows[i]);
    }
}

extern "C" void j716f_gpu_probe(void) {
    static int started = 0;
    unsigned mode = 0;
    if (!PE_parse_boot_argn("j716f-gpu", &mode, sizeof(mode)) || !mode || mode > 8u) {
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
