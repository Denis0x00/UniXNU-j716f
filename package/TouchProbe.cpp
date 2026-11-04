// SPDX-License-Identifier: BSD-2-Clause-Patent
// Native Goodix GT7386 (goodix,gt738x) GENI I2C touchscreen driver for Lenovo TB-J716F (SM8250).
// Supports both GENI FIFO mode and SE-DMA mode (with SMMU SID 0x43 pass-through).
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <string.h>
#include <libkern/c++/OSSharedPtr.h>
#include <kern/thread.h>
#include <kern/clock.h>
#include <pexpert/pexpert.h>

extern "C" {
#include "TouchBoard.h"
#include "QcomRpmh.h"
#include "SmmuBypass.h"

// Shared touch state consumed by NativeHidInput (TouchState.cpp)
extern volatile uint32_t native_touch_seq, native_touch_count, native_touch_hid_reports;
extern volatile int native_touch_xs[12], native_touch_ys[12], native_touch_ids[12];
void native_touch_published(void);

// Diagnostic storage defined in LinkStubs.c and read by BootRamoops.c
extern char native_touch_rows[4][120];
extern volatile unsigned native_touch_stage;
extern volatile unsigned native_touch_stage_req;
extern volatile unsigned native_touch_stroke_count;
extern volatile int native_touch_strokes[16][7];
extern char native_touch_trace[8][120];
extern volatile int native_touch_extent[4];

// Coordination flag with Buttons.c VolUp+VolDown swipe gesture backup
volatile uint32_t native_touch_injecting = 0;
}

