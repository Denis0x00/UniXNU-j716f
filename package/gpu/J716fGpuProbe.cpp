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
//   9  mode 8 + KGSL SMMU Bank 0 / ASID 2 GPU context (slots 0,1,4,5) + A650 hw_init + SQE Ring 0 + CP_ME_INIT + CP_MEM_WRITE
//  10  mode 8 + Phase 3b: NativeAdrenoGpuContext Bank 0 + A650 hw_init + NativeAdrenoRing + a650_zap.mbn TZ auth +
//      CP_SET_SECURE_MODE 0 + CP_MEMCPY 1023-dw DMA + GPU SPI 300 IRQ + CP SMMU aperture SCM
//  11  mode 10 + Phase 4a: Full A650 KMD (NativeAdrenoSubmit + native_adreno_publish + CP_SMMU_TABLE_UPDATE check +
//      2x IOUserClient/NativeAdrenoABI v2 wrapper rounds + optional AGXAccelerator via j716f-gpu-metal=1)

#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <kern/clock.h>
#include <kern/sched_prim.h>
#include <kern/task.h>
#include <kern/thread.h>
#include <libkern/OSKextLib.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSMetaClass.h>
#include <mach/kmod.h>
#include <pexpert/pexpert.h>
#include <pexpert/arm64/boot.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "AdrenoPM4.h"
#include "NativeAdrenoABI.h"
#include "NativeAdrenoGpuContext.h"
#include "NativeAdrenoMemory.h"
#include "NativeAdrenoRing.h"
#include "NativeAdrenoSubmit.h"
#include "NativeAdrenoTimer.h"
#include "NativeAdrenoUserClient.h"
#include "NativeGpuTrace.h"

