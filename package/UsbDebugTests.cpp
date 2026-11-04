// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host unit tests for TB-J716F SM8250 USB CDC-ACM wire helpers, command parser,
// GSC1 RGB565 frame encoder, and ZUI 15.0.485 hardware register constants.

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "UsbDebugCdc.h"
#include "UsbDebugWire.h"

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Waddress"
#endif

extern "C" {
extern volatile uint64_t native_legacy_gop_virtual __attribute__((weak));
extern volatile uint32_t native_legacy_gop_width   __attribute__((weak));
extern volatile uint32_t native_legacy_gop_height  __attribute__((weak));
extern volatile uint64_t native_legacy_gop_bytes   __attribute__((weak));
}

namespace {
#include "UsbDebugCommands.h"
}

#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

extern "C" {
volatile uint64_t native_legacy_gop_virtual = 0;
volatile uint32_t native_legacy_gop_width   = 0;
volatile uint32_t native_legacy_gop_height  = 0;
volatile uint64_t native_legacy_gop_bytes   = 0;
}

int main(void) {
    // 1. Verify ZUI 15.0.485 SM8250 hardware addresses
    assert(J716F_USB0_CORE_BASE == 0x0A600000ULL);
    assert(J716F_EUD_BASE == 0x088E1000ULL);
    assert(J716F_HSPHY_BASE == 0x088E3000ULL);
    assert(J716F_PS_HOLD_PHYS == 0x00C26000ULL);
    assert(J716F_IMEM_REASON_PAGE == 0x146BF000ULL);
    assert(J716F_IMEM_REASON_OFFSET == 0x0850u);
    assert(DWC3_GTXFIFOSIZ(1) == 0xC304u);
    assert(FEMTO_PARAM_OVRD_X1 == 0x70u);

    // 2. Verify IEEE 802.3 CRC32 and USB CDC-ACM descriptors (VID=0x1209, PID=0x0001)
    assert(usb_crc32(reinterpret_cast<const uint8_t *>("123456789"), 9) == 0xCBF43926u);
    assert(kUsbCdcDeviceDesc[0] == 18u && kUsbCdcDeviceDesc[1] == 1u);
    assert(kUsbCdcDeviceDesc[8] == 0x09u && kUsbCdcDeviceDesc[9] == 0x12u);
    assert(kUsbCdcDeviceDesc[10] == 0x01u && kUsbCdcDeviceDesc[11] == 0x00u);

    uint8_t cfg[67] = {};
    assert(usb_cdc_build_config_desc(cfg, 512) == 67u);
    assert(cfg[2] == 67u && cfg[4] == 2u);
    assert(cfg[57] == 0x00u && cfg[58] == 0x02u);
    assert(cfg[64] == 0x00u && cfg[65] == 0x02u);

    // 3. Verify GSC1 RGB565 downsampler & packet framing
    static uint32_t mockFb[160 * 100] = {};
    mockFb[0] = 0xFFFF0000u;  // BGRA8888: B=0, G=0, R=255 -> RGB565 0xF800
    native_legacy_gop_virtual = reinterpret_cast<uint64_t>(mockFb);
    native_legacy_gop_width   = 160u;
    native_legacy_gop_height  = 100u;
    native_legacy_gop_bytes   = sizeof(mockFb);

    static uint8_t rgb[160 * 100 * 2] = {};
    static uint8_t ringBuf[65536] = {};
    UsbReplyRing ring{};
    ring.buf = ringBuf;
    ring.cap = sizeof(ringBuf);
    ring.reset();

    UsbScreenState sc{};
    sc.init(rgb, sizeof(rgb));
    sc.maxEdge = 160u;
    assert(usbCaptureAndQueueGsc1(sc, ring, true));
    assert(rgb[0] == 0x00u && rgb[1] == 0xF8u);
    assert(ring.used > 44u);

    uint8_t hdr[44] = {};
    assert(ring.pop(hdr, sizeof(hdr)) == 44u);
    assert(hdr[0] == 0x00u && memcmp(hdr + 1, "GSC1", 4) == 0);

    ring.reset();
    usbQueueGsc1Stop(sc, ring);
    assert(ring.used == 44u);

    // 4. Verify command string helpers
    const char *args = " 1280 15";
    bool ok1 = false, ok2 = false;
    assert(parseUint(args, &ok1) == 1280u && ok1);
    assert(parseUint(args, &ok2) == 15u && ok2);
    assert(strEq("reboot.fastboot", "reboot.fastboot"));
    assert(strStarts("screen.configure 960 8", "screen.configure"));

    printf("UsbDebugTests: all checks passed\n");
    return 0;
}