namespace {

// GENI I2C & SE-DMA register offsets (within 16 KiB SE window)
constexpr uint32_t kGeniForceDefault  = 0x020u;
constexpr uint32_t kGeniOutputCtrl    = 0x024u;
constexpr uint32_t kGeniCgcCtrl       = 0x028u;
constexpr uint32_t kGeniStatus        = 0x040u;
constexpr uint32_t kGeniClkCfg        = 0x048u;
constexpr uint32_t kGeniIfDisable     = 0x064u;
constexpr uint32_t kGeniFwRevision    = 0x068u;
constexpr uint32_t kGeniClkSel        = 0x07cu;
constexpr uint32_t kGeniByteGran      = 0x254u;
constexpr uint32_t kGeniDmaModeEn     = 0x258u;
constexpr uint32_t kGeniTxPack0       = 0x260u;
constexpr uint32_t kGeniTxPack1       = 0x264u;
constexpr uint32_t kGeniTxLen         = 0x26cu;
constexpr uint32_t kGeniRxLen         = 0x270u;
constexpr uint32_t kGeniSclCounters   = 0x278u;
constexpr uint32_t kGeniRxPack0       = 0x284u;
constexpr uint32_t kGeniRxPack1       = 0x288u;
constexpr uint32_t kGeniMCmd0         = 0x600u;
constexpr uint32_t kGeniMCmdCtrl      = 0x604u;
constexpr uint32_t kGeniMIrqStatus    = 0x610u;
constexpr uint32_t kGeniMIrqEn        = 0x614u;
constexpr uint32_t kGeniMIrqClear     = 0x618u;
constexpr uint32_t kGeniSIrqClear     = 0x648u;
constexpr uint32_t kGeniTxFifo        = 0x700u;
constexpr uint32_t kGeniRxFifo        = 0x780u;
constexpr uint32_t kGeniRxFifoStatus  = 0x804u;
constexpr uint32_t kGeniTxWatermark   = 0x80cu;
constexpr uint32_t kGeniRxWatermark   = 0x810u;
constexpr uint32_t kGeniRxRfrWatermark= 0x814u;
constexpr uint32_t kSeDmaTxPtrL       = 0xc30u;
constexpr uint32_t kSeDmaTxPtrH       = 0xc34u;
constexpr uint32_t kSeDmaTxAttr       = 0xc38u;
constexpr uint32_t kSeDmaTxLen        = 0xc3cu;
constexpr uint32_t kSeDmaTxIrqStat    = 0xc40u;
constexpr uint32_t kSeDmaTxIrqClr     = 0xc44u;
constexpr uint32_t kSeDmaTxIrqEnSet   = 0xc4cu;
constexpr uint32_t kSeDmaTxFsmRst     = 0xc58u;
constexpr uint32_t kSeDmaRxPtrL       = 0xd30u;
constexpr uint32_t kSeDmaRxPtrH       = 0xd34u;
constexpr uint32_t kSeDmaRxAttr       = 0xd38u;
constexpr uint32_t kSeDmaRxLen        = 0xd3cu;
constexpr uint32_t kSeDmaRxIrqStat    = 0xd40u;
constexpr uint32_t kSeDmaRxIrqClr     = 0xd44u;
constexpr uint32_t kSeDmaRxIrqEnSet   = 0xd4cu;
constexpr uint32_t kSeDmaRxFsmRst     = 0xd58u;
constexpr uint32_t kSeGsiEventEn      = 0xe18u;
constexpr uint32_t kSeIrqEn           = 0xe1cu;
constexpr uint32_t kSeHwParam0        = 0xe24u;
constexpr uint32_t kSeDmaGeneralCfg   = 0xe30u;

// GENI M_IRQ & command constants
constexpr uint32_t kMCmdDone          = (1u << 0);
constexpr uint32_t kMCmdOverrun       = (1u << 1);
constexpr uint32_t kMIllegalCmd       = (1u << 2);
constexpr uint32_t kMCmdFailure       = (1u << 3);
constexpr uint32_t kMCmdCancelDone    = (1u << 4);
constexpr uint32_t kMCmdAbortDone     = (1u << 5);
constexpr uint32_t kMGpIrq            = (0x3fu << 9);  // I2C NACK / bus error / arb lost
constexpr uint32_t kMRxFifoRdErr      = (1u << 24);
constexpr uint32_t kMRxFifoWrErr      = (1u << 25);
constexpr uint32_t kMRxWatermark      = (1u << 26);
constexpr uint32_t kMRxLast           = (1u << 27);
constexpr uint32_t kMTxFifoRdErr      = (1u << 28);
constexpr uint32_t kMTxFifoWrErr      = (1u << 29);
constexpr uint32_t kMTxWatermark      = (1u << 30);
constexpr uint32_t kMErrors = kMCmdOverrun | kMIllegalCmd | kMCmdFailure | kMGpIrq |
                              kMRxFifoRdErr | kMRxFifoWrErr | kMTxFifoRdErr | kMTxFifoWrErr;

constexpr uint32_t kCmdCtrlAbort      = (1u << 1);
constexpr uint32_t kCmdCtrlCancel     = (1u << 2);
constexpr uint32_t kI2cOpWrite        = 1u;
constexpr uint32_t kI2cOpRead         = 2u;
constexpr uint32_t kI2cStopStretch    = (1u << 2);
constexpr uint32_t kDmaDone           = (1u << 0);
constexpr uint32_t kDmaEot            = (1u << 1);
constexpr uint32_t kDmaAhbErr         = (1u << 2);

constexpr size_t kDmaBufHalf          = 4096;  // 8 KiB total (4 KiB TX + 4 KiB RX)

struct Region { OSSharedPtr<IOMemoryMap> keep; uintptr_t va = 0; };

bool mapRegion(uint64_t phys, uint64_t length, Region &region) {
    IODeviceMemory *memory = IODeviceMemory::withRange(phys, length);
    if (!memory) return false;
    region.keep = OSSharedPtr<IOMemoryMap>(memory->map(kIOMapAnywhere | kIOMapInhibitCache), OSNoRetain);
    memory->release();
    region.va = region.keep ? static_cast<uintptr_t>(region.keep->getVirtualAddress()) : 0;
    return region.va != 0;
}

inline uint32_t rd32(uintptr_t base, uint32_t reg) {
    return *reinterpret_cast<volatile uint32_t *>(base + reg);
}

inline void wr32(uintptr_t base, uint32_t reg, uint32_t value) {
    *reinterpret_cast<volatile uint32_t *>(base + reg) = value;
    __asm__ volatile("dsb sy" ::: "memory");
}

void cacheRange(void *p, size_t len, int op) { // 0 clean, 1 clean+inval, 2 inval
    uint64_t ctr = 0;
    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
    size_t line = 4u << ((ctr >> 16) & 15u);
    uintptr_t a = reinterpret_cast<uintptr_t>(p) & ~(line - 1u);
    uintptr_t end = reinterpret_cast<uintptr_t>(p) + len;
    for (; a < end; a += line) {
        if (op == 0) __asm__ volatile("dc cvac, %0" :: "r"(a) : "memory");
        else if (op == 1) __asm__ volatile("dc civac, %0" :: "r"(a) : "memory");
        else __asm__ volatile("dc ivac, %0" :: "r"(a) : "memory");
    }
    __asm__ volatile("dsb sy" ::: "memory");
}

uint32_t smmuRd(void *ctx, unsigned o) { return rd32(reinterpret_cast<uintptr_t>(ctx), o); }
void smmuWr(void *ctx, unsigned o, uint32_t v) { wr32(reinterpret_cast<uintptr_t>(ctx), o, v); }

struct TouchPoint {
    int id;
    int x;
    int y;
};

struct Session {
    Region gccLow, gccQup1, gccCore, gccVote, tlmmPins, wrapper, se, smmu, cmdDb, rscDrv;
    IOBufferMemoryDescriptor *dmaMem = nullptr;
    uint8_t *dmaTx = nullptr;
    uint8_t *dmaRx = nullptr;
    uint64_t dmaTxPhys = 0;
    uint64_t dmaRxPhys = 0;
    bool useDma = false;
    unsigned txFifoDepth = 16;
    unsigned clkSel = 0;
    uint8_t slaveAddr = GTX8_ADDR_PRIMARY;
    unsigned step = 0;
    int err = 0;
    int rpmhRc[3] = {0, 0, 0};
    int smmuSlot = -1, smmuBank = -1, smmuErr = 0;
    uint32_t qupVer = 0, fwRev = 0, ifDis = 0, hwParam0 = 0, clkCfg = 0, sclCfg = 0;
    uint32_t voteReg = 0, cbcr[5] = {};
    uint32_t pinCfg[4] = {};
    uint8_t verBuf[12] = {};
    char pidStr[8] = "-";
    uint32_t lastIrq = 0, lastDmaTx = 0, lastDmaRx = 0;
    uint32_t polls = 0, readyFrames = 0, validFrames = 0, badCsum = 0, i2cErrors = 0, irqEdges = 0;
    int lastX = -1, lastY = -1, lastContacts = 0;
    uint8_t lastRaw[16] = {};
    unsigned flipX = 0, flipY = 0, swapXY = 0;
};

struct Stroke { int id, x0, y0, x1, y1, frames; uint32_t t0; };
Stroke gStrokes[10];
unsigned gStrokeCount = 0;

uint32_t touchMs() {
    uint64_t f = 0;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f));
    return static_cast<uint32_t>(mach_absolute_time() / ((f ? f : 19200000ULL) / 1000ULL));
}

void trackStrokes(unsigned count, const int *ids, const int *xs, const int *ys) {
    const uint32_t now = touchMs();
    for (unsigned i = 0; i < count; ++i) {
        if (xs[i] < native_touch_extent[0]) native_touch_extent[0] = xs[i];
        if (xs[i] > native_touch_extent[1]) native_touch_extent[1] = xs[i];
        if (ys[i] < native_touch_extent[2]) native_touch_extent[2] = ys[i];
        if (ys[i] > native_touch_extent[3]) native_touch_extent[3] = ys[i];
        unsigned k = 0;
        while (k < gStrokeCount && gStrokes[k].id != ids[i]) ++k;
        if (k == gStrokeCount) {
            if (gStrokeCount == 10) continue;
            gStrokes[gStrokeCount++] = {ids[i], xs[i], ys[i], xs[i], ys[i], 0, now};
        }
        gStrokes[k].x1 = xs[i];
        gStrokes[k].y1 = ys[i];
        ++gStrokes[k].frames;
    }
    for (unsigned k = 0; k < gStrokeCount;) {
        bool live = false;
        for (unsigned i = 0; i < count && !live; ++i) live = (ids[i] == gStrokes[k].id);
        if (live) { ++k; continue; }
        volatile int *r = native_touch_strokes[native_touch_stroke_count & 15u];
        r[0] = static_cast<int>(gStrokes[k].t0);
        r[1] = static_cast<int>(now - gStrokes[k].t0);
        r[2] = gStrokes[k].x0; r[3] = gStrokes[k].y0;
        r[4] = gStrokes[k].x1; r[5] = gStrokes[k].y1;
        r[6] = gStrokes[k].frames;
        native_touch_stroke_count = native_touch_stroke_count + 1;
        gStrokes[k] = gStrokes[--gStrokeCount];
    }
}

