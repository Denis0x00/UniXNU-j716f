// SPDX-License-Identifier: BSD-2-Clause-Patent
// TB-J716F (SM8250-AC / PM8150B) battery & charger IOPMPowerSource provider for iPadOS 17.7.11.
// Uses PM8150B gen4 fuel gauge (PPID 0x240..0x243) and SMB5 charger (PPID 0x210..0x216)
// over the SPMI arbiter observer channels (same verified read-only path as J716fButtons.c).
//
// Boot-arg j716f-battery=<mode>:
//   0 (default) disabled
//   1           pure read-only SPMI observer (zero SPMI writes; recommended)
//   2           observer + optional FG SRAM temperature read

#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/pwr_mgt/IOPMPrivate.h>
#include <IOKit/pwr_mgt/IOPMPowerSource.h>
#include <IOKit/pwr_mgt/RootDomain.h>
#include <kern/thread.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSSymbol.h>
#include <mach/kmod.h>
#include <pexpert/pexpert.h>
#include <stddef.h>
#include <stdint.h>

#include "J716fBatteryBoard.h"

extern "C" {
extern kmod_info_t g_kernel_kmod_info;
uint64_t ml_io_map(uint64_t phys_addr, uint64_t size);
void j716f_battery_static_init(void) __asm__("__GLOBAL__sub_I_J716fBattery.cpp");

char j716f_battery_row[128] = {};
volatile uint32_t j716f_battery_changes = 0;
volatile int j716f_battery_error = 0;
volatile uint32_t j716f_battery_published = 0;
volatile uint32_t j716f_battery_match = 0;
}