extern "C" {
#include "ArmSmmu4KMmio.h"
#include "J716fA650Cp.h"
#include "J716fA650Gmu.h"
#include "J716fA650GmuContext.h"
#include "J716fA650Zap.h"
#include "J716fGpuPowerBoard.h"

extern const uint8_t j716f_gpu_gmu_firmware[];
extern const uint32_t j716f_gpu_gmu_firmware_size;
extern const uint8_t j716f_gpu_sqe_firmware[];
extern const uint32_t j716f_gpu_sqe_firmware_size;
extern const uint8_t j716f_gpu_zap_firmware[];
extern const uint32_t j716f_gpu_zap_firmware_size;

/* Optional weak hooks referenced by unmodified upstream drivers/Adreno740 KMD objects */
extern volatile uint32_t native_adreno_diag;
extern uint64_t (*native_adreno_vm_now_us)(void);
volatile uint32_t native_gpu_dcvs[15] __attribute__((weak)) = {};
int boot_log_export_put(uint32_t, uint32_t, uint32_t, const void *, uint32_t) __attribute__((weak));
int boot_log_export_put(uint32_t, uint32_t, uint32_t, const void *, uint32_t) { return -1; }
kern_return_t iokit_physical_descriptor_close_notify(IOMemoryDescriptor *, void (*)(void *), void *) __attribute__((weak));
kern_return_t iokit_physical_descriptor_close_notify(IOMemoryDescriptor *, void (*)(void *), void *) {
    return KERN_NOT_SUPPORTED;
}
unsigned int native_pmap_user_mappings(ppnum_t, void **, uint64_t *, char *) __attribute__((weak));
unsigned int native_pmap_user_mappings(ppnum_t, void **, uint64_t *, char *) { return 0; }

extern kmod_info_t g_kernel_kmod_info;
extern int get_preemption_level(void);
extern boolean_t ml_get_interrupts_enabled(void);
extern boolean_t ml_at_interrupt_context(void);

extern IOReturn armcpu_spi_attach(uint32_t intid, bool edge, void (*handler)(void *), void *context,
                                  uint32_t *readback, uint64_t *router) __attribute__((weak));
extern volatile uint64_t armcpu_spi_unhandled __attribute__((weak));

extern IOReturn native_adreno_publish(NativeAdrenoVm *, NativeAdrenoSubmit *, NativeAdrenoRing *,
                                      const NativeAdrenoRingClock *, const NativeAdrenoTimer *,
                                      const NativeAdrenoAbiDeviceInfo *, uint32_t, uint32_t) __attribute__((weak));
extern uint32_t native_adreno_publish_stats(uint32_t *) __attribute__((weak));
extern IOService *native_adreno_published_service(void) __attribute__((weak));
extern void native_adreno_gpu_interrupt(uint32_t status) __attribute__((weak));
extern volatile uint32_t native_adreno_cp_aperture[4] __attribute__((weak));
extern volatile uint32_t native_adreno_cp_check[6] __attribute__((weak));
extern volatile uint32_t native_adreno_io_coherent __attribute__((weak));
extern volatile uint32_t native_adreno_wc_enabled __attribute__((weak));
extern volatile uint32_t native_adreno_owned_coherent __attribute__((weak));
extern volatile uint32_t native_adreno_import_rw_coherent __attribute__((weak));
extern volatile uint32_t native_adreno_walk_coherent[3] __attribute__((weak));
extern volatile uint32_t native_adreno_coherent_skips[4] __attribute__((weak));
extern IOReturn native_adreno_pool_add(uint64_t physical, uint64_t bytes) __attribute__((weak));

static uint64_t gJ716fWcnDmaPa = 0ULL;
static uint64_t gJ716fWcnDmaBytes = 0ULL;

extern "C" bool j716f_wcn_dma_arena(uint64_t *outPa, uint64_t *outBytes) {
    if (!gJ716fWcnDmaPa || !gJ716fWcnDmaBytes) return false;
    if (outPa) *outPa = gJ716fWcnDmaPa;
    if (outBytes) *outBytes = gJ716fWcnDmaBytes;
    return true;
}
extern volatile uint32_t native_adreno_pool_state[12] __attribute__((weak));
extern volatile uint32_t native_adreno_pool_fallbacks[8] __attribute__((weak));
extern volatile uint32_t native_adreno_fence_wait[8] __attribute__((weak));
extern volatile uint32_t native_adreno_job_time[8] __attribute__((weak));
extern volatile uint64_t native_adreno_wait_outer[5] __attribute__((weak));
extern volatile uint64_t native_adreno_fence_wake[8] __attribute__((weak));
extern volatile uint64_t native_adreno_submit_us[8] __attribute__((weak));
extern volatile uint32_t native_adreno_stall[2 + NativeAdrenoRingStallWords] __attribute__((weak));
extern volatile uint32_t native_adreno_stall_mem[4 + 3 * 17 + 4] __attribute__((weak));
extern volatile uint32_t native_adreno_method_stats[20][4] __attribute__((weak));
extern volatile uint32_t native_adreno_table_switches[8] __attribute__((weak));
extern volatile uint32_t j716f_gpu_actlr_before __attribute__((weak));
extern volatile uint32_t j716f_gpu_actlr_after __attribute__((weak));
extern void native_adreno_accelerator_static_init(void) asm("__GLOBAL__sub_I_NativeAdrenoAccelerator.cpp") __attribute__((weak));
extern IOService *native_adreno_accelerator_allocate(void) __attribute__((weak));
extern unsigned native_adreno_accelerator_instance_size(void) __attribute__((weak));

/* Upstream zzy-native-gpu-addon-mount.patch + NativeAdrenoPublish.cpp live telemetry */
extern volatile int native_gpu_addon_files __attribute__((weak));
extern volatile int native_gpu_addon_written __attribute__((weak));
extern volatile int native_gpu_addon_same __attribute__((weak));
extern volatile int native_gpu_addon_failed __attribute__((weak));
extern volatile int native_gpu_addon_error __attribute__((weak));
extern volatile int native_gpu_addon_stage __attribute__((weak));
extern volatile int native_gpu_addon_status __attribute__((weak));
extern volatile int native_gpu_addon2_stage __attribute__((weak));
extern volatile int native_gpu_addon2_status __attribute__((weak));

struct J716fMetalDiagView {
    uint32_t opens, busy, bad, closes, reports, property_reads, foreign_opens, foreign_type;
    uint32_t name_reads, calls, last_selector, last_status, ext_entries, ext_selector, ext_status;
    uint32_t ext_in, ext_out, tunnel_calls, probes, sel_bits[8], mb_maps, mb_calls, marks_total;
    uint32_t marks[16], chk_macf, chk_res, chk_type, type_pass_lo, type_pass_hi, attach_status;
    uint32_t inflight, inflight_since_calls, forced_unmaps;
    char opener[3][17];
    char reader[3][17];
    char name_reader[17];
    char starter[3][17];
    char report[64];
    char report2[64];
    uint32_t grants;
};
extern J716fMetalDiagView native_adreno_metal_diag __attribute__((weak));
extern char native_adreno_metal_rows[48][96] __attribute__((weak));
extern volatile uint32_t native_adreno_metal_rows_seq __attribute__((weak));
struct J716fClientRecordView {
    char name[20];
    uint32_t opens, refused, closes, slot, peak_mib, last_refusal;
};
struct J716fClientTextView {
    char name[20];
    char text[64];
};
struct J716fAskerRecordView {
    char name[20];
    uint32_t reads;
};
extern J716fClientRecordView native_adreno_client_log[8] __attribute__((weak));
extern J716fClientTextView native_adreno_client_text[8] __attribute__((weak));
extern J716fAskerRecordView native_adreno_metal_askers[12] __attribute__((weak));

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
    kRowGpuCtx,
    kRowCp,
    kRowCpState,
    kRowZap,
    kRowIrq,
    kRowKmd,
    kRowUcWrap,
    kRowMetal,
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
J716fGpuCtxReport j716f_gpu_ctx = {};
J716fA650CpReport j716f_gpu_cp = {};
J716fZapReport j716f_gpu_zap = {};
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

// Mode 9 persistent GPU Bank 0 SMMU state and DMA buffers (retained once Bank 0 is touched).
J716fGpuCtxState gGpuCtxState = {};
ContigBuffer gGpuBufs[J716F_GPU_BUF_COUNT] = {};
ContigBuffer gGpuTableBuf = {};

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

int cpGpuRead(void *, uint32_t dword, uint32_t *out) {
    if (!gVa[GPB_WIN_GPU] || dword >= GPB_GPU_WINDOW / 4u || !out) return 1;
    *out = *reinterpret_cast<volatile uint32_t *>(gVa[GPB_WIN_GPU] + dword * 4u);
    return 0;
}

int cpGpuWrite(void *, uint32_t dword, uint32_t value) {
    if (!gVa[GPB_WIN_GPU] || dword >= GPB_GPU_WINDOW / 4u) return 1;
    *reinterpret_cast<volatile uint32_t *>(gVa[GPB_WIN_GPU] + dword * 4u) = value;
    __asm__ volatile("dsb sy" ::: "memory");
    return 0;
}

int cpGmuRead(void *, size_t byteOff, uint32_t *out) {
    return gmuRead(nullptr, A650GmuCore, byteOff, out);
}

int cpGmuWrite(void *, size_t byteOff, uint32_t value) {
    return gmuWrite(nullptr, A650GmuCore, byteOff, value);
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

void runMode9CpSmoke() {
    char *ctxRow = j716f_gpu_rows[kRowGpuCtx];
    char *cpRow = j716f_gpu_rows[kRowCp];
    char *stRow = j716f_gpu_rows[kRowCpState];
    const size_t cap = sizeof(j716f_gpu_rows[0]);

    j716f_gpu_step = 110u;
    if (j716f_gpu_sqe_firmware_size < 8u) {
        snprintf(ctxRow, cap, "GPUCTX no SQE firmware linked (build with J716F_GPU_FIRMWARE_DIR)");
        return;
    }
    if (!gSmmuFullMap || !gVa[GPB_WIN_GPU] || !gGmuWin[A650GmuCore] || !j716f_gpu_gmu_ctx.output_bits) {
        snprintf(ctxRow, cap, "GPUCTX missing prerequisite mappings");
        return;
    }

    const unsigned outputBits = j716f_gpu_gmu_ctx.output_bits;
    J716fGpuCtxConfig ctxCfg = {};
    J716fGpuCtxInitDefaultConfig(&ctxCfg);
    unsigned coherentOpt = 0;
    if (PE_parse_boot_argn("j716f-gpu-coherent", &coherentOpt, sizeof(coherentOpt))) {
        ctxCfg.coherent_walk = coherentOpt ? 1u : 0u;
    }

    for (unsigned i = 0; i < J716F_GPU_BUF_COUNT; ++i) {
        if (!allocContigBuffer(static_cast<size_t>(ctxCfg.regions[i].bytes), outputBits, true, &gGpuBufs[i])) {
            for (unsigned j = 0; j < i; ++j) releaseContigBuffer(&gGpuBufs[j]);
            snprintf(ctxRow, cap, "GPUCTX alloc buf %u (%llu B) failed", i,
                     static_cast<unsigned long long>(ctxCfg.regions[i].bytes));
            return;
        }
        ctxCfg.regions[i].physical = gGpuBufs[i].phys;
    }
    if (!allocContigBuffer(J716F_GPU_TABLE_BYTES, outputBits, false, &gGpuTableBuf)) {
        for (unsigned j = 0; j < J716F_GPU_BUF_COUNT; ++j) releaseContigBuffer(&gGpuBufs[j]);
        snprintf(ctxRow, cap, "GPUCTX alloc tables (%u B) failed", J716F_GPU_TABLE_BYTES);
        return;
    }
    ctxCfg.table_physical = gGpuTableBuf.phys;
    ctxCfg.table_cpu = reinterpret_cast<uint64_t *>(gGpuTableBuf.cachedVa);
    ctxCfg.table_capacity_pages = J716F_GPU_TABLE_PAGES;
    ctxCfg.publish_tables = publishTablePages;

    j716f_gpu_step = 115u;
    const ArmSmmu4KStatus ctxSt = J716fGpuCtxBringup(&gSmmuIo, &ctxCfg, &gGpuCtxState, &j716f_gpu_ctx);
    const J716fGpuCtxReport &g = j716f_gpu_ctx;
    snprintf(ctxRow, cap,
             "GPUCTX st=%u step=%u b=%u p=%u/%u/%u r=%u/%u tbl=%u ttbr0=%llx smr=%08x s2cr=%08x actlr=%x>%x",
             static_cast<unsigned>(ctxSt), g.step, g.build_status, g.program_status,
             g.program_readback_valid, g.program_enabled, g.route_status, g.routes_verified,
             g.table_pages, static_cast<unsigned long long>(g.ttbr0), g.smr, g.s2cr,
             g.actlr_before, g.actlr_after);
    if (ctxSt != ArmSmmu4KSuccess) return;

    j716f_gpu_step = 120u;
    J716fA650CpIo cpIo = {};
    cpIo.cookie = nullptr;
    cpIo.gpu_read32 = cpGpuRead;
    cpIo.gpu_write32 = cpGpuWrite;
    cpIo.gmu_read32 = cpGmuRead;
    cpIo.gmu_write32 = cpGmuWrite;
    cpIo.barrier = gmuBarrier;
    cpIo.delay_us = delayUs;

    unsigned hwcg = 1u;
    unsigned apriv = 1u;
    PE_parse_boot_argn("j716f-gpu-hwcg", &hwcg, sizeof(hwcg));
    PE_parse_boot_argn("j716f-gpu-apriv", &apriv, sizeof(apriv));

    J716fA650CpConfig cpCfg = {};
    cpCfg.sqe_firmware = j716f_gpu_sqe_firmware;
    cpCfg.sqe_firmware_bytes = j716f_gpu_sqe_firmware_size;
    cpCfg.sqe_cpu = reinterpret_cast<volatile uint8_t *>(gGpuBufs[J716F_GPU_BUF_SQE].wcVa);
    cpCfg.sqe_bytes = gGpuBufs[J716F_GPU_BUF_SQE].bytes;
    cpCfg.sqe_iova = J716F_GPU_SQE_IOVA;
    cpCfg.ring_cpu = gGpuBufs[J716F_GPU_BUF_RING].wcVa;
    cpCfg.ring_bytes = gGpuBufs[J716F_GPU_BUF_RING].bytes;
    cpCfg.ring_iova = J716F_GPU_RING_IOVA;
    cpCfg.memptrs_cpu = gGpuBufs[J716F_GPU_BUF_MEMPTRS].wcVa;
    cpCfg.memptrs_bytes = gGpuBufs[J716F_GPU_BUF_MEMPTRS].bytes;
    cpCfg.memptrs_iova = J716F_GPU_MEMPTRS_IOVA;
    cpCfg.scratch_cpu = gGpuBufs[J716F_GPU_BUF_SCRATCH].wcVa;
    cpCfg.scratch_bytes = gGpuBufs[J716F_GPU_BUF_SCRATCH].bytes;
    cpCfg.scratch_iova = J716F_GPU_SCRATCH_IOVA;
    cpCfg.hwcg_enable = hwcg ? 1u : 0u;
    cpCfg.apriv_enable = apriv ? 1u : 0u;
    cpCfg.fence_poll_budget = 10000u;
    cpCfg.poll_delay_us = 20u;

    const J716fCpStatus cpSt = J716fA650CpStart(&cpIo, &cpCfg, &j716f_gpu_cp);
    const J716fA650CpReport &c = j716f_gpu_cp;
    j716f_gpu_step = (cpSt == J716fCpSuccess) ? 130u : 129u;
    snprintf(cpRow, cap,
             "CP st=%u step=%u sqe=%u/%u f1=%u(%u) f2=%u(%u) mk=%08x hwcg=%x>%x sptp=%x>%x",
             static_cast<unsigned>(cpSt), c.step, c.sqe_bytes, c.sqe_mismatch_index,
             c.init_fence_seen, c.init_polls, c.write_fence_seen, c.write_polls,
             c.marker_value, c.hwcg_before, c.hwcg_after, c.sptp_before, c.sptp_after);

    uint32_t cb0Fsr = 0, cb0FarLo = 0, cb0FarHi = 0;
    if (gGpuCtxState.bank0_offset) {
        const size_t cb0 = static_cast<size_t>(gGpuCtxState.bank0_offset);
        (void)gSmmuIo.read32(gSmmuIo.cookie, cb0 + 0x58u, &cb0Fsr);
        (void)gSmmuIo.read32(gSmmuIo.cookie, cb0 + 0x60u, &cb0FarLo);
        (void)gSmmuIo.read32(gSmmuIo.cookie, cb0 + 0x64u, &cb0FarHi);
    }
    snprintf(stRow, cap,
             "CPSTATE rbbm=%08x rptr=%x wptr=%x hwf=%x cpi=%x int0=%08x cb0_fsr=%08x far=%08x%08x",
             c.rbbm_status, c.rptr, c.wptr, c.hw_fault, c.cp_int, c.rbbm_int0,
             cb0Fsr, cb0FarHi, cb0FarLo);
}

/* ============================================================================
 * Phase 3b (Mode 10) + Phase 4a (Mode 11): Full A650 KMD + Zap + SPI 300 IRQ
 * ============================================================================ */

NativeAdrenoGpuBuffer gKmdBufs[GpuBufferCount] = {};
IOMemoryMap *gKmdWcMaps[GpuBufferCount] = {};
volatile uint8_t *gKmdWcCpu[GpuBufferCount] = {};
NativeAdrenoRing gKmdRing;
NativeAdrenoSubmit gKmdSubmit;
NativeAdrenoTimer gKmdTimer;
NativeAdrenoMemory gZapMetaMem;

volatile bool gIrqAttached = false;
volatile uint32_t gIrqCount = 0;
volatile uint32_t gIrqSeen = 0;
volatile uint32_t gIrqLast = 0;
volatile uint32_t gIrqEmpty = 0;
volatile uint32_t gCpErrCount = 0;
volatile uint32_t gCpErrStatus = 0;
volatile uint32_t gCpErrProtect = 0;
volatile uint32_t gCpErrSqePc = 0;
volatile uint32_t gCpErrSqeHdr = 0;

uint64_t kmdNowUs(void *) {
    uint64_t us = 0;
    (void)gmuNow(nullptr, &us);
    return us;
}

uint64_t kmdVmNowUs(void) {
    return kmdNowUs(nullptr);
}

int kmdRingPublish(void *, const volatile uint32_t *, size_t) {
    __asm__ volatile("dsb sy" ::: "memory");
    return 0;
}

int kmdRingAcquire(void *, volatile uint32_t *, size_t) {
    __asm__ volatile("dsb sy" ::: "memory");
    return 0;
}

int kmdRingIdle(void *) {
    if (gIrqAttached && ml_get_interrupts_enabled() && !ml_at_interrupt_context() && get_preemption_level() == 0) {
        assert_wait_timeout(reinterpret_cast<event_t>(const_cast<uint32_t *>(&gIrqCount)),
                            THREAD_UNINT, 1000u, NSEC_PER_USEC);
        (void)thread_block(THREAD_CONTINUE_NULL);
    } else {
        IODelay(20);
    }
    return 0;
}

int kmdFenceStatus(void *, int clear, uint32_t *status) {
    uint32_t v = 0;
    if (gmuRead(nullptr, A650GmuCore, GPB_GMU_AHB_FENCE_STATUS, &v)) return -1;
    if (clear && (v & 1u)) {
        (void)gmuWrite(nullptr, A650GmuCore, GPB_GMU_REG(0x9314u), 1u);
    }
    if (status) *status = v;
    return 0;
}

int kmdTimerRead(void *, uint32_t reg, uint32_t *val) {
    return cpGpuRead(nullptr, reg, val);
}

void gpuIrqHandler(void *) {
    uint32_t status = 0;
    if (cpGpuRead(nullptr, 0x0201u, &status) != 0) return;
    if (status & (1u << 9)) {
        /* CP_HW_ERROR is level-asserted until CP reset; capture SQE PC + header + protect
         * status and mask bit 9 in RBBM_INT_0_MASK so CPU0 never enters an IRQ storm. */
        uint32_t cpi = 0, prot = 0, sqePc = 0, sqeHdr = 0, mask = 0;
        (void)cpGpuRead(nullptr, 0x0823u, &cpi);
        (void)cpGpuRead(nullptr, 0x0824u, &prot);
        (void)cpGpuWrite(nullptr, 0x0908u, 0u);
        (void)cpGpuRead(nullptr, 0x0909u, &sqePc);
        (void)cpGpuRead(nullptr, 0x0909u, &sqeHdr);
        gCpErrStatus = cpi;
        gCpErrProtect = prot;
        gCpErrSqePc = sqePc;
        gCpErrSqeHdr = sqeHdr;
        gCpErrCount = gCpErrCount + 1u;
        if (cpGpuRead(nullptr, 0x0038u, &mask) == 0) {
            (void)cpGpuWrite(nullptr, 0x0038u, mask & ~(1u << 9));
        }
    }
    if (status) {
        (void)cpGpuWrite(nullptr, 0x0037u, status); /* RBBM_INT_CLEAR_CMD */
        __asm__ volatile("dsb sy" ::: "memory");
    }
    gIrqLast = status;
    gIrqSeen = gIrqSeen | status;
    gIrqCount = gIrqCount + 1u;
    gIrqEmpty = status ? 0u : gIrqEmpty + 1u;
    if (gIrqEmpty > 100000u) {
        (void)cpGpuWrite(nullptr, 0x0038u, 0u); /* Storm guard: mask GPU IRQs */
    }
    if (native_adreno_gpu_interrupt) {
        native_adreno_gpu_interrupt(status);
    }
    thread_wakeup(reinterpret_cast<event_t>(const_cast<uint32_t *>(&gIrqCount)));
}

void scmRawSmc(void *,
               uint64_t x0, uint64_t x1, uint64_t x2, uint64_t x3,
               uint64_t x4, uint64_t x5, uint64_t x6,
               uint64_t *out_x0, uint64_t *out_x1, uint64_t *out_x6) {
    register uint64_t r0 asm("x0") = x0;
    register uint64_t r1 asm("x1") = x1;
    register uint64_t r2 asm("x2") = x2;
    register uint64_t r3 asm("x3") = x3;
    register uint64_t r4 asm("x4") = x4;
    register uint64_t r5 asm("x5") = x5;
    register uint64_t r6 asm("x6") = x6;
    register uint64_t r7 asm("x7") = 0;
    __asm__ volatile("smc #0"
                     : "+r"(r0), "+r"(r1), "+r"(r2), "+r"(r3), "+r"(r4), "+r"(r5), "+r"(r6), "+r"(r7)
                     :
                     : "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
    if (out_x0) *out_x0 = r0;
    if (out_x1) *out_x1 = r1;
    if (out_x6) *out_x6 = r6;
}

void scmSleepMs(void *, uint32_t ms) {
    IOSleep(ms);
}

int zapPrepareMeta(void *, const uint8_t *meta, size_t /*meta_bytes*/, size_t aligned_bytes, uint64_t *out_pa) {
    if (!meta || !aligned_bytes || !out_pa) return -1;
    if (gZapMetaMem.allocate(aligned_bytes, 36u, true, aligned_bytes) != kIOReturnSuccess ||
        gZapMetaMem.spanCount() != 1) {
        return -1;
    }
    uint8_t *dst = static_cast<uint8_t *>(gZapMetaMem.mapping());
    if (!dst) return -1;
    memcpy(dst, meta, aligned_bytes);
    if (gZapMetaMem.publishForDevice(0, aligned_bytes, true) != kIOReturnSuccess) {
        return -1;
    }
    *out_pa = gZapMetaMem.spans()[0].physical;
    return 0;
}

int zapLoadPil(void *, uint64_t pil_phys, uint64_t pil_bytes,
               const uint8_t *fw, size_t fw_size, const J716fZapPlan *plan) {
    IODeviceMemory *dev = IODeviceMemory::withRange(pil_phys, pil_bytes);
    if (!dev) return -1;
    IOMemoryMap *map = dev->map(kIOMapAnywhere | kIOMapWriteCombineCache);
    dev->release();
    if (!map) return -1;

    volatile uint8_t *region = reinterpret_cast<volatile uint8_t *>(map->getVirtualAddress());
    const uint64_t base = plan->relocatable ? plan->min_paddr : pil_phys;
    int rc = 0;
    for (uint32_t i = 0; i < plan->seg_count; ++i) {
        const J716fZapSeg &s = plan->segs[i];
        if (s.p_paddr < base) { rc = -1; break; }
        const uint64_t at = static_cast<uint64_t>(s.p_paddr) - base;
        if (at + s.p_memsz > pil_bytes || s.p_filesz > s.p_memsz ||
            static_cast<uint64_t>(s.p_offset) + s.p_filesz > fw_size) {
            rc = -1;
            break;
        }
        for (uint64_t b = 0; b < s.p_filesz; ++b) region[at + b] = fw[s.p_offset + b];
        for (uint64_t b = s.p_filesz; b < s.p_memsz; ++b) region[at + b] = 0;
    }
    __asm__ volatile("dsb sy" ::: "memory");
    /* Unmap CPU view strictly BEFORE TZ PAS_AUTH_AND_RESET takes region ownership. */
    map->release();
    return rc;
}

bool kmdSubmitAndWait(const uint32_t *words, size_t count, uint32_t *outFence) {
    uint32_t fence = 0, seen = 0;
    if (gKmdRing.submit(words, count, &fence) != kIOReturnSuccess) return false;
    if (outFence) *outFence = fence;
    const NativeAdrenoRingClock clk = {nullptr, kmdNowUs, kmdRingIdle};
    return gKmdRing.wait(fence, &clk, 200000u, &seen) == NativeAdrenoRingFenceComplete;
}

uint32_t kmdReadCb0Fsr() {
    uint32_t fsr = 0;
    if (gSmmuFullMap) {
        /* Bank 0 context offset on QSMMUv500 (num_pages=8 -> 0x8000) */
        const volatile uint8_t *base = reinterpret_cast<const volatile uint8_t *>(gSmmuFullMap->getVirtualAddress());
        fsr = *reinterpret_cast<const volatile uint32_t *>(base + 0x8058u);
    }
    return fsr;
}

IOReturn kmdWrapperCall(IOUserClient *uc, uint32_t method, const void *in, void *out) {
    IOExternalMethodArguments a = {};
    a.version = kIOExternalMethodArgumentsCurrentVersion;
    a.selector = method;
    const size_t is = NativeAdrenoUserClient::inputSize(method);
    const size_t os = NativeAdrenoUserClient::outputSize(method);
    a.structureInput = is ? in : nullptr;
    a.structureInputSize = static_cast<uint32_t>(is);
    a.structureOutput = os ? out : nullptr;
    a.structureOutputSize = static_cast<uint32_t>(os);
    return uc->externalMethod(method, &a, nullptr, nullptr, nullptr);
}

int kmdWrapperRound(IOService *svc, uint32_t pattern, int &ok, int &n, IOReturn &err, int &errAt, uint64_t &tsTicks) {
    auto step = [&](IOReturn s) {
        const int idx = n++;
        if (s == kIOReturnSuccess) {
            ++ok;
            return true;
        }
        if (err == kIOReturnSuccess) {
            err = s;
            errAt = idx;
        }
        return false;
    };

    IOUserClient *uc = nullptr;
    /* NativeAdrenoPublish requires open type 0x4147 ('AG' = NativeAdrenoPluginOpenType). */
    if (!step(svc->newUserClient(kernel_task, nullptr, 0x4147u, nullptr, &uc)) || !uc) return -1;

    NativeAdrenoAbiParams params = {};
    step(kmdWrapperCall(uc, NativeAdrenoMethodQuery, nullptr, &params));
    NativeAdrenoAbiDeviceInfo info = {};
    step(kmdWrapperCall(uc, NativeAdrenoMethodGetDeviceInfo, nullptr, &info));
    step((info.chip_id == GPB_CHIPID_A650_V2 && info.gmem_bytes == (1u << 20)) ? kIOReturnSuccess : kIOReturnError);

    NativeAdrenoAbiBoAlloc alloc = {4096u, 12u, 0u};
    NativeAdrenoAbiBoAllocReply ib = {}, tgt = {};
    int matched = -1;
    IOMemoryDescriptor *desc[2] = {};
    IOMemoryMap *maps[2] = {};

    if (step(kmdWrapperCall(uc, NativeAdrenoMethodBoAlloc, &alloc, &ib)) &&
        step(kmdWrapperCall(uc, NativeAdrenoMethodBoAlloc, &alloc, &tgt))) {
        const uint64_t ibIova = GpuClientVaStart + 0x4000ULL;
        const uint64_t tgtIova = GpuClientVaStart + 0x5000ULL;
        NativeAdrenoAbiBoMap mapIb = {ib.handle, ibIova, 1u, 0u, 2u, 0u};
        NativeAdrenoAbiBoMap mapTgt = {tgt.handle, tgtIova, 3u, 0u, 2u, 0u};
        NativeAdrenoAbiBoMapReply r = {};
        NativeAdrenoAbiBoCpuMap cmIb = {ib.handle}, cmTgt = {tgt.handle};
        NativeAdrenoAbiBoCpuMapReply crIb = {}, crTgt = {};

        if (step(kmdWrapperCall(uc, NativeAdrenoMethodBoMap, &mapIb, &r)) &&
            step(kmdWrapperCall(uc, NativeAdrenoMethodBoMap, &mapTgt, &r)) &&
            step(kmdWrapperCall(uc, NativeAdrenoMethodBoCpuMap, &cmIb, &crIb)) &&
            step(kmdWrapperCall(uc, NativeAdrenoMethodBoCpuMap, &cmTgt, &crTgt))) {
            IOOptionBits opt[2] = {};
            const uint32_t tokens[2] = {crIb.token, crTgt.token};
            bool mapped = true;
            for (int i = 0; i < 2; ++i) {
                const IOReturn s = uc->clientMemoryForType(tokens[i], &opt[i], &desc[i]);
                if (s != kIOReturnSuccess || !desc[i]) { mapped = false; continue; }
                maps[i] = desc[i]->createMappingInTask(kernel_task, 0, opt[i]);
                if (!maps[i]) mapped = false;
            }
            step(mapped ? kIOReturnSuccess : kIOReturnNoMemory);
            if (mapped) {
                volatile uint32_t *iw = reinterpret_cast<volatile uint32_t *>(maps[0]->getVirtualAddress());
                volatile uint32_t *tw = reinterpret_cast<volatile uint32_t *>(maps[1]->getVirtualAddress());
                for (unsigned i = 0; i < 1024; ++i) tw[i] = 0xDEADBEEFu;
                const uint32_t values[3] = {pattern, ~pattern, 0xA6500001u};
                uint32_t words[12] = {};
                bool built = true;
                for (unsigned i = 0; i < 3; ++i) {
                    built = built && (AdrenoPM4Type7(0x3Du, 3u, &words[i * 4]) == AdrenoPM4Success);
                    words[i * 4 + 1] = static_cast<uint32_t>(tgtIova + i * 4u);
                    words[i * 4 + 2] = static_cast<uint32_t>((tgtIova + i * 4u) >> 32);
                    words[i * 4 + 3] = values[i];
                }
                for (unsigned i = 0; i < 12; ++i) iw[i] = words[i];
                __asm__ volatile("dsb sy" ::: "memory");

                static NativeAdrenoAbiSubmit submit;
                memset(&submit, 0, sizeof(submit));
                submit.buffer_count = 1u;
                submit.buffers[0] = {ib.handle, ibIova, sizeof(words)};
                NativeAdrenoAbiSubmitReply sr = {};
                if (step(built ? kIOReturnSuccess : kIOReturnInternalError) &&
                    step(kmdWrapperCall(uc, NativeAdrenoMethodSubmit, &submit, &sr))) {
                    NativeAdrenoAbiWaitFence wf = {sr.fence, 200000000ULL};
                    if (step(kmdWrapperCall(uc, NativeAdrenoMethodWaitFence, &wf, nullptr))) {
                        __asm__ volatile("dsb sy" ::: "memory");
                        matched = (tw[0] == values[0]) + (tw[1] == values[1]) +
                                  (tw[2] == values[2]) + (tw[3] == 0xDEADBEEFu);
                    }
                }
                NativeAdrenoAbiTimestamp ts = {};
                if (step(kmdWrapperCall(uc, NativeAdrenoMethodGetTimestamp, nullptr, &ts))) {
                    tsTicks = ts.ticks;
                }
            }
        }
    }
    for (auto *m : maps) if (m) m->release();
    for (auto *d : desc) if (d) d->release();
    step(uc->clientClose());
    uc->release();
    return matched;
}

void publishA650MetalAccelerator() {
    char *row = j716f_gpu_rows[kRowMetal];
    const size_t cap = sizeof(j716f_gpu_rows[0]);
    if (!native_adreno_accelerator_static_init || !native_adreno_accelerator_allocate ||
        !native_adreno_accelerator_instance_size) {
        snprintf(row, cap, "METAL nolink");
        return;
    }
    const int kextRc = OSKextLoadKextWithIdentifier("com.apple.iokit.IOGPUFamily");
    if (kextRc) {
        snprintf(row, cap, "METAL kext_err=%x", static_cast<unsigned>(kextRc));
        return;
    }
    const OSSymbol *sym = OSSymbol::withCString("IOAcceleratorES");
    const OSMetaClass *parent = sym ? OSMetaClass::getMetaClassWithName(sym) : nullptr;
    if (sym) sym->release();
    if (!parent || parent->getClassSize() != 136) {
        snprintf(row, cap, "METAL parent_err=%u", parent ? parent->getClassSize() : 0u);
        return;
    }
    void *handle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
    if (!handle) { snprintf(row, cap, "METAL preModLoad_err"); return; }
    native_adreno_accelerator_static_init();
    const kern_return_t postRc = OSMetaClass::postModLoad(handle);
    if (postRc != KERN_SUCCESS || native_adreno_accelerator_instance_size() != 136) {
        snprintf(row, cap, "METAL post_err=%x sz=%u", static_cast<unsigned>(postRc),
                 native_adreno_accelerator_instance_size());
        return;
    }
    IOService *accel = native_adreno_accelerator_allocate();
    if (!accel || !accel->init()) {
        if (accel) accel->release();
        snprintf(row, cap, "METAL alloc_err");
        return;
    }
    accel->setProperty("MetalPluginName", "NativeAdrenoMetal");
    accel->setProperty("MetalPluginClassName", "NativeAdrenoMetalDevice");
    /* iPadOS 17 zzw-native-adreno-sandbox-grant.patch matches exact comma-separated process names
     * (up to 16 chars each, no '*' wildcard). Include render server, SpringBoard, and Metal clients.
     * Deliberately exclude mediaserverd: its Metal path hits RGBA8Unorm attachment refusal and
     * 75 MiB EXC_RESOURCE jetsam crash-loop (~15x/min), causing dyld unnest & UFS flush storms. */
    accel->setProperty(
        "NativeSandboxGrantProcess",
        "backboardd,SpringBoard,adrenoprobed,iconservicesagen,MTLCompilerServi,"
        "PosterBoard,chronod,MobileSlideShow,Preferences,MobileSafari,com.apple.WebKit,"
        "UserEventAgent,CARenderServer,WindowServer,WidgetRenderer_D,PhotosPosterProv,"
        "CollectionsPoste,PridePosterExten,KaleidoscopePost,AegirPoster,UnityPosterExten,"
        "GradientPosterEx,EmojiPosterExten,PhotosUIService,Spotlight,"
        "MobileNotes,MobileTimer,MobileCal,Weather,Files,Maps,Books,Music,Tips,Reminders,"
        "Contacts,Shortcuts");
    if (!accel->attach(IOService::getPlatform())) {
        accel->release();
        snprintf(row, cap, "METAL attach_err");
        return;
    }
    accel->registerService();
    snprintf(row, cap, "METAL published AGXAccelerator(IOAcceleratorES,136B) plugin=NativeAdrenoMetal");
    IOLog("[J716F gpu] [live] %s\n", row);
}

NativeAdrenoAbiDeviceInfo gA650DeviceInfo = {
    GPB_CHIPID_A650_V2,      /* chip_id: Adreno 650 v2.0 (0x06050002) */
    1u << 20,                /* gmem_bytes: 1 MiB */
    0ULL,                    /* gmem_base: 0 on A650 (ADRENO_6XX_GEN3, Linux adreno_gpu.c) */
    0x0001FFFFFFFFF000ULL,   /* uche_trap_base */
    GpuClientVaStart,        /* va_start: 4 GiB */
    0ULL,                    /* va_size: filled from SMMU inputBits() */
    GPB_HIGHEST_BANK_BIT,    /* highest_bank_bit: 16 */
    6u,                      /* ubwc_swizzle: level 2 + level 3 */
    1u,                      /* macrotile_mode: 8-channel */
    GPB_UBWC_MODE,           /* ubwc_version: 4 (UBWC 4.0) */
    NativeAdrenoDeviceClientIova,
    0u
};

char gCoherenceRow[120] = {};
char gWalkRow[80] = {};

bool readDramPageUncached(uint64_t pa, uint32_t *out) {
    IODeviceMemory *dev = IODeviceMemory::withRange(pa, 0x1000u);
    if (!dev) return false;
    IOMemoryMap *map = dev->map(kIOMapAnywhere | kIOMapInhibitCache);
    dev->release();
    if (!map) return false;
    const volatile uint32_t *src = reinterpret_cast<const volatile uint32_t *>(map->getVirtualAddress());
    for (unsigned i = 0; i < 1024u; ++i) out[i] = src[i];
    map->release();
    return true;
}

bool gpuCopyPage4k(uint64_t src, uint64_t dst) {
    uint32_t words[7] = {};
    if (AdrenoPM4Type7(0x75u, 5u, &words[0]) != AdrenoPM4Success ||
        AdrenoPM4Type7(0x12u, 0u, &words[6]) != AdrenoPM4Success) {
        return false;
    }
    words[1] = 1024u;
    words[2] = static_cast<uint32_t>(src);
    words[3] = static_cast<uint32_t>(src >> 32);
    words[4] = static_cast<uint32_t>(dst);
    words[5] = static_cast<uint32_t>(dst >> 32);
    return kmdSubmitAndWait(words, 7u, nullptr);
}

bool runWalkCoherenceProbe(NativeAdrenoVm *vm) {
    if (!&native_adreno_walk_coherent) {
        snprintf(gWalkRow, sizeof(gWalkRow), "W nolink -> off");
        return false;
    }
    constexpr uint64_t kBase = 0x18100000ULL;
    constexpr size_t kBytes = 2u * 4096u;
    uint8_t *buf = static_cast<uint8_t *>(IOMallocAligned(kBytes, 4096u));
    if (!buf) {
        snprintf(gWalkRow, sizeof(gWalkRow), "W nomem -> off");
        return false;
    }
    volatile uint32_t *a = reinterpret_cast<volatile uint32_t *>(buf);
    volatile uint32_t *b = a + 1024;
    for (unsigned i = 0; i < 1024u; ++i) { a[i] = 0x66660000u | i; b[i] = 0u; }
    (void)NativeAdrenoMaintainOwnedPages(buf, kBytes, NativeAdrenoCleanInvalidate);

    NativeAdrenoMemory mem;
    IOReturn st = mem.importTask(kernel_task, reinterpret_cast<uint64_t>(buf), kBytes, 36u, kBytes);
    uint64_t phys[2] = {};
    for (size_t j = 0; !st && j < mem.spanCount(); ++j) {
        for (size_t at = 0; at < mem.spans()[j].bytes; at += 4096u) {
            phys[(mem.spans()[j].offset + at) / 4096u] = mem.spans()[j].physical + at;
        }
    }
    static uint32_t words[1024];
    unsigned dirtyAt = 0, copied = 0;
    uint32_t fault[4] = {};
    for (unsigned attempt = 0; attempt < 8u && !st && phys[0] && phys[1] && !dirtyAt; ++attempt) {
        uint64_t handle = 0, pte = 0;
        native_adreno_walk_coherent[0] = 1u;
        st = vm->map(&mem, 0u, kBase + attempt * 0x200000ULL, 3u, 1u, 3u, &handle);
        native_adreno_walk_coherent[0] = 0u;
        if (st) break;
        if (vm->leafPhysical(phys[0], &pte) && readDramPageUncached(pte & ~0xFFFULL, words)) {
            const uint64_t inDram = static_cast<uint64_t>(words[(pte & 0xFFFu) / 4u]) |
                                    (static_cast<uint64_t>(words[(pte & 0xFFFu) / 4u + 1u]) << 32);
            if ((inDram & 0x0000FFFFFFFFF000ULL) != phys[0]) {
                dirtyAt = attempt + 1u;
                const uint64_t iova = kBase + attempt * 0x200000ULL;
                if (gpuCopyPage4k(iova, iova + 4096u) && readDramPageUncached(phys[1], words)) {
                    for (unsigned i = 0; i < 1024u; ++i) copied += (words[i] == (0x66660000u | i)) ? 1u : 0u;
                }
                (void)vm->faultState(fault);
            }
        }
        (void)vm->unmap(handle);
    }
    if (fault[0]) {
        uint32_t far[3] = {};
        if (NativeAdrenoGpuContext *ctx = NativeAdrenoGpuContextObject()) {
            (void)ctx->clearFault(fault[0], far);
        }
    }
    const bool pass = !st && dirtyAt && copied == 1024u && !fault[0];
    (void)NativeAdrenoMaintainOwnedPages(buf, kBytes, NativeAdrenoCleanInvalidate);
    (void)mem.release();
    IOFreeAligned(buf, kBytes);
    snprintf(gWalkRow, sizeof(gWalkRow), "W d%u c%u f%x s%x -> %s",
             dirtyAt, copied, fault[0], static_cast<unsigned>(st), pass ? "on" : "off");
    return pass;
}

void runIoCoherenceProbe(NativeAdrenoVm *vm) {
    unsigned wanted = 1u;
    PE_parse_boot_argn("gpu-coherent", &wanted, sizeof(wanted));
    if (!wanted || !vm || !&native_adreno_io_coherent) {
        snprintf(gCoherenceRow, sizeof(gCoherenceRow), "H skip(arg=%u)", wanted);
        return;
    }
    constexpr uint64_t kIova = 0x18000000ULL;
    constexpr size_t kBytes = 4u * 4096u;
    uint8_t *buf = static_cast<uint8_t *>(IOMallocAligned(kBytes, 4096u));
    NativeAdrenoMemory mem;
    uint64_t handle = 0;
    uint32_t k[7] = {};
    unsigned attempts = 0;
    IOReturn st = buf ? mem.importTask(kernel_task, reinterpret_cast<uint64_t>(buf), kBytes, 36u, kBytes)
                      : kIOReturnNoMemory;
    k[0] = 1u;
    if (!st) {
        k[0] = 2u;
        st = vm->map(&mem, 0u, kIova, 3u, 1u, 3u, &handle);
    }
    uint64_t phys[4] = {};
    for (size_t j = 0; !st && j < mem.spanCount(); ++j) {
        for (size_t at = 0; at < mem.spans()[j].bytes; at += 4096u) {
            phys[(mem.spans()[j].offset + at) / 4096u] = mem.spans()[j].physical + at;
        }
    }
    static uint32_t words[1024];
    bool pass = false;
    if (!st && phys[0] && phys[1] && phys[2] && phys[3]) {
        k[0] = 3u;
        volatile uint32_t *s = reinterpret_cast<volatile uint32_t *>(buf);
        volatile uint32_t *d = s + 1024;
        volatile uint32_t *r = s + 2048;
        volatile uint32_t *t = s + 3072;
        bool dirty = false;
        for (unsigned attempt = 0; attempt < 8u && !st && !dirty; ++attempt) {
            attempts = attempt + 1u;
            for (unsigned i = 0; i < 1024u; ++i) {
                s[i] = 0x11110000u | i;
                d[i] = 0u;
                r[i] = 0x33330000u | i;
                t[i] = 0x44440000u | i;
            }
            st = NativeAdrenoMaintainOwnedPages(buf, kBytes, NativeAdrenoCleanInvalidate);
            for (unsigned i = 0; i < 1024u; ++i) s[i] = 0x22220000u | i;
            __asm__ volatile("dsb sy" ::: "memory");
            k[1] = 0u;
            if (!st && readDramPageUncached(phys[0], words)) {
                for (unsigned i = 0; i < 1024u; i += 16u) k[1] += (words[i] == (0x11110000u | i)) ? 1u : 0u;
            }
            dirty = (k[1] >= 48u);
        }
        if (!st && dirty) {
            k[0] = 4u;
            if (gpuCopyPage4k(kIova, kIova + 4096u) && readDramPageUncached(phys[1], words)) {
                k[0] = 5u;
                for (unsigned i = 0; i < 1024u; ++i) {
                    k[2] += (words[i] == (0x22220000u | i)) ? 1u : 0u;
                    k[3] += (words[i] == (0x11110000u | i)) ? 1u : 0u;
                }
            }
        }
        if (!st && k[0] == 5u) {
            st = NativeAdrenoMaintainOwnedPages(const_cast<uint32_t *>(t), 4096u, NativeAdrenoClean);
        }
        uint32_t touch = 0;
        for (unsigned i = 0; i < 1024u; ++i) touch += t[i];
        __asm__ volatile("dsb sy" ::: "memory");
        if (!st && k[0] == 5u && gpuCopyPage4k(kIova + 8192u, kIova + 12288u)) {
            k[0] = 6u;
            for (unsigned i = 0; i < 1024u; ++i) k[4] += (t[i] == (0x33330000u | i)) ? 1u : 0u;
            if (readDramPageUncached(phys[3], words)) {
                for (unsigned i = 0; i < 1024u; ++i) k[5] += (words[i] == (0x33330000u | i)) ? 1u : 0u;
            }
        }
        (void)touch;
        pass = (k[0] == 6u && k[1] >= 48u && k[2] == 1024u && k[4] == 1024u && k[5] == 1024u);
    }
    k[6] = static_cast<uint32_t>(st);
    if (handle) (void)vm->unmap(handle);
    if (buf) (void)NativeAdrenoMaintainOwnedPages(buf, kBytes, NativeAdrenoCleanInvalidate);
    (void)mem.release();
    if (buf) IOFreeAligned(buf, kBytes);

    native_adreno_io_coherent = pass ? 1u : 0u;
    if (&native_adreno_owned_coherent) {
        /* UniXNU/metal2vulkan requires HOST_VISIBLE|HOST_COHERENT|HOST_CACHED (SHARED_MEMORY),
         * advertised by Turnip when NativeAdrenoParamsOwnedCoherent is set. Default on once the
         * live hardware IO coherence probe passes (overrideable via gpu-owned-coherent=0). */
        unsigned owned = 1u;
        PE_parse_boot_argn("gpu-owned-coherent", &owned, sizeof(owned));
        native_adreno_owned_coherent = (pass && owned) ? 1u : 0u;
    }
    if (&native_adreno_walk_coherent) {
        unsigned walk = 1u;
        PE_parse_boot_argn("gpu-walk", &walk, sizeof(walk));
        native_adreno_walk_coherent[0] = (pass && walk && runWalkCoherenceProbe(vm)) ? 1u : 0u;
    }
    snprintf(gCoherenceRow, sizeof(gCoherenceRow), "H d%u/64 a%u r%u/%u w%u/%u s%x k%u -> %s",
             k[1], attempts, k[2], k[3], k[4], k[5], k[6], k[0], pass ? "on" : "off");
    IOLog("[J716F gpu] [live] COH %s | %s\n", gCoherenceRow, gWalkRow);
}

void runMode10And11Kmd() {
    const size_t cap = sizeof(j716f_gpu_rows[0]);
    char *ctxRow = j716f_gpu_rows[kRowGpuCtx];
    char *cpRow = j716f_gpu_rows[kRowCp];
    char *stRow = j716f_gpu_rows[kRowCpState];
    char *zapRow = j716f_gpu_rows[kRowZap];
    char *irqRow = j716f_gpu_rows[kRowIrq];
    char *kmdRow = j716f_gpu_rows[kRowKmd];
    char *ucRow = j716f_gpu_rows[kRowUcWrap];

    j716f_gpu_step = 110u;
    if (j716f_gpu_sqe_firmware_size < 8u || !gSmmuFullMap || !gVa[GPB_WIN_GPU] || !gGmuWin[A650GmuCore]) {
        snprintf(ctxRow, cap, "GPUCTX missing SQE(%u) or prerequisite maps", j716f_gpu_sqe_firmware_size);
        return;
    }

    native_adreno_vm_now_us = kmdVmNowUs;

    /* Step 1: Bring up KGSL SMMU Bank 0 / ASID 2 via NativeAdrenoGpuContext (4 SMR routes + ACTLR CPRE clear). */
    NativeAdrenoGmuContextReport ctxRep = {};
    volatile uint8_t *smmuVa = reinterpret_cast<volatile uint8_t *>(gSmmuFullMap->getVirtualAddress());
    const IOReturn ctxSt = NativeAdrenoGpuContextBringup(smmuVa, 0x10000u, 0u, 2u, 0u, 0x0000u, 1u, 0x0001u,
                                                         nullptr, &ctxRep);
    const unsigned ias = NativeAdrenoGpuContextInputBits();
    snprintf(ctxRow, cap,
             "GPUCTX st=%x step=%u p=%u/%u/%u r=%u/%u ias=%u oas=%u actlr=%x>%x spans=%u",
             static_cast<unsigned>(ctxSt), ctxRep.step, ctxRep.program_status,
             ctxRep.program_readback_valid, ctxRep.program_enabled,
             ctxRep.route_status, ctxRep.routes_verified, ias, ctxRep.output_bits,
             &j716f_gpu_actlr_before ? j716f_gpu_actlr_before : 0u,
             &j716f_gpu_actlr_after ? j716f_gpu_actlr_after : 0u, ctxRep.spans);
    IOLog("[J716F gpu] [live] %s\n", ctxRow);
    if (ctxSt != kIOReturnSuccess) return;

    if (NativeAdrenoGpuContextBuffers(gKmdBufs) != kIOReturnSuccess) {
        snprintf(cpRow, cap, "CP failed to get kernel GPU buffers");
        return;
    }
    for (unsigned i = 0; i < 5u; ++i) {
        IODeviceMemory *m = IODeviceMemory::withRange(gKmdBufs[i].physical, gKmdBufs[i].bytes);
        if (m) {
            gKmdWcMaps[i] = m->map(kIOMapAnywhere | kIOMapWriteCombineCache);
            m->release();
        }
        if (!gKmdWcMaps[i]) {
            snprintf(cpRow, cap, "CP WC map failed for buf %u", i);
            return;
        }
        gKmdWcCpu[i] = reinterpret_cast<volatile uint8_t *>(gKmdWcMaps[i]->getVirtualAddress());
    }

    /* Step 2: A650 hw_init + SQE firmware load + CP_SQE_INSTR_BASE */
    j716f_gpu_step = 120u;
    J716fA650CpIo cpIo = {nullptr, cpGpuRead, cpGpuWrite, cpGmuRead, cpGmuWrite, gmuBarrier, delayUs};
    J716fA650CpConfig cpCfg = {};
    cpCfg.sqe_firmware = j716f_gpu_sqe_firmware;
    cpCfg.sqe_firmware_bytes = j716f_gpu_sqe_firmware_size;
    cpCfg.sqe_cpu = gKmdWcCpu[0];
    cpCfg.sqe_bytes = static_cast<size_t>(gKmdBufs[0].bytes);
    cpCfg.sqe_iova = gKmdBufs[0].iova;
    cpCfg.hwcg_enable = 1u;
    cpCfg.apriv_enable = 1u;
    const J716fCpStatus hwSt = J716fA650HwInitAndLoadSqe(&cpIo, &cpCfg, &j716f_gpu_cp);
    if (hwSt != J716fCpSuccess) {
        snprintf(cpRow, cap, "CP hw_init err=%u step=%u sqe=%u/%u",
                 static_cast<unsigned>(hwSt), j716f_gpu_cp.step,
                 j716f_gpu_cp.sqe_bytes, j716f_gpu_cp.sqe_mismatch_index);
        return;
    }

    /* Step 3: Attach NativeAdrenoRing (J716fA650Ring.cpp) + release SQE + CP_ME_INIT + CP_MEM_WRITE */
    const NativeAdrenoRingIo ringIo = {nullptr, cpGpuWrite, cpGpuRead, gmuBarrier, nullptr, kmdFenceStatus};
    const NativeAdrenoRingCache ringCache = {nullptr, kmdRingPublish, kmdRingAcquire};
    const IOReturn ringAtt = gKmdRing.attach(&ringIo, &ringCache,
                                             const_cast<uint8_t *>(gKmdWcCpu[1]),
                                             static_cast<size_t>(gKmdBufs[1].bytes), gKmdBufs[1].iova,
                                             const_cast<uint8_t *>(gKmdWcCpu[2]),
                                             static_cast<size_t>(gKmdBufs[2].bytes), gKmdBufs[2].iova);
    if (ringAtt != kIOReturnSuccess || cpGpuWrite(nullptr, 0x0808u, 1u) != 0) {
        snprintf(cpRow, cap, "CP ring attach=%x", static_cast<unsigned>(ringAtt));
        return;
    }
    __asm__ volatile("dsb sy" ::: "memory");

    uint32_t meInit[9] = {};
    (void)AdrenoPM4Type7(0x48u, 8u, &meInit[0]);
    meInit[1] = 0x2Fu;
    meInit[2] = 0x00000003u;
    meInit[3] = 0x20000000u;
    uint32_t f1 = 0, f2 = 0;
    const bool ok1 = kmdSubmitAndWait(meInit, 9u, &f1);

    volatile uint32_t *scratch = reinterpret_cast<volatile uint32_t *>(gKmdWcCpu[4]);
    scratch[0] = 0u;
    uint32_t memWrite[4] = {};
    (void)AdrenoPM4Type7(0x3Du, 3u, &memWrite[0]);
    memWrite[1] = static_cast<uint32_t>(gKmdBufs[4].iova);
    memWrite[2] = static_cast<uint32_t>(gKmdBufs[4].iova >> 32);
    memWrite[3] = J716F_GPU_SMOKE_MARKER;
    const bool ok2 = ok1 && kmdSubmitAndWait(memWrite, 4u, &f2);
    __asm__ volatile("dsb sy" ::: "memory");
    const uint32_t marker = scratch[0];

    snprintf(cpRow, cap,
             "CP st=%u sqe=%u/%u f1=%u(%u) f2=%u(%u) mk=%08x hwcg=%x>%x",
             (ok1 && ok2 && marker == J716F_GPU_SMOKE_MARKER) ? 0u : 1u,
             j716f_gpu_cp.sqe_bytes, j716f_gpu_cp.sqe_mismatch_index,
             f1, ok1 ? 1u : 0u, f2, ok2 ? 1u : 0u, marker,
             j716f_gpu_cp.hwcg_before, j716f_gpu_cp.hwcg_after);

    uint32_t rbbm = 0, rptr = 0, wptr = 0, hwf = 0, cpi = 0, int0 = 0;
    (void)cpGpuRead(nullptr, 0x0210u, &rbbm);
    (void)cpGpuRead(nullptr, 0x0806u, &rptr);
    (void)cpGpuRead(nullptr, 0x0807u, &wptr);
    (void)cpGpuRead(nullptr, 0x0821u, &hwf);
    (void)cpGpuRead(nullptr, 0x0823u, &cpi);
    (void)cpGpuRead(nullptr, 0x0201u, &int0);
    snprintf(stRow, cap,
             "CPSTATE rbbm=%08x rptr=%x wptr=%x hwf=%x cpi=%x int0=%08x cb0_fsr=%08x",
             rbbm, rptr, wptr, hwf, cpi, int0, kmdReadCb0Fsr());
    IOLog("[J716F gpu] [live] %s\n", cpRow);
    IOLog("[J716F gpu] [live] %s\n", stRow);
    if (!ok1 || !ok2 || marker != J716F_GPU_SMOKE_MARKER) return;

    /* Step 4 (Phase 3b): a650_zap.mbn TZ PAS 13 authentication + CP_SET_SECURE_MODE 0 */
    j716f_gpu_step = 135u;
    const J716fScmOps scmOps = {nullptr, scmRawSmc, kmdNowUs, scmSleepMs};
    const J716fZapMemOps zapMemOps = {nullptr, zapPrepareMeta, zapLoadPil};
    unsigned zapLimit = 4u;
    PE_parse_boot_argn("j716f-gpu-zap", &zapLimit, sizeof(zapLimit));
    uint32_t trustBefore = 0xDEADu, trustAfter = 0xDEADu;
    (void)cpGpuRead(nullptr, 0xF400u, &trustBefore); /* RBBM_SECVID_TRUST_CNTL */

    if (zapLimit == 0u || j716f_gpu_zap_firmware_size < 52u) {
        snprintf(zapRow, cap, "ZAP skip limit=%u sz=%u trust=%x", zapLimit, j716f_gpu_zap_firmware_size, trustBefore);
    } else {
        const int zrc = J716fZapAuthenticate(j716f_gpu_zap_firmware, j716f_gpu_zap_firmware_size,
                                             GPB_PIL_GPU_BASE, GPB_PIL_GPU_SIZE,
                                             zapLimit, &scmOps, &zapMemOps, &j716f_gpu_zap);
        j716f_gpu_zap.trust_before = trustBefore;
        int32_t secRc = -99;
        if (zrc == J716F_ZAP_OK && zapLimit >= 4u) {
            uint32_t secWords[2] = {};
            if (AdrenoPM4Type7(0x66u, 1u, &secWords[0]) == AdrenoPM4Success && kmdSubmitAndWait(secWords, 2u, nullptr)) {
                secRc = 0;
            } else {
                secRc = -1;
            }
        }
        (void)cpGpuRead(nullptr, 0xF400u, &trustAfter);
        j716f_gpu_zap.trust_after = trustAfter;
        j716f_gpu_zap.sec_fence_rc = secRc;
        const J716fZapReport &z = j716f_gpu_zap;
        snprintf(zapRow, cap,
                 "ZAP rc=%d stg=%u init=%d(%ums/%ur) mem=%d auth=%d(%ums) sec=%d trust=%x>%x",
                 zrc, z.stage, z.init_rc, z.init_ms, z.init_resumes, z.mem_rc,
                 z.auth_rc, z.auth_ms, z.sec_fence_rc, z.trust_before, z.trust_after);
    }
    IOLog("[J716F gpu] [live] %s\n", zapRow);

    /* Step 5 (Phase 3b): CP_MEMCPY 1023-dword DMA check + SPI 300 (INTID 332) IRQ + CP aperture SCM */
    j716f_gpu_step = 140u;
    for (unsigned i = 1; i < 1024u; ++i) scratch[i] = 0u;
    __asm__ volatile("dsb sy" ::: "memory");
    int memcpyMatched = -1;
    uint32_t mcWords[7] = {};
    if (AdrenoPM4Type7(0x75u, 5u, &mcWords[0]) == AdrenoPM4Success &&
        AdrenoPM4Type7(0x12u, 0u, &mcWords[6]) == AdrenoPM4Success) {
        const uint64_t src = gKmdBufs[0].iova;
        const uint64_t dst = gKmdBufs[4].iova + 4u;
        mcWords[1] = 1023u;
        mcWords[2] = static_cast<uint32_t>(src);
        mcWords[3] = static_cast<uint32_t>(src >> 32);
        mcWords[4] = static_cast<uint32_t>(dst);
        mcWords[5] = static_cast<uint32_t>(dst >> 32);
        if (kmdSubmitAndWait(mcWords, 7u, nullptr)) {
            __asm__ volatile("dsb sy" ::: "memory");
            const volatile uint32_t *sqeWords = reinterpret_cast<const volatile uint32_t *>(gKmdWcCpu[0]);
            memcpyMatched = 0;
            for (unsigned i = 0; i < 1023u; ++i) {
                memcpyMatched += (scratch[1u + i] == sqeWords[i]) ? 1 : 0;
            }
        }
    }

    uint32_t irqReadback = 0;
    uint64_t irqRouter = ~0ULL;
    const IOReturn irqAtt = armcpu_spi_attach
        ? armcpu_spi_attach(GPB_GPU_IRQ_SPI + 32u, false, gpuIrqHandler, nullptr, &irqReadback, &irqRouter)
        : kIOReturnUnsupported;
    gIrqAttached = (irqAtt == kIOReturnSuccess);
    IOSleep(20);
    const uint32_t irqAfterAttach = gIrqCount;
    uint32_t nopWord = 0;
    const bool nopOk = (AdrenoPM4Type7(0x10u, 0u, &nopWord) == AdrenoPM4Success) && kmdSubmitAndWait(&nopWord, 1u, nullptr);
    IOSleep(20);
    const uint32_t irqAfterFence = gIrqCount;
    uint32_t int0Now = 0;
    (void)cpGpuRead(nullptr, 0x0201u, &int0Now);

    uint32_t apAvail = 0;
    int32_t apRc = -1;
    (void)J716fScmOpenCpAperture(&scmOps, 0u, &apAvail, &apRc);
    if (&native_adreno_cp_aperture) {
        native_adreno_cp_aperture[0] = apAvail;
        native_adreno_cp_aperture[1] = static_cast<uint32_t>(apRc);
    }

    snprintf(irqRow, cap,
             "IRQ att=%x gicd=%x n=%u>%u%c seen=%08x now=%08x mc=%d/1023 ap=%x/%d unh=%llu",
             static_cast<unsigned>(irqAtt), irqReadback, irqAfterAttach, irqAfterFence,
             nopOk ? '+' : '-', gIrqSeen, int0Now, memcpyMatched, apAvail, apRc,
             static_cast<unsigned long long>(&armcpu_spi_unhandled ? armcpu_spi_unhandled : 0ULL));
    IOLog("[J716F gpu] [live] %s\n", irqRow);

    if (gMode < 11u) {
        j716f_gpu_step = 150u;
        return;
    }

    /* Step 6 (Phase 4a): Full A650 KMD publication + CP_SMMU_TABLE_UPDATE check + 2x IOUserClient rounds */
    j716f_gpu_step = 160u;
    if (!native_adreno_publish || !native_adreno_published_service || !native_adreno_publish_stats) {
        snprintf(kmdRow, cap, "KMD nolink");
        return;
    }
    NativeAdrenoVm *vm = NativeAdrenoGpuContextVm();
    if (!vm) {
        snprintf(kmdRow, cap, "KMD novm");
        return;
    }

    const NativeAdrenoTimeIo timeIo = {nullptr, kmdTimerRead};
    const IOReturn timerRc = gKmdTimer.attach(&timeIo, NativeAdrenoTimeCp);
    const IOReturn subAtt = gKmdSubmit.attach(vm, &gKmdRing);
    if (subAtt != kIOReturnSuccess) {
        snprintf(kmdRow, cap, "KMD submit_attach=%x", static_cast<unsigned>(subAtt));
        return;
    }

    if (&native_adreno_io_coherent) native_adreno_io_coherent = 0u;
    if (&native_adreno_owned_coherent) native_adreno_owned_coherent = 0u;
    if (&native_adreno_import_rw_coherent) native_adreno_import_rw_coherent = 0u;
    runIoCoherenceProbe(vm);
    if (&native_adreno_io_coherent && native_adreno_io_coherent) {
        gA650DeviceInfo.flags |= NativeAdrenoDeviceCachedCoherent;
        if (&native_adreno_import_rw_coherent) {
            unsigned rwCoh = 0u;
            PE_parse_boot_argn("gpu-import-rw-coherent", &rwCoh, sizeof(rwCoh));
            native_adreno_import_rw_coherent = rwCoh ? 1u : 0u;
        }
    }

    /* Register unused TB-J716F UEFI DRAM strictly outside XNU's owned window as dedicated
     * Write-Combined GPU VRAM pool via /chosen/darwinboot-memory-outside (populated by
     * DarwinBoot after ExitBootServices). Never hardcode 0xD0000000 when XNU's owned window
     * starts at 0xD0000000 (Stage B 4.25 GiB merged High RAM). */
    bool poolActive = false;
    if (native_adreno_pool_add) {
        unsigned poolMode = 2u;
        PE_parse_boot_argn("gpu-pool", &poolMode, sizeof(poolMode));
        if (poolMode != 0u) {
            bool foundOutsideTable = false;
            IORegistryEntry *chosen = IORegistryEntry::fromPath("/chosen", gIODTPlane);
            if (chosen) {
                OSData *table = OSDynamicCast(OSData, chosen->getProperty("darwinboot-memory-outside"));
                if (table && table->getLength() >= 16u) {
                    foundOutsideTable = true;
                    const uint8_t *bytes = static_cast<const uint8_t *>(table->getBytesNoCopy());
                    static const struct { uint64_t lo, hi; } kAllowedDram[3] = {
                        {0x90500000ULL, 0x9BC00000ULL},   /* Low RAM 0 (183 MiB) */
                        {0xB0080000ULL, 0xBCC00000ULL},   /* Low RAM 1 (203.5 MiB) */
                        {0xC0000000ULL, 0x1F8000000ULL},  /* High bank up to 0x1F8000000 */
                    };
                    for (unsigned off = 0; off + 16u <= table->getLength(); off += 16u) {
                        uint64_t base = 0, size = 0;
                        memcpy(&base, bytes + off, 8);
                        memcpy(&size, bytes + off + 8, 8);
                        if (!size || size > UINT64_MAX - base) continue;
                        const uint64_t end = base + size;
                        for (const auto &a : kAllowedDram) {
                            const uint64_t lo = base > a.lo ? base : a.lo;
                            uint64_t hi = end < a.hi ? end : a.hi;
                            if (hi <= lo || (hi - lo) < (1ULL << 20)) continue;
                            /* Carve one 64 MiB, 8 MiB-aligned <4 GiB DMA arena for QCA6390 PCIe0 MHI/BHIe + QMI
                             * (needs 32-bit aligned buffers for amss20.bin, IPCR rings, m3.bin, and Q6 DDR/MSA). */
                            if (!gJ716fWcnDmaBytes && hi <= 0x100000000ULL && (hi - lo) >= 0x5000000ULL) {
                                const uint64_t arenaEnd = hi & ~0x7FFFFFULL;
                                if (arenaEnd >= lo + 0x4800000ULL) {
                                    gJ716fWcnDmaPa = arenaEnd - 0x4000000ULL;
                                    gJ716fWcnDmaBytes = 0x4000000ULL;
                                    hi = gJ716fWcnDmaPa;
                                    IOLog("[J716F gpu] reserved WCN 32-bit DMA arena 0x%llx + 0x%llx\n",
                                          static_cast<unsigned long long>(gJ716fWcnDmaPa),
                                          static_cast<unsigned long long>(gJ716fWcnDmaBytes));
                                }
                            }
                            if (hi <= lo || (hi - lo) < (1ULL << 20)) continue;
                            const IOReturn prc = native_adreno_pool_add(lo, hi - lo);
                            IOLog("[J716F gpu] pool add(out) 0x%llx + 0x%llx -> 0x%x\n",
                                  static_cast<unsigned long long>(lo),
                                  static_cast<unsigned long long>(hi - lo),
                                  static_cast<unsigned>(prc));
                            if (prc == kIOReturnSuccess) poolActive = true;
                        }
                    }
                }
                chosen->release();
            }
            if (!foundOutsideTable && PE_state.bootArgs &&
                reinterpret_cast<const boot_args *>(PE_state.bootArgs)->physBase >= 0xF8000000ULL) {
                static const struct { uint64_t base; uint64_t bytes; } kLegacyPoolRuns[2] = {
                    {0xD0000000ULL, 0x20000000ULL},
                    {0x1E0000000ULL, 0x18000000ULL},
                };
                for (const auto &r : kLegacyPoolRuns) {
                    const IOReturn prc = native_adreno_pool_add(r.base, r.bytes);
                    IOLog("[J716F gpu] pool add(legacy) 0x%llx + 0x%llx -> 0x%x\n",
                          static_cast<unsigned long long>(r.base),
                          static_cast<unsigned long long>(r.bytes),
                          static_cast<unsigned>(prc));
                    if (prc == kIOReturnSuccess) poolActive = true;
                }
            }
        }
    }

    if (&native_adreno_wc_enabled) {
        /* Default WC ON when dedicated NativeAdrenoPool is active (true WC mapping, zero dc cvac/civac);
         * fall back to safe cached BO + explicit dc cvac when pool is disabled. */
        unsigned wc = poolActive ? 1u : 0u;
        PE_parse_boot_argn("gpu-wc", &wc, sizeof(wc));
        native_adreno_wc_enabled = wc ? 1u : 0u;
    }

    gA650DeviceInfo.va_size = (ias > 32u && ias <= 48u) ? ((UINT64_C(1) << ias) - GpuClientVaStart) : 0ULL;
    const NativeAdrenoRingClock kmdClock = {nullptr, kmdNowUs, kmdRingIdle};
    const IOReturn pubRc = native_adreno_publish(vm, &gKmdSubmit, &gKmdRing, &kmdClock,
                                                 timerRc == kIOReturnSuccess ? &gKmdTimer : nullptr,
                                                 &gA650DeviceInfo, ias, 36u);
    snprintf(kmdRow, cap,
             "KMD pub=%x tm=%u cpchk=%x/%x/%x/%x/%x ap2=%x tmr=%x",
             static_cast<unsigned>(pubRc),
             &native_adreno_cp_aperture ? native_adreno_cp_aperture[3] : 0u,
             &native_adreno_cp_check ? native_adreno_cp_check[0] : 0xDEADu,
             &native_adreno_cp_check ? native_adreno_cp_check[1] : 0xDEADu,
             &native_adreno_cp_check ? native_adreno_cp_check[2] : 0xDEADu,
             &native_adreno_cp_check ? native_adreno_cp_check[3] : 0xDEADu,
             &native_adreno_cp_check ? native_adreno_cp_check[4] : 0xDEADu,
             &native_adreno_cp_aperture ? native_adreno_cp_aperture[2] : 0xDEADu,
             static_cast<unsigned>(timerRc));
    IOLog("[J716F gpu] [live] %s\n", kmdRow);

    IOService *svc = (pubRc == kIOReturnSuccess) ? native_adreno_published_service() : nullptr;
    if (svc) {
        j716f_gpu_step = 170u;
        int ok = 0, n = 0, errAt = -1;
        IOReturn err = kIOReturnSuccess;
        uint64_t ts1 = 0, ts2 = 0;
        const int r1 = kmdWrapperRound(svc, 0x0BADC0DEu, ok, n, err, errAt, ts1);
        const int r2 = kmdWrapperRound(svc, 0xA650BEEFu, ok, n, err, errAt, ts2);
        uint32_t closed = 0;
        const uint32_t opened = native_adreno_publish_stats(&closed);
        snprintf(ucRow, cap,
                 "UCWRAP r1=%d/4 r2=%d/4 ok=%d/%d err=%x@%d ts=%llu>%llu o=%u/%u fsr=%08x",
                 r1, r2, ok, n, static_cast<unsigned>(err), errAt,
                 static_cast<unsigned long long>(ts1), static_cast<unsigned long long>(ts2),
                 opened, closed, kmdReadCb0Fsr());
        IOLog("[J716F gpu] [live] %s\n", ucRow);
    }

    unsigned metalOpt = 0u;
    PE_parse_boot_argn("j716f-gpu-metal", &metalOpt, sizeof(metalOpt));
    if (metalOpt && pubRc == kIOReturnSuccess) {
        publishA650MetalAccelerator();
    } else {
        snprintf(j716f_gpu_rows[kRowMetal], cap, "METAL dormant (set j716f-gpu-metal=1 with gpuaddon.bin)");
    }
    j716f_gpu_step = 180u;
}

void probeThread(void *, wait_result_t) {
    // When j716f-gpu-metal=1 is active, register AGXAccelerator early (t+0.8s) so
    // backboardd discovers NativeAdrenoMetal on its first Metal initialization.
    unsigned metalOpt = 0u;
    PE_parse_boot_argn("j716f-gpu-metal", &metalOpt, sizeof(metalOpt));
    IOSleep(metalOpt ? 800u : 6000u);
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
    if (gMode == 9u && gColdOk && j716f_gpu_gx.gx_on) {
        runMode9CpSmoke();
    } else if (gMode >= 10u && gColdOk && j716f_gpu_gx.gx_on) {
        runMode10And11Kmd();
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
    int n = snprintf(out, cap, "[usb] GPU: mode=%u step=%u fw=%u sqe=%u zap=%u\r\n",
                     gMode, j716f_gpu_step, j716f_gpu_gmu_firmware_size,
                     j716f_gpu_sqe_firmware_size, j716f_gpu_zap_firmware_size);
    for (unsigned i = 0; i < kGpuRows && n > 0 && static_cast<size_t>(n) < cap; ++i) {
        if (!j716f_gpu_rows[i][0]) continue;
        n += snprintf(out + n, cap - static_cast<size_t>(n), "[usb] GPU[%u]: %s\r\n", i, j716f_gpu_rows[i]);
    }
    if (&native_gpu_addon_files && n > 0 && static_cast<size_t>(n) < cap) {
        n += snprintf(out + n, cap - static_cast<size_t>(n),
                      "[usb] GPU[ADDON]: files=%d wr=%d same=%d fail=%d err=%d m1=%d/%d m2=%d/%d\r\n",
                      native_gpu_addon_files,
                      &native_gpu_addon_written ? native_gpu_addon_written : 0,
                      &native_gpu_addon_same ? native_gpu_addon_same : 0,
                      &native_gpu_addon_failed ? native_gpu_addon_failed : 0,
                      &native_gpu_addon_error ? native_gpu_addon_error : 0,
                      &native_gpu_addon_stage ? native_gpu_addon_stage : 0,
                      &native_gpu_addon_status ? native_gpu_addon_status : -1,
                      &native_gpu_addon2_stage ? native_gpu_addon2_stage : 0,
                      &native_gpu_addon2_status ? native_gpu_addon2_status : -1);
    }
    if (&native_adreno_metal_diag && n > 0 && static_cast<size_t>(n) < cap) {
        const J716fMetalDiagView &d = native_adreno_metal_diag;
        const uint32_t mt = d.marks_total;
        const uint32_t mcnt = mt < 8u ? mt : 8u;
        n += snprintf(out + n, cap - static_cast<size_t>(n),
                      "[usb] GPU[METAL]: open=%u busy=%u bad=%u cls=%u nr=%u(%s) gr=%u calls=%u sel=%x/%x mk=%u(",
                      d.opens, d.busy, d.bad, d.closes, d.name_reads,
                      d.name_reader[0] ? d.name_reader : "-",
                      d.grants, d.calls, d.last_selector, d.last_status, mt);
        for (uint32_t i = 0; i < mcnt && n > 0 && static_cast<size_t>(n) + 12u < cap; ++i) {
            const uint32_t idx = (mt - mcnt + i) & 15u;
            n += snprintf(out + n, cap - static_cast<size_t>(n), "%s%x", i ? "," : "", d.marks[idx]);
        }
        if (n > 0 && static_cast<size_t>(n) + 4u < cap) {
            n += snprintf(out + n, cap - static_cast<size_t>(n), ")\r\n");
        }
        if ((d.opener[0][0] || d.starter[0][0] || d.report[0] || d.report2[0]) &&
            n > 0 && static_cast<size_t>(n) < cap) {
            n += snprintf(out + n, cap - static_cast<size_t>(n),
                          "[usb] GPU[MPLUG]: op='%s,%s,%s' st='%s,%s,%s' r1='%s' r2='%s'\r\n",
                          d.opener[0], d.opener[1], d.opener[2],
                          d.starter[0], d.starter[1], d.starter[2],
                          d.report, d.report2);
        }
    }
    if (&native_adreno_client_text && n > 0 && static_cast<size_t>(n) < cap) {
        for (unsigned i = 0; i < 8u && n > 0 && static_cast<size_t>(n) < cap; ++i) {
            if (!native_adreno_client_text[i].name[0]) continue;
            n += snprintf(out + n, cap - static_cast<size_t>(n),
                          "[usb] GPU[CTXT%u]: %.19s='%.63s'\r\n", i,
                          native_adreno_client_text[i].name, native_adreno_client_text[i].text);
        }
    }
    if (&native_adreno_client_log && n > 0 && static_cast<size_t>(n) < cap) {
        for (unsigned i = 0; i < 8u && n > 0 && static_cast<size_t>(n) < cap; ++i) {
            if (!native_adreno_client_log[i].name[0]) continue;
            const J716fClientRecordView &c = native_adreno_client_log[i];
            n += snprintf(out + n, cap - static_cast<size_t>(n),
                          "[usb] GPU[CLOG%u]: %.19s o=%u r=%u c=%u pk=%uM ref=%x\r\n",
                          i, c.name, c.opens, c.refused, c.closes, c.peak_mib, c.last_refusal);
        }
    }
    if (&native_adreno_metal_askers && n > 0 && static_cast<size_t>(n) < cap) {
        bool any = false;
        for (unsigned i = 0; i < 12u; ++i) if (native_adreno_metal_askers[i].name[0]) { any = true; break; }
        if (any) {
            n += snprintf(out + n, cap - static_cast<size_t>(n), "[usb] GPU[ASK]:");
            for (unsigned i = 0; i < 12u && n > 0 && static_cast<size_t>(n) + 28u < cap; ++i) {
                if (!native_adreno_metal_askers[i].name[0]) continue;
                n += snprintf(out + n, cap - static_cast<size_t>(n), " %.19s(%u)",
                              native_adreno_metal_askers[i].name, native_adreno_metal_askers[i].reads);
            }
            if (n > 0 && static_cast<size_t>(n) + 4u < cap) {
                n += snprintf(out + n, cap - static_cast<size_t>(n), "\r\n");
            }
        }
    }
    if (&native_adreno_metal_rows && n > 0 && static_cast<size_t>(n) < cap) {
        unsigned shown = 0;
        for (unsigned r = 0; r < 48u && shown < 24u && n > 0 && static_cast<size_t>(n) < cap; ++r) {
            if (!native_adreno_metal_rows[r][0]) continue;
            n += snprintf(out + n, cap - static_cast<size_t>(n),
                          "[usb] GPU[MTBL%u]: %.95s\r\n", r, native_adreno_metal_rows[r]);
            ++shown;
        }
    }
    if (gVa[GPB_WIN_GPU] && j716f_gpu_gx.gx_on && n > 0 && static_cast<size_t>(n) < cap) {
        uint32_t rbbm = 0, rptr = 0, wptr = 0, hwf = 0, cpi = 0, int0 = 0;
        uint32_t ib1lo = 0, ib1hi = 0, ib1rem = 0, ib2lo = 0, ib2hi = 0, ib2rem = 0;
        (void)cpGpuRead(nullptr, 0x0210u, &rbbm);
        (void)cpGpuRead(nullptr, 0x0806u, &rptr);
        (void)cpGpuRead(nullptr, 0x0807u, &wptr);
        (void)cpGpuRead(nullptr, 0x0821u, &hwf);
        (void)cpGpuRead(nullptr, 0x0823u, &cpi);
        (void)cpGpuRead(nullptr, 0x0201u, &int0);
        (void)cpGpuRead(nullptr, 0x0928u, &ib1lo);
        (void)cpGpuRead(nullptr, 0x0929u, &ib1hi);
        (void)cpGpuRead(nullptr, 0x092Au, &ib1rem);
        (void)cpGpuRead(nullptr, 0x092Bu, &ib2lo);
        (void)cpGpuRead(nullptr, 0x092Cu, &ib2hi);
        (void)cpGpuRead(nullptr, 0x092Du, &ib2rem);
        uint32_t fsr = 0, farlo = 0, farhi = 0, fsynr0 = 0;
        if (gSmmuFullMap) {
            const volatile uint8_t *sb = reinterpret_cast<const volatile uint8_t *>(gSmmuFullMap->getVirtualAddress());
            fsr = *reinterpret_cast<const volatile uint32_t *>(sb + 0x8058u);
            farlo = *reinterpret_cast<const volatile uint32_t *>(sb + 0x8060u);
            farhi = *reinterpret_cast<const volatile uint32_t *>(sb + 0x8064u);
            fsynr0 = *reinterpret_cast<const volatile uint32_t *>(sb + 0x8068u);
        }
        n += snprintf(out + n, cap - static_cast<size_t>(n),
                      "[usb] GPU[LIVE]: rbbm=%08x rp=%x wp=%x hwf=%08x cpi=%08x int0=%08x irq=%u/%08x/%08x cpe=%u(%x/%08x/%x/%08x) wc=%u\r\n",
                      rbbm, rptr, wptr, hwf, cpi, int0, gIrqCount, gIrqSeen, gIrqLast,
                      gCpErrCount, gCpErrStatus, gCpErrProtect, gCpErrSqePc, gCpErrSqeHdr,
                      &native_adreno_wc_enabled ? native_adreno_wc_enabled : 9u);
        if (n > 0 && static_cast<size_t>(n) < cap) {
            n += snprintf(out + n, cap - static_cast<size_t>(n),
                          "[usb] GPU[LIVE2]: ib1=%08x%08x/%x ib2=%08x%08x/%x fsr=%08x far=%08x%08x syn=%08x\r\n",
                          ib1hi, ib1lo, ib1rem, ib2hi, ib2lo, ib2rem, fsr, farhi, farlo, fsynr0);
        }
    }
    if (gCoherenceRow[0] && n > 0 && static_cast<size_t>(n) < cap) {
        n += snprintf(out + n, cap - static_cast<size_t>(n),
                      "[usb] GPU[COH]: %s | %s | coh=%u owned=%u rwimp=%u skips=%u/%u/%u/%u\r\n",
                      gCoherenceRow, gWalkRow[0] ? gWalkRow : "-",
                      &native_adreno_io_coherent ? native_adreno_io_coherent : 0u,
                      &native_adreno_owned_coherent ? native_adreno_owned_coherent : 0u,
                      &native_adreno_import_rw_coherent ? native_adreno_import_rw_coherent : 0u,
                      &native_adreno_coherent_skips ? native_adreno_coherent_skips[0] : 0u,
                      &native_adreno_coherent_skips ? native_adreno_coherent_skips[1] : 0u,
                      &native_adreno_coherent_skips ? native_adreno_coherent_skips[2] : 0u,
                      &native_adreno_coherent_skips ? native_adreno_coherent_skips[3] : 0u);
    }
    if (&native_adreno_pool_state && native_adreno_pool_state[0] && n > 0 && static_cast<size_t>(n) < cap) {
        n += snprintf(out + n, cap - static_cast<size_t>(n),
                      "[usb] GPU[POOL]: runs=%u tot=%uK free=%uK lo=%uK live=%u(%uK) ok=%u rec=%u ret=%u(%uK) m=%u fb=%u/%u/%u/%u\r\n",
                      native_adreno_pool_state[0], native_adreno_pool_state[1],
                      native_adreno_pool_state[2], native_adreno_pool_state[3],
                      native_adreno_pool_state[4], native_adreno_pool_state[5],
                      native_adreno_pool_state[6], native_adreno_pool_state[7],
                      native_adreno_pool_state[8], native_adreno_pool_state[9],
                      native_adreno_pool_state[10],
                      &native_adreno_pool_fallbacks ? native_adreno_pool_fallbacks[0] : 0u,
                      &native_adreno_pool_fallbacks ? native_adreno_pool_fallbacks[2] : 0u,
                      &native_adreno_pool_fallbacks ? native_adreno_pool_fallbacks[3] : 0u,
                      &native_adreno_pool_fallbacks ? native_adreno_pool_fallbacks[6] : 0u);
    }
    if (&native_adreno_fence_wait && n > 0 && static_cast<size_t>(n) < cap) {
        n += snprintf(out + n, cap - static_cast<size_t>(n),
                      "[usb] GPU[FENCE]: c=%u to=%u last=%u gpu=%u sub=%u done=%u obs=%u bad=%u sw=%u/%u\r\n",
                      native_adreno_fence_wait[0], native_adreno_fence_wait[1],
                      native_adreno_fence_wait[2], native_adreno_fence_wait[3],
                      native_adreno_fence_wait[4], native_adreno_fence_wait[5],
                      native_adreno_fence_wait[6], native_adreno_fence_wait[7],
                      &native_adreno_table_switches ? native_adreno_table_switches[0] : 0u,
                      &native_adreno_table_switches ? native_adreno_table_switches[6] : 0u);
    }
    if (&native_adreno_job_time && native_adreno_job_time[0] && n > 0 && static_cast<size_t>(n) < cap) {
        n += snprintf(out + n, cap - static_cast<size_t>(n),
                      "[usb] GPU[JOB]: n=%u stamp=%u >8ms=%u >16ms=%u >33ms=%u >100ms=%u max=%uus\r\n",
                      native_adreno_job_time[0], native_adreno_job_time[1],
                      native_adreno_job_time[2], native_adreno_job_time[3],
                      native_adreno_job_time[4], native_adreno_job_time[5],
                      native_adreno_job_time[6]);
    }
    if (&native_adreno_wait_outer && native_adreno_wait_outer[0] && n > 0 && static_cast<size_t>(n) < cap) {
        const uint64_t avgUs = native_adreno_wait_outer[1] / native_adreno_wait_outer[0];
        n += snprintf(out + n, cap - static_cast<size_t>(n),
                      "[usb] GPU[WAIT]: n=%llu avg=%lluus max=%lluus pause=%llu fast=%llu | wake irq=%llu/%llu pass=%llu w=%llu slp=%llu to=%llu per=%llu\r\n",
                      static_cast<unsigned long long>(native_adreno_wait_outer[0]),
                      static_cast<unsigned long long>(avgUs),
                      static_cast<unsigned long long>(native_adreno_wait_outer[3]),
                      static_cast<unsigned long long>(native_adreno_wait_outer[2]),
                      static_cast<unsigned long long>(native_adreno_wait_outer[4]),
                      static_cast<unsigned long long>(&native_adreno_fence_wake ? native_adreno_fence_wake[0] : 0ULL),
                      static_cast<unsigned long long>(&native_adreno_fence_wake ? native_adreno_fence_wake[1] : 0ULL),
                      static_cast<unsigned long long>(&native_adreno_fence_wake ? native_adreno_fence_wake[2] : 0ULL),
                      static_cast<unsigned long long>(&native_adreno_fence_wake ? native_adreno_fence_wake[3] : 0ULL),
                      static_cast<unsigned long long>(&native_adreno_fence_wake ? native_adreno_fence_wake[4] : 0ULL),
                      static_cast<unsigned long long>(&native_adreno_fence_wake ? native_adreno_fence_wake[5] : 0ULL),
                      static_cast<unsigned long long>(&native_adreno_fence_wake ? native_adreno_fence_wake[6] : 0ULL));
    }
    if (&native_adreno_submit_us && native_adreno_submit_us[0] && n > 0 && static_cast<size_t>(n) < cap) {
        const uint64_t cnt = native_adreno_submit_us[0];
        n += snprintf(out + n, cap - static_cast<size_t>(n),
                      "[usb] GPU[SUB]: n=%llu avg=%lluus (val=%llu wc=%llu pub=%llu log=%llu ring=%llu)\r\n",
                      static_cast<unsigned long long>(cnt),
                      static_cast<unsigned long long>(native_adreno_submit_us[6] / cnt),
                      static_cast<unsigned long long>(native_adreno_submit_us[1] / cnt),
                      static_cast<unsigned long long>(native_adreno_submit_us[2] / cnt),
                      static_cast<unsigned long long>(native_adreno_submit_us[3] / cnt),
                      static_cast<unsigned long long>(native_adreno_submit_us[4] / cnt),
                      static_cast<unsigned long long>(native_adreno_submit_us[5] / cnt));
    }
    if (&native_adreno_stall && native_adreno_stall[0] && n > 0 && static_cast<size_t>(n) < cap) {
        n += snprintf(out + n, cap - static_cast<size_t>(n),
                      "[usb] GPU[STALL]: f=%u to=%u rbbm=%08x int0=%08x hwf=%08x rp=%x wp=%x ib1=%08x%08x/%x\r\n",
                      native_adreno_stall[0], native_adreno_stall[1],
                      native_adreno_stall[2], native_adreno_stall[6], native_adreno_stall[7],
                      native_adreno_stall[10], native_adreno_stall[11],
                      native_adreno_stall[13], native_adreno_stall[12], native_adreno_stall[14]);
        if (&native_adreno_stall_mem && n > 0 && static_cast<size_t>(n) < cap) {
            n += snprintf(out + n, cap - static_cast<size_t>(n),
                          "[usb] GPU[SMEM]: fsr=%08x far=%08x%08x syn=%08x ib1[%u]=%08x %08x %08x %08x %08x %08x\r\n",
                          native_adreno_stall_mem[0], native_adreno_stall_mem[2],
                          native_adreno_stall_mem[1], native_adreno_stall_mem[3],
                          native_adreno_stall_mem[4], native_adreno_stall_mem[5],
                          native_adreno_stall_mem[6], native_adreno_stall_mem[7],
                          native_adreno_stall_mem[8], native_adreno_stall_mem[9],
                          native_adreno_stall_mem[10]);
        }
    }
    if (&native_adreno_method_stats && n > 0 && static_cast<size_t>(n) < cap) {
        n += snprintf(out + n, cap - static_cast<size_t>(n), "[usb] GPU[METH]:");
        for (unsigned m = 1u; m <= 18u && n > 0 && static_cast<size_t>(n) + 28u < cap; ++m) {
            if (!native_adreno_method_stats[m][0]) continue;
            n += snprintf(out + n, cap - static_cast<size_t>(n), " %u=%u/%u(%x)",
                          m, native_adreno_method_stats[m][0],
                          native_adreno_method_stats[m][1], native_adreno_method_stats[m][2]);
        }
        if (n > 0 && static_cast<size_t>(n) + 2u < cap) {
            n += snprintf(out + n, cap - static_cast<size_t>(n), "\r\n");
        }
    }
    if (native_gpu_trace_enabled() && n > 0 && static_cast<size_t>(n) < cap) {
        static uint8_t traceBuf[sizeof(NativeGpuTraceHeader) + 256u * sizeof(NativeGpuTraceRecord)];
        const uint32_t got = native_gpu_trace_snapshot(traceBuf, static_cast<uint32_t>(sizeof(traceBuf)));
        if (got >= sizeof(NativeGpuTraceHeader)) {
            NativeGpuTraceHeader hdr = {};
            memcpy(&hdr, traceBuf, sizeof(hdr));
            n += snprintf(out + n, cap - static_cast<size_t>(n),
                          "[usb] GPU[TRACE]: on=1 issued=%llu dropped=%llu cnt=%u\r\n",
                          static_cast<unsigned long long>(hdr.issued),
                          static_cast<unsigned long long>(hdr.dropped), hdr.count);
            const uint32_t maxRecs = (got - static_cast<uint32_t>(sizeof(hdr))) /
                                     static_cast<uint32_t>(sizeof(NativeGpuTraceRecord));
            const uint32_t count = hdr.count < maxRecs ? hdr.count : maxRecs;
            unsigned shown = 0;
            for (uint32_t i = 0; i < count && shown < 48u && n > 0 && static_cast<size_t>(n) + 128u < cap; ++i) {
                NativeGpuTraceRecord rec = {};
                memcpy(&rec, traceBuf + sizeof(hdr) + i * sizeof(rec), sizeof(rec));
                if (rec.event != GpuTraceProblem) continue;
                char txt[89];
                memcpy(txt, &rec.value[1], 88u);
                txt[88] = '\0';
                for (unsigned k = 0; k < 88u && txt[k]; ++k) {
                    const unsigned char ch = static_cast<unsigned char>(txt[k]);
                    if (ch < 0x20u || ch > 0x7Eu) txt[k] = ' ';
                }
                n += snprintf(out + n, cap - static_cast<size_t>(n),
                              "[usb] GPU[TPROB%u]: pid=%u p=%llu '%s'\r\n",
                              shown, rec.pid, static_cast<unsigned long long>(rec.value[0]), txt);
                ++shown;
            }
        }
    }
}

extern "C" void j716f_gpu_probe(void) {
    static int started = 0;
    unsigned mode = 0;
    if (!PE_parse_boot_argn("j716f-gpu", &mode, sizeof(mode)) || !mode || mode > 11u) {
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