void updateDiag(const Session &s) {
    snprintf(native_touch_rows[0], sizeof native_touch_rows[0],
        "J716F-GT7386 stg%u/%u stp%u err%d rpmh(%d,%d,%d) vote%08x cbcr(%08x,%08x,%08x)",
        native_touch_stage, native_touch_stage_req, s.step, s.err,
        s.rpmhRc[0], s.rpmhRc[1], s.rpmhRc[2], s.voteReg, s.cbcr[0], s.cbcr[2], s.cbcr[4]);
    snprintf(native_touch_rows[1], sizeof native_touch_rows[1],
        "TCH se fw%08x if%x hw%08x dma%u sm(%d/%d/e%d) clk(%u,%04x,%08x) pin(%03x,%03x,%03x,%03x)",
        s.fwRev, s.ifDis, s.hwParam0, s.useDma ? 1u : 0u, s.smmuSlot, s.smmuBank, s.smmuErr,
        s.clkSel, s.clkCfg & 0xffffu, s.sclCfg, s.pinCfg[0], s.pinCfg[1], s.pinCfg[2], s.pinCfg[3]);
    snprintf(native_touch_rows[2], sizeof native_touch_rows[2],
        "TCH addr0x%02x pid'%s' rdy%u ok%u bad%u i2ce%u irqe%u c%d (%d,%d) hid%u",
        s.slaveAddr, s.pidStr, s.readyFrames, s.validFrames, s.badCsum, s.i2cErrors, s.irqEdges,
        s.lastContacts, s.lastX, s.lastY, native_touch_hid_reports);
    snprintf(native_touch_rows[3], sizeof native_touch_rows[3],
        "TCH lastIrq%08x dTx%x dRx%x ver(%02x%02x%02x%02x_%02x%02x%02x%02x) raw(%02x%02x_%02x%02x%02x%02x%02x)",
        s.lastIrq, s.lastDmaTx, s.lastDmaRx,
        s.verBuf[0], s.verBuf[1], s.verBuf[2], s.verBuf[3], s.verBuf[4], s.verBuf[5], s.verBuf[6], s.verBuf[7],
        s.lastRaw[0], s.lastRaw[1], s.lastRaw[2], s.lastRaw[3], s.lastRaw[4], s.lastRaw[5], s.lastRaw[6]);
    snprintf(native_touch_trace[3], sizeof native_touch_trace[3],
        "TCH polls%u rdy%u ok%u bad%u raw %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
        s.polls, s.readyFrames, s.validFrames, s.badCsum,
        s.lastRaw[0], s.lastRaw[1], s.lastRaw[2], s.lastRaw[3], s.lastRaw[4],
        s.lastRaw[5], s.lastRaw[6], s.lastRaw[7], s.lastRaw[8], s.lastRaw[9]);
}

int voteRpmhRail(uintptr_t dbVa, uintptr_t tcsVa, const QcomRscLayout &layout, const char *name, uint32_t mv) {
    QcomRpmhResource res = {};
    int r = qcom_cmd_db_find(reinterpret_cast<const uint8_t *>(dbVa), TB_CMD_DB_SIZE, name, &res);
    if (r != QcomRpmhOk) return r;
    const uint32_t addrs[3] = { res.address + 8u, res.address + 0u, res.address + 4u };
    const uint32_t vals[3]  = { 7u, mv, 1u }; // mode 7 (HPM), voltage mV, enable 1
    QcomRpmhWrite writes[16] = {};
    // Wait briefly if TCS 0 is busy
    for (unsigned i = 0; i < 2000; ++i) {
        if (rd32(tcsVa, layout.status) != 0u) break; // status bit0 == 1 means idle in RSC v2/v3? Check irq/busy safely
        IODelay(5);
    }
    // Clear any stale completion status on TCS 0 before issuing active request
    QcomRpmhWrite fin[8] = {};
    unsigned fn = qcom_rpmh_finish(&layout, 0, fin, 8);
    for (unsigned i = 0; i < fn; ++i) wr32(tcsVa, fin[i].offset, fin[i].value);

    unsigned wn = qcom_rpmh_active_request(&layout, 0, addrs, vals, 3, writes, 16);
    if (!wn) return -10;
    for (unsigned i = 0; i < wn; ++i) wr32(tcsVa, writes[i].offset, writes[i].value);
    bool done = false;
    for (unsigned i = 0; i < 5000; ++i) {
        if (rd32(tcsVa, layout.irq_status) & 1u) { done = true; break; }
        IODelay(5);
    }
    fn = qcom_rpmh_finish(&layout, 0, fin, 8);
    for (unsigned i = 0; i < fn; ++i) wr32(tcsVa, fin[i].offset, fin[i].value);
    IOSleep(2);
    return done ? 0 : -110;
}

void votePowerRails(Session &s) {
    s.step = 1;
    if (!mapRegion(TB_CMD_DB_BASE, TB_CMD_DB_SIZE, s.cmdDb) ||
        !mapRegion(TB_RSC_DRV_BASE, TB_RSC_DRV_SIZE, s.rscDrv)) {
        s.rpmhRc[0] = -12;
        return;
    }
    const uint32_t drvId = rd32(s.rscDrv.va, 0);
    const QcomRscLayout layout = qcom_rsc_layout(drvId);
    const uintptr_t tcsVa = s.rscDrv.va + TB_RSC_TCS_OFFSET;
    // Ensure TCS 0/1 irq status latching is enabled (TCS_IRQ_ENABLE at +0x00 of TCS block)
    wr32(tcsVa, 0u, rd32(tcsVa, 0u) | 3u);
    s.rpmhRc[0] = voteRpmhRail(s.cmdDb.va, tcsVa, layout, "ldoa13", 3008);
    s.rpmhRc[1] = voteRpmhRail(s.cmdDb.va, tcsVa, layout, "ldoa14", 1800);
    s.rpmhRc[2] = voteRpmhRail(s.cmdDb.va, tcsVa, layout, "ldoc1",  1800);
    IOSleep(25);
}