namespace {

constexpr uint64_t kSpmiCore          = 0x0C440000ULL;
constexpr uint64_t kSpmiChnls         = 0x0C600000ULL;
constexpr uint64_t kSpmiObsrvr        = 0x0E600000ULL;
constexpr uint64_t kSpmiCnfg          = 0x0C40A000ULL;
constexpr uint32_t kSpmiApidMap       = 0x900u;
constexpr uint32_t kSpmiCnfgOwner     = 0x700u;
constexpr uint32_t kSpmiMaxApids      = 512u;
constexpr uint64_t kSpmiObsApidStride = 0x80ULL;
constexpr uint64_t kSpmiChnlStride    = 0x10000ULL;
constexpr uint32_t kSpmiCmd           = 0x00u;
constexpr uint32_t kSpmiStatus        = 0x08u;
constexpr uint32_t kSpmiWdata0        = 0x10u;
constexpr uint32_t kSpmiRdata0        = 0x18u;
constexpr uint32_t kSpmiPolls         = 100000u;
constexpr uint32_t kPollMs            = 10000u;
constexpr uint32_t kRdyTries          = 100u;
constexpr uint32_t kImaTries          = 8u;

struct BatterySample {
    unsigned percent, stage, charge_enable, wd_cfg, soc_raw, temp_step;
    int voltage_mv, current_ma, temp_centi_c, temp_error;
    bool present, external, charging, full, temp_valid, temp_sram;
};

BatterySample gSample = {};
unsigned gMode = 0;
unsigned gFailures = 0;

volatile uint32_t *gCore = nullptr;
volatile uint32_t *gCnfg = nullptr;
uint32_t gApids = 0;
volatile uint32_t *gFgSoc = nullptr;
volatile uint32_t *gFgInfo = nullptr;
volatile uint32_t *gFgMemIf = nullptr;
volatile uint32_t *gChgr = nullptr;
volatile uint32_t *gBatIf = nullptr;
volatile uint32_t *gUsbIn = nullptr;
volatile uint32_t *gMisc = nullptr;
volatile uint32_t *gFgMemIfCh = nullptr;

volatile uint32_t *pageOf(uint64_t phys) {
    uint64_t page = ml_io_map(phys & ~0xFFFULL, 0x1000u);
    return page ? reinterpret_cast<volatile uint32_t *>(static_cast<uintptr_t>(page + (phys & 0xFFFu))) : nullptr;
}

int apidOf(uint32_t ppid, uint32_t *apid) {
    int found = -1;
    for (uint32_t i = 0; i < gApids; ++i) {
        const uint32_t map = gCore[(kSpmiApidMap + 4u * i) / 4u];
        if (!map || ((map >> 8) & 0xFFFu) != ppid) continue;
        if (found < 0 || (gCnfg[(kSpmiCnfgOwner + 4u * i) / 4u] & 7u) == 0u) {
            found = static_cast<int>(i);
        }
    }
    if (found < 0) return -1;
    *apid = static_cast<uint32_t>(found);
    return 0;
}

int waitDone(volatile uint32_t *engine) {
    for (uint32_t poll = 0; poll < kSpmiPolls; ++poll) {
        const uint32_t status = engine[kSpmiStatus / 4u];
        if (status & 1u) return (status & 0xEu) ? 2 : 0;
    }
    return 1;
}

int read8(volatile uint32_t *observer, uint8_t address, uint8_t *value) {
    if (!observer) return 3;
    observer[kSpmiCmd / 4u] = (1u << 27) | (static_cast<uint32_t>(address) << 4);
    const int rc = waitDone(observer);
    if (!rc) *value = static_cast<uint8_t>(observer[kSpmiRdata0 / 4u]);
    return rc;
}

int write8(volatile uint32_t *channel, uint8_t address, uint8_t value) {
    if (!channel) return 3;
    channel[kSpmiWdata0 / 4u] = value;
    channel[kSpmiCmd / 4u] = static_cast<uint32_t>(address) << 4;
    return waitDone(channel);
}

int write16(volatile uint32_t *channel, uint8_t address, uint16_t value) {
    if (!channel) return 3;
    channel[kSpmiWdata0 / 4u] = static_cast<uint32_t>(value);
    channel[kSpmiCmd / 4u] = (static_cast<uint32_t>(address) << 4) | 1u;
    return waitDone(channel);
}

int read16(volatile uint32_t *observer, uint8_t address, uint16_t *value) {
    uint8_t bytes[2] = {0, 0};
    int rc = read8(observer, address, &bytes[0]);
    if (!rc) rc = read8(observer, static_cast<uint8_t>(address + 1u), &bytes[1]);
    if (!rc) *value = battery_u16(bytes);
    return rc;
}

int iacsReady(volatile uint32_t *memif) {
    for (unsigned i = 0; i < kRdyTries; ++i) {
        uint8_t status = 0;
        if (read8(memif, BAT_FG_IMA_OPR_STS, &status)) return -1;
        if (status & BAT_FG_IACS_RDY) return 0;
        IOSleep(5);
    }
    return -2;
}

int fgSramRead16(uint16_t word, uint16_t *value, unsigned *step) {
    uint8_t cfg = 0;
    *step = 1;
    if (read8(gFgMemIf, BAT_FG_MEM_INTF_CFG, &cfg)) return -1;
    for (unsigned i = 0; i < kImaTries && (cfg & BAT_FG_MEM_GNT); ++i) {
        IOSleep(4);
        if (read8(gFgMemIf, BAT_FG_MEM_INTF_CFG, &cfg)) return -1;
    }
    if (cfg & BAT_FG_MEM_GNT) { *step = 2; return -2; }
    *step = 3;
    int rc = write8(gFgMemIfCh, BAT_FG_MEM_INTF_CFG, static_cast<uint8_t>(cfg | BAT_FG_MEM_GNT | BAT_FG_MEM_IACS_SLCT));
    uint8_t control = 0;
    if (!rc) rc = read8(gFgMemIf, BAT_FG_IMA_CTL, &control);
    if (!rc) rc = write8(gFgMemIfCh, BAT_FG_IMA_CTL, static_cast<uint8_t>(control & 0x3Fu));
    *step = 4;
    if (!rc) rc = iacsReady(gFgMemIf);
    *step = 5;
    if (!rc) rc = write16(gFgMemIfCh, BAT_FG_ADDR_LSB, word);
    *step = 6;
    if (!rc) rc = iacsReady(gFgMemIf);
    *step = 7;
    uint8_t bytes[2] = {0, 0};
    if (!rc) rc = read8(gFgMemIf, BAT_FG_SRAM_RD_DATA0, &bytes[0]);
    if (!rc) rc = read8(gFgMemIf, static_cast<uint8_t>(BAT_FG_SRAM_RD_DATA0 + 1u), &bytes[1]);
    uint8_t release = 0;
    if (!read8(gFgMemIf, BAT_FG_MEM_INTF_CFG, &release)) {
        (void)write8(gFgMemIfCh, BAT_FG_MEM_INTF_CFG,
                     static_cast<uint8_t>(release & ~(BAT_FG_MEM_GNT | BAT_FG_MEM_IACS_SLCT)));
    }
    if (!rc) *value = battery_u16(bytes);
    return rc;
}

int sampleTemp(BatterySample *s) {
    uint16_t first = 0, second = 0;
    int srma = -1;
    if (gMode == 2u && gFgMemIfCh) {
        srma = fgSramRead16(BAT_FG_TEMP_SRAM_WORD, &first, &s->temp_step);
        if (!srma) {
            unsigned step = 0;
            if (fgSramRead16(BAT_FG_TEMP_SRAM_WORD, &second, &step) || first != second) srma = -3;
        }
        if (!srma) {
            const int centi = battery_temp_millic(first) / 10;
            if (centi >= 500 && centi <= 5500) {
                s->temp_centi_c = centi;
                s->temp_valid = true;
                s->temp_sram = true;
                return 0;
            }
            srma = -7;
        }
    }
    if (read16(gFgInfo, BAT_FG_BATT_TEMP_LSB, &first)) return srma < -1 ? srma : -4;
    if (!first) return srma < -1 ? srma : -6;
    const int centi = battery_temp_millic(first) / 10;
    if (centi <= -4000 || centi >= 12500) return srma < -1 ? srma : -5;
    s->temp_centi_c = centi;
    s->temp_valid = true;
    s->temp_sram = false;
    return 0;
}

int sampleNow(BatterySample *s) {
    uint8_t soc[2] = {0, 0}, stage = 0, enable = 0, usbin = 0, batif = 0, wd = 0;
    uint16_t vbat = 0, vbat_cp = 0, ibat = 0, ibat_cp = 0;
    if (read8(gFgSoc, BAT_FG_MSOC, &soc[0]) || read8(gFgSoc, BAT_FG_MSOC_CP, &soc[1]) || soc[0] != soc[1]) return -1;
    if (read16(gFgInfo, BAT_FG_VBATT_LSB, &vbat) || read16(gFgInfo, BAT_FG_VBATT_LSB_CP, &vbat_cp) || vbat != vbat_cp) return -2;
    if (read16(gFgInfo, BAT_FG_IBATT_LSB, &ibat) || read16(gFgInfo, BAT_FG_IBATT_LSB_CP, &ibat_cp) || ibat != ibat_cp) return -3;
    if (read8(gChgr, BAT_SMB_CHG_STAGE, &stage) || read8(gChgr, BAT_SMB_CHG_ENABLE, &enable)) return -4;
    if (read8(gUsbIn, BAT_SMB_USBIN_RT_STS, &usbin) || read8(gBatIf, BAT_SMB_BATIF_RT_STS, &batif)) return -5;
    if (read8(gMisc, BAT_SMB_WDOG_CFG, &wd)) return -6;

    s->soc_raw = soc[0];
    s->voltage_mv = battery_voltage_uv(vbat) / 1000;
    s->percent = battery_percent_with_voltage_guard(soc[0], s->voltage_mv);
    s->current_ma = -battery_current_ua(ibat) / 1000;
    s->stage = stage & 7u;
    s->charge_enable = enable & 1u;
    s->wd_cfg = wd;
    s->external = (usbin & BAT_SMB_USBIN_PLUGIN) != 0u;
    s->present = (batif & BAT_SMB_BATT_MISSING) == 0u;
    s->charging = battery_charging(s->stage) && s->external;
    s->full = s->stage == BAT_SMB_STAGE_TERMINATE;
    s->temp_valid = false;
    s->temp_sram = false;
    s->temp_step = 0;
    s->temp_error = sampleTemp(s);
    return 0;
}

void formatTemp(int centi, char *text, unsigned capacity) {
    const int absC = centi < 0 ? -centi : centi;
    snprintf(text, capacity, "%s%d.%02dC", centi < 0 ? "-" : "", absC / 100, absC % 100);
}

void publishRow(const BatterySample &s) {
    char temperature[16] = "";
    char source[20] = "";
    if (s.temp_valid) {
        formatTemp(s.temp_centi_c, temperature, sizeof(temperature));
        snprintf(source, sizeof(source), "%s", s.temp_sram ? "sram" : "reg");
    } else {
        snprintf(source, sizeof(source), "ro");
    }
    snprintf(j716f_battery_row, sizeof(j716f_battery_row),
             "%u%%(r%u) %dmV %dmA %s stg%u en%u ext%u bat%u wd53=%02x t:%s%s",
             s.percent, s.soc_raw, s.voltage_mv, s.current_ma,
             s.temp_valid ? temperature : "n/a", s.stage, s.charge_enable,
             s.external ? 1u : 0u, s.present ? 1u : 0u, s.wd_cfg, source,
             s.charging ? " chg" : "");
}

} // namespace

