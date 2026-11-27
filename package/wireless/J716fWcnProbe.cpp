// SPDX-License-Identifier: BSD-2-Clause-Patent
// Stage C1/C2 QCA6390 ("Hastings" v2.0) wireless power, Bluetooth GENI UART6
// HCI chip-ID handshake, and SM8250 PCIe0 link training + MHI/BHI probe for
// Lenovo TB-J716F.
//
// Strictly opt-in:
//   - Boot-arg j716f-wcn=1 runs BT probe at boot; j716f-wcn=2 also trains PCIe0.
//   - COM9 `wcn` prints read-only power/GPIO/UART6/PCIe0 state.
//   - COM9 `wcn.bt` runs 7-rail RPMh power-on + BT_EN + UART6 HCI version probe.
//   - COM9 `wcn.wifi` runs QCA6390 + PCIe0 PHY power-on, GCC/GDSC, QMP PHY
//     99-step sequence, PARF/DBI/iATU setup, PERST# release, LTSSM L0 training,
//     Endpoint 17cb:1101 discovery, BAR0 assignment, and MHI/BHI register read.
#include "J716fWcnBoard.h"
#include "J716fQcomRpmh.h"
#include "J716fSmmuBypass.h"
#include "J716fTouchBoard.h"
#include "Sm8250GeniUart.h"

#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <kern/clock.h>
#include <libkern/libkern.h>
#include <pexpert/pexpert.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>

namespace {

struct Region {
    IOMemoryMap *map = nullptr;
    volatile uint32_t *va = nullptr;
};

Region gCmdDb;
Region gRscDrv;
Region gGcc;
Region gTlmmNorth;
Region gUart6;
Region gSmmu;

bool gMapped = false;
bool gPowerAttempted = false;
bool gPciePowerAttempted = false;
bool gBtProbed = false;
bool gWifiProbed = false;

static const J716fWcnRailSpec kRails[J716F_WCN_RAIL_COUNT] = {
    {"smpa6",  950u},  /* vdd-wlan-aon / bt-vdd-aon */
    {"smpf2",  950u},  /* vdd-wlan-dig / bt-vdd-dig */
    {"smpa4", 1800u},  /* vdd-wlan-io */
    {"smpa5", 1900u},  /* vdd-wlan-rfa1 / bt-vdd-rfa1 */
    {"smpc8", 1350u},  /* vdd-wlan-rfa2 / bt-vdd-rfa2 */
    {"ldoc5", 1800u},  /* wlan-ant-switch */
    {"ldoa16", 3025u}, /* bt-vdd-asd */
};

static const J716fWcnRailSpec kPcieRails[J716F_PCIE_RAIL_COUNT] = {
    {"ldoa5",  880u},  /* pm8150_l5: PCIe0 PHY 0.88 V */
    {"ldoa9", 1200u},  /* pm8150_l9: PCIe0 PHY 1.20 V */
};

struct WcnState {
    int railRc[J716F_WCN_RAIL_COUNT];
    uint32_t railAddr[J716F_WCN_RAIL_COUNT];
    int pcieRailRc[J716F_PCIE_RAIL_COUNT];
    uint32_t pcieRailAddr[J716F_PCIE_RAIL_COUNT];

    /* Bluetooth UART6 probe state */
    int btStage;
    int btErr;
    uint32_t uartClkHz;
    uint32_t uartFwRev;
    uint32_t uartIfDis;
    uint32_t uartHwParam1;
    uint32_t uartStatusBefore;
    uint32_t uartStatusAfter;
    uint32_t ctsBefore;
    uint32_t ctsAfter;
    uint32_t swCtrlAfter;
    bool usedDma;
    int smmuSlot;
    int smmuBank;
    uint8_t rxBuf[64];
    uint32_t rxLen;
    J716fQcaBtVersion ver;