bool enableClocksAndPins(Session &s) {
    s.step = 2;
    if (!mapRegion(TB_GCC_BASE + 0x00000u, 0x1000u, s.gccLow) ||
        !mapRegion(TB_GCC_BASE + 0x18000u, 0x1000u, s.gccQup1) ||
        !mapRegion(TB_GCC_BASE + 0x23000u, 0x1000u, s.gccCore) ||
        !mapRegion(TB_GCC_BASE + 0x52000u, 0x1000u, s.gccVote) ||
        !mapRegion(TB_TLMM_SOUTH_BASE + TB_GPIO_I2C_SDA * TB_GPIO_STRIDE, 4u * TB_GPIO_STRIDE, s.tlmmPins)) {
        s.err = -12;
        return false;
    }

    // Vote GPLL0 & QUPv3 Wrap1 SE5 clocks
    wr32(s.gccVote.va, TB_GCC_GPLL0_ENABLE & 0xfffu, rd32(s.gccVote.va, TB_GCC_GPLL0_ENABLE & 0xfffu) | 1u);
    for (unsigned i = 0; i < 5000 && !(rd32(s.gccLow.va, TB_GCC_GPLL0_MODE) & (1u << 31)); ++i) IODelay(10);

    wr32(s.gccVote.va, TB_GCC_VOTE_ENABLE & 0xfffu, rd32(s.gccVote.va, TB_GCC_VOTE_ENABLE & 0xfffu) | TB_VOTE_ALL);
    auto running = [](uint32_t cbcr) { return !(cbcr & TB_CBCR_CLK_OFF) || (cbcr & 2u); };
    bool ok = false;
    for (unsigned i = 0; i < 3000 && !ok; ++i) {
        s.cbcr[0] = rd32(s.gccQup1.va, TB_HALT_M_AHB & 0xfffu);
        s.cbcr[1] = rd32(s.gccQup1.va, TB_HALT_S_AHB & 0xfffu);
        s.cbcr[2] = rd32(s.gccCore.va, TB_HALT_CORE & 0xfffu);
        s.cbcr[3] = rd32(s.gccCore.va, TB_HALT_CORE_2X & 0xfffu);
        s.cbcr[4] = rd32(s.gccQup1.va, TB_HALT_S5 & 0xfffu);
        ok = running(s.cbcr[0]) && running(s.cbcr[1]) && running(s.cbcr[2]) && running(s.cbcr[3]) && !(s.cbcr[4] & TB_CBCR_CLK_OFF);
        if (!ok) IODelay(10);
    }
    s.voteReg = rd32(s.gccVote.va, TB_GCC_VOTE_ENABLE & 0xfffu);
    if (!ok) { s.err = -110; return false; }

    // Select 19.2 MHz source on S5_RCG (0x18600)
    const uint32_t dfsr = rd32(s.gccQup1.va, (TB_S5_RCG + TB_RCG_DFSR_CMD) & 0xfffu);
    s.clkSel = 0;
    if (dfsr & 1u) {
        for (unsigned l = 0; l < TB_DFS_LEVELS; ++l) {
            uint32_t cfg = rd32(s.gccQup1.va, (TB_S5_RCG + TB_RCG_PERF_CFG(l)) & 0xfffu);
            uint32_t m   = rd32(s.gccQup1.va, (TB_S5_RCG + TB_RCG_PERF_M(l)) & 0xfffu);
            uint32_t n   = rd32(s.gccQup1.va, (TB_S5_RCG + TB_RCG_PERF_N(l)) & 0xfffu);
            if (tb_dfs_level_hz(cfg, m, n) == 19200000ULL) { s.clkSel = l; break; }
        }
    } else {
        uint32_t cfg = rd32(s.gccQup1.va, (TB_S5_RCG + TB_RCG_CFG) & 0xfffu);
        if (tb_dfs_level_hz(cfg, 0, 0) != 19200000ULL) {
            wr32(s.gccQup1.va, (TB_S5_RCG + TB_RCG_CFG) & 0xfffu, 0u); // TCXO 19.2 MHz, div 1
            wr32(s.gccQup1.va, (TB_S5_RCG + TB_RCG_CMD) & 0xfffu, rd32(s.gccQup1.va, (TB_S5_RCG + TB_RCG_CMD) & 0xfffu) | 1u);
            for (unsigned i = 0; i < 1000 && (rd32(s.gccQup1.va, (TB_S5_RCG + TB_RCG_CMD) & 0xfffu) & 1u); ++i) IODelay(5);
        }
    }

    // Configure GPIO 36 (SDA) & GPIO 37 (SCL): func 1 (qup13), 2mA (0), pull-up (3) -> 0x07
    wr32(s.tlmmPins.va, 0u * TB_GPIO_STRIDE, 0x007u);
    wr32(s.tlmmPins.va, 1u * TB_GPIO_STRIDE, 0x007u);
    // Configure GPIO 39 (IRQ): input, pull-up (3), 8mA (3<<6) -> 0x0c3; latch falling edge raw status without GIC IRQ
    wr32(s.tlmmPins.va, 3u * TB_GPIO_STRIDE, 0x0c3u);
    wr32(s.tlmmPins.va, 3u * TB_GPIO_STRIDE + 0x8u, (1u << 4) | (2u << 2));
    wr32(s.tlmmPins.va, 3u * TB_GPIO_STRIDE + 0xcu, 0u);
    // Configure GPIO 38 (Reset): output enabled (1<<9), 8mA (3<<6), pull-up (3) -> 0x2c3; pulse LOW 15ms -> HIGH 440ms
    wr32(s.tlmmPins.va, 2u * TB_GPIO_STRIDE, 0x2c3u);
    wr32(s.tlmmPins.va, 2u * TB_GPIO_STRIDE + 0x4u, 0u);   // Reset LOW
    IOSleep(15);
    wr32(s.tlmmPins.va, 2u * TB_GPIO_STRIDE + 0x4u, 2u);   // Reset HIGH
    IOSleep(440);

    for (unsigned i = 0; i < 4; ++i) s.pinCfg[i] = rd32(s.tlmmPins.va, i * TB_GPIO_STRIDE);
    return true;
}

