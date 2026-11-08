// SPDX-License-Identifier: BSD-2-Clause-Patent
// AppleARMBacklight display brightness service for Lenovo Xiaoxin Pad Pro 2021 (TB-J716F).
//
// TB-J716F uses an EDO RM692E6 2560x1600 90Hz Dual-DSI Command-Mode AMOLED panel
// (Panel_rm692e6_amoled_2k_cmd.xml, 11-bit brightness range 0..2047).
// Because reading/writing MDSS/DSI registers from EL1 without full clock/GDSC ownership
// triggers an asynchronous SError, and AMOLED subpixels are self-emissive (0x000000 = 0 nits),
// this service publishes the AppleARMBacklight interface expected by CoreBrightness in
// backboardd (IODisplayParameters, BrightnessMilliNits, calibration properties, and
// FadePeriod ramps) and drives the real-time AMOLED scanout brightness scale (0..256).

#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <kern/thread.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSBoolean.h>
#include <libkern/c++/OSString.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSDictionary.h>
#include <pexpert/pexpert.h>
#include <stdint.h>
#include <string.h>

extern "C" {
// [0] stage (1 init, 4 ready)
// [1] current level (0..2047)
// [2] target level (0..2047)
// [3] brightness updates applied
// [4] errors (0)
// [5] setProperties calls from CoreBrightness (backboardd)
// [6] last FadePeriod (ms)
// [7] current AMOLED pixel scale (0..256)
volatile uint32_t native_backlight_stats[8];
volatile uint32_t native_backlight_scale = 256;
void native_legacy_refresh_brightness(void) __attribute__((weak_import));
}

namespace {

constexpr uint32_t kMaxLevel = 2047;
constexpr uint32_t kInitialLevel = 2047;
constexpr uint32_t kMaxFadeMs = 2000;
constexpr uint32_t kStepMs = 16;
// Millinits range for iPadOS CoreBrightness: 2 nits min, 500 nits max
constexpr int32_t kMinMilliNits = 2000;
constexpr int32_t kMaxMilliNits = 500000;
constexpr uint32_t kLmaxProduct = 500u << 16;   // 16.16 fixed-point nits
constexpr uint32_t kLmidProduct = 140;
constexpr uint32_t kLminProduct = 2;

static uint32_t levelToScale(uint32_t level) {
    if (level == 0) return 0;
    if (level >= kMaxLevel) return 256;
    // Map 1..2047 to 24..256 (~9.4% to 100%) so minimum slider stays readable
    return 24 + (level * 232) / kMaxLevel;
}

static bool extractU32(const OSObject *obj, uint32_t *out) {
    if (!obj) return false;
    if (const OSNumber *num = OSDynamicCast(OSNumber, obj)) {
        *out = num->unsigned32BitValue();
        return true;
    }
    if (const OSDictionary *dict = OSDynamicCast(OSDictionary, obj)) {
        if (const OSNumber *val = OSDynamicCast(OSNumber, dict->getObject("value"))) {
            *out = val->unsigned32BitValue();
            return true;
        }
    }
    return false;
}

static uint32_t nitsFixedToLevel(uint32_t fixed) {
    int32_t nits = static_cast<int32_t>(fixed);
    if (nits <= 0) return 0;
    int64_t milli = (static_cast<int64_t>(nits) * 1000) >> 16;
    if (milli <= kMinMilliNits) return 1;
    if (milli >= kMaxMilliNits) return kMaxLevel;
    return static_cast<uint32_t>(1 + (milli - kMinMilliNits) * (kMaxLevel - 1) / (kMaxMilliNits - kMinMilliNits));
}

static OSDictionary *makeRangeDict(int64_t minVal, int64_t maxVal, int64_t curVal) {
    OSDictionary *dict = OSDictionary::withCapacity(3);
    if (!dict) return nullptr;
    OSNumber *nMin = OSNumber::withNumber(static_cast<uint64_t>(minVal), 64);
    OSNumber *nMax = OSNumber::withNumber(static_cast<uint64_t>(maxVal), 64);
    OSNumber *nVal = OSNumber::withNumber(static_cast<uint64_t>(curVal), 64);
    if (nMin) { dict->setObject("min", nMin); nMin->release(); }
    if (nMax) { dict->setObject("max", nMax); nMax->release(); }
    if (nVal) { dict->setObject("value", nVal); nVal->release(); }
    return dict;
}

} // namespace