    /* PCIe0 / QCA6390 Wi-Fi probe state */
    int wifiStage;
    int wifiErr;
    uint32_t gdscAfter;
    uint32_t clkOffMask;
    uint32_t phyStatus;
    uint32_t phyPolls;
    uint32_t pipeCbcrAfterPhy;
    uint32_t sidHash;
    uint32_t rpId;
    uint32_t linkMs;
    uint32_t portDebug0;
    uint32_t portDebug1;
    uint32_t parfLtssm;
    uint32_t linkCtrlStat;
    uint32_t epId;
    uint32_t epClass;
    uint32_t epSubsys;
    uint32_t epBar0Mask;
    uint32_t epBar0Assigned;
    uint32_t epCmdAfter;
    uint32_t rpMemWin;
    uint32_t mhiRegLen;
    uint32_t mhiVer;
    uint32_t mhiCfg;
    uint32_t bhiOff;
    uint32_t bhieOff;
    uint32_t mhiCtrl;
    uint32_t mhiStatus;
    uint32_t bhiVer;
    uint32_t bhiEe;
    uint32_t bhiStatus;
    uint32_t bhiErrCode;
} gState = {};

bool mapRegion(uint64_t phys, uint64_t len, Region &r) {
    if (r.va) return true;
    IODeviceMemory *mem = IODeviceMemory::withRange(phys, len);
    r.map = mem ? mem->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
    if (mem) mem->release();
    if (r.map) {
        r.va = reinterpret_cast<volatile uint32_t *>(r.map->getVirtualAddress());
    }
    return r.va != nullptr;
}

void unmapRegion(Region &r) {
    if (r.map) r.map->release();
    r.map = nullptr;
    r.va = nullptr;
}

inline uint32_t rd32(const Region &r, uint32_t off) {
    return r.va[off / 4u];
}

inline void wr32(const Region &r, uint32_t off, uint32_t val) {
    r.va[off / 4u] = val;
    __asm__ volatile("dsb sy" ::: "memory");
}

inline void barrier() {
    __asm__ volatile("dsb sy" ::: "memory");
}

void cleanInv(const void *ptr, size_t len) {
    uintptr_t p = reinterpret_cast<uintptr_t>(ptr) & ~uintptr_t(63u);
    const uintptr_t end = reinterpret_cast<uintptr_t>(ptr) + len;
    for (; p < end; p += 64u) {
        __asm__ volatile("dc civac, %0" :: "r"(p) : "memory");
    }
    barrier();
}

void appendf(char *out, size_t cap, size_t &pos, const char *fmt, ...) {
    if (!out || pos >= cap) return;
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(out + pos, cap - pos, fmt, ap);
    va_end(ap);
    if (n > 0) {
        const size_t adv = static_cast<size_t>(n);
        pos = (pos + adv < cap) ? (pos + adv) : (cap - 1u);
    }
}

void ensureMapped() {
    if (gMapped) return;
    (void)mapRegion(TB_CMD_DB_BASE, TB_CMD_DB_SIZE, gCmdDb);
    (void)mapRegion(TB_RSC_DRV_BASE, TB_RSC_DRV_SIZE, gRscDrv);
    (void)mapRegion(SM8250_GCC_BASE, SM8250_GCC_SIZE, gGcc);
    (void)mapRegion(J716F_TLMM_NORTH_BASE, J716F_TLMM_NORTH_SIZE, gTlmmNorth);
    (void)mapRegion(SM8250_UART6_BASE, SM8250_UART6_SIZE, gUart6);
    (void)mapRegion(TB_APPS_SMMU_BASE, 0xD0000ULL, gSmmu);
    gMapped = true;
}

uint32_t gpioCfgOff(unsigned pin) { return pin * J716F_TLMM_GPIO_STRIDE; }
uint32_t gpioIoOff(unsigned pin)  { return pin * J716F_TLMM_GPIO_STRIDE + 4u; }

void setNorthPin(unsigned pin, uint32_t cfg, int outLevel) {
    if (!gTlmmNorth.va) return;
    if (outLevel >= 0) wr32(gTlmmNorth, gpioIoOff(pin), outLevel ? 2u : 0u);
    wr32(gTlmmNorth, gpioCfgOff(pin), cfg);
    if (outLevel >= 0) wr32(gTlmmNorth, gpioIoOff(pin), outLevel ? 2u : 0u);
}

uint32_t readNorthPinIn(unsigned pin) {
    if (!gTlmmNorth.va) return 0u;
    return rd32(gTlmmNorth, gpioIoOff(pin)) & 1u;
}

int voteRpmhRail(const QcomRscLayout &layout, const char *name, uint32_t mv, uint32_t *outAddr) {
    QcomRpmhResource res = {};
    const int r = qcom_cmd_db_find(reinterpret_cast<const uint8_t *>(const_cast<const uint32_t *>(gCmdDb.va)),
                                   TB_CMD_DB_SIZE, name, &res);
    if (outAddr) *outAddr = res.address;
    if (r != QcomRpmhOk) return r;

    const uint32_t addrs[3] = {res.address + 8u, res.address + 0u, res.address + 4u};
    const uint32_t vals[3]  = {7u, mv, 1u};  /* mode 7 (HPM), voltage mV, enable 1 */
    const uint32_t tcsOff = TB_RSC_TCS_OFFSET;

    QcomRpmhWrite fin[8] = {};
    unsigned fn = qcom_rpmh_finish(&layout, 0u, fin, 8u);
    for (unsigned i = 0; i < fn; ++i) wr32(gRscDrv, tcsOff + fin[i].offset, fin[i].value);

    QcomRpmhWrite writes[16] = {};
    const unsigned wn = qcom_rpmh_active_request(&layout, 0u, addrs, vals, 3u, writes, 16u);
    if (!wn) return -10;
    for (unsigned i = 0; i < wn; ++i) wr32(gRscDrv, tcsOff + writes[i].offset, writes[i].value);

    bool done = false;
    for (unsigned i = 0; i < 5000u; ++i) {
        if (rd32(gRscDrv, tcsOff + layout.irq_status) & 1u) {
            done = true;
            break;
        }
        IODelay(5);
    }
    fn = qcom_rpmh_finish(&layout, 0u, fin, 8u);
    for (unsigned i = 0; i < fn; ++i) wr32(gRscDrv, tcsOff + fin[i].offset, fin[i].value);
    IOSleep(2);
    return done ? 0 : -110;
}

bool powerOnWcnRails(bool includePciePhy) {
    if (!gCmdDb.va || !gRscDrv.va) return false;
    gPowerAttempted = true;
    const uint32_t drvId = rd32(gRscDrv, 0u);
    const QcomRscLayout layout = qcom_rsc_layout(drvId);
    wr32(gRscDrv, TB_RSC_TCS_OFFSET, rd32(gRscDrv, TB_RSC_TCS_OFFSET) | 3u);

    bool allOk = true;
    for (unsigned i = 0; i < J716F_WCN_RAIL_COUNT; ++i) {
        gState.railRc[i] = voteRpmhRail(layout, kRails[i].name, kRails[i].mv, &gState.railAddr[i]);
        if (gState.railRc[i] != 0) allOk = false;
    }
    if (includePciePhy) {
        gPciePowerAttempted = true;
        for (unsigned i = 0; i < J716F_PCIE_RAIL_COUNT; ++i) {
            gState.pcieRailRc[i] = voteRpmhRail(layout, kPcieRails[i].name, kPcieRails[i].mv,
                                                &gState.pcieRailAddr[i]);
            if (gState.pcieRailRc[i] != 0) allOk = false;
        }
    }
    IOSleep(25);
    return allOk;
}

uint32_t gccIoRead(void *, uint32_t reg) { return rd32(gGcc, reg); }
void gccIoWrite(void *, uint32_t reg, uint32_t val) { wr32(gGcc, reg, val); }
void gccIoDelay(void *, unsigned int us) { IODelay(us); }

uint32_t smmuIoRead(void *, unsigned off) { return rd32(gSmmu, off); }
void smmuIoWrite(void *, unsigned off, uint32_t val) { wr32(gSmmu, off, val); }

void ensureGpll0() {
    wr32(gGcc, SM8250_GCC_APCS_GPLL_ENA_VOTE, rd32(gGcc, SM8250_GCC_APCS_GPLL_ENA_VOTE) | 1u);
    for (unsigned i = 0; i < 5000u && !(rd32(gGcc, 0x0u) & (1u << 31)); ++i) IODelay(10);
}

bool enableUart6Clocks() {
    if (!gGcc.va) return false;
    ensureGpll0();

    const struct sm8250_geni_clock_io clkIo = {nullptr, gccIoRead, gccIoWrite, gccIoDelay};
    uint32_t hz = 0u;
    if (sm8250_geni_uart6_source(&clkIo, 115200u, &hz) != 0) return false;
    gState.uartClkHz = hz;

    if (!sm8250_geni_uart6_vote(gGcc.va + SM8250_GCC_APCS_CLOCK_BRANCH_ENA_VOTE / 4u, true)) {
        return false;
    }
    auto running = [](uint32_t cbcr) { return !(cbcr & (1u << 31)) || (cbcr & 2u); };
    for (unsigned i = 0; i < 3000u; ++i) {
        const uint32_t m = rd32(gGcc, J716F_GCC_QUP0_M_AHB_CBCR);
        const uint32_t s = rd32(gGcc, J716F_GCC_QUP0_S_AHB_CBCR);
        const uint32_t c = rd32(gGcc, J716F_GCC_QUP0_CORE_CBCR);
        const uint32_t c2 = rd32(gGcc, J716F_GCC_QUP0_CORE_2X_CBCR);
        const uint32_t s6 = rd32(gGcc, SM8250_GCC_QUPV3_WRAP0_S6_CBCR);
        if (running(m) && running(s) && running(c) && running(c2) && !(s6 & (1u << 31))) {
            return true;
        }
        IODelay(10);
    }
    return false;
}

void configureBtPinsAndReset() {
    setNorthPin(J716F_GPIO_BT_UART_CTS, (1u << 2) | (0u << 6) | 1u, -1); /* CTS: pull-down, func 1 */
    setNorthPin(J716F_GPIO_BT_UART_RTS, (1u << 2) | (0u << 6) | 0u, -1); /* RTS: no-pull, func 1 */
    setNorthPin(J716F_GPIO_BT_UART_TX,  (1u << 2) | (0u << 6) | 0u, -1); /* TX:  no-pull, func 1 */
    setNorthPin(J716F_GPIO_BT_UART_RX,  (1u << 2) | (0u << 6) | 3u, -1); /* RX:  pull-up, func 1 */
    setNorthPin(J716F_GPIO_SW_CTRL, 0u, -1);

    const uint32_t btEnCfg = (7u << 6) | (1u << 9);
    setNorthPin(J716F_GPIO_BT_EN, btEnCfg, 0);
    IOSleep(60);
    gState.ctsBefore = readNorthPinIn(J716F_GPIO_BT_UART_CTS);

    setNorthPin(J716F_GPIO_BT_EN, btEnCfg, 1);
    IOSleep(160);
    gState.ctsAfter = readNorthPinIn(J716F_GPIO_BT_UART_CTS);
    gState.swCtrlAfter = readNorthPinIn(J716F_GPIO_SW_CTRL);
}

void cancelActiveSequencers() {
    const uint32_t st = rd32(gUart6, J716F_GENI_STATUS);
    if (st & 1u) {
        wr32(gUart6, J716F_GENI_M_CMD_CTRL, (1u << 2));
        for (unsigned i = 0; i < 2000u && (rd32(gUart6, J716F_GENI_STATUS) & 1u); ++i) IODelay(10);
        if (rd32(gUart6, J716F_GENI_STATUS) & 1u) {
            wr32(gUart6, J716F_GENI_M_CMD_CTRL, (1u << 1));
            IODelay(500);
        }
    }
    if (st & (1u << 12)) {
        wr32(gUart6, J716F_GENI_S_CMD_CTRL, (1u << 2));
        for (unsigned i = 0; i < 2000u && (rd32(gUart6, J716F_GENI_STATUS) & (1u << 12)); ++i) IODelay(10);
        if (rd32(gUart6, J716F_GENI_STATUS) & (1u << 12)) {
            wr32(gUart6, J716F_GENI_S_CMD_CTRL, (1u << 1));
            IODelay(500);
        }
    }
}

void initUart6Common(bool dmaMode) {
    cancelActiveSequencers();

    wr32(gUart6, J716F_GENI_IRQ_EN, 0u);
    wr32(gUart6, J716F_GENI_GSI_EVENT_EN, 0u);
    wr32(gUart6, J716F_GENI_M_IRQ_EN, 0u);
    wr32(gUart6, J716F_GENI_S_IRQ_EN, 0u);
    wr32(gUart6, J716F_GENI_M_IRQ_CLEAR, 0xFFFFFFFFu);
    wr32(gUart6, J716F_GENI_S_IRQ_CLEAR, 0xFFFFFFFFu);
    wr32(gUart6, J716F_GENI_DMA_TX_IRQ_CLR, 0xFFFFFFFFu);
    wr32(gUart6, J716F_GENI_DMA_RX_IRQ_CLR, 0xFFFFFFFFu);

    wr32(gUart6, J716F_GENI_CGC_CTRL, rd32(gUart6, J716F_GENI_CGC_CTRL) | 0x7Fu);
    wr32(gUart6, J716F_GENI_DMA_CFG, rd32(gUart6, J716F_GENI_DMA_CFG) | 0x0Fu);
    wr32(gUart6, J716F_GENI_OUTPUT_CTRL, 0x7Fu);
    wr32(gUart6, J716F_GENI_FORCE_DEFAULT, 1u);

    wr32(gUart6, J716F_GENI_DMA_MODE_EN, dmaMode ? 1u : 0u);
    if (dmaMode) {
        wr32(gUart6, J716F_GENI_DMA_IF_EN, rd32(gUart6, J716F_GENI_DMA_IF_EN) | 1u);
    }

    /* 4x 8-bit little-endian byte packing (matches qbt_geni.c & geni_se_config_packing). */
    wr32(gUart6, J716F_GENI_TX_PACK0, 0x4380Eu);
    wr32(gUart6, J716F_GENI_TX_PACK1, 0xC3E0Eu);
    wr32(gUart6, J716F_GENI_RX_PACK0, 0x4380Eu);
    wr32(gUart6, J716F_GENI_RX_PACK1, 0xC3E0Eu);
    wr32(gUart6, J716F_GENI_BYTE_GRAN, 0u);

    wr32(gUart6, J716F_GENI_UART_TX_WORD, 8u);
    wr32(gUart6, J716F_GENI_UART_RX_WORD, 8u);
    wr32(gUart6, J716F_GENI_UART_TX_STOP, 0u);
    wr32(gUart6, J716F_GENI_UART_TX_PARITY, 0u);
    wr32(gUart6, J716F_GENI_UART_RX_PARITY, 0u);
    wr32(gUart6, J716F_GENI_UART_TX_CFG, 0u);
    wr32(gUart6, J716F_GENI_UART_RX_CFG, 0u);
    wr32(gUart6, J716F_GENI_UART_LOOPBACK, 0u);
    wr32(gUart6, J716F_GENI_UART_RX_STALE, 160u);

    const uint32_t depth = (gState.uartHwParam1 >> 16) & 0xFFu;
    wr32(gUart6, J716F_GENI_RX_WM, 1u);
    wr32(gUart6, J716F_GENI_RX_RFR_WM, depth > 4u ? (depth - 2u) : 2u);

    /* 7.3728 MHz / (4 * 16) = 115200 bps exact. */
    const uint32_t div = (gState.uartClkHz + (115200u * 16u) / 2u) / (115200u * 16u);
    const uint32_t clkCfg = ((div ? div : 1u) << 4) | 1u;
    wr32(gUart6, J716F_GENI_M_CLK_CFG, clkCfg);
    wr32(gUart6, J716F_GENI_S_CLK_CFG, clkCfg);
    wr32(gUart6, J716F_GENI_CLK_SEL, 0u);
    wr32(gUart6, J716F_GENI_UART_MANUAL_RFR, 0u);
}

void runFifoHandshake(const uint8_t *tx, uint32_t txLen) {
    initUart6Common(false);

    wr32(gUart6, J716F_GENI_S_CMD0, (1u << 27) | (1u << 7));
    IODelay(200);

    wr32(gUart6, J716F_GENI_M_IRQ_CLEAR, 0xFFFFFFFFu);
    wr32(gUart6, J716F_GENI_UART_TX_LEN, txLen);
    wr32(gUart6, J716F_GENI_M_CMD0, (1u << 27));
    for (uint32_t off = 0; off < txLen; off += 4u) {
        uint32_t word = 0u;
        for (uint32_t b = 0; b < 4u && (off + b) < txLen; ++b) {
            word |= static_cast<uint32_t>(tx[off + b]) << (8u * b);
        }
        wr32(gUart6, J716F_GENI_TX_FIFO, word);
    }

    /* Poll RX FIFO up to 500 ms; once bytes arrive, drain until quiet for 20 ms
     * so we capture all 21 bytes of the QCA6390 HCI_EV_CMD_COMPLETE frame. */
    uint32_t got = 0u;
    unsigned idleMs = 0u;
    for (unsigned ms = 0; ms < 500u && got < sizeof(gState.rxBuf); ++ms) {
        const uint32_t rfs = rd32(gUart6, J716F_GENI_RX_FIFO_STATUS);
        const uint32_t words = rfs & 0x01FFFFFFu;
        const bool lastValid = (rfs & (1u << 31)) != 0u;
        const uint32_t lastBytes = (rfs >> 28) & 7u;
        if (words > 0u) {
            idleMs = 0u;
            for (uint32_t w = 0; w < words && got < sizeof(gState.rxBuf); ++w) {
                const uint32_t v = rd32(gUart6, J716F_GENI_RX_FIFO);
                uint32_t nBytes = 4u;
                if (w + 1u == words && lastValid && lastBytes > 0u && lastBytes <= 4u) {
                    nBytes = lastBytes;
                }
                for (uint32_t b = 0; b < nBytes && got < sizeof(gState.rxBuf); ++b) {
                    gState.rxBuf[got++] = static_cast<uint8_t>((v >> (8u * b)) & 0xFFu);
                }
            }
        } else if (got >= 21u) {
            if (++idleMs >= 15u) break;
        }
        IOSleep(1);
    }
    gState.rxLen = got;
    gState.uartStatusAfter = rd32(gUart6, J716F_GENI_STATUS);
}

void runDmaHandshake(const uint8_t *tx, uint32_t txLen) {
    IOBufferMemoryDescriptor *desc = IOBufferMemoryDescriptor::inTaskWithOptions(
        kernel_task,
        static_cast<IOOptionBits>(kIODirectionInOut) |
        static_cast<IOOptionBits>(kIOMemoryPhysicallyContiguous) |
        static_cast<IOOptionBits>(kIOMemoryMapperNone),
        8192u, 4096u);
    if (!desc) {
        gState.btErr = -12;
        return;
    }
    uint8_t *virt = static_cast<uint8_t *>(desc->getBytesNoCopy());
    const uint64_t pa = desc->getPhysicalSegment(0, nullptr, kIOMemoryMapperNone);
    memset(virt, 0, 8192u);
    memcpy(virt, tx, txLen);
    cleanInv(virt, 8192u);

    const struct smmu_io sio = {nullptr, smmuIoRead, smmuIoWrite};
    const struct smmu_plan plan = smmu_plan_bypass(&sio, SM8250_QUP0_SID, pa, 8192u);
    gState.smmuSlot = static_cast<int>(plan.slot);
    gState.smmuBank = static_cast<int>(plan.bank);
    const char *serr = smmu_configure_bypass(&sio, &plan, SM8250_QUP0_SID);
    if (serr) {
        desc->release();
        gState.btErr = -5;
        return;
    }

    initUart6Common(true);
    wr32(gUart6, J716F_GENI_S_CMD0, (1u << 27) | (1u << 7));
    wr32(gUart6, J716F_GENI_DMA_RX_IRQ_CLR, 0xFFFFFFFFu);
    wr32(gUart6, J716F_GENI_DMA_RX_IRQ_EN_SET, 7u);
    wr32(gUart6, J716F_GENI_DMA_RX_PTR_L, static_cast<uint32_t>(pa + 4096u));
    wr32(gUart6, J716F_GENI_DMA_RX_PTR_H, static_cast<uint32_t>((pa + 4096u) >> 32));
    wr32(gUart6, J716F_GENI_DMA_RX_ATTR, 0u);
    wr32(gUart6, J716F_GENI_DMA_RX_LEN, 64u);
    IODelay(200);

    wr32(gUart6, J716F_GENI_DMA_TX_IRQ_CLR, 0xFFFFFFFFu);
    wr32(gUart6, J716F_GENI_DMA_TX_IRQ_EN_SET, 7u);
    wr32(gUart6, J716F_GENI_UART_TX_LEN, txLen);
    wr32(gUart6, J716F_GENI_M_CMD0, (1u << 27));
    wr32(gUart6, J716F_GENI_DMA_TX_PTR_L, static_cast<uint32_t>(pa));
    wr32(gUart6, J716F_GENI_DMA_TX_PTR_H, static_cast<uint32_t>(pa >> 32));
    wr32(gUart6, J716F_GENI_DMA_TX_ATTR, 0u);
    wr32(gUart6, J716F_GENI_DMA_TX_LEN, txLen);

    for (unsigned ms = 0; ms < 500u; ++ms) {
        const uint32_t ri = rd32(gUart6, J716F_GENI_DMA_RX_IRQ_STAT);
        if (ri & 3u) {
            const uint32_t n = rd32(gUart6, J716F_GENI_DMA_RX_LEN_IN);
            cleanInv(virt + 4096u, 64u);
            gState.rxLen = n <= 64u ? n : 64u;
            memcpy(gState.rxBuf, virt + 4096u, gState.rxLen);
            break;
        }
        IOSleep(1);
    }
    gState.uartStatusAfter = rd32(gUart6, J716F_GENI_STATUS);
    cancelActiveSequencers();
    IOSleep(5);
    desc->release();
}

void pulseGccReset(uint32_t bcr) {
    wr32(gGcc, bcr, rd32(gGcc, bcr) | 1u);
    IODelay(1000);
    wr32(gGcc, bcr, rd32(gGcc, bcr) & ~1u);
    IODelay(1000);
}

bool waitGccClear(uint32_t reg, uint32_t mask, unsigned tries) {
    for (unsigned i = 0; i < tries; ++i) {
        if ((rd32(gGcc, reg) & mask) == 0u) return true;
        IODelay(100);
    }
    return (rd32(gGcc, reg) & mask) == 0u;
}

bool enablePcie0ClocksAndGdsc() {
    ensureGpll0();

    /* 1. Power on non-votable pcie_0_gdsc (0x6B004): clear SW_COLLAPSE (bit 0), wait PWR_ON (bit 31). */
    wr32(gGcc, J716F_GCC_PCIE0_GDSCR, rd32(gGcc, J716F_GCC_PCIE0_GDSCR) & ~1u);
    bool gdscOn = false;
    for (unsigned i = 0; i < 500u; ++i) {
        const uint32_t v = rd32(gGcc, J716F_GCC_PCIE0_GDSCR);
        gState.gdscAfter = v;
        if (v & (1u << 31)) {
            gdscOn = true;
            break;
        }
        IODelay(100);
    }
    if (!gdscOn) {
        gState.wifiErr = -11;
        return false;
    }

    /* 2. Configure aux_clk_src (0x6B038) to 19.2 MHz BI_TCXO (src=0, div=1, M=N=D=0). */
    wr32(gGcc, J716F_GCC_PCIE0_AUX_CFG_RCGR, 0u);
    wr32(gGcc, J716F_GCC_PCIE0_AUX_CMD_RCGR + 0x08u, 0u); /* M */
    wr32(gGcc, J716F_GCC_PCIE0_AUX_CMD_RCGR + 0x0Cu, 0u); /* N */
    wr32(gGcc, J716F_GCC_PCIE0_AUX_CMD_RCGR + 0x10u, 0u); /* D */
    wr32(gGcc, J716F_GCC_PCIE0_AUX_CMD_RCGR, rd32(gGcc, J716F_GCC_PCIE0_AUX_CMD_RCGR) | 1u);
    (void)waitGccClear(J716F_GCC_PCIE0_AUX_CMD_RCGR, 1u, 100u);

    /* 3. Configure phy_refgen_clk_src (0x6F014) to 100 MHz (GPLL0_OUT_MAIN src=1, div=6 -> hid=11). */
    wr32(gGcc, J716F_GCC_PCIE_PHY_REFGEN_CFG, (1u << 8) | 11u);
    wr32(gGcc, J716F_GCC_PCIE_PHY_REFGEN_CMD, rd32(gGcc, J716F_GCC_PCIE_PHY_REFGEN_CMD) | 1u);
    (void)waitGccClear(J716F_GCC_PCIE_PHY_REFGEN_CMD, 1u, 100u);

    /* 4. Enable direct PCIe0 PHY / refclk / NoC branches. */
    wr32(gGcc, J716F_GCC_PCIE_PHY_AUX_CBCR, rd32(gGcc, J716F_GCC_PCIE_PHY_AUX_CBCR) | 1u);
    wr32(gGcc, J716F_GCC_PCIE0_REFGEN_CBCR, rd32(gGcc, J716F_GCC_PCIE0_REFGEN_CBCR) | 1u);
    wr32(gGcc, J716F_GCC_PCIE_WIFI_CLKREF_EN, rd32(gGcc, J716F_GCC_PCIE_WIFI_CLKREF_EN) | 1u);
    wr32(gGcc, J716F_GCC_PCIE_TBU_CBCR, rd32(gGcc, J716F_GCC_PCIE_TBU_CBCR) | 1u);
    wr32(gGcc, J716F_GCC_DDRSS_PCIE_SF_TBU_CBCR, rd32(gGcc, J716F_GCC_DDRSS_PCIE_SF_TBU_CBCR) | 1u);

    /* 5. Vote PCIe0 branches (slv_axi, mstr_axi, cfg_ahb, aux, pipe, slv_q2a). */
    wr32(gGcc, SM8250_GCC_APCS_CLOCK_BRANCH_ENA_VOTE,
         rd32(gGcc, SM8250_GCC_APCS_CLOCK_BRANCH_ENA_VOTE) | J716F_GCC_PCIE0_VOTE8_MASK);
    IODelay(200);

    static const uint32_t kCheckCbcrs[10] = {
        J716F_GCC_PCIE0_SLV_AXI_CBCR,     /* bit 0 */
        J716F_GCC_PCIE0_MSTR_AXI_CBCR,    /* bit 1 */
        J716F_GCC_PCIE0_CFG_AHB_CBCR,     /* bit 2 */
        J716F_GCC_PCIE0_AUX_CBCR,         /* bit 3 */
        J716F_GCC_PCIE0_SLV_Q2A_CBCR,     /* bit 4 */
        J716F_GCC_PCIE_PHY_AUX_CBCR,      /* bit 5 */
        J716F_GCC_PCIE0_REFGEN_CBCR,      /* bit 6 */
        J716F_GCC_PCIE_WIFI_CLKREF_EN,    /* bit 7 */
        J716F_GCC_PCIE_TBU_CBCR,          /* bit 8 */
        J716F_GCC_DDRSS_PCIE_SF_TBU_CBCR, /* bit 9 */
    };
    uint32_t offMask = 0u;
    for (unsigned attempt = 0; attempt < 200u; ++attempt) {
        offMask = 0u;
        for (unsigned i = 0; i < 10u; ++i) {
            const uint32_t v = rd32(gGcc, kCheckCbcrs[i]);
            const bool hwcg = (kCheckCbcrs[i] == J716F_GCC_PCIE0_SLV_AXI_CBCR ||
                               kCheckCbcrs[i] == J716F_GCC_PCIE0_CFG_AHB_CBCR);
            const bool on = !(v & (1u << 31)) || (hwcg && (v & 2u));
            if (!on) offMask |= (1u << i);
        }
        if (offMask == 0u) break;
        IODelay(50);
    }
    gState.clkOffMask = offMask;
    /* Core controller access requires slv_axi(0), mstr_axi(1), cfg_ahb(2), aux(3), slv_q2a(4). */
    if (offMask & 0x1Fu) {
        gState.wifiErr = -12;
        return false;
    }

    pulseGccReset(J716F_GCC_PCIE0_BCR);
    pulseGccReset(J716F_GCC_PCIE0_PHY_BCR);
    return true;
}

}  // namespace

