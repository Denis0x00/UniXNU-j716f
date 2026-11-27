// SPDX-License-Identifier: BSD-2-Clause-Patent
// TB-J716F (SM8250 / EDO RM692E6 dual-DSI AMOLED) CoreBrightness backlight
// provider for iPadOS 17.7.11 (21H461, arm64).
//
// Boot-arg j716f-backlight=<mode> (also accepts backlight=1 for upstream parity):
//   0 (default) disabled
//   1           enabled, HS mode DCS 0x51 via DSI0 (0x0AE94000, qcom,dcs-cmd-by-left)
//   2           enabled, LP mode DCS 0x51 via DSI0
// Optional kill-switch: backlight-control=0 skips AppleARMBacklight impersonation
// and backlight-control=true (keeps service diagnostic-only without engaging libEDR).
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOService.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOWorkLoop.h>
#include <kern/clock.h>
#include <libkern/OSKextLib.h>
#include <libkern/c++/OSBoolean.h>
#include <libkern/c++/OSCollectionIterator.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSSymbol.h>
#include <mach/kmod.h>
#include <pexpert/pexpert.h>
#include <stdint.h>
#include <string.h>

#include "J716fBacklightDsi.h"

extern "C" {
extern kmod_info_t g_kernel_kmod_info;
void j716f_backlight_static_init(void) __asm__("__GLOBAL__sub_I_J716fBacklight.cpp");

// Telemetry array:
//   [0] stage (1 mapped, 2 hw ready, 3 published diag, 4 published as AppleARMBacklight, <0 failure)
//   [1] CoreBrightness setProperties requests
//   [2] last level written to hardware (0..2047)
//   [3] successful DSI0 TX count
//   [4] failed DSI0 TX count
//   [5] last error code
//   [6] DSI_HW_VERSION
//   [7] DSI_CTRL
//   [8] DSI_STATUS (last observed)
//   [9] DSI_TRIG_CTRL
//  [10] DSI_CLK_CTRL
//  [11] use_lpm (0=HS, 1=LP)
//  [12] last requested goal
//  [13] last requested fade ms
//  [14] live override calls
//  [15] impersonated flag (0/1)
volatile int32_t j716f_backlight_stats[16] = {};
}

namespace {

constexpr uint32_t kStepMs = 16u;
constexpr uint32_t kMaxFadeMs = 5000u;

IOMemoryMap *gDsiMap = nullptr;
volatile uint32_t *gDsiVa = nullptr;
volatile uint32_t gUseLpm = 0u;
volatile uint64_t gRequest = 0ULL;
IOTimerEventSource *volatile gRequestTimer = nullptr;
char gBlTrace[16][128] = {};
volatile uint32_t gBlTraceCount = 0u;

uint32_t dsiRead(void *ctx, uint32_t reg) {
    return reinterpret_cast<volatile uint32_t *>(ctx)[reg / 4u];
}

void dsiWrite(void *ctx, uint32_t reg, uint32_t val) {
    reinterpret_cast<volatile uint32_t *>(ctx)[reg / 4u] = val;
    __asm__ volatile("dsb sy" ::: "memory");
}

void dsiDelayUs(void *, uint32_t us) { IODelay(us); }

void dsiBarrier(void *) { __asm__ volatile("dsb sy" ::: "memory"); }

J716fDsiIo makeDsiIo() {
    J716fDsiIo io = {dsiRead, dsiWrite, dsiDelayUs, dsiBarrier, const_cast<uint32_t *>(gDsiVa)};
    return io;
}

void recordDiag(const J716fDsiDiag &d) {
    j716f_backlight_stats[6]  = static_cast<int32_t>(d.hw_ver);
    j716f_backlight_stats[7]  = static_cast<int32_t>(d.ctrl);
    j716f_backlight_stats[8]  = static_cast<int32_t>(d.status);
    j716f_backlight_stats[9]  = static_cast<int32_t>(d.trig_ctrl);
    j716f_backlight_stats[10] = static_cast<int32_t>(d.clk_ctrl);
}

void failStage(int stage, int err) {
    j716f_backlight_stats[5] = err;
    j716f_backlight_stats[0] = -stage;
}

}  // namespace

class J716fBacklight : public IOService {
    OSDeclareDefaultStructors(J716fBacklight)

