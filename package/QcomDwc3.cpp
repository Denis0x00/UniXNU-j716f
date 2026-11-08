// SPDX-License-Identifier: BSD-2-Clause-Patent
// SM8250 (TB-J716F) USB device controller behind Apple's native iPadOS 17.7.11 (21H461) USB stack.
// Subclasses AppleUSBXDCI and publishes an IOUSBDeviceController instance matched by AppleUSBDeviceMux,
// configd (USBDeviceConfiguration), and lockdownd (enumerating as Apple iPad 05ac:12ab).

#include "OriginalUsbXdciABI.h"
#include "UsbDebugCdc.h"
#include "UsbDebugWire.h"

#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <libkern/c++/OSSharedPtr.h>
#include <kern/task.h>
#include <kern/thread.h>
#include <kern/clock.h>
#include <pexpert/pexpert.h>
#include <string.h>

extern "C" {
#include "QcomRpmh.h"
#include "SmmuBypass.h"

int armcpu_spi_attach(uint32_t, bool, void (*)(void *), void *, uint32_t *, uint64_t *);

// [0] stage (negative: step that failed), [1] GSNPSID, [2] GHWPARAMS0, [3] GHWPARAMS3, [4] GHWPARAMS7,
// [5] GDSC USB30, [6] PHY mode, [7] SMMU slot|bank<<16, [8] SMMU SMR, [9] RPMh rails, [10] VBUS override state,
// [11] interrupts, [12] powerOn calls, [13] powerOff calls, [14] controllerReset result, [15] DSTS at power on,
// [16] DCFG after reset, [17] GCTL after reset, [18] GUSB2PHYCFG after reset, [19] start result,
// [20] goOnBus result, [21] kext load result, [22] GSBUSCFG0, [23] GSBUSCFG1, [24] GUCTL1, [25] SMMU GFSR,
// [26] DSTS, [27] DALEPENA, [28] GEVNTCOUNT, [29] DEPCMD(EP0_OUT), [30] SMMU CB FSR, [31] GEVNTSIZ.
volatile uint32_t native_usb_stats[54];
}