extern "C" int j716f_wcn_bt_probe_now(void) {
    ensureMapped();
    gBtProbed = true;
    gState.btStage = 1;
    gState.btErr = 0;
    gState.rxLen = 0u;
    gState.ver = {};

    if (!gGcc.va || !gTlmmNorth.va || !gUart6.va || !gCmdDb.va || !gRscDrv.va) {
        gState.btErr = -12;
        return -12;
    }

    gState.btStage = 2;
    (void)powerOnWcnRails(false);

    gState.btStage = 3;
    if (!enableUart6Clocks()) {
        gState.btErr = -110;
        return -110;
    }

    gState.btStage = 4;
    gState.uartFwRev = rd32(gUart6, J716F_GENI_FW_REV);
    gState.uartIfDis = rd32(gUart6, J716F_GENI_IF_DISABLE);
    gState.uartHwParam1 = rd32(gUart6, J716F_GENI_HW_PARAM1);
    gState.uartStatusBefore = rd32(gUart6, J716F_GENI_STATUS);

    gState.btStage = 5;
    configureBtPinsAndReset();

    static const uint8_t kGetVerCmd[5] = {0x01u, 0x00u, 0xFCu, 0x01u, 0x19u};
    gState.btStage = 6;
    gState.usedDma = (gState.uartIfDis & 1u) != 0u;
    if (gState.usedDma) {
        runDmaHandshake(kGetVerCmd, sizeof(kGetVerCmd));
    } else {
        runFifoHandshake(kGetVerCmd, sizeof(kGetVerCmd));
    }

    (void)j716f_qca_parse_ver_evt(gState.rxBuf, gState.rxLen, &gState.ver);
    gState.btStage = gState.ver.valid ? 7 : 6;
    IOLog("[J716F wcn] bt stg=%d err=%d fw=0x%08x ifdis=0x%x dma=%u cts=%u->%u sw=%u rx=%u valid=%u soc=0x%08x rom=0x%04x patch=0x%04x\n",
          gState.btStage, gState.btErr, gState.uartFwRev, gState.uartIfDis,
          gState.usedDma ? 1u : 0u, gState.ctsBefore, gState.ctsAfter, gState.swCtrlAfter,
          gState.rxLen, gState.ver.valid ? 1u : 0u,
          gState.ver.soc_id, gState.ver.rom_ver, gState.ver.patch_ver);
    return gState.ver.valid ? 0 : (gState.btErr ? gState.btErr : -61);
}

