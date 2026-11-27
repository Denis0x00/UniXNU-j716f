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
#include "J716fBtTransport.h"
#include "J716fMhiBoot.h"
#include "J716fMhiQrtr.h"
#include "J716fQcomRpmh.h"
#include "J716fSmmuBypass.h"
#include "J716fTouchBoard.h"
#include "Sm8250GeniUart.h"

#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOService.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/c++/OSData.h>
#include <libkern/libkern.h>
#include <pexpert/pexpert.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>

struct proc;

extern "C" {
extern const uint8_t j716f_wcn_bt_tlv_fw[];
extern const uint32_t j716f_wcn_bt_tlv_fw_size;
extern const uint8_t j716f_wcn_bt_nvm_fw[];
extern const uint32_t j716f_wcn_bt_nvm_fw_size;
extern const uint8_t j716f_wcn_wifi_amss_fw[];
extern const uint32_t j716f_wcn_wifi_amss_fw_size;
extern const uint8_t j716f_wcn_wifi_m3_fw[];
extern const uint32_t j716f_wcn_wifi_m3_fw_size;
extern const uint8_t j716f_wcn_wifi_bdf_fw[];
extern const uint32_t j716f_wcn_wifi_bdf_fw_size;
extern bool j716f_wcn_dma_arena(uint64_t *outPa, uint64_t *outBytes) __attribute__((weak_import));
extern void proc_iterate(unsigned, int (*)(struct proc *, void *), void *,
                         int (*)(struct proc *, void *), void *) __attribute__((weak_import));
extern int proc_pid(struct proc *) __attribute__((weak_import));
}

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

    /* Stage C3a Bluetooth firmware download & BLE scan state */
    bool btFwAttempted;
    int btFwStage;
    int btFwErr;
    uint32_t btBaudHz;
    uint32_t btTlvChunks;
    uint32_t btNvmChunks;
    uint32_t btNvmPatched;
    uint32_t btDownloadMs;
    uint8_t btLocalHciVer;
    uint16_t btLocalHciRev;
    uint8_t btLocalLmpVer;
    uint16_t btLocalMfg;
    uint16_t btLocalLmpSubver;
    uint8_t btBdAddr[6];
    uint8_t btDiagLogStatus;
    uint32_t btPostSirq;
    uint32_t btPostBaud;
    uint8_t btIbsWake;
    uint8_t btPostRx[32];
    uint32_t btPostRxLen;
    uint32_t btBleReports;
    uint32_t btBleCount;
    J716fBleAdv btBle[16];

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

    /* Stage C4a QCA6390 Wi-Fi SMMU + MHI/BHIe Mission Mode boot state */
    bool wifiFwAttempted;
    int wifiFwStage;
    int wifiFwErr;
    int wifiSmmuSlot;
    int wifiSmmuBank;
    uint32_t wifiSmmuGfsr;
    uint64_t wifiSeg0Pa;
    uint32_t wifiNumSegs;
    uint32_t wifiMhiPhase;
    uint32_t wifiMhiErr;
    uint32_t wifiMhiFailPhase;
    uint32_t wifiMhiEe;
    uint32_t wifiMhiState;
    uint32_t wifiBhiStatus;
    uint32_t wifiBhiErr;
    uint32_t wifiBhieTxStatus;
    uint32_t wifiMhiStatus;
    uint32_t wifiEventsAcked;
    uint32_t wifiBootMs;
    uint32_t wifiMsiCapAfter;
    uint32_t wifiSinkWord0;

    /* Stage C3b-1 System Bluetooth (com.apple.uart.sk.bluetooth + H4/IBS worker) */
    bool btSvcStarted;
    volatile uint32_t btSvcState; /* 0=off, 1=booting, 2=running, 3=error */
    int btSvcErr;
    bool btSvcAddrPublished;
    uint8_t btSvcAddr[6];
    volatile uint32_t btSvcTxBytes;
    volatile uint32_t btSvcRxBytes;
    volatile uint32_t btSvcTxFrames;
    volatile uint32_t btSvcRxFrames;
    volatile uint32_t btSvcIbsWakeRx;
    volatile uint32_t btSvcIbsSleepRx;
    volatile uint32_t btSvcTxCmds;
    uint16_t btSvcLastOps[12];
    uint32_t btSvcLastOpsCount;
    volatile uint32_t btSvcPidBtd;
    volatile uint32_t btSvcPidBtl;

    /* Stage C4b-1 QCA6390 Wi-Fi MHI IPCR Ch20/21 + QRTR + QMI WLFW state */
    bool wifiQmiAttempted;
    int wifiQmiStage;
    int wifiQmiErr;
    uint32_t wifiQmiCmd20;
    uint32_t wifiQmiCmd21;
    uint32_t wifiQmiQrtrRx;
    uint32_t wifiQmiQrtrTx;
    uint32_t wifiQmiWlfwNode;
    uint32_t wifiQmiWlfwPort;
    uint32_t wifiQmiWlfwInst;
    uint32_t wifiQmiMemCount;
    uint32_t wifiQmiMemTotal;
    J716fQmiMemReqSeg wifiQmiMemSegs[J716F_QMI_MAX_MEM_SEGS];
    J716fQmiCapInfo wifiQmiCap;
    uint32_t wifiQmiBdfChunks;
    uint32_t wifiQmiBdfErr;
    uint32_t wifiQmiM3Err;
    bool wifiQmiMemReady;
    bool wifiQmiFwReady;
    bool wifiQmiFwInitDone;
    uint32_t wifiQmiMs;
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

/* ---- Stage C3a: FIFO-mode H4 UART6 framing, baud switch, TLV/NVM download & BLE scan ---- */

struct Uart6RxRing {
    uint8_t buf[4096];
    uint32_t head;
    uint32_t count;
};

Uart6RxRing gUart6Rx = {};
bool gUart6RawTap = false;

void uart6RxReset() {
    gUart6Rx.head = 0u;
    gUart6Rx.count = 0u;
}

void uart6DrainRx() {
    for (unsigned guard = 0; guard < 32u; ++guard) {
        const uint32_t rfs = rd32(gUart6, J716F_GENI_RX_FIFO_STATUS);
        const uint32_t words = rfs & 0x01FFFFFFu;
        if (words == 0u) break;
        const bool lastValid = (rfs & (1u << 31)) != 0u;
        const uint32_t lastBytes = (rfs >> 28) & 7u;
        for (uint32_t w = 0; w < words; ++w) {
            const uint32_t v = rd32(gUart6, J716F_GENI_RX_FIFO);
            uint32_t nBytes = 4u;
            if (w + 1u == words && lastValid && lastBytes > 0u && lastBytes <= 4u) {
                nBytes = lastBytes;
            }
            for (uint32_t b = 0; b < nBytes; ++b) {
                const uint8_t ch = static_cast<uint8_t>((v >> (8u * b)) & 0xFFu);
                if (gUart6RawTap && gState.btPostRxLen < sizeof(gState.btPostRx)) {
                    gState.btPostRx[gState.btPostRxLen++] = ch;
                }
                if (gUart6Rx.count < sizeof(gUart6Rx.buf)) {
                    const uint32_t tail = (gUart6Rx.head + gUart6Rx.count) % sizeof(gUart6Rx.buf);
                    gUart6Rx.buf[tail] = ch;
                    ++gUart6Rx.count;
                }
            }
        }
    }
}

uint8_t uart6RxPeek(uint32_t idx) {
    return gUart6Rx.buf[(gUart6Rx.head + idx) % sizeof(gUart6Rx.buf)];
}

void uart6RxDrop(uint32_t n) {
    if (n > gUart6Rx.count) n = gUart6Rx.count;
    gUart6Rx.head = (gUart6Rx.head + n) % sizeof(gUart6Rx.buf);
    gUart6Rx.count -= n;
}

void uart6SetFlow(bool enable) {
    const uint32_t cfg = rd32(gUart6, J716F_GENI_UART_TX_CFG);
    wr32(gUart6, J716F_GENI_UART_TX_CFG, enable ? (cfg & ~2u) : (cfg | 2u));
    wr32(gUart6, J716F_GENI_UART_MANUAL_RFR, enable ? 0u : J716F_GENI_RFR_OFF);
}

bool uart6SendFifo(const uint8_t *tx, uint32_t txLen) {
    if (!tx || txLen == 0u) return true;
    for (unsigned i = 0; i < 20000u && (rd32(gUart6, J716F_GENI_STATUS) & 1u); ++i) {
        uart6DrainRx();
        IODelay(10);
    }
    if (rd32(gUart6, J716F_GENI_STATUS) & 1u) return false;

    wr32(gUart6, J716F_GENI_M_IRQ_CLEAR, 0xFFFFFFFFu);
    wr32(gUart6, J716F_GENI_UART_TX_LEN, txLen);
    wr32(gUart6, J716F_GENI_M_CMD0, (1u << 27));
    for (uint32_t off = 0; off < txLen; off += 4u) {
        for (unsigned w = 0; w < 20000u; ++w) {
            uart6DrainRx();
            if ((rd32(gUart6, J716F_GENI_TX_FIFO_STATUS) & 0x0FFFFFFFu) < 12u) break;
            IODelay(5);
        }
        uint32_t word = 0u;
        for (uint32_t b = 0; b < 4u && (off + b) < txLen; ++b) {
            word |= static_cast<uint32_t>(tx[off + b]) << (8u * b);
        }
        wr32(gUart6, J716F_GENI_TX_FIFO, word);
    }
    for (unsigned i = 0; i < 40000u; ++i) {
        uart6DrainRx();
        if (!(rd32(gUart6, J716F_GENI_STATUS) & 1u) &&
            (rd32(gUart6, J716F_GENI_TX_FIFO_STATUS) & 0x0FFFFFFFu) == 0u) {
            return true;
        }
        IODelay(5);
    }
    return false;
}

bool uart6TryPopEvent(uint8_t *out, uint32_t cap, uint32_t *outLen) {
    uart6DrainRx();
    while (gUart6Rx.count > 0u) {
        const uint8_t pktType = uart6RxPeek(0u);
        if (pktType == 0x04u) {
            if (gUart6Rx.count < 3u) return false;
            const uint32_t total = 3u + static_cast<uint32_t>(uart6RxPeek(2u));
            if (gUart6Rx.count < total) return false;
            const uint32_t copy = total <= cap ? total : cap;
            for (uint32_t i = 0; i < copy; ++i) out[i] = uart6RxPeek(i);
            uart6RxDrop(total);
            if (outLen) *outLen = copy;
            return true;
        }
        if (pktType == 0x02u) {
            if (gUart6Rx.count < 5u) return false;
            const uint32_t alen = static_cast<uint32_t>(uart6RxPeek(3u)) |
                                  (static_cast<uint32_t>(uart6RxPeek(4u)) << 8);
            const uint32_t total = 5u + alen;
            if (total > sizeof(gUart6Rx.buf)) {
                uart6RxDrop(1u);
                continue;
            }
            if (gUart6Rx.count < total) return false;
            uart6RxDrop(total);
            continue;
        }
        if (pktType == 0xFDu) {
            /* QCA IBS WAKE_IND from controller -> reply WAKE_ACK (0xFC) so it transmits. */
            uart6RxDrop(1u);
            static const uint8_t kWakeAck[1] = {0xFCu};
            (void)uart6SendFifo(kWakeAck, 1u);
            continue;
        }
        /* Skip stray noise or IBS SLEEP_IND (0xFE) / WAKE_ACK (0xFC). */
        uart6RxDrop(1u);
    }
    return false;
}

bool uart6WaitCmdComplete(uint16_t opcode, uint8_t *out, uint32_t cap, uint32_t *outLen,
                          unsigned timeoutMs, bool ignoreNonZeroTlv) {
    const uint64_t tStart = mach_absolute_time();
    const uint64_t limitNs = static_cast<uint64_t>(timeoutMs) * 1000000ULL;
    uint8_t ev[260];
    uint32_t evLen = 0u;
    for (;;) {
        while (uart6TryPopEvent(ev, sizeof(ev), &evLen)) {
            if (evLen >= 7u && ev[0] == 0x04u && ev[1] == 0x0Eu) {
                const uint16_t evOp = static_cast<uint16_t>(ev[4]) |
                                      (static_cast<uint16_t>(ev[5]) << 8);
                if (evOp == opcode) {
                    if (ignoreNonZeroTlv && opcode == 0xFC00u && evLen >= 8u &&
                        ev[7] == 0x1Eu && ev[6] != 0u) {
                        continue;
                    }
                    const uint32_t copy = evLen <= cap ? evLen : cap;
                    if (out && copy > 0u) memcpy(out, ev, copy);
                    if (outLen) *outLen = copy;
                    return true;
                }
            }
        }
        uint64_t elapsedNs = 0;
        absolutetime_to_nanoseconds(mach_absolute_time() - tStart, &elapsedNs);
        if (elapsedNs >= limitNs) return false;
        IODelay(100);
    }
}

bool uart6HciCmdSync(uint16_t opcode, const uint8_t *params, uint8_t plen,
                     uint8_t *out, uint32_t cap, uint32_t *outLen, unsigned timeoutMs) {
    uint8_t frame[260];
    frame[0] = 0x01u;
    frame[1] = static_cast<uint8_t>(opcode & 0xFFu);
    frame[2] = static_cast<uint8_t>(opcode >> 8);
    frame[3] = plen;
    if (plen > 0u && params) memcpy(frame + 4u, params, plen);
    if (!uart6SendFifo(frame, 4u + static_cast<uint32_t>(plen))) return false;
    return uart6WaitCmdComplete(opcode, out, cap, outLen, timeoutMs, false);
}