namespace {

#include "UsbDebugHw.h"

constexpr uint64_t kCore = J716F_USB0_CORE_BASE;         // 0x0a600000
constexpr uint64_t kAppleWindow = kCore + 0xc000;        // 0x0a60c000
constexpr uint64_t kQscratch = kCore + 0xf8800;          // 0x0a6f8800
constexpr uint64_t kSmmuBase = 0x15000000ULL;            // Apps SMMUv2
constexpr uint32_t kIrq = 133 + 32;                      // GIC SPI 165

constexpr uint32_t kCbSctlr = 0x0;
constexpr uint32_t kCbTcr2 = 0x10;
constexpr uint32_t kCbTtbr0 = 0x20;
constexpr uint32_t kCbTtbr1 = 0x28;
constexpr uint32_t kCbTcr = 0x30;
constexpr uint32_t kCbMair0 = 0x38;
constexpr uint32_t kCbMair1 = 0x3c;
constexpr uint32_t kCbFsr = 0x58;
constexpr uint32_t kCbTlbiAsid = 0x610;
constexpr uint32_t kCbTlbSync = 0x7f0;
constexpr uint32_t kCbTlbStatus = 0x7f4;

struct Region {
    IOMemoryMap *map = nullptr;
    volatile uint32_t *va = nullptr;
};

static UsbHw gHw;
static Region gScratch;
static Region gWindow;
static Region gGicd;
static Region gSmmu;
static IOBufferMemoryDescriptor *gSmmuTable = nullptr;
static unsigned gSmmuBank = 0;
static unsigned gSmmuPages = 0;
static bool gSuperSpeed = false;

static void stage(int value) {
    native_usb_stats[0] = static_cast<uint32_t>(value);
}

static void barrier() {
    __asm__ volatile("dsb sy" ::: "memory");
}

static bool mapRegion(Region &r, uint64_t pa, uint64_t bytes) {
    if (r.va) return true;
    IODeviceMemory *d = IODeviceMemory::withRange(pa, bytes);
    r.map = d ? d->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
    OSSafeReleaseNULL(d);
    r.va = r.map ? reinterpret_cast<volatile uint32_t *>(r.map->getVirtualAddress()) : nullptr;
    return r.va != nullptr;
}

static void selectUtmiClock() {
    if (!gScratch.va) return;
    volatile uint32_t *cfg = gScratch.va + (0x08 / 4);
    *cfg = *cfg | (1u << 8);
    barrier();
    IODelay(100);
    *cfg = *cfg | (1u << 0) | (1u << 3);
    barrier();
    IODelay(100);
    *cfg = *cfg & ~(1u << 8);
    barrier();
}

static void vbusOverride(bool on) {
    if (!gScratch.va) return;
    volatile uint32_t *ss = gScratch.va + (0x30 / 4);
    volatile uint32_t *hs = gScratch.va + (0x10 / 4);
    const uint32_t ssBits = (1u << 24);
    const uint32_t hsBits = (1u << 20) | (1u << 28);
    if (on) {
        *ss = *ss | ssBits;
        *hs = *hs | hsBits;
    } else {
        *ss = *ss & ~ssBits;
        *hs = *hs & ~hsBits;
    }
    barrier();
    native_usb_stats[10] = *hs;
}

// Hardware DMA coherence via Apps SMMUv2 Stage-1 identity map:
// AppleUSBXDCI performs no software cache maintenance because Apple's fabric is coherent.
// On SM8250, configuring USB0's SMMU context bank with a Stage-1 1 GiB block identity map
// (MAIR0 = 0xff Normal Write-Back, Inner Shareable) makes DWC3 DMA hardware cache-coherent
// without any software pointer chasing or cache flushes.
static bool smmuIdentity() {
    if (!mapRegion(gSmmu, kSmmuBase, 0xd0000)) return false;
    const uint32_t idr0 = gSmmu.va[0x20 / 4];
    const uint32_t idr1 = gSmmu.va[0x24 / 4];
    unsigned groups = idr0 & 0xffu;
    if (groups > 128u) groups = 128u;
    const unsigned banks = idr1 & 0xffu;
    const unsigned pages = 1u << (((idr1 >> 28) & 7u) + 1u);
    if ((idr1 >> 31) || !groups || banks < 2u || banks > 128u || pages > 128u ||
        (pages + banks) * 4096u > 0xd0000u) {
        return false;
    }

    static const uint16_t kProtectedSids[] = {
        0x0060u, 0x00E0u, 0x04E0u, // UFS0
        0x00C0u, 0x04A0u,          // SDC1
        0x0043u, 0x0056u, 0x0076u, 0x05A3u, 0x05B6u, // QGP0 / Touch QUP
        0x0800u, 0x0820u, 0x0821u, 0x0822u, 0x0C20u, 0x0C21u, 0x0C22u // MDP / Display
    };

    uint64_t used[2] = {};
    unsigned usbSlot = 0xffffffffu;
    for (unsigned i = 0; i < groups; ++i) {
        const uint32_t smr = gSmmu.va[(0x800 + 4 * i) / 4];
        if (!(smr & 0x80000000u)) continue;
        const uint32_t s2cr = gSmmu.va[(0xc00 + 4 * i) / 4];
        const uint32_t mask = (smr >> 16) & 0x7fffu;

        bool hitsProtected = false;
        for (unsigned p = 0; p < sizeof(kProtectedSids) / sizeof(kProtectedSids[0]); ++p) {
            if (((smr ^ kProtectedSids[p]) & 0x7fffu & ~mask) == 0u) {
                hitsProtected = true;
                break;
            }
        }
        if (!hitsProtected && ((smr ^ J716F_USB0_SMMU_SID_0) & 0x7fffu & ~mask) == 0u) {
            if (usbSlot == 0xffffffffu) {
                usbSlot = i;
                continue;
            }
        }
        if (((s2cr >> 16) & 3u) == 0u) {
            const unsigned b = s2cr & 0x7fu;
            used[b / 64u] |= 1ull << (b % 64u);
        }
    }

    if (usbSlot == 0xffffffffu) return false;

    const uint32_t origS2cr = gSmmu.va[(0xc00 + 4 * usbSlot) / 4];
    unsigned bank = 0xffffffffu;
    if (((origS2cr >> 16) & 3u) == 0u) {
        const unsigned curBank = origS2cr & 0x7fu;
        if (curBank < banks - 1u && ((used[curBank / 64u] >> (curBank % 64u)) & 1ull) == 0ull) {
            bank = curBank;
        }
    }
    if (bank == 0xffffffffu) {
        for (unsigned b = banks - 1u; b-- > 0u;) {
            if ((used[b / 64u] >> (b % 64u)) & 1ull) continue;
            if (gSmmu.va[(pages + b) * 1024u] & 1u) continue;
            bank = b;
            break;
        }
    }
    if (bank == 0xffffffffu) return false;

    if (!gSmmuTable) {
        gSmmuTable = IOBufferMemoryDescriptor::withOptions(
            static_cast<IOOptionBits>(kIODirectionInOut) | static_cast<IOOptionBits>(kIOMemoryPhysicallyContiguous),
            4096, 4096);
        if (!gSmmuTable || gSmmuTable->prepare() != kIOReturnSuccess) return false;
        uint64_t *table = static_cast<uint64_t *>(gSmmuTable->getBytesNoCopy());
        bzero(table, 4096);
        // T0SZ=28 (64 GiB, L1 walk): 1 GiB blocks for 2..64 GiB with AF (1<<10), Inner Shareable (3<<8),
        // AP unprivileged RW (1<<6), AttrIndx=0 (Normal WB), Block descriptor (1).
        for (uint64_t i = 2; i < 64; ++i) {
            table[i] = (i << 30) | (1ull << 10) | (3ull << 8) | (1ull << 6) | 1ull;
        }
        for (uintptr_t at = reinterpret_cast<uintptr_t>(table);
             at < reinterpret_cast<uintptr_t>(table) + 4096u;
             at += 64u) {
            __asm__ volatile("dc cvac, %0" :: "r"(at) : "memory");
        }
        barrier();
    }

    const uint64_t tablePa = gSmmuTable->getPhysicalSegment(0, nullptr, 0);
    if (!tablePa) return false;

    volatile uint32_t *cb = gSmmu.va + (pages + bank) * 1024u;
    cb[kCbSctlr / 4] = 0u;
    barrier();
    gSmmu.va[(0x1800 + 4 * bank) / 4] = 1u;                                        // CBA2R: AArch64
    gSmmu.va[(0x1000 + 4 * bank) / 4] = (1u << 16) | (0xfu << 12) | (3u << 8);     // CBAR: Stage-1 + Stage-2 bypass + Inner Shareable + WB
    cb[kCbTcr2 / 4] = 2u | (7u << 15);                                             // 40-bit PA, upstream sign extension
    cb[kCbTcr / 4] = 28u | (1u << 8) | (1u << 10) | (3u << 12) | (1u << 23);       // T0SZ=28, IRGN0/ORGN0 WBWA, SH0 Inner Shareable, 4 KiB, EPD1=1
    cb[kCbTtbr0 / 4] = static_cast<uint32_t>(tablePa);
    cb[kCbTtbr0 / 4 + 1] = static_cast<uint32_t>(tablePa >> 32);
    cb[kCbTtbr1 / 4] = 0u;
    cb[kCbTtbr1 / 4 + 1] = 0u;
    cb[kCbMair0 / 4] = 0xffu;                                                      // Attr0: Normal Write-Back Read/Write-Allocate
    cb[kCbMair1 / 4] = 0u;
    barrier();
    cb[kCbTlbiAsid / 4] = 0u;
    cb[kCbTlbSync / 4] = 0u;
    barrier();
    for (unsigned i = 0; i < 1000u && (cb[kCbTlbStatus / 4] & 1u); ++i) {
        IODelay(10);
    }
    cb[kCbFsr / 4] = cb[kCbFsr / 4];
    cb[kCbSctlr / 4] = 1u | (1u << 1) | (1u << 2) | (1u << 5) | (1u << 12);        // M | TRE | AFE | CFRE | ASIDPNE
    barrier();
    gSmmu.va[(0xc00 + 4 * usbSlot) / 4] = bank;
    barrier();

    gSmmuBank = bank;
    gSmmuPages = pages;
    native_usb_stats[7] = usbSlot | (bank << 16);
    native_usb_stats[8] = gSmmu.va[(0x800 + 4 * usbSlot) / 4];

    const bool ok = (cb[kCbSctlr / 4] & 1u) != 0u &&
                    (gSmmu.va[(0xc00 + 4 * usbSlot) / 4] & 0xffu) == bank;
    if (!ok) {
        IOLog("[qcom-dwc3] SMMU context readback failed sctlr=0x%x s2cr=0x%x smr=0x%x\n",
              cb[kCbSctlr / 4], gSmmu.va[(0xc00 + 4 * usbSlot) / 4], native_usb_stats[8]);
    }
    return ok;
}

static bool bringUp() {
    stage(1);
    gHw.initDefaults();
    usbVoteRpmhRailsOnce();
    native_usb_stats[9] = 1;
    stage(2);

    if (!mapRegion(gScratch, kQscratch, 0x400) ||
        !mapRegion(gWindow, kAppleWindow, 0x1000) ||
        !mapRegion(gGicd, 0x17a00000ULL, 0x1000ULL)) {
        stage(-2);
        return false;
    }
    stage(3);

    if (!smmuIdentity()) {
        stage(-5);
        return false;
    }
    stage(5);

    // Fresh Femto v2 HS PHY initialization + GCC clocks + EUD attach
    gHw.phyInitMode = 1u;
    if (!usbGccAndPhyInit(gHw)) {
        stage(-4);
        return false;
    }
    native_usb_stats[5] = gHw.grd(GCC_USB30_PRIM_GDSCR);
    native_usb_stats[6] = gHw.phyInitMode;
    stage(7);

    gSuperSpeed = false;
    selectUtmiClock();
    vbusOverride(true);
    stage(8);

    const uint32_t id = gWindow.va[0x120 / 4];
    native_usb_stats[1] = id;
    if ((id & 0xffff0000u) != 0x55330000u &&
        (id & 0xffff0000u) != 0x33310000u &&
        (id & 0xffff0000u) != 0x33320000u) {
        stage(-9);
        return false;
    }
    stage(9);
    IOLog("[qcom-dwc3] bringUp OK gsnpsid=0x%08x smr=0x%08x slot_bank=0x%08x\n",
          id, native_usb_stats[8], native_usb_stats[7]);
    return true;
}

static void maskSpi(uint32_t intid, bool masked) {
    if (!gGicd.va) return;
    const uint32_t word = (intid / 32u) * 4u;
    const uint32_t bit = 1u << (intid % 32u);
    gGicd.va[((masked ? 0x180u : 0x100u) + word) / 4u] = bit;
    barrier();
}

} // namespace