extern "C" int j716f_wcn_wifi_probe_now(void) {
    ensureMapped();
    gWifiProbed = true;
    gState.wifiStage = 1;
    gState.wifiErr = 0;
    gState.gdscAfter = 0u;
    gState.clkOffMask = 0u;
    gState.phyStatus = 0u;
    gState.phyPolls = 0u;
    gState.pipeCbcrAfterPhy = 0u;
    gState.sidHash = 0u;
    gState.rpId = 0u;
    gState.linkMs = 0u;
    gState.portDebug0 = 0u;
    gState.portDebug1 = 0u;
    gState.parfLtssm = 0u;
    gState.linkCtrlStat = 0u;
    gState.epId = 0u;
    gState.epClass = 0u;
    gState.epSubsys = 0u;
    gState.epBar0Mask = 0u;
    gState.epBar0Assigned = 0u;
    gState.epCmdAfter = 0u;
    gState.rpMemWin = 0u;
    gState.mhiRegLen = 0u;
    gState.mhiVer = 0u;
    gState.mhiCfg = 0u;
    gState.bhiOff = 0u;
    gState.bhieOff = 0u;
    gState.mhiCtrl = 0u;
    gState.mhiStatus = 0u;
    gState.bhiVer = 0u;
    gState.bhiEe = 0u;
    gState.bhiStatus = 0u;
    gState.bhiErrCode = 0u;

    if (!gGcc.va || !gTlmmNorth.va || !gCmdDb.va || !gRscDrv.va) {
        gState.wifiErr = -12;
        return -12;
    }

    /* Stage 2: Vote QCA6390 7 rails + PCIe0 PHY 2 rails (ldoa5 0.88 V, ldoa9 1.20 V). */
    gState.wifiStage = 2;
    if (!powerOnWcnRails(true)) {
        gState.wifiErr = -9;
        return -9;
    }

    /* Stage 3: Hold PERST# LOW, set CLKREQ# to func 1 (pci_e0), toggle WLAN_EN LOW->HIGH. */
    gState.wifiStage = 3;
    const uint32_t perstCfg = 1u | (1u << 9);                 /* pull-down, 2mA, OE */
    const uint32_t clkreqCfg = 3u | (1u << 2);                /* pull-up, func 1 = pci_e0 */
    const uint32_t wakeCfg = 3u;                              /* pull-up, GPIO input */
    const uint32_t wlanEnCfg = 3u | (7u << 6) | (1u << 9);    /* pull-up, 16mA, OE */
    setNorthPin(J716F_GPIO_PCIE0_PERST, perstCfg, 0);
    setNorthPin(J716F_GPIO_PCIE0_CLKREQ, clkreqCfg, -1);
    setNorthPin(J716F_GPIO_PCIE0_WAKE, wakeCfg, -1);
    setNorthPin(J716F_GPIO_WLAN_EN, wlanEnCfg, 0);
    IOSleep(150);
    setNorthPin(J716F_GPIO_WLAN_EN, wlanEnCfg, 1);
    IOSleep(60);

    /* Stage 4..5: Power on pcie_0_gdsc, configure RCGs + branch clocks, pulse BCR resets. */
    gState.wifiStage = 4;
    if (!enablePcie0ClocksAndGdsc()) {
        IOLog("[J716F wcn] wifi failed at clk/gdsc err=%d gdsc=0x%08x off=0x%x\n",
              gState.wifiErr, gState.gdscAfter, gState.clkOffMask);
        return gState.wifiErr;
    }

    /* Stage 6: PARF init (qcom_pcie_init_2_7_0 / cfg_1_9_0). */
    gState.wifiStage = 6;
    Region parf, dbi, phy, atu, cfg, bar;
    if (!mapRegion(J716F_PCIE0_PARF_BASE, J716F_PCIE0_PARF_SIZE, parf) ||
        !mapRegion(J716F_PCIE0_DBI_BASE, J716F_PCIE0_DBI_SIZE, dbi)) {
        unmapRegion(dbi);
        unmapRegion(parf);
        gState.wifiErr = -13;
        return -13;
    }

    wr32(parf, J716F_PARF_DEVICE_TYPE, 4u); /* Root Complex */
    wr32(parf, J716F_PARF_PHY_CTRL, rd32(parf, J716F_PARF_PHY_CTRL) & ~1u);
    wr32(parf, J716F_PARF_DBI_BASE_V2, static_cast<uint32_t>(J716F_PCIE0_DBI_BASE));
    wr32(parf, J716F_PARF_DBI_BASE_V2_HI, static_cast<uint32_t>(J716F_PCIE0_DBI_BASE >> 32));
    wr32(parf, J716F_PARF_ATU_BASE, static_cast<uint32_t>(J716F_PCIE0_ATU_BASE));
    wr32(parf, J716F_PARF_ATU_BASE_HI, static_cast<uint32_t>(J716F_PCIE0_ATU_BASE >> 32));
    wr32(parf, J716F_PARF_SLV_SPACE_V2, 0u);
    wr32(parf, J716F_PARF_SLV_SPACE_V2_HI, 0x80000000u);
    wr32(parf, J716F_PARF_SYS_CTRL, rd32(parf, J716F_PARF_SYS_CTRL) & ~(1u << 29));
    wr32(parf, J716F_PARF_MHI_CLOCK_RESET_CTRL, rd32(parf, J716F_PARF_MHI_CLOCK_RESET_CTRL) | (1u << 4));
    wr32(parf, J716F_PARF_PM_CTRL, rd32(parf, J716F_PARF_PM_CTRL) & ~(1u << 5));
    wr32(parf, J716F_PARF_AXI_MSTR_WR_ADDR_HALT2, rd32(parf, J716F_PARF_AXI_MSTR_WR_ADDR_HALT2) | (1u << 31));

    /* Stage 7: Program SM8250 QMP Gen3x1 PCIe0 PHY (exact 99-step sequence from running.dtb). */
    gState.wifiStage = 7;
    if (!mapRegion(J716F_PCIE0_PHY_BASE, J716F_PCIE0_PHY_SIZE, phy)) {
        unmapRegion(dbi);
        unmapRegion(parf);
        gState.wifiErr = -14;
        return -14;
    }
    for (unsigned i = 0; i < J716F_PCIE0_PHY_SEQ_LEN; ++i) {
        wr32(phy, kJ716fPcie0PhySeq[i].offset, kJ716fPcie0PhySeq[i].value);
    }
    IODelay(1000);
    bool phyReady = false;
    for (unsigned p = 0; p < 500u; ++p) {
        const uint32_t st = rd32(phy, J716F_PCIE0_PHY_STATUS);
        gState.phyStatus = st;
        gState.phyPolls = p;
        if ((st & J716F_PCIE0_PHY_STATUS_BUSY_BIT) == 0u) {
            phyReady = true;
            break;
        }
        IODelay(100);
    }
    gState.pipeCbcrAfterPhy = rd32(gGcc, J716F_GCC_PCIE0_PIPE_CBCR);
    unmapRegion(phy);
    if (!phyReady) {
        unmapRegion(dbi);
        unmapRegion(parf);
        gState.wifiErr = -14;
        IOLog("[J716F wcn] wifi PHY timeout st=0x%08x pipe=0x%08x\n",
              gState.phyStatus, gState.pipeCbcrAfterPhy);
        return -14;
    }

    /* Stage 8: PARF SID hash table (0x000->0x1C00, 0x100->0x1C01) & DBI Root Port setup. */
    gState.wifiStage = 8;
    for (unsigned i = 0; i < 256u; ++i) {
        wr32(parf, J716F_PARF_BDF_TO_SID_TABLE + 4u * i, 0u);
    }
    wr32(parf, J716F_PARF_BDF_TO_SID_CFG, rd32(parf, J716F_PARF_BDF_TO_SID_CFG) & ~1u);
    const uint8_t hash = j716f_pcie_crc8_bdf(0x0100u);
    wr32(parf, J716F_PARF_BDF_TO_SID_TABLE + 4u * hash, (0x0100u << 16) | (1u << 8));
    gState.sidHash = hash;

    wr32(dbi, J716F_DBI_BUS_NUMBERS, 0x00010100u);
    wr32(dbi, J716F_DBI_COMMAND, rd32(dbi, J716F_DBI_COMMAND) | 0x6u);
    gState.rpId = rd32(dbi, 0u);

    /* Stage 9: Release PERST#, enable LTSSM, wait up to 1000 ms for L0 (0x11). */
    gState.wifiStage = 9;
    IOSleep(100);
    setNorthPin(J716F_GPIO_PCIE0_PERST, perstCfg, 1);
    IODelay(1000);
    wr32(parf, J716F_PARF_LTSSM, rd32(parf, J716F_PARF_LTSSM) | (1u << 8));

    const uint64_t tStart = mach_absolute_time();
    bool linkUp = false;
    for (unsigned ms = 0; ms < 1000u; ++ms) {
        const uint32_t d0 = rd32(dbi, J716F_DBI_PORT_DEBUG0);
        const uint32_t d1 = rd32(dbi, J716F_DBI_PORT_DEBUG1);
        if ((d1 & (1u << 4)) && ((d0 & 0x3Fu) == 0x11u)) {
            linkUp = true;
            break;
        }
        IOSleep(1);
    }
    uint64_t linkNs = 0;
    absolutetime_to_nanoseconds(mach_absolute_time() - tStart, &linkNs);
    gState.linkMs = static_cast<uint32_t>(linkNs / 1000000ULL);
    gState.portDebug0 = rd32(dbi, J716F_DBI_PORT_DEBUG0);
    gState.portDebug1 = rd32(dbi, J716F_DBI_PORT_DEBUG1);
    gState.parfLtssm = rd32(parf, J716F_PARF_LTSSM);
    gState.linkCtrlStat = rd32(dbi, J716F_DBI_LINK_CTRL_STATUS);
    if (!linkUp) {
        unmapRegion(dbi);
        unmapRegion(parf);
        gState.wifiErr = -16;
        IOLog("[J716F wcn] wifi LTSSM timeout rp=0x%08x d0=0x%08x d1=0x%08x ltssm=0x%08x lcs=0x%08x\n",
              gState.rpId, gState.portDebug0, gState.portDebug1, gState.parfLtssm, gState.linkCtrlStat);
        return -16;
    }

    /* Stage 10: iATU region 0 -> CFG0 bus 1 dev 0, read QCA6390 ID (expected 0x110117cb). */
    gState.wifiStage = 10;
    if (!mapRegion(J716F_PCIE0_ATU_BASE, J716F_PCIE0_ATU_SIZE, atu) ||
        !mapRegion(J716F_PCIE0_CFG_BASE, 0x1000u, cfg)) {
        unmapRegion(cfg);
        unmapRegion(atu);
        unmapRegion(dbi);
        unmapRegion(parf);
        gState.wifiErr = -17;
        return -17;
    }

    wr32(atu, 0x08u, static_cast<uint32_t>(J716F_PCIE0_CFG_BASE));
    wr32(atu, 0x0Cu, static_cast<uint32_t>(J716F_PCIE0_CFG_BASE >> 32));
    wr32(atu, 0x10u, static_cast<uint32_t>(J716F_PCIE0_CFG_BASE + 0xFFFFFu));
    wr32(atu, 0x20u, static_cast<uint32_t>((J716F_PCIE0_CFG_BASE + 0xFFFFFu) >> 32));
    wr32(atu, 0x14u, (1u << 24)); /* Bus 1, Dev 0, Func 0 */
    wr32(atu, 0x18u, 0u);
    wr32(atu, 0x00u, 0x4u);       /* Type 0 Config */
    wr32(atu, 0x04u, (1u << 31)); /* Region Enable */
    for (unsigned i = 0; i < 10u && !(rd32(atu, 0x04u) & (1u << 31)); ++i) IODelay(10);

    gState.epId = rd32(cfg, 0x00u);
    gState.epClass = rd32(cfg, 0x08u);
    gState.epSubsys = rd32(cfg, 0x2Cu);

    /* Stage 11: If QCOM endpoint found, program iATU region 1 + BAR0 and read MHI/BHI. */
    if ((gState.epId & 0xFFFFu) == 0x17CBu) {
        gState.wifiStage = 11;
        const uint64_t memEnd = J716F_PCIE0_MEM_BASE + J716F_PCIE0_MEM_SIZE - 1ULL;
        wr32(atu, 0x208u, static_cast<uint32_t>(J716F_PCIE0_MEM_BASE));
        wr32(atu, 0x20Cu, static_cast<uint32_t>(J716F_PCIE0_MEM_BASE >> 32));
        wr32(atu, 0x210u, static_cast<uint32_t>(memEnd));
        wr32(atu, 0x220u, static_cast<uint32_t>(memEnd >> 32));
        wr32(atu, 0x214u, static_cast<uint32_t>(J716F_PCIE0_MEM_BASE));
        wr32(atu, 0x218u, static_cast<uint32_t>(J716F_PCIE0_MEM_BASE >> 32));
        wr32(atu, 0x200u, 0x0u);       /* Memory */
        wr32(atu, 0x204u, (1u << 31)); /* Region Enable */
        for (unsigned i = 0; i < 10u && !(rd32(atu, 0x204u) & (1u << 31)); ++i) IODelay(10);

        const uint32_t memWin = static_cast<uint32_t>((memEnd & 0xFFF00000u) |
                                                      ((J716F_PCIE0_MEM_BASE >> 16) & 0xFFF0u));
        wr32(dbi, J716F_DBI_MEM_BASE_LIMIT, memWin);
        gState.rpMemWin = rd32(dbi, J716F_DBI_MEM_BASE_LIMIT);

        const uint32_t savedBar0 = rd32(cfg, 0x10u);
        wr32(cfg, 0x10u, 0xFFFFFFFFu);
        const uint32_t mask = rd32(cfg, 0x10u);
        wr32(cfg, 0x10u, savedBar0);
        gState.epBar0Mask = mask;

        const uint32_t barSize = ~(mask & ~0xFu) + 1u;
        if ((mask & ~0xFu) && barSize && barSize <= J716F_PCIE0_MEM_SIZE) {
            const uint32_t barAddr = static_cast<uint32_t>(
                (J716F_PCIE0_MEM_BASE + barSize - 1ULL) & ~static_cast<uint64_t>(barSize - 1u));
            wr32(cfg, 0x10u, barAddr | (mask & 0xFu));
            if ((mask & 6u) == 4u) wr32(cfg, 0x14u, 0u);
            wr32(cfg, 0x04u, (rd32(cfg, 0x04u) & 0xFFFFu) | 0x6u);
            gState.epBar0Assigned = rd32(cfg, 0x10u);
            gState.epCmdAfter = rd32(cfg, 0x04u);

            const uint32_t mapBytes = barSize < 0x20000u ? barSize : 0x20000u;
            if (mapRegion(barAddr, mapBytes, bar)) {
                gState.mhiRegLen = rd32(bar, 0x00u);
                gState.mhiVer    = rd32(bar, 0x08u);
                gState.mhiCfg    = rd32(bar, 0x10u);
                gState.bhiOff    = rd32(bar, 0x28u);
                gState.bhieOff   = rd32(bar, 0x2Cu);
                gState.mhiCtrl   = rd32(bar, 0x38u);
                gState.mhiStatus = rd32(bar, 0x48u);
                if (gState.bhiOff && gState.bhiOff < mapBytes - 0x40u) {
                    const uint32_t b = gState.bhiOff;
                    gState.bhiVer     = (rd32(bar, b + 0x00u) << 16) | (rd32(bar, b + 0x04u) & 0xFFFFu);
                    gState.bhiEe      = rd32(bar, b + 0x28u);
                    gState.bhiStatus  = rd32(bar, b + 0x2Cu);
                    gState.bhiErrCode = rd32(bar, b + 0x30u);
                    gState.wifiStage = 12;
                }
                unmapRegion(bar);
            }
        }
    }

    unmapRegion(cfg);
    unmapRegion(atu);
    unmapRegion(dbi);
    unmapRegion(parf);

    IOLog("[J716F wcn] wifi stg=%d link=%ums rp=0x%08x ep=0x%08x sub=0x%08x bar0=0x%08x mhiver=0x%08x bhi=0x%x ee=0x%x\n",
          gState.wifiStage, gState.linkMs, gState.rpId, gState.epId, gState.epSubsys,
          gState.epBar0Assigned, gState.mhiVer, gState.bhiOff, gState.bhiEe);
    return (gState.wifiStage >= 11) ? 0 : -19;
}