void geniRecover(Session &s) {
    const uintptr_t se = s.se.va;
    wr32(se, kGeniTxWatermark, 0);
    wr32(se, kGeniMCmdCtrl, kCmdCtrlCancel);
    for (unsigned i = 0; i < 5000; ++i) {
        uint32_t st = rd32(se, kGeniMIrqStatus);
        if (st & (kMCmdCancelDone | kMCmdDone)) { wr32(se, kGeniMIrqClear, st); break; }
        IODelay(2);
    }
    if (rd32(se, kGeniStatus) & 1u) {
        wr32(se, kGeniMCmdCtrl, kCmdCtrlAbort);
        for (unsigned i = 0; i < 5000; ++i) {
            uint32_t st = rd32(se, kGeniMIrqStatus);
            if (st & (kMCmdAbortDone | kMCmdDone)) { wr32(se, kGeniMIrqClear, st); break; }
            IODelay(2);
        }
    }
    if (s.useDma) {
        wr32(se, kSeDmaTxFsmRst, 1u);
        wr32(se, kSeDmaRxFsmRst, 1u);
        wr32(se, kSeDmaTxIrqClr, 0xffffffffu);
        wr32(se, kSeDmaRxIrqClr, 0xffffffffu);
    }
    wr32(se, kGeniMIrqClear, 0xffffffffu);
}

bool initGeniI2c(Session &s) {
    s.step = 3;
    if (!mapRegion(TB_QUP_WRAPPER_BASE, 0x2000u, s.wrapper) ||
        !mapRegion(TB_SE_BASE, TB_SE_SIZE, s.se)) {
        s.err = -12;
        return false;
    }
    const uintptr_t se = s.se.va;
    s.qupVer   = rd32(s.wrapper.va, 0x4u);
    s.fwRev    = rd32(se, kGeniFwRevision);
    s.ifDis    = rd32(se, kGeniIfDisable);
    s.hwParam0 = rd32(se, kSeHwParam0);

    const uint32_t proto = (s.fwRev >> 8) & 0xffu;
    if (proto != 3u) { s.err = -71; return false; } // Must be GENI_PROTO_I2C (3)

    uint32_t major = s.qupVer >> 28, minor = (s.qupVer >> 16) & 0xfffu;
    uint32_t depthMask = ((major == 3 && minor >= 10) || major > 3) ? 0xff0000u : 0x3f0000u;
    s.txFifoDepth = (s.hwParam0 & depthMask) >> 16;
    if (s.txFifoDepth < 4) s.txFifoDepth = 16;

    // Common GENI SE init & 400 kHz I2C timing from 19.2 MHz source
    wr32(se, kSeGsiEventEn, 0u);
    wr32(se, kGeniMIrqClear, 0xffffffffu);
    wr32(se, kGeniSIrqClear, 0xffffffffu);
    wr32(se, kSeDmaTxIrqClr, 0xffffffffu);
    wr32(se, kSeDmaRxIrqClr, 0xffffffffu);
    wr32(se, kGeniCgcCtrl, rd32(se, kGeniCgcCtrl) | 0x7fu);
    wr32(se, kSeDmaGeneralCfg, rd32(se, kSeDmaGeneralCfg) | 0x0fu);
    wr32(se, kGeniOutputCtrl, 0x7fu);
    wr32(se, kGeniForceDefault, 1u);

    wr32(se, kGeniClkSel, s.clkSel);
    wr32(se, kGeniClkCfg, (2u << 4) | 1u);
    wr32(se, kGeniSclCounters, (5u << 20) | (11u << 10) | 22u);
    s.clkCfg = rd32(se, kGeniClkCfg);
    s.sclCfg = rd32(se, kGeniSclCounters);

    // 8-bit byte packing (4 bytes per 32-bit word, LSB first on wire)
    wr32(se, kGeniTxPack0, 0x7f8feu);
    wr32(se, kGeniTxPack1, 0xffefeu);
    wr32(se, kGeniRxPack0, 0x7f8feu);
    wr32(se, kGeniRxPack1, 0xffefeu);
    wr32(se, kGeniByteGran, 0u);

    s.useDma = (s.ifDis & 1u) != 0u;
    if (!s.useDma) {
        wr32(se, kGeniDmaModeEn, 0u);
        wr32(se, kGeniTxWatermark, 0u);
        wr32(se, kGeniRxWatermark, s.txFifoDepth - 3u);
        wr32(se, kGeniRxRfrWatermark, s.txFifoDepth - 2u);
        wr32(se, kGeniMIrqEn, kMCmdDone | kMErrors | kMCmdCancelDone | kMCmdAbortDone |
                              kMTxWatermark | kMRxWatermark | kMRxLast);
        wr32(se, kSeIrqEn, rd32(se, kSeIrqEn) | (1u << 2));
        return true;
    }

    // SE-DMA fallback when bootloader left FIFO_IF_DISABLE = 1 (GSI mode)
    s.step = 31;
    s.dmaMem = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task,
        static_cast<IOOptionBits>(kIODirectionInOut) | static_cast<IOOptionBits>(kIOMemoryPhysicallyContiguous),
        2 * kDmaBufHalf, 0xfffffff000ULL);
    if (!s.dmaMem || s.dmaMem->prepare() != kIOReturnSuccess) { s.err = -12; return false; }
    addr64_t segLen = 0;
    uint64_t phys = s.dmaMem->getPhysicalSegment(0, &segLen);
    uint8_t *virt = static_cast<uint8_t *>(s.dmaMem->getBytesNoCopy());
    if (!virt || !phys || segLen < 2 * kDmaBufHalf) { s.err = -14; return false; }
    memset(virt, 0, 2 * kDmaBufHalf);
    cacheRange(virt, 2 * kDmaBufHalf, 1);
    s.dmaTx = virt;
    s.dmaRx = virt + kDmaBufHalf;
    s.dmaTxPhys = phys;
    s.dmaRxPhys = phys + kDmaBufHalf;

    if (!mapRegion(TB_APPS_SMMU_BASE, TB_APPS_SMMU_SIZE, s.smmu)) { s.err = -12; return false; }
    struct smmu_io sio = { reinterpret_cast<void *>(s.smmu.va), smmuRd, smmuWr };
    struct smmu_plan plan = smmu_plan_bypass(&sio, TB_QUP_SMMU_SID, phys, 2 * kDmaBufHalf);
    s.smmuSlot = static_cast<int>(plan.slot);
    s.smmuBank = static_cast<int>(plan.bank);
    if (plan.error) { s.smmuErr = 1; s.err = -101; return false; }
    if (smmu_configure_bypass(&sio, &plan, TB_QUP_SMMU_SID)) { s.smmuErr = 2; s.err = -102; return false; }

    wr32(se, kGeniMIrqEn, kMErrors | kMCmdCancelDone | kMCmdAbortDone);
    wr32(se, kGeniDmaModeEn, 1u);
    wr32(se, kSeIrqEn, 0x7u); // DMA_RX | DMA_TX | M_IRQ
    return true;
}