    IOWorkLoop *loop = nullptr;
    IOTimerEventSource *fadeTimer = nullptr;
    IOTimerEventSource *requestTimer = nullptr;
    uint32_t level = J716F_BL_DEFAULT_LEVEL;
    uint32_t from = J716F_BL_DEFAULT_LEVEL;
    uint32_t to = J716F_BL_DEFAULT_LEVEL;
    uint64_t fadeStart = 0;
    uint64_t fadePeriod = 0;

    static inline const OSMetaClass *gAppleClass = nullptr;
    static const OSMetaClass *appleMetaClass(const OSMetaClassBase *) { return gAppleClass; }

    static uint64_t now() { return mach_absolute_time(); }

    bool applyHardware(uint32_t value) {
        if (!gDsiVa) return false;
        const bool lpm = __atomic_load_n(&gUseLpm, __ATOMIC_ACQUIRE) != 0u;
        const J716fDsiIo io = makeDsiIo();
        J716fDsiDiag diag = {};
        const int rc = j716f_dsi_send_brightness(&io, value, lpm, &diag);
        recordDiag(diag);
        if (rc == J716F_BL_OK) {
            j716f_backlight_stats[2] = static_cast<int32_t>(value);
            j716f_backlight_stats[3] = j716f_backlight_stats[3] + 1;
            j716f_backlight_stats[5] = 0;
            return true;
        }
        j716f_backlight_stats[4] = j716f_backlight_stats[4] + 1;
        j716f_backlight_stats[5] = rc;
        return false;
    }

    void publishLevel(uint32_t value) {
        // iPadOS 17.7.11 CoreBrightness reads IODisplayParameters.brightness {min, max, value}
        // and sends {brightness, FadePeriod (16.16 s)} via setProperties. Do NOT publish
        // iPadOS 27 nits/calibration tables (BrightnessMilliNits / backlight-marketing-table)
        // on iPadOS 17, which pushes CoreBrightness into an EDR calibration path incompatible
        // with legacy GOP scanout.
        OSDictionary *brightness = OSDictionary::withCapacity(3);
        OSDictionary *parameters = OSDictionary::withCapacity(1);
        OSNumber *min = OSNumber::withNumber(0ULL, 32);
        OSNumber *max = OSNumber::withNumber(J716F_BL_MAX_LEVEL, 32);
        OSNumber *current = OSNumber::withNumber(value, 32);
        if (brightness && parameters && min && max && current) {
            brightness->setObject("min", min);
            brightness->setObject("max", max);
            brightness->setObject("value", current);
            parameters->setObject("brightness", brightness);
            setProperty("IODisplayParameters", parameters);
        }
        OSSafeReleaseNULL(min);
        OSSafeReleaseNULL(max);
        OSSafeReleaseNULL(current);
        OSSafeReleaseNULL(brightness);
        OSSafeReleaseNULL(parameters);
    }

    // Impersonate AppleARMBacklight (slot 7 getMetaClass on arm64 iPadOS 17.7.11)
    // so CoreBrightness's CBDisplayContaineriOS recognizes the built-in display
    // backlight and starts com.apple.libEDR.Notification cleanly.
    // Note: IOService has 168 vtable slots on arm64; J716fBacklight adds no new
    // virtual methods, so copying 176 slots covers the full vtable safely.
    bool impersonate() {
        enum { kSlots = 176, kGetMetaClass = 7 };
        const OSSymbol *name = OSSymbol::withCString("AppleARMBacklight");
        gAppleClass = name ? OSMetaClass::getMetaClassWithName(name) : nullptr;
        OSSafeReleaseNULL(name);
        if (!gAppleClass) return false;
        uintptr_t *copy = static_cast<uintptr_t *>(IOMalloc(kSlots * sizeof(uintptr_t)));
        if (!copy) return false;
        memcpy(copy, *reinterpret_cast<uintptr_t **>(this), kSlots * sizeof(uintptr_t));
        copy[kGetMetaClass] = reinterpret_cast<uintptr_t>(&appleMetaClass);
        __asm__ volatile("dmb ish" ::: "memory");
        *reinterpret_cast<uintptr_t **>(this) = copy;
        return true;
    }