class QcomDwc3Nub;
static QcomDwc3Nub *volatile gActiveNub = nullptr;
static bool gLineAttached = false;

class QcomDwc3Nub : public IOService {
    OSDeclareDefaultStructors(QcomDwc3Nub)
    IOInterruptAction action = nullptr;
    OSObject *target = nullptr;
    void *refcon = nullptr;
    bool enabled = false;

    static void primary(void *) {
        QcomDwc3Nub *self = gActiveNub;
        native_usb_stats[11] = native_usb_stats[11] + 1;
        if (self && self->action) {
            self->action(self->target, self->refcon, self, 0);
        } else {
            maskSpi(kIrq, true);
        }
    }

    void unmask() {
        if (gActiveNub == this && gLineAttached) {
            maskSpi(kIrq, !enabled);
        }
    }

public:
    static void route(QcomDwc3Nub *nub) {
        if (gLineAttached) {
            maskSpi(kIrq, true);
        }
        gActiveNub = nub;
        __asm__ volatile("dmb ish" ::: "memory");
        if (nub) nub->unmask();
    }

    IOReturn getInterruptType(int source, int *type) override {
        if (source != 0 || !type) return kIOReturnNoInterrupt;
        *type = kIOInterruptTypeLevel;
        return kIOReturnSuccess;
    }