extern "C" void j716f_wcn_probe(void) {
    ensureMapped();
    unsigned mode = 0u;
    PE_parse_boot_argn("j716f-wcn", &mode, sizeof(mode));
    if (mode >= 1u) {
        (void)j716f_wcn_bt_probe_now();
    }
    if (mode >= 2u) {
        (void)j716f_wcn_wifi_probe_now();
    }
}

extern "C" void j716f_wcn_snapshot(char *out, size_t cap) {
    if (!out || cap == 0u) return;
    out[0] = '\0';
    size_t pos = 0;
    ensureMapped();

    appendf(out, cap, pos, "[usb] WCN: mapped=%u pwr=%u bt=%u(stg=%d err=%d) wifi=%u(stg=%d err=%d)\r\n",
            gMapped ? 1u : 0u, gPowerAttempted ? 1u : 0u,
            gBtProbed ? 1u : 0u, gState.btStage, gState.btErr,
            gWifiProbed ? 1u : 0u, gState.wifiStage, gState.wifiErr);

    if (gPowerAttempted) {
        appendf(out, cap, pos, "[usb] WCN_RAILS:");
        for (unsigned i = 0; i < J716F_WCN_RAIL_COUNT; ++i) {
            appendf(out, cap, pos, " %s(0x%x:%umV)=%d",
                    kRails[i].name, gState.railAddr[i], kRails[i].mv, gState.railRc[i]);
        }
        if (gPciePowerAttempted) {
            for (unsigned i = 0; i < J716F_PCIE_RAIL_COUNT; ++i) {
                appendf(out, cap, pos, " %s(0x%x:%umV)=%d",
                        kPcieRails[i].name, gState.pcieRailAddr[i], kPcieRails[i].mv, gState.pcieRailRc[i]);
            }
        }
        appendf(out, cap, pos, "\r\n");
    }

    if (gTlmmNorth.va) {
        appendf(out, cap, pos,
                "[usb] WCN_GPIO: wlan_en(20)=%x/%x bt_en(21)=%x/%x sw(124)=%x/%x perst(79)=%x/%x clkreq(80)=%x/%x wake(81)=%x/%x cts(16)=%x/%x rx(19)=%x/%x\r\n",
                rd32(gTlmmNorth, gpioCfgOff(20)), rd32(gTlmmNorth, gpioIoOff(20)),
                rd32(gTlmmNorth, gpioCfgOff(21)), rd32(gTlmmNorth, gpioIoOff(21)),
                rd32(gTlmmNorth, gpioCfgOff(124)), rd32(gTlmmNorth, gpioIoOff(124)),
                rd32(gTlmmNorth, gpioCfgOff(79)), rd32(gTlmmNorth, gpioIoOff(79)),
                rd32(gTlmmNorth, gpioCfgOff(80)), rd32(gTlmmNorth, gpioIoOff(80)),
                rd32(gTlmmNorth, gpioCfgOff(81)), rd32(gTlmmNorth, gpioIoOff(81)),
                rd32(gTlmmNorth, gpioCfgOff(16)), rd32(gTlmmNorth, gpioIoOff(16)),
                rd32(gTlmmNorth, gpioCfgOff(19)), rd32(gTlmmNorth, gpioIoOff(19)));
    }

    if (gGcc.va) {
        appendf(out, cap, pos,
                "[usb] WCN_PCIE0_GCC: vote8=0x%08x gdsc=0x%08x cfg=0x%08x aux=0x%08x pipe=0x%08x mstr=0x%08x slv=0x%08x refgen=0x%08x clkref=0x%08x tbu=0x%08x ddrss=0x%08x\r\n",
                rd32(gGcc, SM8250_GCC_APCS_CLOCK_BRANCH_ENA_VOTE),
                rd32(gGcc, J716F_GCC_PCIE0_GDSCR),
                rd32(gGcc, J716F_GCC_PCIE0_CFG_AHB_CBCR),
                rd32(gGcc, J716F_GCC_PCIE0_AUX_CBCR),
                rd32(gGcc, J716F_GCC_PCIE0_PIPE_CBCR),
                rd32(gGcc, J716F_GCC_PCIE0_MSTR_AXI_CBCR),
                rd32(gGcc, J716F_GCC_PCIE0_SLV_AXI_CBCR),
                rd32(gGcc, J716F_GCC_PCIE0_REFGEN_CBCR),
                rd32(gGcc, J716F_GCC_PCIE_WIFI_CLKREF_EN),
                rd32(gGcc, J716F_GCC_PCIE_TBU_CBCR),
                rd32(gGcc, J716F_GCC_DDRSS_PCIE_SF_TBU_CBCR));
    }

    if (gBtProbed) {
        appendf(out, cap, pos,
                "[usb] WCN_BT: clk=%uHz fw=0x%08x ifdis=0x%x dma=%u cts=%u->%u sw=%u rx=%u\r\n",
                gState.uartClkHz, gState.uartFwRev, gState.uartIfDis,
                gState.usedDma ? 1u : 0u, gState.ctsBefore, gState.ctsAfter,
                gState.swCtrlAfter, gState.rxLen);
        if (gState.rxLen > 0u) {
            appendf(out, cap, pos, "[usb] WCN_BT_RX:");
            for (uint32_t i = 0; i < gState.rxLen && i < 32u; ++i) {
                appendf(out, cap, pos, " %02x", gState.rxBuf[i]);
            }
            appendf(out, cap, pos, "\r\n");
        }
        if (gState.ver.valid) {
            appendf(out, cap, pos,
                    "[usb] WCN_BT_QCA6390: ev=0x%02x prod=0x%08x patch=0x%04x rom=0x%04x soc_id=0x%08x\r\n",
                    gState.ver.event_code, gState.ver.product_id, gState.ver.patch_ver,
                    gState.ver.rom_ver, gState.ver.soc_id);
        }
    }

    if (gWifiProbed) {
        appendf(out, cap, pos,
                "[usb] WCN_WIFI_LINK: stg=%d err=%d gdsc=0x%08x clk_off=0x%x phy=0x%08x(%u) pipe=0x%08x hash=0x%02x link=%ums d0=0x%08x d1=0x%08x ltssm=0x%08x lcs=0x%08x\r\n",
                gState.wifiStage, gState.wifiErr, gState.gdscAfter, gState.clkOffMask,
                gState.phyStatus, gState.phyPolls, gState.pipeCbcrAfterPhy,
                gState.sidHash, gState.linkMs, gState.portDebug0, gState.portDebug1,
                gState.parfLtssm, gState.linkCtrlStat);
        appendf(out, cap, pos,
                "[usb] WCN_WIFI_PCI: rp=0x%08x ep=0x%08x class=0x%08x subsys=0x%08x bar0_mask=0x%08x bar0=0x%08x cmd=0x%08x win=0x%08x\r\n",
                gState.rpId, gState.epId, gState.epClass, gState.epSubsys,
                gState.epBar0Mask, gState.epBar0Assigned, gState.epCmdAfter, gState.rpMemWin);
        if (gState.wifiStage >= 11) {
            appendf(out, cap, pos,
                    "[usb] WCN_WIFI_MHI: reglen=0x%x ver=0x%08x cfg=0x%08x bhi=0x%x bhie=0x%x ctrl=0x%08x status=0x%08x | bhi_ver=0x%08x ee=0x%x st=0x%08x err=0x%08x\r\n",
                    gState.mhiRegLen, gState.mhiVer, gState.mhiCfg, gState.bhiOff, gState.bhieOff,
                    gState.mhiCtrl, gState.mhiStatus, gState.bhiVer, gState.bhiEe,
                    gState.bhiStatus, gState.bhiErrCode);
        }
    }
}