    static void fadeStep(OSObject *owner, IOTimerEventSource *sender) {
        J716fBacklight *self = static_cast<J716fBacklight *>(owner);
        if (!self) return;
        const uint64_t elapsed = now() - self->fadeStart;
        uint32_t next = self->to;
        if (elapsed < self->fadePeriod && self->fadePeriod != 0ULL) {
            const int64_t delta = static_cast<int64_t>(self->to) - static_cast<int64_t>(self->from);
            next = static_cast<uint32_t>(
                static_cast<int64_t>(self->from) +
                (delta * static_cast<int64_t>(elapsed)) / static_cast<int64_t>(self->fadePeriod));
        }
        if (next != self->level || next == self->to) {
            self->applyHardware(next);
            self->level = next;
        }
        if (next != self->to) {
            sender->setTimeoutMS(kStepMs);
        }
    }

    void beginFade(uint32_t goal, uint32_t milliseconds, bool forceWrite) {
        fadeTimer->cancelTimeout();
        from = level;
        to = goal;
        fadeStart = now();
        nanoseconds_to_absolutetime(static_cast<uint64_t>(milliseconds) * NSEC_PER_MSEC, &fadePeriod);
        if (milliseconds == 0u && forceWrite) {
            applyHardware(goal);
            level = goal;
            return;
        }
        fadeStep(this, fadeTimer);
    }

    static void requestFired(OSObject *owner, IOTimerEventSource *) {
        J716fBacklight *self = static_cast<J716fBacklight *>(owner);
        if (!self) return;
        const uint64_t req = __atomic_exchange_n(&gRequest, 0ULL, __ATOMIC_ACQUIRE);
        if (req >> 63) {
            const bool force = ((req >> 62) & 1ULL) != 0ULL;
            const uint32_t goal = static_cast<uint32_t>((req >> 16) & 0xFFFFu);
            const uint32_t ms = static_cast<uint32_t>(req & 0xFFFFu);
            self->beginFade(goal, ms, force);
        }
    }

public:
    bool startBacklight() {
        loop = IOWorkLoop::workLoop();
        fadeTimer = IOTimerEventSource::timerEventSource(this, &J716fBacklight::fadeStep);
        requestTimer = IOTimerEventSource::timerEventSource(this, &J716fBacklight::requestFired);
        if (!loop || !fadeTimer || !requestTimer) return false;
        if (loop->addEventSource(fadeTimer) || loop->addEventSource(requestTimer)) return false;

        __atomic_store_n(&gRequestTimer, requestTimer, __ATOMIC_RELEASE);

        uint32_t control = 1u;
        PE_parse_boot_argn("backlight-control", &control, sizeof(control));
        const bool impersonated = (control == 1u) && impersonate();
        if (impersonated) {
            // Only publish backlight-control=true when AppleARMBacklight conformance holds;
            // publishing it without conformance blocks IOMFBServer on libEDR (sheng int-236).
            setProperty("NativeSandboxGrantProcess", "backboardd");
            setProperty("backlight-control", true);
        }
        j716f_backlight_stats[15] = impersonated ? 1 : 0;

        publishLevel(level);
        registerService();
        j716f_backlight_stats[0] = impersonated ? 4 : 3;
        return true;
    }

