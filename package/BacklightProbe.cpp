// SPDX-License-Identifier: BSD-2-Clause-Patent
// Late registration probe for AppleARMBacklight on TB-J716F.
// Enabled by default (j716f-backlight=1 / sheng-backlight=1; pass j716f-backlight=0 to disable).

#include <IOKit/IOService.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOLib.h>
#include <libkern/OSBase.h>
#include <pexpert/pexpert.h>
#include <stdint.h>

extern "C" void sheng_backlight_static_init(void) __asm__("__GLOBAL__sub_I_AppleARMBacklight.cpp");
extern "C" void *shengxnu_backlight_create_instance(void);
extern "C" const void *_ZTV14ShengBacklight[];
extern "C" {
int sheng_backlight_probe_result = 0;
int sheng_backlight_probe_stage = 0;
}

namespace {

static const OSMetaClass *gAppleBacklightMeta;
static const void *gPatchedBacklightVtable[256];

static const OSMetaClass *appleBacklightGetMetaClass(const OSObject *) {
    return gAppleBacklightMeta;
}

static bool patchBacklightVtable(IOService *service) {
    gAppleBacklightMeta = OSMetaClass::getMetaClassWithName(
        OSSymbol::withCStringNoCopy("AppleARMBacklight"));
    if (!gAppleBacklightMeta) return false;
    for (size_t i = 0; i < 189; ++i) {
        gPatchedBacklightVtable[i] = _ZTV14ShengBacklight[i];
    }
    gPatchedBacklightVtable[2 + 7] = reinterpret_cast<const void *>(&appleBacklightGetMetaClass);
    *reinterpret_cast<const void ***>(service) = &gPatchedBacklightVtable[2];
    return true;
}

} // namespace

extern "C" void sheng_backlight_probe(void) {
    static bool started = false;
    if (started) return;
    uint32_t enabled = 1;
    PE_parse_boot_argn("j716f-backlight", &enabled, sizeof(enabled));
    PE_parse_boot_argn("sheng-backlight", &enabled, sizeof(enabled));
    if (!enabled) return;
    started = true;
    sheng_backlight_probe_stage = 1;

    void *handle = OSMetaClass::preModLoad("com.apple.driver.AppleARMPlatform");
    if (!handle) {
        sheng_backlight_probe_result = -1;
        return;
    }
    sheng_backlight_static_init();
    if (OSMetaClass::postModLoad(handle) != kIOReturnSuccess) {
        sheng_backlight_probe_result = -2;
        return;
    }
    sheng_backlight_probe_stage = 2;

    IOService *platform = IOService::getPlatform();
    if (!platform) {
        sheng_backlight_probe_result = -3;
        return;
    }
    IOService *service = static_cast<IOService *>(shengxnu_backlight_create_instance());
    if (!service) {
        sheng_backlight_probe_result = -4;
        return;
    }
    uint32_t control = 1;
    PE_parse_boot_argn("sheng-backlight-control", &control, sizeof(control));
    if (control && !patchBacklightVtable(service)) {
        service->release();
        sheng_backlight_probe_result = -6;
        return;
    }
    if (!service->attach(platform)) {
        service->release();
        sheng_backlight_probe_result = -5;
        return;
    }
    sheng_backlight_probe_stage = 3;
    if (!service->start(platform)) {
        service->detach(platform);
        service->release();
        sheng_backlight_probe_result = -6;
        return;
    }
    sheng_backlight_probe_stage = 4;
    sheng_backlight_probe_result = 1;
}