int i2cWriteRaw(Session &s, uint8_t addr, const uint8_t *buf, uint32_t len, bool hold) {
    if (!len || len > 512) return -22;
    const uintptr_t se = s.se.va;
    const uint32_t param = (static_cast<uint32_t>(addr) << 9) | (hold ? kI2cStopStretch : 0u);

    if (s.useDma) {
        for (uint32_t i = 0; i < len; ++i) s.dmaTx[i] = buf[i];
        cacheRange(s.dmaTx, len, 0);
        wr32(se, kGeniMIrqClear, 0xffffffffu);
        wr32(se, kSeDmaTxIrqClr, 0xffffffffu);
        wr32(se, kGeniTxLen, len);
        wr32(se, kGeniMCmd0, (kI2cOpWrite << 27) | param);
        wr32(se, kSeDmaTxIrqEnSet, kDmaDone | kDmaEot | kDmaAhbErr);
        wr32(se, kSeDmaTxPtrL, static_cast<uint32_t>(s.dmaTxPhys));
        wr32(se, kSeDmaTxPtrH, static_cast<uint32_t>(s.dmaTxPhys >> 32));
        wr32(se, kSeDmaTxAttr, 1u);
        wr32(se, kSeDmaTxLen, len);
        for (unsigned poll = 0; poll < 50000; ++poll) {
            uint32_t ts = rd32(se, kSeDmaTxIrqStat);
            uint32_t ms = rd32(se, kGeniMIrqStatus);
            if (ts) wr32(se, kSeDmaTxIrqClr, ts);
            if (ms) wr32(se, kGeniMIrqClear, ms);
            if ((ts & kDmaAhbErr) || (ms & kMErrors)) {
                s.lastIrq = ms; s.lastDmaTx = ts; ++s.i2cErrors;
                geniRecover(s);
                return -5;
            }
            if (ts & kDmaDone) return 0;
            IODelay(1);
        }
        s.lastIrq = rd32(se, kGeniMIrqStatus); s.lastDmaTx = rd32(se, kSeDmaTxIrqStat); ++s.i2cErrors;
        geniRecover(s);
        return -110;
    }

    // FIFO mode
    wr32(se, kGeniMIrqClear, 0xffffffffu);
    wr32(se, kGeniTxLen, len);
    wr32(se, kGeniMCmd0, (kI2cOpWrite << 27) | param);
    uint32_t pushed = 0;
    wr32(se, kGeniTxWatermark, 1u);
    for (unsigned poll = 0; poll < 50000; ++poll) {
        uint32_t st = rd32(se, kGeniMIrqStatus);
        if (!st) { IODelay(1); continue; }
        wr32(se, kGeniMIrqClear, st);
        if (st & kMErrors) {
            s.lastIrq = st; ++s.i2cErrors;
            geniRecover(s);
            return -5;
        }
        if ((st & kMTxWatermark) && pushed < len) {
            unsigned space = s.txFifoDepth > 1 ? (s.txFifoDepth - 1u) : 1u;
            for (unsigned w = 0; w < space && pushed < len; ++w) {
                uint32_t word = 0;
                for (unsigned b = 0; b < 4 && pushed < len; ++b, ++pushed) {
                    word |= static_cast<uint32_t>(buf[pushed]) << (8u * b);
                }
                wr32(se, kGeniTxFifo, word);
            }
            if (pushed >= len) wr32(se, kGeniTxWatermark, 0u);
        }
        if (st & kMCmdDone) {
            wr32(se, kGeniTxWatermark, 0u);
            return (pushed >= len) ? 0 : -5;
        }
    }
    s.lastIrq = rd32(se, kGeniMIrqStatus); ++s.i2cErrors;
    geniRecover(s);
    return -110;
}