    IOReturn registerInterrupt(int source, OSObject *owner, IOInterruptAction handler, void *context) override {
        if (source != 0 || !handler) return kIOReturnNoInterrupt;
        target = owner;
        refcon = context;
        action = handler;
        __asm__ volatile("dmb ish" ::: "memory");
        if (!gLineAttached) {
            if (armcpu_spi_attach(kIrq, false, primary, nullptr, nullptr, nullptr) == kIOReturnSuccess) {
                gLineAttached = true;
            }
        }
        unmask();
        return kIOReturnSuccess;
    }

    IOReturn unregisterInterrupt(int source) override {
        if (source != 0) return kIOReturnNoInterrupt;
        enabled = false;
        unmask();
        action = nullptr;
        target = nullptr;
        refcon = nullptr;
        return kIOReturnSuccess;
    }

    IOReturn enableInterrupt(int source) override {
        if (source != 0) return kIOReturnNoInterrupt;
        enabled = true;
        unmask();
        return kIOReturnSuccess;
    }

    IOReturn disableInterrupt(int source) override {
        if (source != 0) return kIOReturnNoInterrupt;
        enabled = false;
        unmask();
        return kIOReturnSuccess;
    }
};
OSDefineMetaClassAndStructors(QcomDwc3Nub, IOService)