class J716fBattery : public IOPMPowerSource {
    OSDeclareDefaultStructors(J716fBattery)
    const OSSymbol *fFullyCharged = nullptr;
    const OSSymbol *fDesignCapacity = nullptr;
    const OSSymbol *fTemperature = nullptr;
    const OSSymbol *fTransport = nullptr;
    const OSSymbol *fType = nullptr;
    bool fPublished = false;
    bool fHasExternal = false;
    bool fExternal = false;

    void setNumber(const OSSymbol *key, long long value) {
        OSNumber *number = OSNumber::withNumber(static_cast<unsigned long long>(value), 32);
        if (!number) return;
        setPSProperty(key, number);
        number->release();
    }

public:
    bool start(IOService *provider) override {
        if (!IOPMPowerSource::start(provider)) return false;
        fFullyCharged = OSSymbol::withCString("FullyCharged");
        fDesignCapacity = OSSymbol::withCString("DesignCapacity");
        fTemperature = OSSymbol::withCString("Temperature");
        fTransport = OSSymbol::withCString("Internal");
        fType = OSSymbol::withCString("InternalBattery");
        return fFullyCharged && fDesignCapacity && fTemperature && fTransport && fType;
    }

    void stop(IOService *provider) override {
        OSSafeReleaseNULL(fFullyCharged);
        OSSafeReleaseNULL(fDesignCapacity);
        OSSafeReleaseNULL(fTemperature);
        OSSafeReleaseNULL(fTransport);
        OSSafeReleaseNULL(fType);
        IOPMPowerSource::stop(provider);
    }