int i2cReadRaw(Session &s, uint8_t addr, uint8_t *out, uint32_t len) {
    if (!len || len > 512) return -22;
    const uintptr_t se = s.se.va;
    const uint32_t param = (static_cast<uint32_t>(addr) << 9);

    if (s.useDma) {
        for (uint32_t i = 0; i < len; ++i) s.dmaRx[i] = 0x5au;
        cacheRange(s.dmaRx, len, 1);
        wr32(se, kGeniMIrqClear, 0xffffffffu);
        wr32(se, kSeDmaRxIrqClr, 0xffffffffu);
        wr32(se, kGeniRxLen, len);
        wr32(se, kGeniMCmd0, (kI2cOpRead << 27) | param);
        wr32(se, kSeDmaRxIrqEnSet, kDmaDone | kDmaEot | kDmaAhbErr);
        wr32(se, kSeDmaRxPtrL, static_cast<uint32_t>(s.dmaRxPhys));
        wr32(se, kSeDmaRxPtrH, static_cast<uint32_t>(s.dmaRxPhys >> 32));
        wr32(se, kSeDmaRxAttr, 0u);
        wr32(se, kSeDmaRxLen, len);
        for (unsigned poll = 0; poll < 50000; ++poll) {
            uint32_t rs = rd32(se, kSeDmaRxIrqStat);
            uint32_t ms = rd32(se, kGeniMIrqStatus);
            if (rs) wr32(se, kSeDmaRxIrqClr, rs);
            if (ms) wr32(se, kGeniMIrqClear, ms);
            if ((rs & kDmaAhbErr) || (ms & kMErrors)) {
                s.lastIrq = ms; s.lastDmaRx = rs; ++s.i2cErrors;
                geniRecover(s);
                return -5;
            }
            if (rs & kDmaDone) {
                cacheRange(s.dmaRx, len, 2);
                for (uint32_t i = 0; i < len; ++i) out[i] = s.dmaRx[i];
                return 0;
            }
            IODelay(1);
        }
        s.lastIrq = rd32(se, kGeniMIrqStatus); s.lastDmaRx = rd32(se, kSeDmaRxIrqStat); ++s.i2cErrors;
        geniRecover(s);
        return -110;
    }

    // FIFO mode
    wr32(se, kGeniMIrqClear, 0xffffffffu);
    wr32(se, kGeniRxLen, len);
    wr32(se, kGeniMCmd0, (kI2cOpRead << 27) | param);
    uint32_t got = 0;
    for (unsigned poll = 0; poll < 50000; ++poll) {
        uint32_t st = rd32(se, kGeniMIrqStatus);
        if (!st) { IODelay(1); continue; }
        wr32(se, kGeniMIrqClear, st);
        if (st & kMErrors) {
            s.lastIrq = st; ++s.i2cErrors;
            geniRecover(s);
            return -5;
        }
        if (st & (kMRxWatermark | kMRxLast | kMCmdDone)) {
            uint32_t fs = rd32(se, kGeniRxFifoStatus);
            uint32_t wc = fs & 0x1ffffffu;
            for (uint32_t w = 0; w < wc; ++w) {
                uint32_t word = rd32(se, kGeniRxFifo);
                for (unsigned b = 0; b < 4 && got < len; ++b, ++got) {
                    out[got] = static_cast<uint8_t>(word >> (8u * b));
                }
            }
        }
        if (st & kMCmdDone) {
            return (got >= len) ? 0 : -5;
        }
    }
    s.lastIrq = rd32(se, kGeniMIrqStatus); ++s.i2cErrors;
    geniRecover(s);
    return -110;
}

int gtx8WriteReg(Session &s, uint16_t reg, const uint8_t *data, uint32_t len) {
    uint8_t buf[32];
    if (len + 2 > sizeof buf) return -22;
    buf[0] = static_cast<uint8_t>(reg >> 8);
    buf[1] = static_cast<uint8_t>(reg & 0xffu);
    for (uint32_t i = 0; i < len; ++i) buf[2 + i] = data[i];
    return i2cWriteRaw(s, s.slaveAddr, buf, len + 2u, false);
}

int gtx8ReadReg(Session &s, uint16_t reg, uint8_t *out, uint32_t len) {
    const uint8_t regBe[2] = { static_cast<uint8_t>(reg >> 8), static_cast<uint8_t>(reg & 0xffu) };
    int rc = i2cWriteRaw(s, s.slaveAddr, regBe, 2u, true); // Repeated Start (STOP_STRETCH)
    if (rc != 0) {
        // Fallback: non-stretched write + read in case controller firmware dislikes stretch
        rc = i2cWriteRaw(s, s.slaveAddr, regBe, 2u, false);
        if (rc != 0) return rc;
    }
    return i2cReadRaw(s, s.slaveAddr, out, len);
}

bool probeGoodix(Session &s) {
    s.step = 4;
    const uint8_t candidates[2] = { GTX8_ADDR_PRIMARY, GTX8_ADDR_FALLBACK };
    for (unsigned c = 0; c < 2; ++c) {
        s.slaveAddr = candidates[c];
        const uint8_t hs = 0xaa;
        (void)gtx8WriteReg(s, GTX8_REG_HANDSHAKE, &hs, 1u);
        IOSleep(5);
        memset(s.verBuf, 0, sizeof s.verBuf);
        if (gtx8ReadReg(s, GTX8_REG_VERSION, s.verBuf, 12u) == 0) {
            for (unsigned i = 0; i < 4; ++i) {
                char ch = static_cast<char>(s.verBuf[4 + i]);
                s.pidStr[i] = (ch >= 0x20 && ch <= 0x7e) ? ch : '?';
            }
            s.pidStr[4] = 0;
            const uint8_t zero2[2] = {0, 0};
            (void)gtx8WriteReg(s, GTX8_REG_TOUCH_DATA, zero2, 2u);
            return true;
        }
    }
    s.err = -19; // ENODEV
    return false;
}

void publishPoints(Session &s, unsigned count, const TouchPoint *pts) {
    if (__atomic_load_n(&native_touch_injecting, __ATOMIC_ACQUIRE)) return;
    if (count > 10) count = 10;
    int ids[10], xs[10], ys[10];
    for (unsigned i = 0; i < count; ++i) {
        int rx = pts[i].x;
        int ry = pts[i].y;
        int maxX = static_cast<int>(TB_PANEL_MAX_X) - 1;
        int maxY = static_cast<int>(TB_PANEL_MAX_Y) - 1;
        if (s.swapXY) {
            int tmp = rx; rx = ry; ry = tmp;
            tmp = maxX; maxX = maxY; maxY = tmp;
        }
        if (rx < 0) rx = 0; else if (rx > maxX) rx = maxX;
        if (ry < 0) ry = 0; else if (ry > maxY) ry = maxY;
        if (s.flipX) rx = maxX - rx;
        if (s.flipY) ry = maxY - ry;
        ids[i] = pts[i].id;
        xs[i] = static_cast<int>(static_cast<int64_t>(rx) * TB_HID_MAX_X / maxX);
        ys[i] = static_cast<int>(static_cast<int64_t>(ry) * TB_HID_MAX_Y / maxY);
    }
    trackStrokes(count, ids, xs, ys);
    s.lastContacts = static_cast<int>(count);
    if (count > 0) { s.lastX = xs[0]; s.lastY = ys[0]; }

    native_touch_seq = native_touch_seq + 1u;
    __asm__ volatile("dmb ish" ::: "memory");
    native_touch_count = count;
    for (unsigned i = 0; i < count; ++i) {
        native_touch_ids[i] = ids[i];
        native_touch_xs[i]  = xs[i];
        native_touch_ys[i]  = ys[i];
    }
    __asm__ volatile("dmb ish" ::: "memory");
    native_touch_seq = native_touch_seq + 1u;
    native_touch_published();
}