class ShengBacklight : public IOService {
    OSDeclareDefaultStructors(ShengBacklight)

private:
    IOLock *fLock;
    uint32_t fCurrent;
    uint32_t fTarget;
    uint32_t fSavedNonZero;
    uint32_t fFadeStart;
    uint32_t fFadeSteps;
    uint32_t fFadeStep;
    bool fEnabled;
    bool fStopping;

    void applyLevel(uint32_t level) {
        if (level > kMaxLevel) level = kMaxLevel;
        fCurrent = level;
        uint32_t scale = levelToScale(level);
        native_backlight_stats[1] = level;
        native_backlight_stats[7] = scale;
        if (native_backlight_scale != scale) {
            native_backlight_scale = scale;
            native_backlight_stats[3] = native_backlight_stats[3] + 1;
            if (&native_legacy_refresh_brightness) {
                native_legacy_refresh_brightness();
            }
        }
    }

    void publishDisplayParameters(uint32_t level) {
        OSDictionary *params = OSDictionary::withCapacity(3);
        if (!params) return;
        if (OSDictionary *br = makeRangeDict(0, kMaxLevel, level)) {
            params->setObject("brightness", br);
            br->release();
        }
        int64_t milli = level ? kMinMilliNits + static_cast<int64_t>(level) * (kMaxMilliNits - kMinMilliNits) / kMaxLevel : 0;
        if (OSDictionary *nits = makeRangeDict(kMinMilliNits, kMaxMilliNits, milli)) {
            params->setObject("BrightnessMilliNits", nits);
            nits->release();
        }
        if (OSDictionary *fade = makeRangeDict(0, kMaxFadeMs, 0)) {
            params->setObject("FadePeriod", fade);
            fade->release();
        }
        setProperty("IODisplayParameters", params);
        params->release();
    }

    static void fadeThreadMain(void *arg, wait_result_t) {
        static_cast<ShengBacklight *>(arg)->fadeLoop();
    }

    void fadeLoop() {
        for (;;) {
            IOSleep(kStepMs);
            if (!fLock) break;
            IOLockLock(fLock);
            if (fStopping) {
                IOLockUnlock(fLock);
                break;
            }
            if (fCurrent != fTarget) {
                uint32_t next = fTarget;
                if (fFadeSteps > 1 && fFadeStep + 1 < fFadeSteps) {
                    ++fFadeStep;
                    int32_t start = static_cast<int32_t>(fFadeStart);
                    int32_t delta = static_cast<int32_t>(fTarget) - start;
                    next = static_cast<uint32_t>(start + delta * static_cast<int32_t>(fFadeStep) / static_cast<int32_t>(fFadeSteps));
                }
                applyLevel(next);
            }
            IOLockUnlock(fLock);
        }
    }

public:
    bool init(OSDictionary *dict = nullptr) override {
        if (!IOService::init(dict)) return false;
        fLock = IOLockAlloc();
        fCurrent = kInitialLevel;
        fTarget = kInitialLevel;
        fSavedNonZero = kInitialLevel;
        fFadeStart = kInitialLevel;
        fFadeSteps = 0;
        fFadeStep = 0;
        fEnabled = true;
        fStopping = false;
        return fLock != nullptr;
    }

    void free() override {
        if (fLock) {
            IOLockFree(fLock);
            fLock = nullptr;
        }
        IOService::free();
    }