    void update(const BatterySample &s) {
        setBatteryInstalled(s.present);
        setExternalConnected(s.external);
        setExternalChargeCapable(s.external);
        setIsCharging(s.charging);
        setCurrentCapacity(s.present ? s.percent : 0u);
        setMaxCapacity(100u);
        setVoltage(s.voltage_mv > 0 ? static_cast<unsigned>(s.voltage_mv) : 0u);
        setAmperage(s.current_ma);
        setAtWarnLevel(s.percent <= 20u);
        setAtCriticalLevel(s.percent <= 5u);
        unsigned minutes = BAT_TIME_UNKNOWN;
        const int mag = s.current_ma < 0 ? -s.current_ma : s.current_ma;
        if (s.present && mag > 20) {
            const unsigned long long ma = static_cast<unsigned long long>(mag);
            const unsigned long long cap =
                (s.charging ? (100u - s.percent) : s.percent) * static_cast<unsigned long long>(BAT_DESIGN_MAH) / 100ULL;
            const unsigned long long est = cap * 60ULL / ma;
            if (est < 0x7FFFULL) minutes = static_cast<unsigned>(est);
        }
        setTimeRemaining(static_cast<int>(minutes));
        setNumber(fFullyCharged, s.full ? 1 : 0);
        setNumber(fDesignCapacity, BAT_DESIGN_MAH);
        if (s.temp_valid) setNumber(fTemperature, s.temp_centi_c > 0 ? s.temp_centi_c : 0);
        setPSProperty(fTransport, const_cast<OSSymbol *>(fTransport));
        setPSProperty(fType, const_cast<OSSymbol *>(fType));
        updateStatus();
        if (!fPublished) {
            fPublished = true;
            j716f_battery_published = 1u;
            char temperature[16] = "";
            if (s.temp_valid) formatTemp(s.temp_centi_c, temperature, sizeof(temperature));
            IOLog("[J716F bat] published %u%% %dmV %dmA %s stg=%u%s%s\n",
                  s.percent, s.voltage_mv, s.current_ma,
                  s.temp_valid ? temperature : "n/a", s.stage,
                  s.charging ? " chg" : "", s.external ? " ext" : "");
        }
        if (!fHasExternal || fExternal != s.external) {
            IOPMrootDomain *domain = IOService::getPMRootDomain();
            if (domain) {
                domain->receivePowerNotification(kIOPMSetACAdaptorConnected | (s.external ? kIOPMSetValue : 0u));
            }
            fHasExternal = true;
            fExternal = s.external;
        }
    }
};