    IOReturn setProperties(OSObject *properties) override {
        OSDictionary *dict = OSDynamicCast(OSDictionary, properties);
        if (!dict) return kIOReturnBadArgument;

        const uint32_t seq = __atomic_fetch_add(&gBlTraceCount, 1u, __ATOMIC_RELAXED);
        if (seq < 16u) {
            uint64_t ns = 0;
            absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
            char *line = gBlTrace[seq];
            size_t pos = 0;
            int n = snprintf(line, sizeof(gBlTrace[seq]), "t=%llums",
                             static_cast<unsigned long long>(ns / 1000000ULL));
            if (n > 0) pos = static_cast<size_t>(n);
            OSCollectionIterator *it = OSCollectionIterator::withCollection(dict);
            if (it) {
                while (const OSSymbol *k = OSDynamicCast(OSSymbol, it->getNextObject())) {
                    const OSObject *v = dict->getObject(k);
                    if (const OSNumber *num = OSDynamicCast(OSNumber, v)) {
                        n = snprintf(line + pos, sizeof(gBlTrace[seq]) - pos, " %s=%llu(0x%llx)",
                                     k->getCStringNoCopy(),
                                     static_cast<unsigned long long>(num->unsigned64BitValue()),
                                     static_cast<unsigned long long>(num->unsigned64BitValue()));
                    } else if (const OSBoolean *b = OSDynamicCast(OSBoolean, v)) {
                        n = snprintf(line + pos, sizeof(gBlTrace[seq]) - pos, " %s=%s",
                                     k->getCStringNoCopy(), b->isTrue() ? "true" : "false");
                    } else {
                        n = snprintf(line + pos, sizeof(gBlTrace[seq]) - pos, " %s=<%s>",
                                     k->getCStringNoCopy(),
                                     v && v->getMetaClass() ? v->getMetaClass()->getClassName() : "?");
                    }
                    if (n > 0) pos = (pos + static_cast<size_t>(n) < sizeof(gBlTrace[seq]))
                                         ? (pos + static_cast<size_t>(n))
                                         : (sizeof(gBlTrace[seq]) - 1u);
                }
                it->release();
            }
            IOLog("[J716F bl] prop#%u %s\n", seq, line);
        }

        // iPadOS 17.7.11 (21H461) CoreBrightness drives level-mode brightness exclusively via
        // {brightness, FadePeriod}. During initial startup it also emits a dummy uncalibrated
        // brightness-nits=0 sidecar write (~3 ms after brightness=1024) because BrightnessMilliNits
        // is not published; ignore brightness-nits here so it never blanks the panel at boot.
        uint32_t goal = 0u;
        uint64_t milliseconds = 0ULL;
        if (OSNumber *brightness = OSDynamicCast(OSNumber, dict->getObject("brightness"))) {
            OSNumber *fadePeriodProp = OSDynamicCast(OSNumber, dict->getObject("FadePeriod"));
            goal = j716f_bl_clamp_level(static_cast<int32_t>(brightness->unsigned32BitValue()));
            milliseconds = fadePeriodProp
                ? ((static_cast<uint64_t>(fadePeriodProp->unsigned32BitValue()) * 1000ULL) >> 16)
                : 0ULL;
            if (milliseconds > kMaxFadeMs) milliseconds = kMaxFadeMs;
        } else {
            return kIOReturnSuccess;
        }

        j716f_backlight_stats[1]  = j716f_backlight_stats[1] + 1;
        j716f_backlight_stats[12] = static_cast<int32_t>(goal);
        j716f_backlight_stats[13] = static_cast<int32_t>(milliseconds);

        // Non-blocking handoff to work loop (never runAction here to avoid backboardd deadlock).
        __atomic_store_n(&gRequest,
                         (1ULL << 63) | (static_cast<uint64_t>(goal) << 16) | milliseconds,
                         __ATOMIC_RELEASE);
        requestTimer->setTimeoutUS(1);
        publishLevel(goal);
        return kIOReturnSuccess;
    }
};

OSDefineMetaClassAndStructors(J716fBacklight, IOService)

extern "C" int j716f_backlight_set_override(uint32_t level, int lpm_override) {
    if (!gDsiVa || j716f_backlight_stats[0] < 3) return -1;
    const uint32_t clamped = level > J716F_BL_MAX_LEVEL ? J716F_BL_MAX_LEVEL : level;
    if (lpm_override == 0 || lpm_override == 1) {
        __atomic_store_n(&gUseLpm, static_cast<uint32_t>(lpm_override), __ATOMIC_RELEASE);
        j716f_backlight_stats[11] = lpm_override;
    }
    j716f_backlight_stats[12] = static_cast<int32_t>(clamped);
    j716f_backlight_stats[13] = 0;
    j716f_backlight_stats[14] = j716f_backlight_stats[14] + 1;
    __atomic_store_n(&gRequest,
                     (1ULL << 63) | (1ULL << 62) | (static_cast<uint64_t>(clamped) << 16),
                     __ATOMIC_RELEASE);
    IOTimerEventSource *timer = __atomic_load_n(&gRequestTimer, __ATOMIC_ACQUIRE);
    if (!timer) return -2;
    timer->setTimeoutUS(1);
    return 0;
}