static volatile bool gDeviceStackAllowed = true;

class QcomDwc3Xdci : public AppleUSBXDCI {
    OSDeclareDefaultStructors(QcomDwc3Xdci)

    uint32_t read(uint32_t offset) {
        return (*reinterpret_cast<volatile uint32_t **>(reinterpret_cast<uint8_t *>(this) + 0x238))[offset / 4];
    }
    void write(uint32_t offset, uint32_t value) {
        (*reinterpret_cast<volatile uint32_t **>(reinterpret_cast<uint8_t *>(this) + 0x238))[offset / 4] = value;
        barrier();
    }

public:
    bool init(OSDictionary *properties = nullptr) override {
        if (!AppleUSBXDCI::init(properties)) return false;
        uint8_t *self = reinterpret_cast<uint8_t *>(this);
        *reinterpret_cast<uint32_t *>(self + 0x23d8) = gSuperSpeed ? 3u : 2u;
        *reinterpret_cast<uint32_t *>(self + 0x23dc) = gSuperSpeed ? 3u : 2u;
        return true;
    }

    bool start(IOService *provider) override {
        uint8_t *self = reinterpret_cast<uint8_t *>(this);
        IOMemoryMap *map = provider->mapDeviceMemoryWithIndex(0, kIOMapInhibitCache);
        if (!map) return false;
        *reinterpret_cast<IOMemoryMap **>(self + 0x230) = map;
        *reinterpret_cast<uint64_t *>(self + 0x238) = map->getVirtualAddress();
        *reinterpret_cast<uint32_t *>(self + 0x23d8) = gSuperSpeed ? 3u : 2u;
        *reinterpret_cast<uint32_t *>(self + 0x23dc) = gSuperSpeed ? 3u : 2u;
        return AppleUSBXDCI::start(provider);
    }

    bool XDCI_KEEP_CONFIGURATIONS() override {
        return false;
    }

    bool XDCI_TRANSPORT_ACTIVE() override {
        return gDeviceStackAllowed;
    }

    IOReturn XDCI_POWER_ON() override {
        native_usb_stats[12] = native_usb_stats[12] + 1;
        vbusOverride(true);
        native_usb_stats[15] = read(0x70c);
        IOLog("[qcom-dwc3] XDCI_POWER_ON count=%u dsts=0x%08x\n", native_usb_stats[12], native_usb_stats[15]);
        return kIOReturnSuccess;
    }

    IOReturn XDCI_POWER_OFF() override {
        native_usb_stats[13] = native_usb_stats[13] + 1;
        vbusOverride(false);
        IOLog("[qcom-dwc3] XDCI_POWER_OFF count=%u\n", native_usb_stats[13]);
        return kIOReturnSuccess;
    }

    // Matches AppleT8011USBXDCI (0xfffffff00655aa30 in 21H461) and UniXNU-sheng:
    // fields[0] = 1 event buffer, fields[1] = scale, fields[2] = nump, fields[3] = 4.
    void XDCI_PARAMETERS() override {
        uint32_t *fields = reinterpret_cast<uint32_t *>(reinterpret_cast<uint8_t *>(this) + 0x2410);
        fields[0] = 1;
        fields[1] = 0;
        fields[2] = 4;
        fields[3] = 4;
    }