bool uart6SwitchBaud(uint32_t baud, uint8_t qcaCode) {
    uart6SetFlow(false);
    const uint8_t cmd[5] = {0x01u, 0x48u, 0xFCu, 0x01u, qcaCode};
    if (!uart6SendFifo(cmd, sizeof(cmd))) {
        uart6SetFlow(true);
        return false;
    }
    IOSleep(2);

    /* Stop RX secondary sequencer cleanly before changing SE6 RCG frequency. */
    if (rd32(gUart6, J716F_GENI_STATUS) & (1u << 12)) {
        wr32(gUart6, J716F_GENI_S_CMD_CTRL, (1u << 2));
        for (unsigned i = 0; i < 2000u && (rd32(gUart6, J716F_GENI_STATUS) & (1u << 12)); ++i) IODelay(10);
        if (rd32(gUart6, J716F_GENI_STATUS) & (1u << 12)) {
            wr32(gUart6, J716F_GENI_S_CMD_CTRL, (1u << 1));
            IODelay(500);
        }
    }

    const struct sm8250_geni_clock_io clkIo = {nullptr, gccIoRead, gccIoWrite, gccIoDelay};
    uint32_t hz = 0u;
    if (sm8250_geni_uart6_source(&clkIo, baud, &hz) != 0) {
        uart6SetFlow(true);
        return false;
    }
    gState.uartClkHz = hz;
    gState.btBaudHz = baud;
    const uint32_t target = baud * 16u;
    const uint32_t div = (hz + target / 2u) / target;
    const uint32_t clkCfg = ((div ? div : 1u) << 4) | 1u;
    wr32(gUart6, J716F_GENI_M_CLK_CFG, clkCfg);
    wr32(gUart6, J716F_GENI_S_CLK_CFG, clkCfg);
    wr32(gUart6, J716F_GENI_CLK_SEL, 0u);

    uart6RxReset();
    wr32(gUart6, J716F_GENI_S_IRQ_CLEAR, 0xFFFFFFFFu);
    wr32(gUart6, J716F_GENI_S_CMD0, (1u << 27) | (1u << 7));
    IODelay(200);
    uart6SetFlow(true);

    /* Wait for QCA6390's 0xFC48 completion at the new baudrate (up to 300 ms) + settle. */
    uint8_t ev[32];
    uint32_t evLen = 0u;
    (void)uart6WaitCmdComplete(0xFC48u, ev, sizeof(ev), &evLen, 300u, false);
    IOSleep(25);
    uart6DrainRx();
    uart6RxReset();
    return true;
}

bool qcaDownloadTlv(const uint8_t *data, uint32_t len, uint8_t dnldMode, uint32_t *outChunks) {
    if (!data || len < 4u) return false;
    uint8_t frame[4u + 2u + J716F_QCA_TLV_MAX_SEG];
    uint8_t ev[32];
    uint32_t evLen = 0u;
    uint32_t chunks = 0u;
    uint32_t off = 0u;

    while (off < len) {
        const uint32_t rem = len - off;
        const uint32_t seg = rem > J716F_QCA_TLV_MAX_SEG ? J716F_QCA_TLV_MAX_SEG : rem;
        const bool isLast = (off + seg >= len);

        frame[0] = 0x01u;
        frame[1] = 0x00u;
        frame[2] = 0xFCu;
        frame[3] = static_cast<uint8_t>(2u + seg);
        frame[4] = 0x1Eu; /* EDL_PATCH_TLV_REQ_CMD */
        frame[5] = static_cast<uint8_t>(seg);
        memcpy(frame + 6u, data + off, seg);

        if (!uart6SendFifo(frame, 6u + seg)) {
            if (outChunks) *outChunks = chunks;
            return false;
        }
        ++chunks;
        off += seg;

        /* QCA_SKIP_EVT_VSE_CC (3): skip per-chunk CC on intermediate full-size segments,
         * but ALWAYS wait for final CC (status 0) on the last segment.
         * NVM (dnldMode 0): wait for CC on every segment. */
        const bool waitCc = isLast || (dnldMode == 0u) || (dnldMode == 1u);
        if (waitCc) {
            if (!uart6WaitCmdComplete(0xFC00u, ev, sizeof(ev), &evLen, isLast ? 2000u : 500u, true)) {
                if (outChunks) *outChunks = chunks;
                return false;
            }
            if (evLen < 7u || ev[6] != 0u) {
                if (outChunks) *outChunks = chunks;
                return false;
            }
        } else {
            uart6DrainRx();
            if (gUart6Rx.count > 1024u) uart6RxReset();
        }
    }
    if (outChunks) *outChunks = chunks;
    return true;
}