OSDefineMetaClassAndStructors(J716fBattery, IOPMPowerSource)

namespace {

J716fBattery *gBattery = nullptr;

int loadClass() {
    static int state = 0;
    if (state) return state > 0 ? 0 : state;
    void *handle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
    if (!handle) {
        state = -1;
        return state;
    }
    j716f_battery_static_init();
    state = OSMetaClass::postModLoad(handle) ? -2 : 1;
    return state > 0 ? 0 : state;
}

unsigned verifyMatch(J716fBattery *battery) {
    OSDictionary *matching = IOService::serviceMatching("IOPMPowerSource");
    IOService *found = matching ? IOService::copyMatchingService(matching) : nullptr;
    const unsigned result = found == battery ? 1u : (found ? 2u : 0u);
    if (found) found->release();
    OSSafeReleaseNULL(matching);
    return result;
}

bool attachBattery(const BatterySample &sample) {
    if (loadClass()) return false;
    J716fBattery *battery = new J716fBattery;
    if (!battery) return false;
    if (!battery->init()) {
        battery->release();
        return false;
    }
    IOService *domain = IOService::getPMRootDomain();
    if (!domain || !battery->attach(domain) || !battery->start(domain)) {
        battery->release();
        return false;
    }
    battery->setName("J716fBattery");
    battery->update(sample);
    battery->registerService(kIOServiceSynchronous);
    battery->retain();
    gBattery = battery;
    j716f_battery_match = verifyMatch(battery);
    IOLog("[J716F bat] IOPMPowerSource match=%u\n", j716f_battery_match);
    return true;
}

void batteryThread(void *, wait_result_t) {
    bool publishFailLogged = false;
    bool first = true;
    for (;;) {
        BatterySample sample = {};
        if (sampleNow(&sample)) {
            j716f_battery_error = -1;
            if (!gFailures++) {
                snprintf(j716f_battery_row, sizeof(j716f_battery_row), "no sample (PM8150B FG did not answer)");
                j716f_battery_changes = j716f_battery_changes + 1u;
            }
        } else {
            const bool changed = first || sample.percent != gSample.percent ||
                                 sample.charging != gSample.charging ||
                                 sample.external != gSample.external ||
                                 sample.stage != gSample.stage ||
                                 sample.voltage_mv != gSample.voltage_mv ||
                                 sample.current_ma != gSample.current_ma;
            first = false;
            gSample = sample;
            gFailures = 0;
            j716f_battery_error = 0;
            if (!gBattery && !attachBattery(sample)) {
                j716f_battery_error = -2;
                if (!publishFailLogged) {
                    publishFailLogged = true;
                    IOLog("[J716F bat] publish failed, retrying every %us\n", kPollMs / 1000u);
                }
            } else if (gBattery) {
                gBattery->update(sample);
                if (!j716f_battery_match) {
                    const unsigned found = verifyMatch(gBattery);
                    if (found) j716f_battery_match = found;
                }
                publishRow(sample);
                if (changed) j716f_battery_changes = j716f_battery_changes + 1u;
            }
        }
        IOSleep(kPollMs);
    }
}

int mapHardware() {
    gCore = reinterpret_cast<volatile uint32_t *>(static_cast<uintptr_t>(ml_io_map(kSpmiCore, 0x2000u)));
    if (!gCore) return -1;
    gApids = gCore[1] & 0x7FFu;
    if (gCore[0] < 0x50000000u || gCore[0] >= 0x70000000u || !gApids || gApids > kSpmiMaxApids) return -2;
    gCnfg = reinterpret_cast<volatile uint32_t *>(static_cast<uintptr_t>(ml_io_map(kSpmiCnfg, 0x1000u)));
    if (!gCnfg) return -3;

    static const struct { uint32_t ppid; volatile uint32_t **observer; } kMap[] = {
        {BAT_PPID_FG_SOC, &gFgSoc},
        {BAT_PPID_FG_INFO, &gFgInfo},
        {BAT_PPID_FG_MEMIF, &gFgMemIf},
        {BAT_PPID_SMB_CHGR, &gChgr},
        {BAT_PPID_SMB_BATIF, &gBatIf},
        {BAT_PPID_SMB_USBIN, &gUsbIn},
        {BAT_PPID_SMB_MISC, &gMisc},
    };
    for (unsigned i = 0; i < sizeof(kMap) / sizeof(kMap[0]); ++i) {
        uint32_t apid = 0;
        if (apidOf(kMap[i].ppid, &apid)) {
            snprintf(j716f_battery_row, sizeof(j716f_battery_row),
                     "SPMI missing PPID 0x%x (apids=%u)", kMap[i].ppid, gApids);
            j716f_battery_changes = j716f_battery_changes + 1u;
            return -4;
        }
        *kMap[i].observer = pageOf(kSpmiObsrvr + kSpmiObsApidStride * apid);
        if (!*kMap[i].observer) return -5;
    }
    if (gMode == 2u) {
        uint32_t apid = 0;
        if (apidOf(BAT_PPID_FG_MEMIF, &apid)) return -6;
        gFgMemIfCh = pageOf(kSpmiChnls + kSpmiChnlStride * apid);
        if (!gFgMemIfCh) return -7;
    }
    return 0;
}

} // namespace

extern "C" void j716f_battery_snapshot(char *out, size_t cap) {
    if (!out || cap == 0) return;
    snprintf(out, cap, "[usb] BATTERY: mode=%u pub=%u match=%u err=%d %s\r\n",
             gMode, j716f_battery_published, j716f_battery_match, j716f_battery_error,
             j716f_battery_row[0] ? j716f_battery_row : "(pending)");
}

extern "C" void j716f_battery_probe(void) {
    static int started = 0;
    unsigned mode = 0;
    if (!PE_parse_boot_argn("j716f-battery", &mode, sizeof(mode)) || !mode || mode > 2u) {
        return;
    }
    if (__atomic_exchange_n(&started, 1, __ATOMIC_ACQ_REL)) return;
    gMode = mode;
    IOLog("[J716F bat] probe mode=%u\n", gMode);
    const int rc = mapHardware();
    if (rc) {
        j716f_battery_error = rc;
        IOLog("[J716F bat] SPMI map failed rc=%d\n", rc);
        return;
    }
    thread_t thread = nullptr;
    if (kernel_thread_start(batteryThread, nullptr, &thread) == KERN_SUCCESS && thread) {
        thread_deallocate(thread);
    } else {
        j716f_battery_error = -12;
    }
}