    IOReturn XDCI_CONTROLLER_RESET() override {
        const IOReturn result = AppleUSBXDCI::XDCI_CONTROLLER_RESET();
        native_usb_stats[14] = static_cast<uint32_t>(result);
        if (result != kIOReturnSuccess) {
            return result;
        }

        uint32_t *fields = reinterpret_cast<uint32_t *>(reinterpret_cast<uint8_t *>(this) + 0x2410);
        const uint32_t gctl = read(0x110);
        fields[1] = gctl >> 19;
        write(0x110, (gctl & ~(3u << 12)) | (2u << 12));

        const uint32_t hw0 = read(0x140), hw6 = read(0x158), hw7 = read(0x15c);
        const uint32_t mdwidth = ((hw0 >> 8) & 0xffu) | (((hw6 >> 8) & 3u) << 8);
        const uint32_t ram2 = hw7 >> 16;
        uint32_t nump = (ram2 * mdwidth / 8 > 40) ? ((ram2 * mdwidth / 8 - 40) / 1024) : 1u;
        fields[2] = (nump > 16) ? 16 : (nump ? nump : 1);
        native_usb_stats[2] = hw0;
        native_usb_stats[3] = read(0x14c);
        native_usb_stats[4] = hw7;

        // Disable U2 PHY suspend / LPM sleep from core, matching Linux dwc3-qcom quirks & UniXNU-sheng
        write(0x200, read(0x200) & ~((1u << 6) | (1u << 8)));
        write(0x2c0, read(0x2c0) & ~(1u << 17));
        if (!gSuperSpeed) {
            write(0x700, read(0x700) & ~7u);  // High-Speed
        }
        native_usb_stats[16] = read(0x700);
        native_usb_stats[17] = read(0x110);
        native_usb_stats[18] = read(0x200);
        IOLog("[qcom-dwc3] XDCI_CONTROLLER_RESET ok dcfg=0x%08x gctl=0x%08x usb2=0x%08x\n",
              native_usb_stats[16], native_usb_stats[17], native_usb_stats[18]);
        return result;
    }

    // In 21H461 (iPadOS 17.7.11), AppleT8011USBXDCI (slot 267, 0xfffffff00655aec0) transitions the bus
    // via slot 204 (XDCI_GO_ON_BUS) and slot 205 (XDCI_GO_OFF_BUS).
    IOReturn cable(bool connected) {
        return connected ? AppleUSBXDCI::XDCI_GO_ON_BUS() : AppleUSBXDCI::XDCI_GO_OFF_BUS();
    }

    // Matches AppleT8011USBXDCI slot 264 (0xfffffff00655bdb0) & slot 265 (0xfffffff00655bdb4) in 21H461
    uint32_t XDCI_PERIODIC_USAGE() override {
        return 0;
    }

    void XDCI_PERIODIC_SET(uint32_t) override {}
};
OSDefineMetaClassAndStructors(QcomDwc3Xdci, AppleUSBXDCI)

static QcomDwc3Xdci *gController = nullptr;
static QcomDwc3Nub *gDeviceNub = nullptr;

extern "C" void qcom_dwc3_vbus(int online) {
    if (!gController) return;
    if (online) {
        gDeviceStackAllowed = true;
        QcomDwc3Nub::route(gDeviceNub);
        IOService::getResourceService()->setProperty("AppleUSBCableType", "USBHost");
        native_usb_stats[20] = static_cast<uint32_t>(gController->cable(true));
    } else {
        gDeviceStackAllowed = false;
        IOService::getResourceService()->removeProperty("AppleUSBCableType");
        const uint32_t before = native_usb_stats[13];
        native_usb_stats[20] = static_cast<uint32_t>(gController->cable(false));
        for (unsigned i = 0; i < 100 && native_usb_stats[13] == before; ++i) {
            IOSleep(10);
        }
        QcomDwc3Nub::route(nullptr);
    }
}

extern "C" bool qcom_dwc3_bring_up(void) {
    return bringUp();
}

