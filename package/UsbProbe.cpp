// SPDX-License-Identifier: BSD-2-Clause-Patent
// TB-J716F (SM8250) board entry for the QcomDwc3 AppleUSBXDCI driver (iPadOS 17.7.11 21H461).

#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <libkern/c++/OSMetaClass.h>
#include <libkern/OSKextLib.h>
#include <pexpert/pexpert.h>
#include <kern/thread.h>

extern kmod_info_t g_kernel_kmod_info;
extern "C" bool qcom_dwc3_bring_up(void);
extern "C" void qcom_dwc3_publish(void);
extern "C" void qcom_dwc3_vbus(int online);
extern "C" void qcom_dwc3_static_init(void) __asm__("__GLOBAL__sub_I_QcomDwc3.cpp");
extern "C" volatile uint32_t native_usb_stats[54];

static bool gMetaRegistered = false;

extern "C" void native_usb_xdci_activate(void) {
    if (!qcom_dwc3_bring_up()) {
        return;
    }
    if (!gMetaRegistered) {
        const OSReturn loaded = OSKextLoadKextWithIdentifier("com.apple.driver.AppleUSBXDCI");
        (void)OSKextLoadKextWithIdentifier("com.apple.driver.AppleUSBDeviceMux");
        native_usb_stats[21] = static_cast<uint32_t>(loaded);
        if (loaded != kOSReturnSuccess && loaded != kOSKextReturnStartStopError) {
            native_usb_stats[0] = static_cast<uint32_t>(-9);
            return;
        }
        void *handle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
        if (!handle) {
            native_usb_stats[0] = static_cast<uint32_t>(-10);
            return;
        }
        qcom_dwc3_static_init();
        if (OSMetaClass::postModLoad(handle)) {
            native_usb_stats[0] = static_cast<uint32_t>(-11);
            return;
        }
        gMetaRegistered = true;
    }
    qcom_dwc3_publish();
}

extern "C" void native_usb_xdci_deactivate(void) {
    qcom_dwc3_vbus(0);
}

static void worker(void *, wait_result_t) {
    IOSleep(3000);
    native_usb_xdci_activate();
    thread_terminate(current_thread());
}

extern "C" void sheng_usb_probe(void) {
    unsigned enabled = 0;
    if (!PE_parse_boot_argn("sheng-usb", &enabled, sizeof(enabled)) || !enabled) return;
    unsigned cdc = 1;
    (void)PE_parse_boot_argn("j716f-usb", &cdc, sizeof(cdc));
    if (cdc) {
        // When j716f-usb=1 is active, UsbDebugCdc owns 0x0a600000 at boot and can hand off via usb.xdci / usb.xdci.30
        return;
    }
    thread_t thread = nullptr;
    if (kernel_thread_start(worker, nullptr, &thread) == KERN_SUCCESS && thread) {
        thread_deallocate(thread);
    }
}