    bool start(IOService *provider) override {
        if (!IOService::start(provider)) return false;
        native_backlight_stats[0] = 1;

        setName("backlight");
        setProperty("IONameMatch", "backlight");
        setProperty("IONameMatched", "backlight");
        // Grant backboardd (CoreBrightness) permission to call IORegistryEntrySetCFProperties on this service.
        setProperty("NativeSandboxGrantProcess", "backboardd");

        uint32_t control = 1;
        PE_parse_boot_argn("sheng-backlight-control", &control, sizeof(control));
        if (control) {
            setProperty("backlight-control", kOSBooleanTrue);
        }

        const uint8_t zeroTable[32] = {};
        if (OSData *data = OSData::withBytes(zeroTable, sizeof(zeroTable))) {
            setProperty("backlight-marketing-table", data);
            setProperty("backlight-calibration-parameters", data);
            data->release();
        }
        const uint32_t lmax = kLmaxProduct;
        if (OSData *data = OSData::withBytes(&lmax, sizeof(lmax))) {
            setProperty("LmaxProduct", data);
            data->release();
        }
        setProperty("LmidProduct", kLmidProduct, 32);
        setProperty("LminProduct", kLminProduct, 32);

        applyLevel(kInitialLevel);
        native_backlight_stats[2] = kInitialLevel;
        publishDisplayParameters(kInitialLevel);

        thread_t thread = THREAD_NULL;
        if (kernel_thread_start(&ShengBacklight::fadeThreadMain, this, &thread) == KERN_SUCCESS) {
            thread_deallocate(thread);
        }

        native_backlight_stats[0] = 4;
        registerService();
        IOLog("[backlight] AppleARMBacklight registered (level=%u/%u scale=%u)\n",
              fCurrent, kMaxLevel, native_backlight_scale);
        return true;
    }

    void stop(IOService *provider) override {
        if (fLock) {
            IOLockLock(fLock);
            fStopping = true;
            IOLockUnlock(fLock);
        }
        IOService::stop(provider);
    }

    IOReturn setProperties(OSObject *properties) override {
        OSDictionary *dict = OSDynamicCast(OSDictionary, properties);
        if (!dict) return kIOReturnBadArgument;

        const OSDictionary *params = OSDynamicCast(OSDictionary, dict->getObject("IODisplayParameters"));
        const OSDictionary *source = params ? params : dict;

        uint32_t fadeMs = 0;
        extractU32(source->getObject("FadePeriod"), &fadeMs);
        if (fadeMs > kMaxFadeMs) fadeMs = kMaxFadeMs;

        uint32_t newLevel = 0;
        bool hasLevel = false;
        if (extractU32(source->getObject("brightness"), &newLevel)) {
            if (newLevel > kMaxLevel) newLevel = kMaxLevel;
            hasLevel = true;
        } else {
            uint32_t fixedNits = 0;
            if (extractU32(source->getObject("brightness-nits"), &fixedNits) ||
                extractU32(source->getObject("BrightnessMilliNits"), &fixedNits)) {
                newLevel = nitsFixedToLevel(fixedNits);
                hasLevel = true;
            }
        }

        if (const OSBoolean *en = OSDynamicCast(OSBoolean, source->getObject("backlight-enable"))) {
            fEnabled = en->isTrue();
            if (!fEnabled) {
                newLevel = 0;
                hasLevel = true;
            } else if (!hasLevel) {
                newLevel = fSavedNonZero ? fSavedNonZero : kInitialLevel;
                hasLevel = true;
            }
        }

        if (!hasLevel) return kIOReturnSuccess;

        native_backlight_stats[5] = native_backlight_stats[5] + 1;
        native_backlight_stats[6] = fadeMs;

        IOLockLock(fLock);
        if (newLevel > 0) {
            fSavedNonZero = newLevel;
        }
        fTarget = newLevel;
        native_backlight_stats[2] = newLevel;
        uint32_t steps = fadeMs / kStepMs;
        if (steps <= 1 || newLevel == fCurrent) {
            fFadeSteps = 0;
            fFadeStep = 0;
            applyLevel(newLevel);
        } else {
            fFadeStart = fCurrent;
            fFadeSteps = steps;
            fFadeStep = 0;
        }
        IOLockUnlock(fLock);

        publishDisplayParameters(newLevel);
        return kIOReturnSuccess;
    }
};

OSDefineMetaClassAndStructors(ShengBacklight, IOService)

extern "C" void *shengxnu_backlight_create_instance(void) {
    ShengBacklight *instance = new ShengBacklight;
    if (!instance) return nullptr;
    if (!instance->init()) {
        instance->release();
        return nullptr;
    }
    return instance;
}