void recordBleAdv(const J716fBleAdv &adv) {
    if (!adv.valid) return;
    ++gState.btBleReports;
    for (uint32_t i = 0; i < gState.btBleCount; ++i) {
        if (memcmp(gState.btBle[i].addr, adv.addr, 6u) == 0) {
            gState.btBle[i].rssi = adv.rssi;
            if (gState.btBle[i].name[0] == '\0' && adv.name[0] != '\0') {
                memcpy(gState.btBle[i].name, adv.name, sizeof(gState.btBle[i].name));
            }
            return;
        }
    }
    if (gState.btBleCount < 16u) {
        gState.btBle[gState.btBleCount++] = adv;
    }
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

extern "C" int j716f_wcn_bt_fw_scan_now(void) {
    ensureMapped();
    gState.btFwAttempted = true;
    gState.btFwStage = 1;
    gState.btFwErr = 0;
    gState.btBaudHz = 115200u;
    gState.btTlvChunks = 0u;
    gState.btNvmChunks = 0u;
    gState.btNvmPatched = 0u;
    gState.btDownloadMs = 0u;
    gState.btLocalHciVer = 0u;
    gState.btLocalHciRev = 0u;
    gState.btLocalLmpVer = 0u;
    gState.btLocalMfg = 0u;
    gState.btLocalLmpSubver = 0u;
    memset(gState.btBdAddr, 0, sizeof(gState.btBdAddr));
    gState.btBleReports = 0u;
    gState.btBleCount = 0u;
    memset(gState.btBle, 0, sizeof(gState.btBle));

    if (j716f_wcn_bt_tlv_fw_size < 40u || j716f_wcn_bt_nvm_fw_size < 32u) {
        gState.btFwErr = -2;
        return -2;
    }

    /* Stage 2: Cold power/reset + 115200 QCA6390 ROM version handshake. */
    gState.btFwStage = 2;
    const int prc = j716f_wcn_bt_probe_now();
    if (prc != 0 || gState.usedDma) {
        gState.btFwErr = prc ? prc : -95;
        return gState.btFwErr;
    }
    uart6RxReset();
    uart6DrainRx();
    uart6RxReset();

    /* Stage 3: Switch UART6 + QCA6390 ROM from 115200 bps to 3.2 Mbps (51.2 MHz / 16). */
    gState.btFwStage = 3;
    if (!uart6SwitchBaud(3200000u, J716F_QCA_BAUD_3200000)) {
        gState.btFwErr = -5;
        return -5;
    }

    /* Stage 4: Verify 3.2 Mbps link with GET_TARGET_VERSION before streaming 204 KiB TLV. */
    gState.btFwStage = 4;
    uint8_t ev[260];
    uint32_t evLen = 0u;
    const uint8_t getVerParam[1] = {0x19u};
    if (!uart6HciCmdSync(0xFC00u, getVerParam, 1u, ev, sizeof(ev), &evLen, 500u)) {
        gState.btFwErr = -110;
        return -110;
    }

    /* Stage 5: Download htbtfw20.tlv rampatch (download_mode at file offset 14). */
    gState.btFwStage = 5;
    const uint64_t tDlStart = mach_absolute_time();
    const uint8_t tlvDnldMode = j716f_wcn_bt_tlv_fw[14];
    if (!qcaDownloadTlv(j716f_wcn_bt_tlv_fw, j716f_wcn_bt_tlv_fw_size, tlvDnldMode, &gState.btTlvChunks)) {
        gState.btFwErr = -71;
        return -71;
    }
    IOSleep(10);

    /* Stage 6: Copy & patch htnv20.bin (3.2 Mbps, IBS=off, DeepSleep=off) and download. */
    gState.btFwStage = 6;
    uint8_t *nvmCopy = static_cast<uint8_t *>(IOMallocData(j716f_wcn_bt_nvm_fw_size));
    if (!nvmCopy) {
        gState.btFwErr = -12;
        return -12;
    }
    memcpy(nvmCopy, j716f_wcn_bt_nvm_fw, j716f_wcn_bt_nvm_fw_size);
    gState.btNvmPatched = j716f_qca_patch_nvm(nvmCopy, j716f_wcn_bt_nvm_fw_size,
                                              J716F_QCA_BAUD_3200000, false, false);
    const bool nvmOk = qcaDownloadTlv(nvmCopy, j716f_wcn_bt_nvm_fw_size, 0u, &gState.btNvmChunks);
    IOFreeData(nvmCopy, j716f_wcn_bt_nvm_fw_size);
    if (!nvmOk) {
        gState.btFwErr = -72;
        return -72;
    }
    uint64_t dlNs = 0;
    absolutetime_to_nanoseconds(mach_absolute_time() - tDlStart, &dlNs);
    gState.btDownloadMs = static_cast<uint32_t>(dlNs / 1000000ULL);

    /* Disable QCA6390 SoC ACL debug logging (0xFC17 subop 0x14, 0x00) before HCI_RESET (Linux btqca.c). */
    gState.btDiagLogStatus = 0xFFu;
    gState.btPostRxLen = 0u;
    memset(gState.btPostRx, 0, sizeof(gState.btPostRx));
    static const uint8_t kDisableLog[2] = {0x14u, 0x00u};
    if (uart6HciCmdSync(0xFC17u, kDisableLog, sizeof(kDisableLog), ev, sizeof(ev), &evLen, 1000u) &&
        evLen >= 7u) {
        gState.btDiagLogStatus = ev[6];
    }

    /* Stage 7: HCI_RESET + READ_LOCAL_VERSION + WRITE/READ BD_ADDR on patched firmware. */
    gState.btFwStage = 7;
    gState.btPostSirq = 0u;
    gState.btPostBaud = 3200000u;
    gState.btIbsWake = 0u;
    wr32(gUart6, J716F_GENI_S_IRQ_CLEAR, 0xFFFFFFFFu);
    if (!uart6HciCmdSync(0x0C03u, nullptr, 0u, ev, sizeof(ev), &evLen, 2000u) ||
        evLen < 7u || ev[6] != 0u) {
        gState.btFwErr = -73;
        return -73;
    }
    IOSleep(25);
    gUart6RawTap = true;
    uart6DrainRx();

    bool verOk = uart6HciCmdSync(0x1001u, nullptr, 0u, ev, sizeof(ev), &evLen, 500u) &&
                 evLen >= 15u && ev[6] == 0u;
    gState.btPostSirq = rd32(gUart6, J716F_GENI_S_IRQ_STATUS);

    if (!verOk) {
        /* Probe 1: send IBS WAKE_IND (0xFD) in case firmware kept sleep active after HCI_RESET. */
        static const uint8_t kWakeInd[1] = {0xFDu};
        (void)uart6SendFifo(kWakeInd, 1u);
        IOSleep(20);
        uart6DrainRx();
        verOk = uart6HciCmdSync(0x1001u, nullptr, 0u, ev, sizeof(ev), &evLen, 500u) &&
                evLen >= 15u && ev[6] == 0u;
        if (verOk) gState.btIbsWake = 1u;
    }

    if (!verOk) {
        /* Probe 2: check if QCA6390 reverted UART baud to 115200 bps after HCI_RESET. */
        uart6SetFlow(false);
        if (rd32(gUart6, J716F_GENI_STATUS) & (1u << 12)) {
            wr32(gUart6, J716F_GENI_S_CMD_CTRL, (1u << 2));
            for (unsigned i = 0; i < 2000u && (rd32(gUart6, J716F_GENI_STATUS) & (1u << 12)); ++i) IODelay(10);
        }
        const struct sm8250_geni_clock_io clkIo = {nullptr, gccIoRead, gccIoWrite, gccIoDelay};
        uint32_t hz = 0u;
        if (sm8250_geni_uart6_source(&clkIo, 115200u, &hz) == 0) {
            const uint32_t div = (hz + (115200u * 16u) / 2u) / (115200u * 16u);
            const uint32_t clkCfg = ((div ? div : 1u) << 4) | 1u;
            wr32(gUart6, J716F_GENI_M_CLK_CFG, clkCfg);
            wr32(gUart6, J716F_GENI_S_CLK_CFG, clkCfg);
            wr32(gUart6, J716F_GENI_CLK_SEL, 0u);
            uart6RxReset();
            wr32(gUart6, J716F_GENI_S_IRQ_CLEAR, 0xFFFFFFFFu);
            wr32(gUart6, J716F_GENI_S_CMD0, (1u << 27) | (1u << 7));
            IODelay(200);
            uart6SetFlow(true);
            IOSleep(10);
            verOk = uart6HciCmdSync(0x1001u, nullptr, 0u, ev, sizeof(ev), &evLen, 500u) &&
                    evLen >= 15u && ev[6] == 0u;
            if (verOk) {
                gState.btPostBaud = 115200u;
                gState.btBaudHz = 115200u;
            }
        } else {
            uart6SetFlow(true);
        }
    }
    gUart6RawTap = false;

    if (verOk) {
        gState.btLocalHciVer    = ev[7];
        gState.btLocalHciRev    = static_cast<uint16_t>(ev[8]) | (static_cast<uint16_t>(ev[9]) << 8);
        gState.btLocalLmpVer    = ev[10];
        gState.btLocalMfg       = static_cast<uint16_t>(ev[11]) | (static_cast<uint16_t>(ev[12]) << 8);
        gState.btLocalLmpSubver = static_cast<uint16_t>(ev[13]) | (static_cast<uint16_t>(ev[14]) << 8);
    }
    static const uint8_t kBdAddr[6] = {0x16u, 0x71u, 0x0Eu, 0x20u, 0xCAu, 0x02u};
    (void)uart6HciCmdSync(0xFC14u, kBdAddr, 6u, ev, sizeof(ev), &evLen, 1000u);
    if (uart6HciCmdSync(0x1009u, nullptr, 0u, ev, sizeof(ev), &evLen, 1000u) &&
        evLen >= 13u && ev[6] == 0u) {
        memcpy(gState.btBdAddr, ev + 7u, 6u);
    }

    /* Stage 8: Configure event masks + 3-second BLE Passive Scan to prove live RF RX. */
    gState.btFwStage = 8;
    static const uint8_t kEvtMask[8]   = {0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0x3Fu};
    static const uint8_t kLeEvtMask[8] = {0xFFu, 0x07u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u, 0x00u};
    static const uint8_t kScanParam[7] = {0x00u, 0x30u, 0x00u, 0x30u, 0x00u, 0x00u, 0x00u};
    static const uint8_t kScanOn[2]    = {0x01u, 0x00u};
    static const uint8_t kScanOff[2]   = {0x00u, 0x00u};
    (void)uart6HciCmdSync(0x0C01u, kEvtMask, sizeof(kEvtMask), ev, sizeof(ev), &evLen, 500u);
    (void)uart6HciCmdSync(0x2001u, kLeEvtMask, sizeof(kLeEvtMask), ev, sizeof(ev), &evLen, 500u);
    if (!uart6HciCmdSync(0x200Bu, kScanParam, sizeof(kScanParam), ev, sizeof(ev), &evLen, 500u) ||
        evLen < 7u || ev[6] != 0u) {
        gState.btFwErr = -74;
        return -74;
    }
    if (!uart6HciCmdSync(0x200Cu, kScanOn, sizeof(kScanOn), ev, sizeof(ev), &evLen, 500u) ||
        evLen < 7u || ev[6] != 0u) {
        gState.btFwErr = -75;
        return -75;
    }

    const uint64_t tScanStart = mach_absolute_time();
    for (;;) {
        while (uart6TryPopEvent(ev, sizeof(ev), &evLen)) {
            J716fBleAdv adv = {};
            if (j716f_hci_parse_le_adv(ev, evLen, &adv)) {
                recordBleAdv(adv);
            }
        }
        uint64_t elapsedNs = 0;
        absolutetime_to_nanoseconds(mach_absolute_time() - tScanStart, &elapsedNs);
        if (elapsedNs >= 3000000000ULL) break;
        IOSleep(1);
    }
    (void)uart6HciCmdSync(0x200Cu, kScanOff, sizeof(kScanOff), ev, sizeof(ev), &evLen, 500u);
    gState.btFwStage = 9;

    IOLog("[J716F wcn] bt.fw stg=%d err=%d baud=%u tlv=%u nvm=%u(%u) dl=%ums lmp=0x%04x mfg=0x%04x ble=%u(%u)\n",
          gState.btFwStage, gState.btFwErr, gState.btBaudHz, gState.btTlvChunks,
          gState.btNvmChunks, gState.btNvmPatched, gState.btDownloadMs,
          gState.btLocalLmpSubver, gState.btLocalMfg, gState.btBleCount, gState.btBleReports);
    return 0;
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

/* ---- Stage C4a: SM8250 PCIe0 SMMU SID 0x1C01 + QCA6390 MHI/BHIe AMSS Mission Mode ---- */

namespace {

constexpr uint16_t kPcie0EpSid = 0x1C01u;
constexpr uint32_t kWifiSegSize = 512u * 1024u; /* QCA6390 ath11k sbl_size / seg_len = 512 KiB */
constexpr uint32_t kWifiMaxSegs = 16u;

struct WifiDmaBuf {
    IOBufferMemoryDescriptor *desc = nullptr;
    uint8_t *va = nullptr;
    uint64_t pa = 0;
    size_t len = 0;
};

struct WifiDmaPool {
    bool allocated = false;
    IOMemoryMap *arenaMap = nullptr;
    uint8_t *arenaVa = nullptr;
    uint64_t arenaPa = 0;
    uint64_t arenaBytes = 0;
    WifiDmaBuf seg[kWifiMaxSegs];
    uint32_t numSegs = 0u;
    WifiDmaBuf table;
    WifiDmaBuf ctx;
    WifiDmaBuf sink;
    uint64_t evAcked[2] = {0, 0};
} gWifiDma;

bool initWifiDmaArena(uint32_t numSegs) {
    if (gWifiDma.allocated) return true;
    uint64_t pa = 0ULL, bytes = 0ULL;
    if (&j716f_wcn_dma_arena && j716f_wcn_dma_arena(&pa, &bytes) &&
        pa > 0ULL && pa + bytes <= 0x100000000ULL && bytes >= 0x800000ULL &&
        (pa & 0x7FFFFULL) == 0ULL) {
        IODeviceMemory *mem = IODeviceMemory::withRange(pa, bytes);
        gWifiDma.arenaMap = mem ? mem->map(kIOMapAnywhere | kIOMapWriteCombineCache) : nullptr;
        if (mem) mem->release();
        if (!gWifiDma.arenaMap) return false;
        gWifiDma.arenaVa = reinterpret_cast<uint8_t *>(gWifiDma.arenaMap->getVirtualAddress());
        gWifiDma.arenaPa = pa;
        gWifiDma.arenaBytes = bytes;
        memset(gWifiDma.arenaVa, 0, 0x600000u);
        for (uint32_t i = 0; i < numSegs; ++i) {
            const uint32_t off = i * kWifiSegSize;
            gWifiDma.seg[i].va = gWifiDma.arenaVa + off;
            gWifiDma.seg[i].pa = gWifiDma.arenaPa + off;
            gWifiDma.seg[i].len = kWifiSegSize;
        }
        const uint32_t metaBase = ((numSegs * kWifiSegSize) + 0xFFFFFu) & ~0xFFFFFu; /* 1 MiB aligned */
        gWifiDma.table.va = gWifiDma.arenaVa + metaBase;
        gWifiDma.table.pa = gWifiDma.arenaPa + metaBase;
        gWifiDma.table.len = 4096u;
        gWifiDma.ctx.va = gWifiDma.arenaVa + metaBase + 0x10000u;
        gWifiDma.ctx.pa = gWifiDma.arenaPa + metaBase + 0x10000u;
        gWifiDma.ctx.len = 32768u;
        gWifiDma.sink.va = gWifiDma.arenaVa + metaBase + 0x20000u;
        gWifiDma.sink.pa = gWifiDma.arenaPa + metaBase + 0x20000u;
        gWifiDma.sink.len = 4096u;
        gWifiDma.numSegs = numSegs;
        gWifiDma.allocated = true;
        return true;
    }
    return false;
}

uint32_t gQcaWindow = 0xFFFFFFFFu;

uint32_t qcaChipRead(const Region &bar, uint32_t off) {
    if (off < 0x80000u) return rd32(bar, off);
    const uint32_t win = (off >> 19) & 0x3Fu;
    if (win != gQcaWindow) {
        wr32(bar, 0x310Cu, 0x40000000u | win);
        (void)rd32(bar, 0x310Cu);
        gQcaWindow = win;
    }
    return rd32(bar, 0x80000u + (off & 0x7FFFFu));
}

void qcaChipWrite(const Region &bar, uint32_t off, uint32_t val) {
    if (off < 0x80000u) {
        wr32(bar, off, val);
        return;
    }
    const uint32_t win = (off >> 19) & 0x3Fu;
    if (win != gQcaWindow) {
        wr32(bar, 0x310Cu, 0x40000000u | win);
        (void)rd32(bar, 0x310Cu);
        gQcaWindow = win;
    }
    wr32(bar, 0x80000u + (off & 0x7FFFFu), val);
}

void qcaSetLinkReg(const Region &bar, uint32_t off, uint32_t val, uint32_t mask) {
    uint32_t v = qcaChipRead(bar, off);
    if ((v & mask) == val) return;
    for (unsigned i = 0; i < 10u; ++i) {
        qcaChipWrite(bar, off, (v & ~mask) | val);
        v = qcaChipRead(bar, off);
        if ((v & mask) == val) return;
        IOSleep(2);
    }
}

/* Exact Linux ath11k_pci_sw_reset(power_on=true) for QCA6390 v2.0 (17cb:1101). */
void qca6390ChipPowerUp(const Region &bar) {
    gQcaWindow = 0xFFFFFFFFu;
    uint32_t v = qcaChipRead(bar, 0x1E081B0u);
    for (unsigned i = 0; v != 0x111u && i < 5u; ++i) {
        if (v == 0xFFFFFFFFu) IOSleep(5);
        qcaChipWrite(bar, 0x1E081B0u, 0x111u);
        v = qcaChipRead(bar, 0x1E081B0u);
    }
    /* QCA6390 GCC_GCC_PCIE_HOT_RST = 0x1E402BC (differs from WCN7850's 0x1E40304). */
    v = qcaChipRead(bar, 0x1E402BCu);
    qcaChipWrite(bar, 0x1E402BCu, v | 0x10u);
    (void)qcaChipRead(bar, 0x1E402BCu);
    IOSleep(5);

    qcaChipWrite(bar, 0x1E08228u, 0xFFFFFFFFu); /* PCIE_PCIE_INT_ALL_CLEAR */
    v = qcaChipRead(bar, 0x1F8031Cu);
    qcaChipWrite(bar, 0x1F8031Cu, v & ~0x4u);   /* WLAON_QFPROM_PWR_CTRL_REG */

    /* QCA6390 v2.0 fix_l1ss (ath11k/hw.c qca6390_regs). */
    qcaSetLinkReg(bar, 0x1E0C0ACu, 0x10u, 0xFFFFFFFFu);
    qcaSetLinkReg(bar, 0x1E0C628u, 0x02u, 0x000000FFu);
    qcaSetLinkReg(bar, 0x1E0C62Cu, 0x52u, 0x000000FFu);
    qcaSetLinkReg(bar, 0x1E0C634u, 0xFFu, 0x000000FFu);

    /* Clear BHIe TX/RX vector doorbells and status using live BHIEOFF (0x224 on QCA6390). */
    const uint32_t bhie = rd32(bar, J716F_MHI_BHIEOFF);
    if (bhie >= 0x100u && bhie < 0x1000u) {
        wr32(bar, bhie + J716F_MHI_BHIE_TXVECDB, 0u);
        wr32(bar, bhie + J716F_MHI_BHIE_TXVECSTATUS, 0u);
        wr32(bar, bhie + J716F_MHI_BHIE_RXVECDB, 0u);
        wr32(bar, bhie + J716F_MHI_BHIE_RXVECSTATUS, 0u);
    }

    (void)qcaChipRead(bar, 0x1F80500u); /* PCIE_Q6_COOKIE_ADDR */
    (void)qcaChipRead(bar, 0x1F80504u); /* WLAON_WARM_SW_ENTRY */
    IOSleep(10);
    qcaChipWrite(bar, 0x1F80504u, 0u);
    IOSleep(10);
    (void)qcaChipRead(bar, 0x1F8060Cu); /* WLAON_SOC_RESET_CAUSE_REG (read-clear) */

    /* PCIE_SOC_GLOBAL_RESET (0x3008) pulse. */
    v = rd32(bar, 0x3008u);
    wr32(bar, 0x3008u, v | 1u);
    (void)rd32(bar, 0x3008u);
    IOSleep(10);
    wr32(bar, 0x3008u, v & ~1u);
    (void)rd32(bar, 0x3008u);
    IOSleep(10);
    wr32(bar, 0x310Cu, 0x40000000u | (gQcaWindow == 0xFFFFFFFFu ? 0u : gQcaWindow));
    (void)rd32(bar, 0x310Cu);

    /* Clear any SYSERR from global reset via MHICTRL_RESET. */
    wr32(bar, J716F_MHI_CTRL, J716F_MHI_CTRL_RESET);
    IOSleep(10);
}

/* Disable ASPM L0s/L1 during MHI download and point MSI at our 32-bit sink page. */
void qca6390EndpointLinkSetup(const Region &cfg, uint64_t sinkPa) {
    uint8_t cap = static_cast<uint8_t>(rd32(cfg, 0x34u) & 0xFFu);
    for (unsigned guard = 0; cap >= 0x40u && guard < 48u; ++guard) {
        const uint32_t w = rd32(cfg, cap);
        const uint8_t id = static_cast<uint8_t>(w);
        const uint8_t next = static_cast<uint8_t>(w >> 8);
        if (id == 0x10u) {
            const uint32_t lc = rd32(cfg, cap + 0x10u);
            wr32(cfg, cap + 0x10u, lc & ~3u);
        } else if (id == 0x05u) {
            const bool is64 = ((w >> 16) & 0x80u) != 0u;
            wr32(cfg, cap + 4u, static_cast<uint32_t>(sinkPa));
            if (is64) {
                wr32(cfg, cap + 8u, static_cast<uint32_t>(sinkPa >> 32));
                wr32(cfg, cap + 0xCu, 0u);
            } else {
                wr32(cfg, cap + 8u, 0u);
            }
            /* Enable MSI with MME = 2 (4 vectors) so MHI vectors 0..2 fire cleanly into sink. */
            wr32(cfg, cap, (w & 0xFFFFu) | ((((w >> 16) & ~0x70u) | 0x20u | 1u) << 16));
            gState.wifiMsiCapAfter = rd32(cfg, cap);
        }
        cap = next;
    }
}

void putBar64(const Region &bar, uint32_t lowOff, uint64_t val) {
    wr32(bar, lowOff, static_cast<uint32_t>(val));
    wr32(bar, lowOff + 4u, static_cast<uint32_t>(val >> 32));
}

/* Program QCA6390 MHI channel/event/command context arrays (128 ch, 2 ER: 32 ctrl + 256 data). */
void programQca6390MhiContexts(const Region &bar, WifiDmaBuf &c, uint32_t mappedBytes) {
    constexpr uint32_t kCtx = 44u, kChannels = 128u, kEvents = 2u, kCmdEl = 8u, kEl = 16u;
    const uint32_t evLen[kEvents] = {32u * kEl, 256u * kEl};
    uint8_t *va = c.va;
    const uint64_t pa = c.pa;
    memset(va, 0, c.len);
    const uint32_t chOff = 0u;
    const uint32_t evOff = kChannels * kCtx;
    const uint32_t cmdOff = evOff + kEvents * kCtx;
    uint32_t cur = (cmdOff + kCtx + 4095u) & ~4095u;

    auto w32 = [&](uint32_t at, uint32_t v) { memcpy(va + at, &v, 4u); };
    auto w64 = [&](uint32_t at, uint64_t v) { memcpy(va + at, &v, 8u); };

    uint64_t evBase[kEvents];
    for (uint32_t i = 0; i < kEvents; ++i) {
        evBase[i] = pa + cur;
        const uint32_t at = evOff + i * kCtx;
        w32(at, i == 0u ? 0u : (1u << 16)); /* irq moderation 0 / 1 ms */
        w32(at + 4u, 1u);                   /* MHI_ER_TYPE_VALID */
        w32(at + 8u, i + 1u);               /* MSI vector 1 / 2 */
        w64(at + 12u, evBase[i]);
        w64(at + 20u, evLen[i]);
        w64(at + 28u, evBase[i]);           /* RP */
        w64(at + 36u, evBase[i] + evLen[i] - kEl); /* WP */
        gWifiDma.evAcked[i] = 0ULL;
        cur += evLen[i];
    }
    const uint64_t cmdBase = pa + cur;
    w64(cmdOff + 12u, cmdBase);
    w64(cmdOff + 20u, kCmdEl * kEl);
    w64(cmdOff + 28u, cmdBase);
    w64(cmdOff + 36u, cmdBase);
    cleanInv(va, c.len);

    putBar64(bar, 0x58u, pa + chOff);  /* CCABAP */
    putBar64(bar, 0x60u, pa + evOff);  /* ECABAP */
    putBar64(bar, 0x68u, pa + cmdOff); /* CRCBAP */
    putBar64(bar, 0x80u, 0ULL); putBar64(bar, 0x88u, 0xFFFFFFFFULL); /* CTRL base/limit */
    putBar64(bar, 0x98u, 0ULL); putBar64(bar, 0xA0u, 0xFFFFFFFFULL); /* DATA base/limit */
    wr32(bar, 0x10u, (rd32(bar, 0x10u) & ~0xFFFF0000u) | (kEvents << 16)); /* MHICFG: NER=2 */

    const uint32_t erdb = rd32(bar, 0x20u);
    if (erdb + 8u * kEvents <= mappedBytes) {
        for (uint32_t i = 0; i < kEvents; ++i) {
            putBar64(bar, erdb + 8u * i, evBase[i] + evLen[i] - kEl);
        }
    }
}

unsigned ackQca6390MhiEvents(const Region &bar, WifiDmaBuf &c) {
    constexpr uint32_t kCtx = 44u, kChannels = 128u;
    const uint32_t erdb = rd32(bar, 0x20u);
    unsigned moved = 0u;
    for (unsigned r = 0; r < 2u; ++r) {
        uint8_t *ctx = c.va + kChannels * kCtx + r * kCtx;
        cleanInv(ctx, kCtx);
        uint64_t base = 0, len = 0, rp = 0;
        memcpy(&base, ctx + 12u, 8u);
        memcpy(&len, ctx + 20u, 8u);
        memcpy(&rp, ctx + 28u, 8u);
        if (rp == gWifiDma.evAcked[r] || rp < base || rp >= base + len) continue;
        gWifiDma.evAcked[r] = rp;
        const uint64_t wp = (rp == base) ? (base + len - 16ULL) : (rp - 16ULL);
        memcpy(ctx + 36u, &wp, 8u);
        cleanInv(ctx + 36u, 8u);
        putBar64(bar, erdb + 8u * r, wp);
        ++moved;
    }
    return moved;
}

uint32_t mhiBarRead(void *ctx, uint32_t reg) {
    return rd32(*static_cast<const Region *>(ctx), reg);
}

void mhiBarWrite(void *ctx, uint32_t reg, uint32_t val) {
    wr32(*static_cast<const Region *>(ctx), reg, val);
}

}  // namespace

extern "C" int j716f_wcn_wifi_fw_now(void) {
    ensureMapped();
    gState.wifiFwAttempted = true;
    gState.wifiFwStage = 1;
    gState.wifiFwErr = 0;
    gState.wifiSmmuSlot = -1;
    gState.wifiSmmuBank = -1;
    gState.wifiSmmuGfsr = 0u;
    gState.wifiSeg0Pa = 0ULL;
    gState.wifiNumSegs = 0u;
    gState.wifiMhiPhase = 0u;
    gState.wifiMhiErr = 0u;
    gState.wifiMhiFailPhase = 0u;
    gState.wifiMhiEe = 0xFFu;
    gState.wifiMhiState = 0xFFu;
    gState.wifiBhiStatus = 0u;
    gState.wifiBhiErr = 0u;
    gState.wifiBhieTxStatus = 0u;
    gState.wifiMhiStatus = 0u;
    gState.wifiEventsAcked = 0u;
    gState.wifiBootMs = 0u;
    gState.wifiMsiCapAfter = 0u;
    gState.wifiSinkWord0 = 0u;

    if (j716f_wcn_wifi_amss_fw_size < kWifiSegSize) {
        gState.wifiFwErr = -2;
        return -2;
    }

    /* Stage 2: Ensure PCIe0 link is trained to L0 + BAR0 assigned (17cb:1101). */
    gState.wifiFwStage = 2;
    if (gState.wifiStage < 12 || (gState.epId & 0xFFFFu) != 0x17CBu) {
        const int prc = j716f_wcn_wifi_probe_now();
        if (prc != 0 || gState.wifiStage < 12) {
            gState.wifiFwErr = prc ? prc : -19;
            return gState.wifiFwErr;
        }
    }

    /* Stage 3: Configure APPS SMMU bypass for PCIe0 endpoint SID 0x1C01. */
    gState.wifiFwStage = 3;
    if (!gSmmu.va) {
        gState.wifiFwErr = -12;
        return -12;
    }
    const struct smmu_io sio = {nullptr, smmuIoRead, smmuIoWrite};
    const struct smmu_plan plan = smmu_plan_bypass(&sio, kPcie0EpSid, 0x80000000ULL, 0x7FFFFFFFULL);
    gState.wifiSmmuSlot = static_cast<int>(plan.slot);
    gState.wifiSmmuBank = static_cast<int>(plan.bank);
    if (const char *serr = smmu_configure_bypass(&sio, &plan, kPcie0EpSid)) {
        IOLog("[J716F wcn] wifi SMMU SID 0x%x failed: %s\n", kPcie0EpSid, serr);
        gState.wifiFwErr = -5;
        return -5;
    }

    /* Stage 4: Allocate 32-bit power-of-two aligned DMA buffers for BHI/BHIe + MHI contexts. */
    gState.wifiFwStage = 4;
    const uint32_t amssSize = j716f_wcn_wifi_amss_fw_size;
    const uint32_t numSegs = (amssSize + kWifiSegSize - 1u) / kWifiSegSize;
    if (numSegs == 0u || numSegs > kWifiMaxSegs) {
        gState.wifiFwErr = -27;
        return -27;
    }
    if (!initWifiDmaArena(numSegs)) {
        gState.wifiFwErr = -12;
        return -12;
    }
    gState.wifiSeg0Pa = gWifiDma.seg[0].pa;
    gState.wifiNumSegs = numSegs;

    /* Copy amss20.bin into 512 KiB segments and build BHIe vector table. */
    uint64_t segAddrs[kWifiMaxSegs];
    for (uint32_t i = 0; i < numSegs; ++i) {
        const uint32_t off = i * kWifiSegSize;
        const uint32_t left = amssSize - off;
        const uint32_t take = left < kWifiSegSize ? left : kWifiSegSize;
        memcpy(gWifiDma.seg[i].va, j716f_wcn_wifi_amss_fw + off, take);
        if (take < kWifiSegSize) memset(gWifiDma.seg[i].va + take, 0, kWifiSegSize - take);
        cleanInv(gWifiDma.seg[i].va, kWifiSegSize);
        segAddrs[i] = gWifiDma.seg[i].pa;
    }
    const uint32_t entries = j716f_mhi_bhie_table(
        reinterpret_cast<J716fMhiBhieEntry *>(gWifiDma.table.va),
        4096u / sizeof(J716fMhiBhieEntry), segAddrs, amssSize, kWifiSegSize);
    if (entries != numSegs) {
        gState.wifiFwErr = -22;
        return -22;
    }
    cleanInv(gWifiDma.table.va, 4096u);
    memset(gWifiDma.sink.va, 0, 4096u);
    cleanInv(gWifiDma.sink.va, 4096u);

    /* Stage 5: Map PCIe0 CFG + 2 MiB BAR0, run QCA6390 chipPowerUp + MSI/ASPM setup. */
    gState.wifiFwStage = 5;
    Region cfg, bar;
    const uint32_t barAddr = gState.epBar0Assigned & ~0xFu;
    constexpr uint32_t kBarMap = 0x200000u;
    if (!barAddr || !mapRegion(J716F_PCIE0_CFG_BASE, 0x1000u, cfg) ||
        !mapRegion(barAddr, kBarMap, bar)) {
        unmapRegion(bar);
        unmapRegion(cfg);
        gState.wifiFwErr = -13;
        return -13;
    }

    const uint64_t t0 = mach_absolute_time();
    qca6390ChipPowerUp(bar);
    qca6390EndpointLinkSetup(cfg, gWifiDma.sink.pa);

    /* Stage 6: Open MHI, pre-program contexts, start BHI SBL -> M0 -> BHIe AMSS boot. */
    gState.wifiFwStage = 6;
    J716fMhiBoot boot = {};
    const J716fMhiIo mio = {mhiBarRead, mhiBarWrite, &bar, kBarMap};
    if (j716f_mhi_boot_open(&boot, &mio) != 0) {
        gState.wifiMhiPhase = boot.phase;
        gState.wifiMhiErr = boot.error;
        gState.wifiFwErr = -6;
        unmapRegion(bar);
        unmapRegion(cfg);
        return -6;
    }
    programQca6390MhiContexts(bar, gWifiDma.ctx, kBarMap);

    uint32_t phase = j716f_mhi_boot_begin(
        &boot, 0u, gWifiDma.seg[0].pa, kWifiSegSize,
        gWifiDma.table.pa, entries * static_cast<uint32_t>(sizeof(J716fMhiBhieEntry)),
        0x5EEDu);
    gState.wifiFwStage = 7;

    uint32_t lastPhase = phase;
    unsigned since = 0u;
    for (unsigned i = 0; i < 3000u && phase != J716fMhiDone && phase != J716fMhiFailed; ++i) {
        if (phase == J716fMhiNeedContexts) {
            programQca6390MhiContexts(bar, gWifiDma.ctx, kBarMap);
            gState.wifiFwStage = 8;
            phase = j716f_mhi_boot_contexts_done(&boot);
        } else {
            if (i > 0u && (i % 40u) == 0u && rd32(bar, J716F_MHI_STATUS) == 0xFFFFFFFFu) {
                /* Recover BAR0/CMD if QCA6390 PCIe core reset during SBL transition. */
                if (rd32(cfg, 0x00u) == gState.epId) {
                    if ((rd32(cfg, 0x10u) & ~0xFu) != barAddr || (rd32(cfg, 0x04u) & 6u) != 6u) {
                        wr32(cfg, 0x10u, gState.epBar0Assigned);
                        wr32(cfg, 0x14u, 0u);
                        wr32(cfg, 0x04u, (rd32(cfg, 0x04u) & 0xFFFFu) | 6u);
                    }
                }
            }
            phase = j716f_mhi_boot_step(&boot);
        }
        if (phase != J716fMhiNeedContexts && phase != J716fMhiDone && phase != J716fMhiFailed) {
            gState.wifiEventsAcked += ackQca6390MhiEvents(bar, gWifiDma.ctx);
        }
        if (phase != lastPhase) {
            lastPhase = phase;
            since = 0u;
        } else if (++since >= 2000u) { /* 10 seconds per phase */
            phase = j716f_mhi_boot_timeout(&boot);
            break;
        }
        IOSleep(5);
    }

    uint64_t bootNs = 0;
    absolutetime_to_nanoseconds(mach_absolute_time() - t0, &bootNs);
    gState.wifiBootMs = static_cast<uint32_t>(bootNs / 1000000ULL);
    gState.wifiMhiPhase = boot.phase;
    gState.wifiMhiErr = boot.error;
    gState.wifiMhiFailPhase = boot.failed_phase;
    gState.wifiMhiEe = j716f_mhi_boot_ee(&boot);
    gState.wifiMhiState = j716f_mhi_boot_state(&boot);
    gState.wifiBhiStatus = rd32(bar, boot.bhi + J716F_MHI_BHI_STATUS);
    gState.wifiBhiErr = rd32(bar, boot.bhi + J716F_MHI_BHI_ERRCODE);
    gState.wifiBhieTxStatus = rd32(bar, boot.bhie + J716F_MHI_BHIE_TXVECSTATUS);
    gState.wifiMhiStatus = rd32(bar, J716F_MHI_STATUS);
    gState.wifiSmmuGfsr = rd32(gSmmu, 0x48u);
    cleanInv(gWifiDma.sink.va, 64u);
    memcpy(&gState.wifiSinkWord0, gWifiDma.sink.va, 4u);
    if (phase == J716fMhiDone) {
        gState.wifiFwStage = 9;
        gState.wifiFwErr = 0;
    } else {
        gState.wifiFwErr = -static_cast<int>(boot.error ? boot.error : 99u);
    }

    unmapRegion(bar);
    unmapRegion(cfg);

    IOLog("[J716F wcn] wifi.fw stg=%d err=%d ph=%u(merr=%u@%u) ee=0x%x st=0x%x bhi=0x%08x/%08x bhie=0x%08x smmu=%d/%d gfsr=0x%x seg0=0x%llx ev=%u ms=%u\n",
          gState.wifiFwStage, gState.wifiFwErr, gState.wifiMhiPhase, gState.wifiMhiErr,
          gState.wifiMhiFailPhase, gState.wifiMhiEe, gState.wifiMhiState,
          gState.wifiBhiStatus, gState.wifiBhiErr, gState.wifiBhieTxStatus,
          gState.wifiSmmuSlot, gState.wifiSmmuBank, gState.wifiSmmuGfsr,
          static_cast<unsigned long long>(gState.wifiSeg0Pa), gState.wifiEventsAcked,
          gState.wifiBootMs);
    return gState.wifiFwErr;
}

/* ---- Stage C3b-1: System Bluetooth (com.apple.uart.sk.bluetooth + H4/IBS worker) ---- */

namespace {

IOService *gBtRadioSvc = nullptr;
volatile uint32_t gBtSvcWake = 0u;
volatile uint32_t gBtSvcPowerReq = 0u;

bool publishBtAddress() {
    if (gState.btSvcAddrPublished) return true;
    uint64_t hash = 0xcbf29ce484222325ULL;
    static const char kDomain[] = "unixnu-j716f-bluetooth";
    for (unsigned i = 0; i < sizeof(kDomain) - 1u; ++i) {
        hash = (hash ^ static_cast<uint8_t>(kDomain[i])) * 0x100000001b3ULL;
    }
    IORegistryEntry *chosen = IORegistryEntry::fromPath("/chosen", gIODTPlane);
    if (chosen) {
        OSData *id = OSDynamicCast(OSData, chosen->getProperty("unique-chip-id"));
        if (id && id->getLength() >= 4u) {
            const uint8_t *seed = static_cast<const uint8_t *>(id->getBytesNoCopy());
            for (unsigned i = 0; i < id->getLength(); ++i) {
                hash = (hash ^ seed[i]) * 0x100000001b3ULL;
            }
        }
    }
    for (unsigned i = 0; i < 6u; ++i) {
        gState.btSvcAddr[i] = static_cast<uint8_t>(hash >> (i * 8u));
    }
    gState.btSvcAddr[0] = (gState.btSvcAddr[0] & 0xFEu) | 0x02u; /* locally administered unicast */

    if (chosen) {
        (void)chosen->setProperty("mac-address-bluetooth0", gState.btSvcAddr, 6u);
    }
    IORegistryEntry *node = IORegistryEntry::fromPath("/arm-io/bluetooth", gIODTPlane);
    if (node) {
        (void)node->setProperty("local-mac-address", gState.btSvcAddr, 6u);
    }
    if (!gBtRadioSvc) {
        IOService *radio = new IOService;
        if (radio && radio->init()) {
            radio->setName("bluetooth");
            uint32_t enc = 1u;
            uint32_t spd = 3200000u;
            (void)radio->setProperty("device_type", "bluetooth");
            (void)radio->setProperty("transport-encoding", &enc, sizeof(enc));
            (void)radio->setProperty("transport-speed", &spd, sizeof(spd));
            (void)radio->setProperty("local-mac-address", gState.btSvcAddr, 6u);
            IOService *plat = IOService::getPlatform();
            if (plat && radio->attach(plat)) {
                gBtRadioSvc = radio;
                radio->registerService();
            } else {
                radio->release();
            }
        } else if (radio) {
            radio->release();
        }
    }
    if (node) node->release();
    if (chosen) chosen->release();
    gState.btSvcAddrPublished = true;
    return true;
}

int btSvcBootPatchedHardware() {
    if (j716f_wcn_bt_tlv_fw_size < 40u || j716f_wcn_bt_nvm_fw_size < 32u) return -2;
    const int prc = j716f_wcn_bt_probe_now();
    if (prc != 0 || gState.usedDma) return prc ? prc : -95;
    uart6RxReset();
    uart6DrainRx();
    uart6RxReset();
    if (!uart6SwitchBaud(3200000u, J716F_QCA_BAUD_3200000)) return -5;

    uint8_t ev[260];
    uint32_t evLen = 0u;
    const uint8_t getVerParam[1] = {0x19u};
    if (!uart6HciCmdSync(0xFC00u, getVerParam, 1u, ev, sizeof(ev), &evLen, 500u)) return -110;

    const uint8_t tlvDnldMode = j716f_wcn_bt_tlv_fw[14];
    if (!qcaDownloadTlv(j716f_wcn_bt_tlv_fw, j716f_wcn_bt_tlv_fw_size, tlvDnldMode, &gState.btTlvChunks)) {
        return -71;
    }
    IOSleep(10);

    uint8_t *nvmCopy = static_cast<uint8_t *>(IOMallocData(j716f_wcn_bt_nvm_fw_size));
    if (!nvmCopy) return -12;
    memcpy(nvmCopy, j716f_wcn_bt_nvm_fw, j716f_wcn_bt_nvm_fw_size);
    gState.btNvmPatched = j716f_qca_patch_nvm(nvmCopy, j716f_wcn_bt_nvm_fw_size,
                                              J716F_QCA_BAUD_3200000, false, false);
    const bool nvmOk = qcaDownloadTlv(nvmCopy, j716f_wcn_bt_nvm_fw_size, 0u, &gState.btNvmChunks);
    IOFreeData(nvmCopy, j716f_wcn_bt_nvm_fw_size);
    if (!nvmOk) return -72;

    static const uint8_t kDisableLog[2] = {0x14u, 0x00u};
    (void)uart6HciCmdSync(0xFC17u, kDisableLog, sizeof(kDisableLog), ev, sizeof(ev), &evLen, 1000u);
    if (!uart6HciCmdSync(0x0C03u, nullptr, 0u, ev, sizeof(ev), &evLen, 2000u) ||
        evLen < 7u || ev[6] != 0u) {
        return -73;
    }
    IOSleep(25);
    /* Write deterministic BD_ADDR in little-endian HCI order. */
    uint8_t bdLe[6];
    for (unsigned i = 0; i < 6u; ++i) bdLe[i] = gState.btSvcAddr[5u - i];
    (void)uart6HciCmdSync(0xFC14u, bdLe, 6u, ev, sizeof(ev), &evLen, 1000u);
    if (uart6HciCmdSync(0x1009u, nullptr, 0u, ev, sizeof(ev), &evLen, 1000u) &&
        evLen >= 13u && ev[6] == 0u) {
        memcpy(gState.btBdAddr, ev + 7u, 6u);
    }
    return 0;
}

void btSvcWakeCb(void *) {
    __atomic_store_n(&gBtSvcWake, 1u, __ATOMIC_RELEASE);
    thread_wakeup(reinterpret_cast<event_t>(const_cast<uint32_t *>(&gBtSvcWake)));
}

int btSvcPowerCb(void *, bool enabled) {
    uint32_t expected = 0u;
    if (!__atomic_compare_exchange_n(&gBtSvcPowerReq, &expected, enabled ? 1u : 2u,
                                     false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        return 16; /* EBUSY */
    }
    btSvcWakeCb(nullptr);
    return 0;
}

int btSvcProcIterCb(struct proc *p, void *ctx) {
    if (!&proc_pid) return 0;
    const char *name = proc_best_name(p);
    if (!name) return 0;
    int *found = static_cast<int *>(ctx);
    if (strcmp(name, "bluetoothd") == 0) found[0] = proc_pid(p);
    else if (strcmp(name, "BlueTool") == 0) found[1] = proc_pid(p);
    return 0;
}

void btSvcPollProcs() {
    if (!&proc_iterate) return;
    int found[2] = {0, 0};
    proc_iterate(1u | 4u, btSvcProcIterCb, found, nullptr, nullptr);
    gState.btSvcPidBtd = static_cast<uint32_t>(found[0]);
    gState.btSvcPidBtl = static_cast<uint32_t>(found[1]);
}

/* Forward complete H4 packets (0x04 Event, 0x02 ACL except 0x2EDC debug, 0x03 SCO, 0x05 ISO)
 * from UART6 RX ring into Skywalk com.apple.uart.sk.bluetooth, handling QCA IBS bytes inline. */
bool btSvcPumpRxToHost() {
    uart6DrainRx();
    bool work = false;
    uint8_t pkt[1040];
    while (gUart6Rx.count > 0u) {
        const uint8_t ty = uart6RxPeek(0u);
        if (ty == 0xFDu) {
            uart6RxDrop(1u);
            static const uint8_t kWakeAck[1] = {0xFCu};
            (void)uart6SendFifo(kWakeAck, 1u);
            gState.btSvcIbsWakeRx = gState.btSvcIbsWakeRx + 1u;
            work = true;
            continue;
        }
        if (ty == 0xFEu) {
            uart6RxDrop(1u);
            gState.btSvcIbsSleepRx = gState.btSvcIbsSleepRx + 1u;
            continue;
        }
        if (ty == 0xFCu) {
            uart6RxDrop(1u);
            continue;
        }
        uint32_t total = 0u;
        bool dropQcaLog = false;
        if (ty == 0x04u) {
            if (gUart6Rx.count < 3u) break;
            total = 3u + static_cast<uint32_t>(uart6RxPeek(2u));
        } else if (ty == 0x02u) {
            if (gUart6Rx.count < 5u) break;
            const uint16_t handle = static_cast<uint16_t>(uart6RxPeek(1u)) |
                                    (static_cast<uint16_t>(uart6RxPeek(2u)) << 8);
            const uint32_t alen = static_cast<uint32_t>(uart6RxPeek(3u)) |
                                  (static_cast<uint32_t>(uart6RxPeek(4u)) << 8);
            total = 5u + alen;
            dropQcaLog = (handle == 0x2EDCu);
        } else if (ty == 0x03u) {
            if (gUart6Rx.count < 4u) break;
            total = 4u + static_cast<uint32_t>(uart6RxPeek(3u));
        } else if (ty == 0x05u) {
            if (gUart6Rx.count < 5u) break;
            const uint32_t ilen = (static_cast<uint32_t>(uart6RxPeek(3u)) |
                                   (static_cast<uint32_t>(uart6RxPeek(4u)) << 8)) & 0x3FFFu;
            total = 5u + ilen;
        } else {
            uart6RxDrop(1u);
            continue;
        }
        if (total > sizeof(pkt) || total > sizeof(gUart6Rx.buf)) {
            uart6RxDrop(1u);
            continue;
        }
        if (gUart6Rx.count < total) break;
        if (dropQcaLog) {
            uart6RxDrop(total);
            continue;
        }
        for (uint32_t i = 0; i < total; ++i) pkt[i] = uart6RxPeek(i);
        if (qbt_transport_active()) {
            const uint32_t taken = qbt_transport_receive(pkt, total);
            if (taken < total) break; /* backpressure */
            gState.btSvcRxBytes = gState.btSvcRxBytes + total;
            gState.btSvcRxFrames = gState.btSvcRxFrames + 1u;
        }
        uart6RxDrop(total);
        work = true;
    }
    return work;
}

void btSvcRecordTxOp(uint16_t op) {
    gState.btSvcTxCmds = gState.btSvcTxCmds + 1u;
    const uint32_t idx = gState.btSvcLastOpsCount % 12u;
    gState.btSvcLastOps[idx] = op;
    ++gState.btSvcLastOpsCount;
}

void btSvcWorker(void *, wait_result_t) {
    thread_set_thread_name(current_thread(), "j716f-bt-svc");
    ensureMapped();
    publishBtAddress();

    const struct qbt_transport_ops ops = {nullptr, btSvcWakeCb, btSvcPowerCb};
    (void)qbt_transport_init(&ops);

    gState.btSvcState = 1u;
    int err = btSvcBootPatchedHardware();
    gState.btSvcErr = err;
    gState.btSvcState = (err == 0) ? 2u : 3u;
    qbt_transport_power_result(err == 0, err < 0 ? -err : err, "qca6390-htbtfw20", 3200000u);
    IOLog("[J716F bt.svc] boot rc=%d bd=%02x:%02x:%02x:%02x:%02x:%02x\n",
          err, gState.btSvcAddr[0], gState.btSvcAddr[1], gState.btSvcAddr[2],
          gState.btSvcAddr[3], gState.btSvcAddr[4], gState.btSvcAddr[5]);

    uint8_t txBuf[1040];
    uint32_t txLen = 0u;
    uint64_t nextProcPoll = 0ULL;

    for (;;) {
        const uint32_t req = __atomic_exchange_n(&gBtSvcPowerReq, 0u, __ATOMIC_ACQ_REL);
        if (req == 1u) {
            if (gState.btSvcState != 2u) {
                gState.btSvcState = 1u;
                err = btSvcBootPatchedHardware();
                gState.btSvcErr = err;
                gState.btSvcState = (err == 0) ? 2u : 3u;
                qbt_transport_power_result(err == 0, err < 0 ? -err : err, "qca6390-htbtfw20", 3200000u);
            } else {
                qbt_transport_power_result(true, 0, "qca6390-htbtfw20", 3200000u);
            }
        } else if (req == 2u) {
            qbt_transport_power_result(false, 0, "qca6390-htbtfw20", 3200000u);
        }

        bool busy = false;
        if (gState.btSvcState == 2u) {
            if (btSvcPumpRxToHost()) busy = true;
            if (qbt_transport_active() && txLen < sizeof(txBuf)) {
                const uint32_t got = qbt_transport_tx_take(txBuf + txLen,
                                                           static_cast<uint32_t>(sizeof(txBuf) - txLen));
                if (got > 0u) {
                    txLen += got;
                    busy = true;
                }
            }
            /* Send complete H4 frames from txBuf so IBS bytes never interleave mid-packet. */
            while (txLen > 0u) {
                const uint8_t ty = txBuf[0];
                uint32_t flen = 0u;
                if (ty == 0x01u) {
                    if (txLen < 4u) break;
                    flen = 4u + static_cast<uint32_t>(txBuf[3]);
                } else if (ty == 0x02u) {
                    if (txLen < 5u) break;
                    flen = 5u + (static_cast<uint32_t>(txBuf[3]) | (static_cast<uint32_t>(txBuf[4]) << 8));
                } else if (ty == 0x03u) {
                    if (txLen < 4u) break;
                    flen = 4u + static_cast<uint32_t>(txBuf[3]);
                } else if (ty == 0x05u) {
                    if (txLen < 5u) break;
                    flen = 5u + ((static_cast<uint32_t>(txBuf[3]) | (static_cast<uint32_t>(txBuf[4]) << 8)) & 0x3FFFu);
                } else {
                    memmove(txBuf, txBuf + 1u, --txLen);
                    continue;
                }
                if (flen > sizeof(txBuf)) {
                    txLen = 0u;
                    break;
                }
                if (txLen < flen) break;
                if (ty == 0x01u) {
                    const uint16_t op = static_cast<uint16_t>(txBuf[1]) | (static_cast<uint16_t>(txBuf[2]) << 8);
                    btSvcRecordTxOp(op);
                }
                (void)uart6SendFifo(txBuf, flen);
                gState.btSvcTxBytes = gState.btSvcTxBytes + flen;
                gState.btSvcTxFrames = gState.btSvcTxFrames + 1u;
                txLen -= flen;
                if (txLen > 0u) memmove(txBuf, txBuf + flen, txLen);
                busy = true;
            }
        }

        uint64_t nowNs = 0ULL;
        absolutetime_to_nanoseconds(mach_absolute_time(), &nowNs);
        if (nowNs >= nextProcPoll) {
            btSvcPollProcs();
            nextProcPoll = nowNs + 2000000000ULL;
        }

        if (!busy) {
            if (!__atomic_exchange_n(&gBtSvcWake, 0u, __ATOMIC_ACQ_REL)) {
                IOSleep(qbt_transport_active() ? 2u : 10u);
            }
        }
    }
}

}  // namespace

extern "C" int j716f_wcn_bt_service_start(void) {
    ensureMapped();
    if (gState.btSvcStarted) {
        btSvcPollProcs();
        return gState.btSvcState == 2u ? 0 : gState.btSvcErr;
    }
    gState.btSvcStarted = true;
    publishBtAddress();
    thread_t th = nullptr;
    if (kernel_thread_start(btSvcWorker, nullptr, &th) != KERN_SUCCESS || !th) {
        gState.btSvcState = 3u;
        gState.btSvcErr = -12;
        return -12;
    }
    thread_deallocate(th);
    /* Wait up to 4 seconds for initial firmware load so interactive COM9 command sees live state. */
    for (unsigned i = 0; i < 400u && gState.btSvcState < 2u; ++i) {
        IOSleep(10);
    }
    btSvcPollProcs();
    return gState.btSvcState == 2u ? 0 : gState.btSvcErr;
}

/* ---- Stage C4b-1: QCA6390 MHI IPCR Ch20/21 + QRTR + QMI WLFW Handshake ---- */

namespace {

constexpr uint32_t kIpcrEls = 64u;
constexpr uint32_t kIpcrRingBytes = kIpcrEls * 16u; /* 1024 B */
constexpr uint32_t kIpcrDlBufs = 32u;
constexpr uint32_t kIpcrUlBufs = 16u;
constexpr uint32_t kIpcrBufBytes = 8192u;
constexpr uint32_t kQmiClientPort = 0x4001u;

struct WifiQmiRings {
    uint64_t erBase[2];
    uint64_t erLen[2];
    uint64_t erRp[2];
    uint8_t *erVa[2];
    uint8_t *erCtx[2];

    uint64_t cmdBase;
    uint64_t cmdLen;
    uint64_t cmdWp;
    uint8_t *cmdVa;
    uint8_t *cmdCtx;

    uint64_t ulRingPa;
    uint8_t *ulRingVa;
    uint64_t ulWp;
    uint32_t ulBufIdx;
    uint64_t ulBufBasePa;
    uint8_t *ulBufBaseVa;

    uint64_t dlRingPa;
    uint8_t *dlRingVa;
    uint64_t dlWp;
    uint64_t dlBufBasePa;
    uint8_t *dlBufBaseVa;

    uint32_t chdb;
    uint32_t erdb;

    uint64_t q6HeapCursor;
    uint64_t q6HeapEnd;
};

void mhiInitRingsFromCtx(const Region &bar, WifiQmiRings &r) {
    constexpr uint32_t kCtx = 44u, kChannels = 128u;
    memset(&r, 0, sizeof(r));
    r.chdb = rd32(bar, J716F_MHI_CHDBOFF);
    r.erdb = rd32(bar, J716F_MHI_ERDBOFF);

    for (unsigned i = 0; i < 2u; ++i) {
        uint8_t *ctx = gWifiDma.ctx.va + kChannels * kCtx + i * kCtx;
        r.erCtx[i] = ctx;
        cleanInv(ctx, kCtx);
        memcpy(&r.erBase[i], ctx + 12u, 8u);
        memcpy(&r.erLen[i],  ctx + 20u, 8u);
        memcpy(&r.erRp[i],   ctx + 28u, 8u);
        r.erVa[i] = gWifiDma.ctx.va + (r.erBase[i] - gWifiDma.ctx.pa);
        /* Ensure event ring WP trails RP by one element so QCA6390 has full ring space. */
        const uint64_t wp = (r.erRp[i] == r.erBase[i]) ? (r.erBase[i] + r.erLen[i] - 16ULL) : (r.erRp[i] - 16ULL);
        memcpy(ctx + 36u, &wp, 8u);
        cleanInv(ctx + 36u, 8u);
        putBar64(bar, r.erdb + 8u * i, wp);
    }

    r.cmdCtx = gWifiDma.ctx.va + (kChannels + 2u) * kCtx;
    cleanInv(r.cmdCtx, kCtx);
    memcpy(&r.cmdBase, r.cmdCtx + 12u, 8u);
    memcpy(&r.cmdLen,  r.cmdCtx + 20u, 8u);
    memcpy(&r.cmdWp,   r.cmdCtx + 36u, 8u);
    r.cmdVa = gWifiDma.ctx.va + (r.cmdBase - gWifiDma.ctx.pa);

    /* Layout IPCR rings & buffers starting at +6 MiB inside our 64 MiB DMA arena. */
    const uint32_t ipcrOff = 0x600000u;
    memset(gWifiDma.arenaVa + ipcrOff, 0, 0x100000u);
    r.ulRingPa = gWifiDma.arenaPa + ipcrOff + 0x0000u;
    r.ulRingVa = gWifiDma.arenaVa + ipcrOff + 0x0000u;
    r.ulWp = r.ulRingPa;

    r.dlRingPa = gWifiDma.arenaPa + ipcrOff + 0x1000u;
    r.dlRingVa = gWifiDma.arenaVa + ipcrOff + 0x1000u;
    r.dlWp = r.dlRingPa;

    r.dlBufBasePa = gWifiDma.arenaPa + ipcrOff + 0x10000u;
    r.dlBufBaseVa = gWifiDma.arenaVa + ipcrOff + 0x10000u;
    r.ulBufBasePa = gWifiDma.arenaPa + ipcrOff + 0x50000u;
    r.ulBufBaseVa = gWifiDma.arenaVa + ipcrOff + 0x50000u;

    r.q6HeapCursor = gWifiDma.arenaPa + 0x800000ULL;
    r.q6HeapEnd = gWifiDma.arenaPa + gWifiDma.arenaBytes;

    /* Program Channel 20 (IPCR UL) and Channel 21 (IPCR DL) contexts in CCABAP. */
    auto setupChan = [&](uint32_t ch, uint32_t chtype, uint64_t rbase) {
        uint8_t *cc = gWifiDma.ctx.va + ch * kCtx;
        const uint32_t chcfg = J716F_MHI_CH_STATE_ENABLED | (J716F_MHI_DB_BRST_DISABLE << 8);
        const uint32_t eridx = 1u;
        const uint64_t rlen = kIpcrRingBytes;
        memcpy(cc + 0u,  &chcfg, 4u);
        memcpy(cc + 4u,  &chtype, 4u);
        memcpy(cc + 8u,  &eridx, 4u);
        memcpy(cc + 12u, &rbase, 8u);
        memcpy(cc + 20u, &rlen, 8u);
        memcpy(cc + 28u, &rbase, 8u);
        memcpy(cc + 36u, &rbase, 8u);
        cleanInv(cc, kCtx);
    };
    setupChan(J716F_MHI_CH_IPCR_UL, J716F_MHI_CH_TYPE_OUTBOUND, r.ulRingPa);
    setupChan(J716F_MHI_CH_IPCR_DL, J716F_MHI_CH_TYPE_INBOUND,  r.dlRingPa);
}

bool mhiPopEvent(const Region &bar, WifiQmiRings &r, unsigned erIdx, J716fMhiRingElement *out) {
    cleanInv(r.erCtx[erIdx], 44u);
    uint64_t devRp = 0ULL;
    memcpy(&devRp, r.erCtx[erIdx] + 28u, 8u);
    if (devRp < r.erBase[erIdx] || devRp >= r.erBase[erIdx] + r.erLen[erIdx]) return false;
    if (r.erRp[erIdx] == devRp) return false;

    const uint64_t off = r.erRp[erIdx] - r.erBase[erIdx];
    cleanInv(r.erVa[erIdx] + off, 16u);
    memcpy(out, r.erVa[erIdx] + off, sizeof(*out));

    r.erRp[erIdx] += 16ULL;
    if (r.erRp[erIdx] >= r.erBase[erIdx] + r.erLen[erIdx]) r.erRp[erIdx] = r.erBase[erIdx];

    const uint64_t newWp = (r.erRp[erIdx] == r.erBase[erIdx])
                               ? (r.erBase[erIdx] + r.erLen[erIdx] - 16ULL)
                               : (r.erRp[erIdx] - 16ULL);
    memcpy(r.erCtx[erIdx] + 36u, &newWp, 8u);
    cleanInv(r.erCtx[erIdx] + 36u, 8u);
    putBar64(bar, r.erdb + 8u * erIdx, newWp);
    return true;
}

uint32_t mhiStartChanSync(const Region &bar, WifiQmiRings &r, uint32_t chid) {
    const uint64_t trePa = r.cmdWp;
    const uint64_t off = trePa - r.cmdBase;
    J716fMhiRingElement cmd = {};
    j716f_mhi_make_cmd_tre(&cmd, chid, J716F_MHI_CMD_START_CHAN);
    memcpy(r.cmdVa + off, &cmd, sizeof(cmd));
    cleanInv(r.cmdVa + off, sizeof(cmd));

    r.cmdWp += 16ULL;
    if (r.cmdWp >= r.cmdBase + r.cmdLen) r.cmdWp = r.cmdBase;
    memcpy(r.cmdCtx + 36u, &r.cmdWp, 8u);
    cleanInv(r.cmdCtx + 36u, 8u);
    putBar64(bar, J716F_MHI_CRDB_LO, r.cmdWp);

    for (unsigned i = 0; i < 400u; ++i) {
        J716fMhiRingElement ev = {};
        while (mhiPopEvent(bar, r, 0u, &ev)) {
            if (j716f_mhi_ev_type(&ev) == J716F_MHI_PKT_CMD_COMPLETION_EVENT &&
                ev.ptr == trePa) {
                return j716f_mhi_ev_code(&ev);
            }
        }
        IOSleep(5);
    }
    return 0xFFu;
}

void mhiQueueDlBuf(const Region &bar, WifiQmiRings &r, uint32_t bufIdx) {
    const uint64_t bufPa = r.dlBufBasePa + static_cast<uint64_t>(bufIdx) * kIpcrBufBytes;
    const uint64_t off = r.dlWp - r.dlRingPa;
    J716fMhiRingElement tre = {};
    j716f_mhi_make_xfer_tre(&tre, bufPa, kIpcrBufBytes, true);
    memcpy(r.dlRingVa + off, &tre, sizeof(tre));
    cleanInv(r.dlRingVa + off, sizeof(tre));

    r.dlWp += 16ULL;
    if (r.dlWp >= r.dlRingPa + kIpcrRingBytes) r.dlWp = r.dlRingPa;
    uint8_t *cc = gWifiDma.ctx.va + J716F_MHI_CH_IPCR_DL * 44u;
    memcpy(cc + 36u, &r.dlWp, 8u);
    cleanInv(cc + 36u, 8u);
    putBar64(bar, r.chdb + 8u * J716F_MHI_CH_IPCR_DL, r.dlWp);
}

bool mhiSendUlPacket(const Region &bar, WifiQmiRings &r, const uint8_t *pkt, size_t len) {
    if (!pkt || len == 0u || len > kIpcrBufBytes) return false;
    const uint32_t idx = r.ulBufIdx % kIpcrUlBufs;
    ++r.ulBufIdx;
    uint8_t *va = r.ulBufBaseVa + static_cast<uint64_t>(idx) * kIpcrBufBytes;
    const uint64_t pa = r.ulBufBasePa + static_cast<uint64_t>(idx) * kIpcrBufBytes;
    memcpy(va, pkt, len);
    cleanInv(va, len);

    const uint64_t off = r.ulWp - r.ulRingPa;
    J716fMhiRingElement tre = {};
    j716f_mhi_make_xfer_tre(&tre, pa, static_cast<uint32_t>(len), true);
    memcpy(r.ulRingVa + off, &tre, sizeof(tre));
    cleanInv(r.ulRingVa + off, sizeof(tre));

    r.ulWp += 16ULL;
    if (r.ulWp >= r.ulRingPa + kIpcrRingBytes) r.ulWp = r.ulRingPa;
    uint8_t *cc = gWifiDma.ctx.va + J716F_MHI_CH_IPCR_UL * 44u;
    memcpy(cc + 36u, &r.ulWp, 8u);
    cleanInv(cc + 36u, 8u);
    putBar64(bar, r.chdb + 8u * J716F_MHI_CH_IPCR_UL, r.ulWp);
    ++gState.wifiQmiQrtrTx;
    return true;
}

bool mhiSendQrtrQmi(const Region &bar, WifiQmiRings &r, uint32_t dstNode, uint32_t dstPort,
                    const uint8_t *qmi, size_t qmiLen) {
    static uint8_t frame[8192];
    const size_t n = j716f_qrtr_encode_v1(frame, sizeof(frame), J716F_QRTR_TYPE_DATA,
                                          J716F_QRTR_LOCAL_NODE, kQmiClientPort,
                                          dstNode, dstPort, 0u, qmi, qmiLen);
    return n > 0u && mhiSendUlPacket(bar, r, frame, n);
}

uint64_t mhiAllocQ6Segment(WifiQmiRings &r, uint32_t size) {
    const uint64_t align = (size >= 0x100000u) ? 0x100000ULL : 0x10000ULL;
    const uint64_t pa = (r.q6HeapCursor + align - 1ULL) & ~(align - 1ULL);
    if (pa + size > r.q6HeapEnd) return 0ULL;
    uint8_t *va = gWifiDma.arenaVa + (pa - gWifiDma.arenaPa);
    memset(va, 0, size);
    cleanInv(va, size);
    r.q6HeapCursor = pa + size;
    return pa;
}

}  // namespace

extern "C" int j716f_wcn_wifi_qmi_now(void) {
    ensureMapped();
    gState.wifiQmiAttempted = true;
    gState.wifiQmiStage = 1;
    gState.wifiQmiErr = 0;
    gState.wifiQmiCmd20 = 0xFFu;
    gState.wifiQmiCmd21 = 0xFFu;
    gState.wifiQmiQrtrRx = 0u;
    gState.wifiQmiQrtrTx = 0u;
    gState.wifiQmiWlfwNode = 0u;
    gState.wifiQmiWlfwPort = 0u;
    gState.wifiQmiWlfwInst = 0u;
    gState.wifiQmiMemCount = 0u;
    gState.wifiQmiMemTotal = 0u;
    memset(gState.wifiQmiMemSegs, 0, sizeof(gState.wifiQmiMemSegs));
    memset(&gState.wifiQmiCap, 0, sizeof(gState.wifiQmiCap));
    gState.wifiQmiBdfChunks = 0u;
    gState.wifiQmiBdfErr = 0u;
    gState.wifiQmiM3Err = 0u;
    gState.wifiQmiMemReady = false;
    gState.wifiQmiFwReady = false;
    gState.wifiQmiFwInitDone = false;
    gState.wifiQmiMs = 0u;

    /* Stage 1: Ensure amss20.bin reached Mission Mode (EE=2) with our 64 MiB DMA arena. */
    if (gState.wifiFwStage < 9 || !gWifiDma.allocated) {
        const int fwrc = j716f_wcn_wifi_fw_now();
        if (fwrc != 0 || gState.wifiFwStage < 9) {
            gState.wifiQmiErr = fwrc ? fwrc : -5;
            return gState.wifiQmiErr;
        }
    }
    if (gWifiDma.arenaBytes < 0x2000000ULL) {
        gState.wifiQmiErr = -12;
        return -12;
    }

    Region bar;
    const uint32_t barAddr = gState.epBar0Assigned & ~0xFu;
    if (!barAddr || !mapRegion(barAddr, 0x200000u, bar)) {
        gState.wifiQmiErr = -13;
        return -13;
    }

    const uint64_t t0 = mach_absolute_time();

    /* Stage 2: Start MHI IPCR Ch20 (UL) & Ch21 (DL) via Command Ring + pre-queue 32 DL buffers. */
    gState.wifiQmiStage = 2;
    WifiQmiRings r = {};
    mhiInitRingsFromCtx(bar, r);
    gState.wifiQmiCmd20 = mhiStartChanSync(bar, r, J716F_MHI_CH_IPCR_UL);
    gState.wifiQmiCmd21 = mhiStartChanSync(bar, r, J716F_MHI_CH_IPCR_DL);
    if (gState.wifiQmiCmd20 != J716F_MHI_EV_CC_SUCCESS ||
        gState.wifiQmiCmd21 != J716F_MHI_EV_CC_SUCCESS) {
        gState.wifiQmiErr = -70;
        unmapRegion(bar);
        return -70;
    }
    for (uint32_t i = 0; i < kIpcrDlBufs; ++i) {
        mhiQueueDlBuf(bar, r, i);
    }

    /* Stage 3: Send QRTR HELLO and wait for WLFW (service 0x45) NEW_SERVER announcement. */
    gState.wifiQmiStage = 3;
    uint8_t txPkt[64];
    const size_t helloLen = j716f_qrtr_encode_hello(txPkt, sizeof(txPkt),
                                                    J716F_QRTR_LOCAL_NODE, J716F_QRTR_NODE_BCAST);
    (void)mhiSendUlPacket(bar, r, txPkt, helloLen);

    static uint8_t qmiBuf[7000];
    uint16_t txnId = 1u;
    bool sentIndReg = false, gotIndRegResp = false;
    bool sentHostCap = false, gotHostCapResp = false;
    bool gotReqMem = false, sentRespMem = false, gotRespMemResp = false;
    bool sentCap = false, gotCapResp = false;
    bool bdfDone = false, bdfInFlight = false;
    uint32_t bdfOff = 0u, bdfSegId = 0u;
    bool sentM3 = false, gotM3Resp = false;
    bool helloReplied = false;
    uint16_t activeTxn = 0u;

    auto sendNextBdf = [&]() -> bool {
        if (j716f_wcn_wifi_bdf_fw_size == 0u) {
            bdfDone = true;
            return true;
        }
        const uint32_t total = j716f_wcn_wifi_bdf_fw_size;
        const uint32_t rem = total - bdfOff;
        const uint16_t take = static_cast<uint16_t>(rem > J716F_QMI_BDF_MAX_CHUNK ? J716F_QMI_BDF_MAX_CHUNK : rem);
        const bool isEnd = (bdfOff + take >= total);
        const uint8_t bdfType = (total >= 4u && j716f_wcn_wifi_bdf_fw[0] == 0x7Fu &&
                                 j716f_wcn_wifi_bdf_fw[1] == 'E')
                                    ? J716F_QMI_BDF_TYPE_ELF
                                    : J716F_QMI_BDF_TYPE_BIN;
        activeTxn = txnId++;
        const size_t n = j716f_qmi_build_bdf_download_req(
            qmiBuf, sizeof(qmiBuf), activeTxn, bdfSegId, total,
            j716f_wcn_wifi_bdf_fw + bdfOff, take, isEnd, bdfType);
        if (n == 0u) return false;
        bdfOff += take;
        ++bdfSegId;
        gState.wifiQmiBdfChunks = bdfSegId;
        bdfInFlight = true;
        return mhiSendQrtrQmi(bar, r, gState.wifiQmiWlfwNode, gState.wifiQmiWlfwPort, qmiBuf, n);
    };

    /* Drive QRTR + QMI WLFW state machine up to 10 seconds. */
    for (unsigned tick = 0; tick < 2000u; ++tick) {
        J716fMhiRingElement ev = {};
        /* Drain any state/EE events on ER0 so ER0 never fills. */
        while (mhiPopEvent(bar, r, 0u, &ev)) {}

        while (mhiPopEvent(bar, r, 1u, &ev)) {
            if (j716f_mhi_ev_type(&ev) != J716F_MHI_PKT_TX_EVENT) continue;
            if (j716f_mhi_ev_chid(&ev) != J716F_MHI_CH_IPCR_DL) continue;

            const uint64_t treOff = ev.ptr - r.dlRingPa;
            const uint32_t xferLen = j716f_mhi_ev_len(&ev);
            if (ev.ptr >= r.dlRingPa && treOff < kIpcrRingBytes && (treOff & 15ULL) == 0ULL) {
                J716fMhiRingElement dlTre = {};
                memcpy(&dlTre, r.dlRingVa + treOff, sizeof(dlTre));
                if (dlTre.ptr >= r.dlBufBasePa &&
                    dlTre.ptr < r.dlBufBasePa + static_cast<uint64_t>(kIpcrDlBufs) * kIpcrBufBytes) {
                    const uint32_t bufIdx = static_cast<uint32_t>((dlTre.ptr - r.dlBufBasePa) / kIpcrBufBytes);
                    const uint8_t *rxVa = r.dlBufBaseVa + static_cast<uint64_t>(bufIdx) * kIpcrBufBytes;
                    cleanInv(rxVa, xferLen);
                    ++gState.wifiQmiQrtrRx;

                    J716fQrtrHeader qh = {};
                    const uint8_t *pl = nullptr;
                    size_t plLen = 0u;
                    if (j716f_qrtr_decode(rxVa, xferLen, &qh, &pl, &plLen)) {
                        if (qh.confirm_rx && qh.type == J716F_QRTR_TYPE_DATA) {
                            uint8_t rtx[64];
                            const size_t rn = j716f_qrtr_encode_resume_tx(
                                rtx, sizeof(rtx), qh.dst_node, qh.dst_port, qh.src_node, qh.src_port);
                            if (rn > 0u) (void)mhiSendUlPacket(bar, r, rtx, rn);
                        }
                        if (qh.type == J716F_QRTR_TYPE_HELLO && !helloReplied && qh.src_node != J716F_QRTR_LOCAL_NODE) {
                            helloReplied = true;
                            const size_t hn = j716f_qrtr_encode_hello(
                                txPkt, sizeof(txPkt), J716F_QRTR_LOCAL_NODE, qh.src_node);
                            (void)mhiSendUlPacket(bar, r, txPkt, hn);
                        } else if (qh.type == J716F_QRTR_TYPE_NEW_SERVER) {
                            J716fQrtrServer srv = {};
                            if (j716f_qrtr_parse_new_server(pl, plLen, &srv) &&
                                srv.service == J716F_QRTR_WLFW_SERVICE_ID && gState.wifiQmiWlfwPort == 0u) {
                                gState.wifiQmiWlfwNode = srv.node;
                                gState.wifiQmiWlfwPort = srv.port;
                                gState.wifiQmiWlfwInst = srv.instance;
                                gState.wifiQmiStage = 4;
                            }
                        } else if (qh.type == J716F_QRTR_TYPE_DATA && qh.dst_port == kQmiClientPort) {
                            J716fQmiHeader mh = {};
                            const uint8_t *tlvs = nullptr;
                            size_t tlvsLen = 0u;
                            if (j716f_qmi_decode_header(pl, plLen, &mh, &tlvs, &tlvsLen)) {
                                uint16_t qres = 0u, qerr = 0u;
                                (void)j716f_qmi_parse_resp_result(tlvs, tlvsLen, &qres, &qerr);
                                if (mh.type == J716F_QMI_TYPE_RESP) {
                                    if (mh.msg_id == J716F_QMI_WLFW_IND_REGISTER_REQ) {
                                        gotIndRegResp = (qres == 0u);
                                        if (!gotIndRegResp) gState.wifiQmiErr = -71;
                                    } else if (mh.msg_id == J716F_QMI_WLFW_HOST_CAP_REQ) {
                                        gotHostCapResp = (qres == 0u);
                                        if (!gotHostCapResp) gState.wifiQmiErr = -72;
                                    } else if (mh.msg_id == J716F_QMI_WLFW_RESPOND_MEM_REQ) {
                                        gotRespMemResp = (qres == 0u);
                                        if (!gotRespMemResp) gState.wifiQmiErr = -73;
                                    } else if (mh.msg_id == J716F_QMI_WLFW_CAP_REQ) {
                                        gotCapResp = j716f_qmi_parse_cap_resp(tlvs, tlvsLen, &gState.wifiQmiCap);
                                        if (!gotCapResp) gState.wifiQmiErr = -74;
                                    } else if (mh.msg_id == J716F_QMI_WLFW_BDF_DOWNLOAD_REQ) {
                                        bdfInFlight = false;
                                        if (qres != 0u) {
                                            gState.wifiQmiBdfErr = (static_cast<uint32_t>(qres) << 16) | qerr;
                                            bdfDone = true;
                                        } else if (bdfOff >= j716f_wcn_wifi_bdf_fw_size) {
                                            bdfDone = true;
                                        }
                                    } else if (mh.msg_id == J716F_QMI_WLFW_M3_INFO_REQ) {
                                        gotM3Resp = true;
                                        if (qres != 0u) {
                                            gState.wifiQmiM3Err = (static_cast<uint32_t>(qres) << 16) | qerr;
                                        }
                                    }
                                } else if (mh.type == J716F_QMI_TYPE_IND) {
                                    if (mh.msg_id == J716F_QMI_WLFW_REQUEST_MEM_IND) {
                                        if (j716f_qmi_parse_request_mem_ind(
                                                tlvs, tlvsLen, gState.wifiQmiMemSegs,
                                                J716F_QMI_MAX_MEM_SEGS, &gState.wifiQmiMemCount)) {
                                            gotReqMem = true;
                                            gState.wifiQmiStage = 5;
                                        }
                                    } else if (mh.msg_id == J716F_QMI_WLFW_FW_MEM_READY_IND) {
                                        gState.wifiQmiMemReady = true;
                                        gState.wifiQmiStage = 6;
                                    } else if (mh.msg_id == J716F_QMI_WLFW_FW_READY_IND) {
                                        gState.wifiQmiFwReady = true;
                                        gState.wifiQmiStage = 9;
                                    } else if (mh.msg_id == J716F_QMI_WLFW_FW_INIT_DONE_IND) {
                                        gState.wifiQmiFwInitDone = true;
                                        gState.wifiQmiStage = 9;
                                    }
                                }
                            }
                        }
                    }
                    mhiQueueDlBuf(bar, r, bufIdx);
                }
            }
        }

        if (gState.wifiQmiErr != 0) break;

        /* Advance QMI WLFW handshake steps once WLFW server is known. */
        if (gState.wifiQmiWlfwPort != 0u) {
            if (!sentIndReg) {
                sentIndReg = true;
                const size_t n = j716f_qmi_build_ind_register_req(qmiBuf, sizeof(qmiBuf), txnId++);
                (void)mhiSendQrtrQmi(bar, r, gState.wifiQmiWlfwNode, gState.wifiQmiWlfwPort, qmiBuf, n);
            } else if (gotIndRegResp && !sentHostCap) {
                sentHostCap = true;
                const size_t n = j716f_qmi_build_host_cap_req(
                    qmiBuf, sizeof(qmiBuf), txnId++, j716f_wcn_wifi_m3_fw_size > 0u);
                (void)mhiSendQrtrQmi(bar, r, gState.wifiQmiWlfwNode, gState.wifiQmiWlfwPort, qmiBuf, n);
            } else if (gotHostCapResp && gotReqMem && !sentRespMem) {
                sentRespMem = true;
                J716fQmiMemRespSeg respSegs[J716F_QMI_MAX_MEM_SEGS] = {};
                uint32_t totalBytes = 0u;
                bool allocOk = true;
                for (uint32_t i = 0u; i < gState.wifiQmiMemCount; ++i) {
                    const uint32_t sz = gState.wifiQmiMemSegs[i].size;
                    const uint64_t pa = mhiAllocQ6Segment(r, sz);
                    if (!pa) {
                        allocOk = false;
                        break;
                    }
                    respSegs[i].addr = pa;
                    respSegs[i].size = sz;
                    respSegs[i].type = gState.wifiQmiMemSegs[i].type;
                    respSegs[i].restore = 0u;
                    totalBytes += sz;
                }
                gState.wifiQmiMemTotal = totalBytes;
                if (!allocOk) {
                    gState.wifiQmiErr = -12;
                    break;
                }
                const size_t n = j716f_qmi_build_respond_mem_req(
                    qmiBuf, sizeof(qmiBuf), txnId++, respSegs, gState.wifiQmiMemCount);
                (void)mhiSendQrtrQmi(bar, r, gState.wifiQmiWlfwNode, gState.wifiQmiWlfwPort, qmiBuf, n);
            } else if (gotRespMemResp && gState.wifiQmiMemReady && !sentCap) {
                sentCap = true;
                const size_t n = j716f_qmi_build_cap_req(qmiBuf, sizeof(qmiBuf), txnId++);
                (void)mhiSendQrtrQmi(bar, r, gState.wifiQmiWlfwNode, gState.wifiQmiWlfwPort, qmiBuf, n);
            } else if (gotCapResp && !bdfDone && !bdfInFlight) {
                gState.wifiQmiStage = 7;
                if (!sendNextBdf()) {
                    gState.wifiQmiErr = -75;
                    break;
                }
            } else if (gotCapResp && bdfDone && !sentM3) {
                sentM3 = true;
                gState.wifiQmiStage = 8;
                if (j716f_wcn_wifi_m3_fw_size > 0u && j716f_wcn_wifi_m3_fw_size <= 0x100000u) {
                    const uint64_t m3Pa = gWifiDma.arenaPa + 0x700000ULL;
                    uint8_t *m3Va = gWifiDma.arenaVa + 0x700000ULL;
                    memcpy(m3Va, j716f_wcn_wifi_m3_fw, j716f_wcn_wifi_m3_fw_size);
                    cleanInv(m3Va, j716f_wcn_wifi_m3_fw_size);
                    const size_t n = j716f_qmi_build_m3_info_req(
                        qmiBuf, sizeof(qmiBuf), txnId++, m3Pa, j716f_wcn_wifi_m3_fw_size);
                    (void)mhiSendQrtrQmi(bar, r, gState.wifiQmiWlfwNode, gState.wifiQmiWlfwPort, qmiBuf, n);
                } else {
                    gotM3Resp = true;
                    break;
                }
            }
        }

        if ((gState.wifiQmiFwReady || gState.wifiQmiFwInitDone) && gotM3Resp) break;
        IOSleep(5);
    }

    uint64_t elapsedNs = 0ULL;
    absolutetime_to_nanoseconds(mach_absolute_time() - t0, &elapsedNs);
    gState.wifiQmiMs = static_cast<uint32_t>(elapsedNs / 1000000ULL);
    unmapRegion(bar);

    if (gState.wifiQmiErr == 0 && !gState.wifiQmiFwReady && !gState.wifiQmiFwInitDone && gState.wifiQmiStage < 7) {
        gState.wifiQmiErr = -110;
    }
    IOLog("[J716F wcn] wifi.qmi stg=%d err=%d wlfw=%u:%u mem=%u(%uB) board=0x%x chip=0x%x/%x soc=0x%x fw=0x%x build=\"%s\" bdf=%u(0x%x) m3=0x%x ready=%u/%u ms=%u\n",
          gState.wifiQmiStage, gState.wifiQmiErr, gState.wifiQmiWlfwNode, gState.wifiQmiWlfwPort,
          gState.wifiQmiMemCount, gState.wifiQmiMemTotal,
          gState.wifiQmiCap.board_id, gState.wifiQmiCap.chip_id, gState.wifiQmiCap.chip_family,
          gState.wifiQmiCap.soc_id, gState.wifiQmiCap.fw_version, gState.wifiQmiCap.fw_build_id,
          gState.wifiQmiBdfChunks, gState.wifiQmiBdfErr, gState.wifiQmiM3Err,
          gState.wifiQmiFwReady ? 1u : 0u, gState.wifiQmiFwInitDone ? 1u : 0u, gState.wifiQmiMs);
    return gState.wifiQmiErr;
}

extern "C" int j716f_wifi_stack_start(void) __attribute__((weak_import));

extern "C" int j716f_wcn_wifi_prepare_for_ath11k(uint32_t *bar_addr_out, uint32_t *bar_size_out) {
    ensureMapped();
    /* Always cold-reset QCA6390 back to clean PBL over PCIe0 so ath11k owns MHI/BHIe from scratch. */
    const int prc = j716f_wcn_wifi_probe_now();
    if (prc != 0 || gState.wifiStage < 12 || (gState.epId & 0xFFFFu) != 0x17CBu) {
        return prc ? prc : -19;
    }
    if (!gSmmu.va) return -12;
    const struct smmu_io sio = {nullptr, smmuIoRead, smmuIoWrite};
    const struct smmu_plan plan = smmu_plan_bypass(&sio, kPcie0EpSid, 0x80000000ULL, 0x7FFFFFFFULL);
    gState.wifiSmmuSlot = static_cast<int>(plan.slot);
    gState.wifiSmmuBank = static_cast<int>(plan.bank);
    if (smmu_configure_bypass(&sio, &plan, kPcie0EpSid) != nullptr) {
        return -5;
    }
    const uint32_t barAddr = gState.epBar0Assigned & ~0xFu;
    const uint32_t barSize = ~(gState.epBar0Mask & ~0xFu) + 1u;
    if (!barAddr || !barSize) return -19;
    if (bar_addr_out) *bar_addr_out = barAddr;
    if (bar_size_out) *bar_size_out = barSize;
    return 0;
}

extern "C" void j716f_wcn_probe(void) {
    ensureMapped();
    unsigned btSvc = 0u;
    PE_parse_boot_argn("j716f-bt", &btSvc, sizeof(btSvc));
    if (btSvc >= 1u) {
        (void)j716f_wcn_bt_service_start();
    }
    unsigned wifiStack = 0u;
    PE_parse_boot_argn("j716f-wifi", &wifiStack, sizeof(wifiStack));
    if (wifiStack >= 1u && &j716f_wifi_stack_start) {
        (void)j716f_wifi_stack_start();
    }
    unsigned mode = 0u;
    PE_parse_boot_argn("j716f-wcn", &mode, sizeof(mode));
    if (mode >= 1u && btSvc == 0u) {
        (void)j716f_wcn_bt_probe_now();
    }
    if (mode >= 2u && wifiStack == 0u) {
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

    if (gState.btFwAttempted) {
        appendf(out, cap, pos,
                "[usb] WCN_BT_FW: stg=%d err=%d baud=%u(post=%u,ibs=%u,sirq=0x%x) tlv=%u(%uB) nvm=%u(%uB,patch=%u) log=0x%02x dl=%ums hci=%u/0x%04x lmp=%u/0x%04x mfg=0x%04x bd=%02x:%02x:%02x:%02x:%02x:%02x ble=%u(rpts=%u)\r\n",
                gState.btFwStage, gState.btFwErr, gState.btBaudHz,
                gState.btPostBaud, gState.btIbsWake, gState.btPostSirq,
                gState.btTlvChunks, j716f_wcn_bt_tlv_fw_size,
                gState.btNvmChunks, j716f_wcn_bt_nvm_fw_size, gState.btNvmPatched,
                gState.btDiagLogStatus, gState.btDownloadMs,
                gState.btLocalHciVer, gState.btLocalHciRev,
                gState.btLocalLmpVer, gState.btLocalLmpSubver, gState.btLocalMfg,
                gState.btBdAddr[5], gState.btBdAddr[4], gState.btBdAddr[3],
                gState.btBdAddr[2], gState.btBdAddr[1], gState.btBdAddr[0],
                gState.btBleCount, gState.btBleReports);
        if (gState.btPostRxLen > 0u) {
            appendf(out, cap, pos, "[usb] WCN_BT_POST_RX:");
            for (uint32_t i = 0; i < gState.btPostRxLen; ++i) {
                appendf(out, cap, pos, " %02x", gState.btPostRx[i]);
            }
            appendf(out, cap, pos, "\r\n");
        }
        for (uint32_t i = 0; i < gState.btBleCount; ++i) {
            const J716fBleAdv &a = gState.btBle[i];
            appendf(out, cap, pos,
                    "[usb] WCN_BT_BLE[%u]: %02x:%02x:%02x:%02x:%02x:%02x rssi=%d name=\"%s\"\r\n",
                    i, a.addr[5], a.addr[4], a.addr[3], a.addr[2], a.addr[1], a.addr[0],
                    static_cast<int>(a.rssi), a.name);
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
        if (gState.wifiFwAttempted) {
            appendf(out, cap, pos,
                    "[usb] WCN_WIFI_FW: stg=%d err=%d amss=%uB(%useg) seg0=0x%llx smmu=%d/%d(gfsr=0x%x) msi=0x%08x sink=0x%x | ph=%u merr=%u@%u ee=0x%x st=0x%x bhi=0x%08x/%08x bhie=0x%08x mhist=0x%08x ev=%u ms=%u\r\n",
                    gState.wifiFwStage, gState.wifiFwErr, j716f_wcn_wifi_amss_fw_size,
                    gState.wifiNumSegs, static_cast<unsigned long long>(gState.wifiSeg0Pa),
                    gState.wifiSmmuSlot, gState.wifiSmmuBank, gState.wifiSmmuGfsr,
                    gState.wifiMsiCapAfter, gState.wifiSinkWord0,
                    gState.wifiMhiPhase, gState.wifiMhiErr, gState.wifiMhiFailPhase,
                    gState.wifiMhiEe, gState.wifiMhiState,
                    gState.wifiBhiStatus, gState.wifiBhiErr, gState.wifiBhieTxStatus,
                    gState.wifiMhiStatus, gState.wifiEventsAcked, gState.wifiBootMs);
        }
        if (gState.wifiQmiAttempted) {
            appendf(out, cap, pos,
                    "[usb] WCN_WIFI_QMI: stg=%d err=%d cmd=%u/%u qrtr=tx%u/rx%u wlfw=%u:%u(i%u) mem=%u(%uB,rdy=%u) bdf=%u(err=0x%x) m3=%uB(err=0x%x) fw_rdy=%u/%u ms=%u\r\n",
                    gState.wifiQmiStage, gState.wifiQmiErr, gState.wifiQmiCmd20, gState.wifiQmiCmd21,
                    gState.wifiQmiQrtrTx, gState.wifiQmiQrtrRx,
                    gState.wifiQmiWlfwNode, gState.wifiQmiWlfwPort, gState.wifiQmiWlfwInst,
                    gState.wifiQmiMemCount, gState.wifiQmiMemTotal, gState.wifiQmiMemReady ? 1u : 0u,
                    gState.wifiQmiBdfChunks, gState.wifiQmiBdfErr,
                    j716f_wcn_wifi_m3_fw_size, gState.wifiQmiM3Err,
                    gState.wifiQmiFwReady ? 1u : 0u, gState.wifiQmiFwInitDone ? 1u : 0u,
                    gState.wifiQmiMs);
            if (gState.wifiQmiMemCount > 0u) {
                appendf(out, cap, pos, "[usb] WCN_WIFI_QMI_MEM:");
                for (uint32_t i = 0; i < gState.wifiQmiMemCount; ++i) {
                    appendf(out, cap, pos, " [t=%u sz=%uK]",
                            gState.wifiQmiMemSegs[i].type, gState.wifiQmiMemSegs[i].size / 1024u);
                }
                appendf(out, cap, pos, "\r\n");
            }
            if (gState.wifiQmiCap.valid) {
                appendf(out, cap, pos,
                        "[usb] WCN_WIFI_QMI_CAP: chip=0x%x/0x%x board_id=0x%x soc=0x%x fw=0x%x macs=%u ts=\"%s\" build=\"%s\"\r\n",
                        gState.wifiQmiCap.chip_id, gState.wifiQmiCap.chip_family,
                        gState.wifiQmiCap.board_id, gState.wifiQmiCap.soc_id,
                        gState.wifiQmiCap.fw_version, gState.wifiQmiCap.num_macs,
                        gState.wifiQmiCap.fw_timestamp, gState.wifiQmiCap.fw_build_id);
            }
        }
    }

    if (gState.btSvcStarted) {
        btSvcPollProcs();
        appendf(out, cap, pos,
                "[usb] WCN_BT_SVC: st=%u err=%d bd=%02x:%02x:%02x:%02x:%02x:%02x pids(btd=%u,btl=%u) act=[%u,%u,%u,%u] kicks=%llu/%llu tx=%uB(%ufr,%ucmd) rx=%uB(%ufr) ibs=w%u/s%u\r\n",
                gState.btSvcState, gState.btSvcErr,
                gState.btSvcAddr[0], gState.btSvcAddr[1], gState.btSvcAddr[2],
                gState.btSvcAddr[3], gState.btSvcAddr[4], gState.btSvcAddr[5],
                gState.btSvcPidBtd, gState.btSvcPidBtl,
                qbt_transport_activity[0], qbt_transport_activity[1],
                qbt_transport_activity[2], qbt_transport_activity[3],
                static_cast<unsigned long long>(qbt_transport_kicks[0]),
                static_cast<unsigned long long>(qbt_transport_kicks[1]),
                gState.btSvcTxBytes, gState.btSvcTxFrames, gState.btSvcTxCmds,
                gState.btSvcRxBytes, gState.btSvcRxFrames,
                gState.btSvcIbsWakeRx, gState.btSvcIbsSleepRx);
        if (gState.btSvcLastOpsCount > 0u) {
            appendf(out, cap, pos, "[usb] WCN_BT_SVC_OPS:");
            const uint32_t n = gState.btSvcLastOpsCount < 12u ? gState.btSvcLastOpsCount : 12u;
            const uint32_t start = gState.btSvcLastOpsCount > 12u ? (gState.btSvcLastOpsCount - 12u) : 0u;
            for (uint32_t i = 0; i < n; ++i) {
                appendf(out, cap, pos, " 0x%04x", gState.btSvcLastOps[(start + i) % 12u]);
            }
            appendf(out, cap, pos, "\r\n");
        }
    }
}