static void pollAndDiagThread(void *, wait_result_t) {
    for (uint32_t tick = 0;; ++tick) {
        if (gDeviceStackAllowed && gWindow.va) {
            native_usb_stats[16] = gWindow.va[0x700 / 4];
            native_usb_stats[17] = gWindow.va[0x110 / 4];
            native_usb_stats[22] = gWindow.va[0x100 / 4];
            native_usb_stats[23] = gWindow.va[0x104 / 4];
            native_usb_stats[24] = gWindow.va[0x11c / 4];
            native_usb_stats[25] = gSmmu.va ? gSmmu.va[0x48 / 4] : 0u;
            native_usb_stats[26] = gWindow.va[0x70c / 4];
            native_usb_stats[27] = gWindow.va[0x720 / 4];
            native_usb_stats[28] = gWindow.va[0x40c / 4];
            native_usb_stats[29] = gWindow.va[0x80c / 4];
            native_usb_stats[30] = (gSmmu.va && gSmmuPages)
                                       ? gSmmu.va[(gSmmuPages + gSmmuBank) * 1024u + (kCbFsr / 4u)]
                                       : 0u;
            native_usb_stats[31] = gWindow.va[0x408 / 4];

            // If configd finished setting USBDeviceConfiguration after initial cable(true), re-trigger goOnBus
            if (gController && native_usb_stats[12] == 0u) {
                IOService::getResourceService()->setProperty("AppleUSBCableType", "USBHost");
                native_usb_stats[20] = static_cast<uint32_t>(gController->cable(true));
            }
            if ((tick % 5u) == 0u) {
                IOLog("[qcom-dwc3] st=%d pwrOn=%u rst=0x%x bus=0x%x dsts=0x%08x dalep=0x%08x evcnt=0x%x irqs=%u fsr=0x%x\n",
                      static_cast<int>(native_usb_stats[0]),
                      native_usb_stats[12],
                      native_usb_stats[14],
                      native_usb_stats[20],
                      native_usb_stats[26],
                      native_usb_stats[27],
                      native_usb_stats[28],
                      native_usb_stats[11],
                      native_usb_stats[30]);
            }
        }
        IOSleep(1000);
    }
}

extern "C" void qcom_dwc3_publish(void) {
    if (gController) {
        // Re-entering device mode after temporary release
        (void)bringUp();
        qcom_dwc3_vbus(1);
        return;
    }
    do {
        OSData *zero = OSData::withBytes("\0\0\0\0", 4);
        QcomDwc3Nub *nub = OSTypeAlloc(QcomDwc3Nub);
        IOService *platform = IOService::getPlatform();
        IODeviceMemory *range = IODeviceMemory::withRange(kAppleWindow, 0x1000);
        const OSObject *rangeObj = range;
        OSArray *ranges = range ? OSArray::withObjects(&rangeObj, 1) : nullptr;
        if (!nub || !nub->init() || !ranges || !zero || !platform) {
            stage(-12);
            break;
        }
        nub->setDeviceMemory(ranges);
        nub->setName("usb-drd0");
        nub->setProperty("bus-number", zero);
        nub->setProperty("configuration-string", (void *)"standardMuxOnly", 16);
        nub->setProperty("clock-gates", zero);
        nub->setProperty("power-gates", zero);
        nub->setProperty("u1-init-disable", true);
        nub->setProperty("u2-init-disable", true);
        if (!nub->attach(platform)) {
            stage(-13);
            break;
        }
        nub->registerService();
        OSSafeReleaseNULL(ranges);
        OSSafeReleaseNULL(range);
        OSSafeReleaseNULL(zero);
        stage(10);

        gDeviceNub = nub;
        QcomDwc3Nub::route(gDeviceNub);

        QcomDwc3Xdci *controller = OSTypeAlloc(QcomDwc3Xdci);
        if (!controller || !controller->init()) {
            stage(-14);
            break;
        }
        if (!controller->attach(nub)) {
            stage(-15);
            break;
        }
        const bool started = controller->start(nub);
        native_usb_stats[19] = started ? 1u : 0u;
        if (!started) {
            stage(-16);
            break;
        }
        gController = controller;
        stage(11);

        qcom_dwc3_vbus(1);
        stage(12);

        thread_t thread = nullptr;
        if (kernel_thread_start(pollAndDiagThread, nullptr, &thread) == KERN_SUCCESS && thread) {
            thread_deallocate(thread);
        }
        IOLog("[qcom-dwc3] published QcomDwc3Xdci start=%u goOnBus=0x%x\n",
              native_usb_stats[19], native_usb_stats[20]);
    } while (false);
}