void touchThread(void *, wait_result_t) {
    Session *s = static_cast<Session *>(IOMallocType(Session));
    if (!s) return;
    bzero(s, sizeof *s);
    s->smmuSlot = -1;
    s->smmuBank = -1;
    s->lastX = -1;
    s->lastY = -1;
    s->pidStr[0] = '-'; s->pidStr[1] = 0;

    PE_parse_boot_argn("j716f-touch-flipx",  &s->flipX,  sizeof s->flipX);
    PE_parse_boot_argn("j716f-touch-flipy",  &s->flipY,  sizeof s->flipY);
    PE_parse_boot_argn("j716f-touch-swapxy", &s->swapXY, sizeof s->swapXY);

    votePowerRails(*s);
    native_touch_stage = 1;
    updateDiag(*s);

    if (!enableClocksAndPins(*s)) { updateDiag(*s); return; }
    native_touch_stage = 2;
    updateDiag(*s);

    if (!initGeniI2c(*s)) { updateDiag(*s); return; }
    native_touch_stage = 3;
    updateDiag(*s);

    if (!probeGoodix(*s)) { updateDiag(*s); return; }
    native_touch_stage = 5;
    s->step = 5;
    updateDiag(*s);

    uint8_t buf[96];
    const uint8_t zero2[2] = {0, 0};
    unsigned prevPublished = 0;
    for (;;) {
        IOSleep(6);
        if (++s->polls % 100u == 0u) updateDiag(*s);

        if (rd32(s->tlmmPins.va, 3u * TB_GPIO_STRIDE + 0xcu) & 1u) {
            wr32(s->tlmmPins.va, 3u * TB_GPIO_STRIDE + 0xcu, 0u);
            ++s->irqEdges;
        }

        if (gtx8ReadReg(*s, GTX8_REG_TOUCH_DATA, buf, 16u) != 0) {
            IOSleep(10);
            continue;
        }

        if (!(buf[0] & 0x80u)) {
            if (buf[0] == 0x01u) (void)gtx8WriteReg(*s, GTX8_REG_TOUCH_DATA, zero2, 2u);
            continue;
        }

        ++s->readyFrames;
        for (unsigned i = 0; i < 16; ++i) s->lastRaw[i] = buf[i];

        const unsigned touchNum = buf[1] & 0x0fu;
        const bool stylus = (buf[1] & 0x20u) != 0u;
        if (touchNum > 10u) {
            (void)gtx8WriteReg(*s, GTX8_REG_TOUCH_DATA, zero2, 2u);
            continue;
        }

        const unsigned hdrLen = stylus ? 16u : 12u;
        const unsigned totalLen = (touchNum >= 2u) ? (hdrLen + (touchNum - 1u) * 8u) : hdrLen;
        if (totalLen > 16u) {
            if (gtx8ReadReg(*s, static_cast<uint16_t>(GTX8_REG_TOUCH_DATA + 16u), buf + 16u, totalLen - 16u) != 0) {
                (void)gtx8WriteReg(*s, GTX8_REG_TOUCH_DATA, zero2, 2u);
                continue;
            }
        }
        // Acknowledge immediately so GT7386 prepares the next frame
        (void)gtx8WriteReg(*s, GTX8_REG_TOUCH_DATA, zero2, 2u);

        if (touchNum > 0u) {
            uint32_t sum = 0;
            for (unsigned i = 1u; i < totalLen; ++i) sum += buf[i];
            if ((sum & 0xffu) != 0u) {
                ++s->badCsum;
                continue;
            }
        }

        ++s->validFrames;
        TouchPoint pts[10];
        unsigned active = 0;
        if (!stylus) {
            for (unsigned i = 0; i < touchNum && active < 10u; ++i) {
                const uint8_t *f = &buf[2u + 8u * i];
                const unsigned tid = f[0];
                const uint16_t rx  = static_cast<uint16_t>(f[1] | (static_cast<uint16_t>(f[2]) << 8));
                const uint16_t ry  = static_cast<uint16_t>(f[3] | (static_cast<uint16_t>(f[4]) << 8));
                const uint16_t rw  = static_cast<uint16_t>(f[5] | (static_cast<uint16_t>(f[6]) << 8));
                if (tid >= 10u || rw == 0u) continue; // rw == 0 indicates lifted slot in GT7386
                pts[active++] = { static_cast<int>(tid), static_cast<int>(rx), static_cast<int>(ry) };
            }
        } else if (touchNum >= 1u) {
            const uint8_t *p = &buf[touchNum * 8u - 6u];
            const uint16_t rx = static_cast<uint16_t>(p[1] | (static_cast<uint16_t>(p[2]) << 8));
            const uint16_t ry = static_cast<uint16_t>(p[3] | (static_cast<uint16_t>(p[4]) << 8));
            const uint16_t rp = static_cast<uint16_t>(p[5] | (static_cast<uint16_t>(p[6]) << 8));
            if (rp > 0u) {
                pts[active++] = { 0, static_cast<int>(rx), static_cast<int>(ry) };
            }
        }

        if (active > 0u || prevPublished > 0u) {
            publishPoints(*s, active, pts);
        }
        prevPublished = active;
    }
}

} // namespace

extern "C" void native_touch_probe(void) {
    unsigned on = 0;
    if (!PE_parse_boot_argn("j716f-touch", &on, sizeof on) || on < 1u || native_touch_stage_req) return;
    native_touch_stage_req = on;
    thread_t thread = nullptr;
    if (kernel_thread_start(touchThread, nullptr, &thread) == KERN_SUCCESS) {
        thread_deallocate(thread);
    }
}