extern "C" void j716f_backlight_snapshot(char *out, size_t cap) {
    if (!out || cap == 0) return;
    int n = snprintf(out, cap,
             "[usb] BACKLIGHT: stg=%d imp=%d lpm=%d lvl=%d goal=%d fade=%dms req=%d ok=%d err=%d last=%d ovr=%d | dsi ver=0x%08x ctrl=0x%08x sts=0x%08x trig=0x%08x clk=0x%08x\r\n",
             j716f_backlight_stats[0],
             j716f_backlight_stats[15],
             j716f_backlight_stats[11],
             j716f_backlight_stats[2],
             j716f_backlight_stats[12],
             j716f_backlight_stats[13],
             j716f_backlight_stats[1],
             j716f_backlight_stats[3],
             j716f_backlight_stats[4],
             j716f_backlight_stats[5],
             j716f_backlight_stats[14],
             static_cast<unsigned>(j716f_backlight_stats[6]),
             static_cast<unsigned>(j716f_backlight_stats[7]),
             static_cast<unsigned>(j716f_backlight_stats[8]),
             static_cast<unsigned>(j716f_backlight_stats[9]),
             static_cast<unsigned>(j716f_backlight_stats[10]));
    size_t pos = (n > 0 && static_cast<size_t>(n) < cap) ? static_cast<size_t>(n) : 0;
    const uint32_t total = __atomic_load_n(&gBlTraceCount, __ATOMIC_ACQUIRE);
    const uint32_t shown = total < 16u ? total : 16u;
    for (uint32_t i = 0; i < shown && pos + 4u < cap; ++i) {
        n = snprintf(out + pos, cap - pos, "[usb] BL_PROP[%u]: %s\r\n", i, gBlTrace[i]);
        if (n > 0) pos = (pos + static_cast<size_t>(n) < cap) ? (pos + static_cast<size_t>(n)) : (cap - 1u);
    }
}

extern "C" void j716f_backlight_probe(void) {
    static int started = 0;
    unsigned mode = 0u;
    if (!PE_parse_boot_argn("j716f-backlight", &mode, sizeof(mode)) || mode == 0u) {
        unsigned compat = 0u;
        if (!PE_parse_boot_argn("backlight", &compat, sizeof(compat)) || compat == 0u) {
            return;
        }
        mode = compat;
    }
    if (mode > 2u) return;
    if (__atomic_exchange_n(&started, 1, __ATOMIC_ACQ_REL)) return;

    const bool useLpm = (mode == 2u);
    __atomic_store_n(&gUseLpm, useLpm ? 1u : 0u, __ATOMIC_RELEASE);
    j716f_backlight_stats[11] = useLpm ? 1 : 0;

    void *handle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
    if (!handle) {
        failStage(5, -12);
        return;
    }
    j716f_backlight_static_init();
    const OSReturn metaRc = OSMetaClass::postModLoad(handle);
    if (metaRc != kOSReturnSuccess) {
        failStage(5, static_cast<int>(metaRc));
        return;
    }

    IODeviceMemory *mem = IODeviceMemory::withRange(J716F_DSI0_PHYS_BASE, J716F_DSI0_PHYS_SIZE);
    gDsiMap = mem ? mem->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
    if (mem) mem->release();
    gDsiVa = gDsiMap ? reinterpret_cast<volatile uint32_t *>(gDsiMap->getVirtualAddress()) : nullptr;
    if (!gDsiVa) {
        failStage(1, -12);
        return;
    }
    j716f_backlight_stats[0] = 1;

    const J716fDsiIo io = makeDsiIo();
    J716fDsiDiag diag = {};
    const int hwRc = j716f_dsi_probe_hw(&io, &diag);
    recordDiag(diag);
    if (hwRc != J716F_BL_OK) {
        failStage(2, hwRc);
        IOLog("[J716F bl] DSI0 not ready rc=%d ver=0x%08x ctrl=0x%08x sts=0x%08x\n",
              hwRc, diag.hw_ver, diag.ctrl, diag.status);
        return;
    }
    j716f_backlight_stats[0] = 2;

    J716fBacklight *bl = OSTypeAlloc(J716fBacklight);
    if (!bl || !bl->init() || !bl->attach(IOService::getPlatform()) || !bl->startBacklight()) {
        if (bl) bl->release();
        failStage(3, -19);
        IOLog("[J716F bl] service start failed\n");
        return;
    }
    bl->release();
    IOLog("[J716F bl] published stg=%d imp=%d lpm=%d dsi_ver=0x%08x ctrl=0x%08x trig=0x%08x\n",
          j716f_backlight_stats[0], j716f_backlight_stats[15], j716f_backlight_stats[11],
          diag.hw_ver, diag.ctrl, diag.trig_ctrl);
}
