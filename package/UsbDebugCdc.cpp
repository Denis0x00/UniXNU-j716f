// SPDX-License-Identifier: BSD-2-Clause-Patent
// Live USB CDC-ACM diagnostic terminal, ramoops streamer, GSC1 framebuffer capture,
// and warm-fastboot reboot controller for Lenovo Xiaoxin Pad Pro 2021 (TB-J716F, SM8250).

#include "UsbDebugCdc.h"
#include "UsbDebugWire.h"

#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <string.h>
#include <libkern/c++/OSSharedPtr.h>
#include <kern/task.h>
#include <kern/thread.h>
#include <pexpert/pexpert.h>

extern "C" {
#include "QcomRpmh.h"
#include "SmmuBypass.h"

extern void boot_progress_stage(const char *text) __attribute__((weak_import));
extern char boot_progress_ext[8][48] __attribute__((weak_import));

extern volatile uint64_t native_legacy_gop_virtual __attribute__((weak_import));
extern volatile uint64_t native_legacy_gop_physical __attribute__((weak_import));
extern volatile uint64_t native_legacy_gop_bytes __attribute__((weak_import));
extern volatile uint32_t native_legacy_gop_width __attribute__((weak_import));
extern volatile uint32_t native_legacy_gop_height __attribute__((weak_import));
extern volatile uint32_t native_legacy_stage __attribute__((weak_import));
extern volatile uint32_t native_legacy_transfer_calls __attribute__((weak_import));
extern volatile uint32_t native_legacy_retirement_calls __attribute__((weak_import));
extern volatile uint32_t native_gop_gpu_offers __attribute__((weak_import));
extern volatile uint32_t native_gop_gpu_completions __attribute__((weak_import));

extern volatile unsigned native_touch_stage __attribute__((weak_import));
extern volatile unsigned native_touch_stroke_count __attribute__((weak_import));
extern char native_touch_rows[4][120] __attribute__((weak_import));

extern volatile unsigned native_gpu_step __attribute__((weak_import));
extern char native_gpu_rows[31][120] __attribute__((weak_import));
extern volatile unsigned native_mdss_step __attribute__((weak_import));
extern char native_mdss_rows[31][120] __attribute__((weak_import));
extern char native_flip_rows[16][120] __attribute__((weak_import));
extern volatile uint32_t native_backlight_stats[8] __attribute__((weak_import));
extern volatile uint32_t native_backlight_scale __attribute__((weak_import));
extern volatile int sheng_backlight_probe_stage __attribute__((weak_import));
extern volatile int sheng_backlight_probe_result __attribute__((weak_import));
extern char native_battery_row[120] __attribute__((weak_import));
extern volatile int native_battery_error __attribute__((weak_import));
extern volatile unsigned native_battery_published __attribute__((weak_import));

extern volatile uint32_t native_wifi_stats[29] __attribute__((weak_import));
extern volatile uint32_t native_wifi_ep[16] __attribute__((weak_import));
extern volatile uint32_t native_wifi_mhi[16] __attribute__((weak_import));
extern char native_wifi_note[128][112] __attribute__((weak_import));
extern volatile uint32_t native_wifi_note_n __attribute__((weak_import));

extern volatile unsigned native_bt_step __attribute__((weak_import));
extern char native_bt_rows[16][120] __attribute__((weak_import));
extern const char *native_bt_stats(void) __attribute__((weak_import));
extern const char *native_bt_uart(void) __attribute__((weak_import));

extern void native_usb_xdci_activate(void) __attribute__((weak_import));
extern void native_usb_xdci_deactivate(void) __attribute__((weak_import));
extern volatile uint32_t native_usb_stats[54] __attribute__((weak_import));

extern volatile uint32_t armcpu_smp_running __attribute__((weak_import));
}

struct UsbPlaceTag {};
inline void *operator new(size_t, void *p, UsbPlaceTag) noexcept { return p; }

namespace {

#include "UsbDebugCommands.h"
#include "UsbDebugHw.h"

static const char *const kUsbStrings[4] = {
    "",
    "UniXNU",
    "TB-J716F Debug CDC",
    "J716FDBG01"
};

static uint64_t nowMs() {
    uint64_t cnt = 0, frq = 0;
    __asm__ volatile("isb\n\tmrs %0, cntvct_el0" : "=r"(cnt) :: "memory");
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(frq));
    return frq ? (cnt * 1000ULL) / frq : 0ULL;
}

static char gUsbHudLine0[112] = "usb: init";
static char gUsbHudLine1[112] = "reg: waiting...";

// Compact 8x8 bitmap font for printable ASCII 0x20..0x7E
static const uint8_t kHudFont8x8[95][8] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, // ' '
    {0x18,0x3C,0x3C,0x18,0x18,0x00,0x18,0x00}, // '!'
    {0x6C,0x6C,0x24,0x00,0x00,0x00,0x00,0x00}, // '"'
    {0x6C,0x6C,0xFE,0x6C,0xFE,0x6C,0x6C,0x00}, // '#'
    {0x18,0x3E,0x60,0x3C,0x06,0x7C,0x18,0x00}, // '$'
    {0x00,0xC6,0xCC,0x18,0x30,0x66,0xC6,0x00}, // '%'
    {0x38,0x6C,0x38,0x76,0xDC,0xCC,0x76,0x00}, // '&'
    {0x18,0x18,0x30,0x00,0x00,0x00,0x00,0x00}, // '\''
    {0x0C,0x18,0x30,0x30,0x30,0x18,0x0C,0x00}, // '('
    {0x30,0x18,0x0C,0x0C,0x0C,0x18,0x30,0x00}, // ')'
    {0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00}, // '*'
    {0x00,0x18,0x18,0x7E,0x18,0x18,0x00,0x00}, // '+'
    {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x30}, // ','
    {0x00,0x00,0x00,0x7E,0x00,0x00,0x00,0x00}, // '-'
    {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x00}, // '.'
    {0x06,0x0C,0x18,0x30,0x60,0xC0,0x80,0x00}, // '/'
    {0x7C,0xC6,0xCE,0xD6,0xE6,0xC6,0x7C,0x00}, // '0'
    {0x18,0x38,0x18,0x18,0x18,0x18,0x7E,0x00}, // '1'
    {0x7C,0xC6,0x06,0x1C,0x30,0x66,0xFE,0x00}, // '2'
    {0x7C,0xC6,0x06,0x3C,0x06,0xC6,0x7C,0x00}, // '3'
    {0x1C,0x3C,0x6C,0xCC,0xFE,0x0C,0x1E,0x00}, // '4'
    {0xFE,0xC0,0xFC,0x06,0x06,0xC6,0x7C,0x00}, // '5'
    {0x38,0x60,0xC0,0xFC,0xC6,0xC6,0x7C,0x00}, // '6'
    {0xFE,0xC6,0x0C,0x18,0x30,0x30,0x30,0x00}, // '7'
    {0x7C,0xC6,0xC6,0x7C,0xC6,0xC6,0x7C,0x00}, // '8'
    {0x7C,0xC6,0xC6,0x7E,0x06,0x0C,0x78,0x00}, // '9'
    {0x00,0x18,0x18,0x00,0x00,0x18,0x18,0x00}, // ':'
    {0x00,0x18,0x18,0x00,0x00,0x18,0x18,0x30}, // ';'
    {0x0C,0x18,0x30,0x60,0x30,0x18,0x0C,0x00}, // '<'
    {0x00,0x00,0x7E,0x00,0x00,0x7E,0x00,0x00}, // '='
    {0x30,0x18,0x0C,0x06,0x0C,0x18,0x30,0x00}, // '>'
    {0x7C,0xC6,0x0C,0x18,0x18,0x00,0x18,0x00}, // '?'
    {0x7C,0xC6,0xDE,0xDE,0xDE,0xC0,0x78,0x00}, // '@'
    {0x38,0x6C,0xC6,0xFE,0xC6,0xC6,0xC6,0x00}, // 'A'
    {0xFC,0x66,0x66,0x7C,0x66,0x66,0xFC,0x00}, // 'B'
    {0x3C,0x66,0xC0,0xC0,0xC0,0x66,0x3C,0x00}, // 'C'
    {0xF8,0x6C,0x66,0x66,0x66,0x6C,0xF8,0x00}, // 'D'
    {0xFE,0x62,0x68,0x78,0x68,0x62,0xFE,0x00}, // 'E'
    {0xFE,0x62,0x68,0x78,0x68,0x60,0xF0,0x00}, // 'F'
    {0x3C,0x66,0xC0,0xC0,0xCE,0x66,0x3E,0x00}, // 'G'
    {0xC6,0xC6,0xC6,0xFE,0xC6,0xC6,0xC6,0x00}, // 'H'
    {0x3C,0x18,0x18,0x18,0x18,0x18,0x3C,0x00}, // 'I'
    {0x1E,0x0C,0x0C,0x0C,0xCC,0xCC,0x78,0x00}, // 'J'
    {0xE6,0x66,0x6C,0x78,0x6C,0x66,0xE6,0x00}, // 'K'
    {0xF0,0x60,0x60,0x60,0x62,0x66,0xFE,0x00}, // 'L'
    {0xC6,0xEE,0xFE,0xFE,0xD6,0xC6,0xC6,0x00}, // 'M'
    {0xC6,0xE6,0xF6,0xDE,0xCE,0xC6,0xC6,0x00}, // 'N'
    {0x7C,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x00}, // 'O'
    {0xFC,0x66,0x66,0x7C,0x60,0x60,0xF0,0x00}, // 'P'
    {0x7C,0xC6,0xC6,0xC6,0xD6,0xDE,0x7C,0x06}, // 'Q'
    {0xFC,0x66,0x66,0x7C,0x6C,0x66,0xE6,0x00}, // 'R'
    {0x7C,0xC6,0x60,0x38,0x0C,0xC6,0x7C,0x00}, // 'S'
    {0x7E,0x7E,0x5A,0x18,0x18,0x18,0x3C,0x00}, // 'T'
    {0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x00}, // 'U'
    {0xC6,0xC6,0xC6,0xC6,0xC6,0x6C,0x38,0x00}, // 'V'
    {0xC6,0xC6,0xD6,0xFE,0xFE,0xEE,0xC6,0x00}, // 'W'
    {0xC6,0xC6,0x6C,0x38,0x6C,0xC6,0xC6,0x00}, // 'X'
    {0x66,0x66,0x66,0x3C,0x18,0x18,0x3C,0x00}, // 'Y'
    {0xFE,0xC6,0x8C,0x18,0x32,0x66,0xFE,0x00}, // 'Z'
    {0x3C,0x30,0x30,0x30,0x30,0x30,0x3C,0x00}, // '['
    {0xC0,0x60,0x30,0x18,0x0C,0x06,0x02,0x00}, // '\\'
    {0x3C,0x0C,0x0C,0x0C,0x0C,0x0C,0x3C,0x00}, // ']'
    {0x10,0x38,0x6C,0xC6,0x00,0x00,0x00,0x00}, // '^'
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF}, // '_'
    {0x30,0x18,0x0C,0x00,0x00,0x00,0x00,0x00}, // '`'
    {0x00,0x00,0x78,0x0C,0x7C,0xCC,0x76,0x00}, // 'a'
    {0xE0,0x60,0x7C,0x66,0x66,0x66,0xDC,0x00}, // 'b'
    {0x00,0x00,0x7C,0xC6,0xC0,0xC6,0x7C,0x00}, // 'c'
    {0x1C,0x0C,0x7C,0xCC,0xCC,0xCC,0x76,0x00}, // 'd'
    {0x00,0x00,0x7C,0xC6,0xFE,0xC0,0x7C,0x00}, // 'e'
    {0x3C,0x66,0x60,0xF8,0x60,0x60,0xF0,0x00}, // 'f'
    {0x00,0x00,0x76,0xCC,0xCC,0x7C,0x0C,0xF8}, // 'g'
    {0xE0,0x60,0x6C,0x76,0x66,0x66,0xE6,0x00}, // 'h'
    {0x18,0x00,0x38,0x18,0x18,0x18,0x3C,0x00}, // 'i'
    {0x06,0x00,0x06,0x06,0x06,0x66,0x66,0x3C}, // 'j'
    {0xE0,0x60,0x66,0x6C,0x78,0x6C,0xE6,0x00}, // 'k'
    {0x38,0x18,0x18,0x18,0x18,0x18,0x3C,0x00}, // 'l'
    {0x00,0x00,0xEC,0xFE,0xD6,0xD6,0xD6,0x00}, // 'm'
    {0x00,0x00,0xDC,0x66,0x66,0x66,0x66,0x00}, // 'n'
    {0x00,0x00,0x7C,0xC6,0xC6,0xC6,0x7C,0x00}, // 'o'
    {0x00,0x00,0xDC,0x66,0x66,0x7C,0x60,0xF0}, // 'p'
    {0x00,0x00,0x76,0xCC,0xCC,0x7C,0x0C,0x1E}, // 'q'
    {0x00,0x00,0xDC,0x76,0x60,0x60,0xF0,0x00}, // 'r'
    {0x00,0x00,0x7E,0xC0,0x7C,0x06,0xFC,0x00}, // 's'
    {0x30,0x30,0xFC,0x30,0x30,0x36,0x1C,0x00}, // 't'
    {0x00,0x00,0xCC,0xCC,0xCC,0xCC,0x76,0x00}, // 'u'
    {0x00,0x00,0xC6,0xC6,0xC6,0x6C,0x38,0x00}, // 'v'
    {0x00,0x00,0xC6,0xD6,0xD6,0xFE,0x6C,0x00}, // 'w'
    {0x00,0x00,0xC6,0x6C,0x38,0x6C,0xC6,0x00}, // 'x'
    {0x00,0x00,0xC6,0xC6,0xC6,0x7E,0x06,0xFC}, // 'y'
    {0x00,0x00,0xFE,0x8C,0x18,0x32,0xFE,0x00}, // 'z'
    {0x0E,0x18,0x18,0x70,0x18,0x18,0x0E,0x00}, // '{'
    {0x18,0x18,0x18,0x00,0x18,0x18,0x18,0x00}, // '|'
    {0x70,0x18,0x18,0x0E,0x18,0x18,0x70,0x00}, // '}'
    {0x76,0xDC,0x00,0x00,0x00,0x00,0x00,0x00}, // '~'
};

static void drawHudString2x(volatile uint32_t *fb, uint32_t fbW, uint32_t fbH,
                            uint32_t x0, uint32_t y0, const char *str,
                            uint32_t fgColor, uint32_t bgColor, uint32_t padCols) {
    const uint32_t scale = 2u;
    const uint32_t glyphW = 8u * scale;
    for (uint32_t col = 0; col < padCols; ++col) {
        const char ch = (str && *str) ? *str++ : ' ';
        const unsigned idx = (ch >= 0x20 && ch <= 0x7E) ? static_cast<unsigned>(ch - 0x20) : 0u;
        const uint8_t *rows = kHudFont8x8[idx];
        const uint32_t gx = x0 + col * glyphW;
        if (gx + glyphW > fbW) break;
        for (uint32_t ry = 0; ry < 8u; ++ry) {
            const uint8_t bits = rows[ry];
            for (uint32_t sy = 0; sy < scale; ++sy) {
                const uint32_t py = y0 + ry * scale + sy;
                if (py >= fbH) continue;
                volatile uint32_t *line = fb + py * fbW + gx;
                for (uint32_t rx = 0; rx < 8u; ++rx) {
                    const uint32_t c = (bits & (0x80u >> rx)) ? fgColor : bgColor;
                    for (uint32_t sx = 0; sx < scale; ++sx) {
                        line[rx * scale + sx] = c;
                    }
                }
            }
        }
    }
}

static void renderHudOverlay() {
    if (!&native_legacy_gop_virtual || !native_legacy_gop_virtual) return;
    if (&native_gop_gpu_completions && native_gop_gpu_completions > 0u) return;
    const uint32_t w = (&native_legacy_gop_width && native_legacy_gop_width) ? native_legacy_gop_width : 2560u;
    const uint32_t h = (&native_legacy_gop_height && native_legacy_gop_height) ? native_legacy_gop_height : 1600u;
    if (w < 640u || h < 80u) return;
    volatile uint32_t *fb = reinterpret_cast<volatile uint32_t *>(static_cast<uintptr_t>(native_legacy_gop_virtual));
    drawHudString2x(fb, w, h, 16u, 12u, gUsbHudLine0, 0xFF00FFCCu, 0xFF0C141Eu, 96u);
    drawHudString2x(fb, w, h, 16u, 32u, gUsbHudLine1, 0xFFFFEE55u, 0xFF0C141Eu, 96u);
}

static void logStage(const char *msg) {
    if (msg) {
        strlcpy(gUsbHudLine0, msg, sizeof(gUsbHudLine0));
        renderHudOverlay();
    }
    if (&boot_progress_stage && boot_progress_stage) {
        boot_progress_stage(msg);
    }
}

static int epCmd(UsbHw &hw, unsigned dep, uint32_t cmd, uint32_t p0, uint32_t p1, uint32_t p2, uint32_t *rawOut = nullptr) {
    const uint32_t cfg = hw.rd(DWC3_GUSB2PHYCFG(0));
    const uint32_t drop = cfg & (DWC3_GUSB2PHYCFG_SUSPHY | DWC3_GUSB2PHYCFG_ENBLSLPM);
    if (drop) hw.wr(DWC3_GUSB2PHYCFG(0), cfg & ~drop);
    hw.wr(DWC3_DEPCMDPAR0(dep), p0);
    hw.wr(DWC3_DEPCMDPAR1(dep), p1);
    hw.wr(DWC3_DEPCMDPAR2(dep), p2);
    hw.wr(DWC3_DEPCMD(dep), cmd | DWC3_DEPCMD_CMDACT);
    int st = -1;
    for (unsigned i = 0; i < 20000; ++i) {
        const uint32_t v = hw.rd(DWC3_DEPCMD(dep));
        if (!(v & DWC3_DEPCMD_CMDACT)) {
            if (rawOut) *rawOut = v;
            st = static_cast<int>(DWC3_DEPCMD_STATUS(v));
            break;
        }
        IODelay(1);
    }
    if (drop) hw.wr(DWC3_GUSB2PHYCFG(0), cfg);
    hw.lastCmdSt = st;
    return st;
}

static void endDepTransfer(UsbHw &hw, unsigned dep, uint32_t &rscIdx, bool &busyFlag) {
    if (rscIdx != 0u) {
        (void)epCmd(hw, dep,
                    DWC3_DEPCMD_ENDTRANSFER | DWC3_DEPCMD_HIPRI_FORCERM |
                    DWC3_DEPCMD_CMDIOC | DWC3_DEPCMD_PARAM(rscIdx),
                    0, 0, 0);
        IODelay(100);
        rscIdx = 0u;
    }
    busyFlag = false;
}

static volatile uint32_t *ep0TrbPtr(UsbHw &hw, unsigned slot = 0) {
    return reinterpret_cast<volatile uint32_t *>(hw.dmaVirt + DMA_EP0_TRB_OFF + slot * 16u);
}

static void ep0SetTrb(UsbHw &hw, unsigned slot, uint64_t pa, uint32_t sz, uint32_t type, bool last) {
    volatile uint32_t *t = ep0TrbPtr(hw, slot);
    t[0] = static_cast<uint32_t>(pa);
    t[1] = static_cast<uint32_t>(pa >> 32);
    t[2] = sz;
    t[3] = type | DWC3_TRB_CTRL_ISP_IMI | (last ? (DWC3_TRB_CTRL_IOC | DWC3_TRB_CTRL_LST) : DWC3_TRB_CTRL_CHN);
}

static void ep0CommitHead(UsbHw &hw) {
    __asm__ volatile("dsb sy" ::: "memory");
    volatile uint32_t *t = ep0TrbPtr(hw, 0);
    t[3] = t[3] | DWC3_TRB_CTRL_HWO;
    usbCacheRange(hw.dmaVirt + DMA_EP0_TRB_OFF, 64u, 1);
}

static void ep0Start(UsbHw &hw, unsigned dep) {
    const uint64_t pa = hw.dmaPhys + DMA_EP0_TRB_OFF;
    (void)epCmd(hw, dep, DWC3_DEPCMD_STARTTRANSFER, static_cast<uint32_t>(pa >> 32), static_cast<uint32_t>(pa), 0);
}

static void ep0ArmSetup(UsbHw &hw) {
    hw.ep0Stage = 0;
    hw.ep0Armed = 0;
    hw.ep0Trbs  = 1;
    usbCacheRange(hw.dmaVirt + DMA_EP0_BUF_OFF, 64u, 1);
    ep0SetTrb(hw, 0, hw.dmaPhys + DMA_EP0_BUF_OFF, 8u, DWC3_TRBCTL_CONTROL_SETUP, true);
    ep0CommitHead(hw);
    ep0Start(hw, 0);
}

static void ep0Restart(UsbHw &hw) {
    (void)epCmd(hw, 0, DWC3_DEPCMD_SETSTALL, 0, 0, 0);
    (void)epCmd(hw, 0, DWC3_DEPCMD_CLEARSTALL, 0, 0, 0);
    ep0ArmSetup(hw);
}

static void ep0StartData(UsbHw &hw, unsigned dep, uint32_t len) {
    hw.ep0Stage = 1;
    hw.ep0Armed = len;
    const uint64_t bufPa = hw.dmaPhys + DMA_EP0_BUF_OFF;
    if (dep == 1u) {
        usbCacheRange(hw.dmaVirt + DMA_EP0_BUF_OFF, len ? len : 64u, 0);
        if (hw.ep0Zlp) {
            hw.ep0Zlp  = false;
            hw.ep0Trbs = 2;
            ep0SetTrb(hw, 1, bufPa, 0u, DWC3_TRBCTL_CONTROL_DATA, true);
            ep0TrbPtr(hw, 1)[3] |= DWC3_TRB_CTRL_HWO;
            __asm__ volatile("dsb sy" ::: "memory");
            ep0SetTrb(hw, 0, bufPa, len, DWC3_TRBCTL_CONTROL_DATA, false);
        } else {
            hw.ep0Trbs = 1;
            ep0SetTrb(hw, 0, bufPa, len, DWC3_TRBCTL_CONTROL_DATA, true);
        }
    } else {
        usbCacheRange(hw.dmaVirt + DMA_EP0_BUF_OFF, 64u, 1);
        hw.ep0Trbs = 1;
        // Synopsys DWC3 Databook 8.2.2: EP0 Control-Data OUT TRB size must be aligned to MaxPacketSize (64B)
        ep0SetTrb(hw, 0, bufPa, 64u, DWC3_TRBCTL_CONTROL_DATA, true);
    }
    ep0CommitHead(hw);
    ep0Start(hw, dep);
}

static void ep0StartStatus(UsbHw &hw, unsigned dep) {
    hw.ep0Stage = 3;
    hw.ep0Trbs  = 1;
    ep0SetTrb(hw, 0, hw.dmaPhys + DMA_EP0_BUF_OFF, 0u,
              hw.ep0Three ? DWC3_TRBCTL_CONTROL_STATUS3 : DWC3_TRBCTL_CONTROL_STATUS2, true);
    ep0CommitHead(hw);
    ep0Start(hw, dep);
}

static void armBulkOut(UsbHw &hw) {
    if (!hw.configured || hw.ep2Armed) return;
    usbCacheRange(hw.dmaVirt + DMA_EP2_BUF_OFF, hw.bulkMps, 1);
    volatile uint32_t *t = reinterpret_cast<volatile uint32_t *>(hw.dmaVirt + DMA_EP2_TRB_OFF);
    const uint64_t bufPa = hw.dmaPhys + DMA_EP2_BUF_OFF;
    const uint64_t trbPa = hw.dmaPhys + DMA_EP2_TRB_OFF;
    t[0] = static_cast<uint32_t>(bufPa);
    t[1] = static_cast<uint32_t>(bufPa >> 32);
    t[2] = hw.bulkMps;
    t[3] = DWC3_TRBCTL_NORMAL | DWC3_TRB_CTRL_CSP | DWC3_TRB_CTRL_ISP_IMI |
           DWC3_TRB_CTRL_IOC | DWC3_TRB_CTRL_LST;
    __asm__ volatile("dsb sy" ::: "memory");
    t[3] |= DWC3_TRB_CTRL_HWO;
    usbCacheRange(hw.dmaVirt + DMA_EP2_TRB_OFF, 64u, 1);
    uint32_t raw = 0;
    if (epCmd(hw, 2, DWC3_DEPCMD_STARTTRANSFER, static_cast<uint32_t>(trbPa >> 32),
              static_cast<uint32_t>(trbPa), 0, &raw) == 0) {
        hw.ep2RscIdx = DWC3_DEPCMD_RSC_IDX(raw);
        hw.ep2Armed  = true;
    }
}

static bool startBulkIn(UsbHw &hw, const uint8_t *data, uint32_t len) {
    if (!hw.configured || hw.ep3Busy || len == 0 || len > DMA_EP3_MAX_CHUNK) return false;
    memcpy(hw.dmaVirt + DMA_EP3_BUF_OFF, data, len);
    usbCacheRange(hw.dmaVirt + DMA_EP3_BUF_OFF, len, 0);
    volatile uint32_t *t0 = reinterpret_cast<volatile uint32_t *>(hw.dmaVirt + DMA_EP3_TRB_OFF);
    const uint64_t bufPa = hw.dmaPhys + DMA_EP3_BUF_OFF;
    const uint64_t trbPa = hw.dmaPhys + DMA_EP3_TRB_OFF;
    const bool needZlp = (len % hw.bulkMps) == 0u;
    if (needZlp) {
        volatile uint32_t *t1 = t0 + 4;
        t1[0] = static_cast<uint32_t>(bufPa);
        t1[1] = static_cast<uint32_t>(bufPa >> 32);
        t1[2] = 0u;
        t1[3] = DWC3_TRBCTL_NORMAL | DWC3_TRB_CTRL_ISP_IMI | DWC3_TRB_CTRL_IOC |
                DWC3_TRB_CTRL_LST | DWC3_TRB_CTRL_HWO;
        __asm__ volatile("dsb sy" ::: "memory");
        t0[0] = static_cast<uint32_t>(bufPa);
        t0[1] = static_cast<uint32_t>(bufPa >> 32);
        t0[2] = len;
        t0[3] = DWC3_TRBCTL_NORMAL | DWC3_TRB_CTRL_ISP_IMI | DWC3_TRB_CTRL_CHN;
    } else {
        t0[0] = static_cast<uint32_t>(bufPa);
        t0[1] = static_cast<uint32_t>(bufPa >> 32);
        t0[2] = len;
        t0[3] = DWC3_TRBCTL_NORMAL | DWC3_TRB_CTRL_ISP_IMI | DWC3_TRB_CTRL_IOC | DWC3_TRB_CTRL_LST;
    }
    __asm__ volatile("dsb sy" ::: "memory");
    t0[3] |= DWC3_TRB_CTRL_HWO;
    usbCacheRange(hw.dmaVirt + DMA_EP3_TRB_OFF, 64u, 1);
    uint32_t raw = 0;
    if (epCmd(hw, 3, DWC3_DEPCMD_STARTTRANSFER, static_cast<uint32_t>(trbPa >> 32),
              static_cast<uint32_t>(trbPa), 0, &raw) == 0) {
        hw.ep3RscIdx  = DWC3_DEPCMD_RSC_IDX(raw);
        hw.ep3Busy    = true;
        hw.ep3ArmTick = nowMs();
        return true;
    }
    return false;
}

static void programEp0(UsbHw &hw) {
    (void)epCmd(hw, 0, DWC3_DEPCMD_DEPSTARTCFG, 0, 0, 0);
    const uint32_t cfg = DWC3_DEPCFG_EP_TYPE(0) | DWC3_DEPCFG_MAX_PACKET_SIZE(64) | DWC3_DEPCFG_FIFO_NUMBER(0);
    const uint32_t ev  = DWC3_DEPCFG_XFER_COMPLETE_EN | DWC3_DEPCFG_XFER_NOT_READY_EN;
    (void)epCmd(hw, 0, DWC3_DEPCMD_SETEPCONFIG, cfg, ev | DWC3_DEPCFG_EP_NUMBER(0), 0);
    (void)epCmd(hw, 0, DWC3_DEPCMD_SETTRANSFRESOURCE, DWC3_DEPXFERCFG_NUM_XFER_RES(1), 0, 0);
    (void)epCmd(hw, 1, DWC3_DEPCMD_SETEPCONFIG, cfg, ev | DWC3_DEPCFG_EP_NUMBER(1), 0);
    (void)epCmd(hw, 1, DWC3_DEPCMD_SETTRANSFRESOURCE, DWC3_DEPXFERCFG_NUM_XFER_RES(1), 0, 0);
    hw.wr(DWC3_DALEPENA, DWC3_DALEPENA_EP(0) | DWC3_DALEPENA_EP(1));
    ep0ArmSetup(hw);
}

static void enableDataEndpoints(UsbHw &hw) {
    endDepTransfer(hw, 2, hw.ep2RscIdx, hw.ep2Armed);
    endDepTransfer(hw, 3, hw.ep3RscIdx, hw.ep3Busy);
    (void)epCmd(hw, 0, DWC3_DEPCMD_DEPSTARTCFG | DWC3_DEPCMD_PARAM(2), 0, 0, 0);
    const uint32_t ev = DWC3_DEPCFG_XFER_COMPLETE_EN | DWC3_DEPCFG_XFER_IN_PROGRESS_EN;
    // dep2 = EP1 OUT (0x01), FIFO 0
    (void)epCmd(hw, 2, DWC3_DEPCMD_SETEPCONFIG,
                DWC3_DEPCFG_EP_TYPE(2) | DWC3_DEPCFG_MAX_PACKET_SIZE(hw.bulkMps) | DWC3_DEPCFG_FIFO_NUMBER(0),
                ev | DWC3_DEPCFG_EP_NUMBER(2), 0);
    (void)epCmd(hw, 2, DWC3_DEPCMD_SETTRANSFRESOURCE, DWC3_DEPXFERCFG_NUM_XFER_RES(1), 0, 0);
    // dep3 = EP1 IN (0x81), dedicated TX FIFO 1
    (void)epCmd(hw, 3, DWC3_DEPCMD_SETEPCONFIG,
                DWC3_DEPCFG_EP_TYPE(2) | DWC3_DEPCFG_MAX_PACKET_SIZE(hw.bulkMps) | DWC3_DEPCFG_FIFO_NUMBER(1),
                ev | DWC3_DEPCFG_EP_NUMBER(3), 0);
    (void)epCmd(hw, 3, DWC3_DEPCMD_SETTRANSFRESOURCE, DWC3_DEPXFERCFG_NUM_XFER_RES(1), 0, 0);
    // dep5 = EP2 IN (0x82), dedicated TX FIFO 2, bInterval=9 -> bInterval_m1=8
    (void)epCmd(hw, 5, DWC3_DEPCMD_SETEPCONFIG,
                DWC3_DEPCFG_EP_TYPE(3) | DWC3_DEPCFG_MAX_PACKET_SIZE(16) | DWC3_DEPCFG_FIFO_NUMBER(2),
                ev | (8u << 16) | DWC3_DEPCFG_EP_NUMBER(5), 0);
    (void)epCmd(hw, 5, DWC3_DEPCMD_SETTRANSFRESOURCE, DWC3_DEPXFERCFG_NUM_XFER_RES(1), 0, 0);
    hw.wr(DWC3_DALEPENA, hw.rd(DWC3_DALEPENA) |
          DWC3_DALEPENA_EP(2) | DWC3_DALEPENA_EP(3) | DWC3_DALEPENA_EP(5));
    armBulkOut(hw);
}

static unsigned buildStringDesc(unsigned idx, uint8_t *out) {
    if (idx == 0) {
        out[0] = 4; out[1] = 3; out[2] = 0x09; out[3] = 0x04;
        return 4;
    }
    if (idx >= 4) return 0;
    const char *s = kUsbStrings[idx];
    unsigned n = 0;
    while (s[n] && n < 60) ++n;
    out[0] = static_cast<uint8_t>(2 + 2 * n);
    out[1] = 3;
    for (unsigned i = 0; i < n; ++i) {
        out[2 + 2 * i] = static_cast<uint8_t>(s[i]);
        out[3 + 2 * i] = 0;
    }
    return 2 + 2 * n;
}

static void handleEp0Setup(UsbHw &hw) {
    const uint8_t *setup = hw.dmaVirt + DMA_EP0_BUF_OFF;
    const uint8_t rt   = setup[0];
    const uint8_t req  = setup[1];
    const uint16_t val = static_cast<uint16_t>(setup[2] | (setup[3] << 8));
    const uint16_t len = static_cast<uint16_t>(setup[6] | (setup[7] << 8));

    hw.ep0Three     = (len != 0);
    hw.ep0In        = hw.ep0Three && ((rt & 0x80u) != 0u);
    hw.ep0Zlp       = false;
    hw.ep0StatusDep = hw.ep0In ? 0u : 1u;

    uint8_t *out = hw.dmaVirt + DMA_EP0_BUF_OFF;
    unsigned resp = 0;
    bool stall = false;

    if ((rt & 0x60u) == 0u) {
        switch (req) {
        case 0x00:  // GET_STATUS
            out[0] = 0; out[1] = 0; resp = 2; break;
        case 0x01:  // CLEAR_FEATURE
        case 0x03:  // SET_FEATURE
        case 0x0B:  // SET_INTERFACE
            break;
        case 0x05:  // SET_ADDRESS
            hw.wr(DWC3_DCFG, (hw.rd(DWC3_DCFG) & ~DWC3_DCFG_DEVADDR_MASK) | DWC3_DCFG_DEVADDR(val & 0x7Fu));
            break;
        case 0x06: {  // GET_DESCRIPTOR
            const unsigned dtype = val >> 8;
            if (dtype == 1u) {
                memcpy(out, kUsbCdcDeviceDesc, sizeof(kUsbCdcDeviceDesc));
                resp = sizeof(kUsbCdcDeviceDesc);
            } else if (dtype == 2u) {
                resp = usb_cdc_build_config_desc(out, hw.bulkMps);
            } else if (dtype == 3u) {
                resp = buildStringDesc(val & 0xFFu, out);
                if (!resp) stall = true;
            } else if (dtype == 6u) {  // DEVICE_QUALIFIER
                static const uint8_t kQual[10] = {10, 6, 0x00, 0x02, 0x02, 0x02, 0x00, 64, 1, 0};
                memcpy(out, kQual, 10);
                resp = 10;
            } else {
                stall = true;
            }
            break;
        }
        case 0x08:  // GET_CONFIGURATION
            out[0] = hw.configured; resp = 1; break;
        case 0x09:  // SET_CONFIGURATION
            hw.configured = static_cast<uint8_t>(val & 0xFFu);
            if (hw.configured) {
                ++hw.usbCfgCount;
                if (hw.usbCfgCount == 1u) {
                    logStage("usb: host SET_CONFIGURATION=1 complete");
                }
                enableDataEndpoints(hw);
            }
            break;
        default:
            stall = true; break;
        }
    } else if ((rt & 0x60u) == 0x20u) {
        if (req == 0x21u) {  // GET_LINE_CODING
            memcpy(out, hw.lineCoding, 7);
            resp = 7;
        } else if (req == 0x20u) {  // SET_LINE_CODING (7B OUT stage)
            resp = len > 64u ? 64u : len;
        } else if (req == 0x22u) {  // SET_CONTROL_LINE_STATE
            const bool prevDtr = (hw.lineState & 1u) != 0u;
            hw.lineState = val;
            if (val & 1u) {
                hw.hostReady = true;
                if (!prevDtr) hw.needReplay = true;
            } else {
                hw.hostReady = false;
            }
        } else if (req == 0x23u) {  // SEND_BREAK
        } else {
            stall = true;
        }
    } else {
        stall = true;
    }

    if (stall) {
        ep0Restart(hw);
        return;
    }
    if (!hw.ep0Three) {
        hw.ep0Stage = 2;
        return;
    }
    if (hw.ep0In) {
        if (resp > len) resp = len;
        hw.ep0Zlp = (resp != 0u) && (resp < len) && ((resp % 64u) == 0u);
        ep0StartData(hw, 1u, resp);
    } else {
        ep0StartData(hw, 0u, len > 64u ? 64u : len);
    }
}

struct TerminalSession {
    UsbHw hw;
    UsbReplyRing reply;
    UsbScreenState screen;
    IOBufferMemoryDescriptor *replyMem = nullptr;
    IOBufferMemoryDescriptor *rgbMem   = nullptr;
    OSSharedPtr<IOMemoryMap> ramoopsMap;
    volatile const uint8_t *ramoops    = nullptr;
    uint32_t lastRamoopsPos            = 0;
    bool pendingFastboot               = false;
    bool pendingXdci                   = false;
    uint32_t pendingXdciSec            = 0;
    char cmdLine[256]                  = {};
    unsigned cmdLen                    = 0;
};

static void replayRamoops(TerminalSession &s, bool currentBootOnly) {
    if (!s.ramoops) return;
    volatile const uint32_t *hdr = reinterpret_cast<volatile const uint32_t *>(s.ramoops);
    const uint32_t sig = hdr[0], start = hdr[1], size = hdr[2];
    if (sig != J716F_RAMOOPS_SIG || start >= J716F_RAMOOPS_DATA || size > J716F_RAMOOPS_DATA) return;
    s.lastRamoopsPos = start;
    if (size == 0u) return;

    const volatile uint8_t *ring = s.ramoops + J716F_RAMOOPS_HEAD;
    const uint32_t oldest = (size < J716F_RAMOOPS_DATA) ? 0u : start;
    uint32_t beginOff = 0u;
    if (currentBootOnly) {
        static const char kMarker[] = "boot_progress_init";
        const uint32_t mLen = sizeof(kMarker) - 1u;
        for (uint32_t pos = size; pos >= mLen; --pos) {
            const uint32_t base = pos - mLen;
            bool match = true;
            for (uint32_t k = 0; k < mLen; ++k) {
                if (ring[(oldest + base + k) % J716F_RAMOOPS_DATA] != static_cast<uint8_t>(kMarker[k])) {
                    match = false; break;
                }
            }
            if (match) {
                beginOff = base > 6u ? base - 6u : 0u;
                break;
            }
            if (pos == mLen) break;
        }
    }
    const uint32_t toSend = size - beginOff;
    if (toSend > s.reply.freeBytes() - 256u) {
        beginOff = size - (s.reply.freeBytes() - 512u);
    }
    s.reply.writeStr("\r\n[usb] === TB-J716F ramoops replay begin ===\r\n");
    for (uint32_t i = beginOff; i < size; ++i) {
        const uint8_t ch = ring[(oldest + i) % J716F_RAMOOPS_DATA];
        (void)s.reply.write(&ch, 1u);
    }
    s.reply.writeStr("\r\n[usb] === TB-J716F ramoops replay end ===\r\n");
}

static void handleCommand(TerminalSession &s, const char *raw) {
    while (*raw == ' ' || *raw == '\t') ++raw;
    if (!*raw) return;
    char buf[512];

    if (strEq(raw, "ping")) {
        s.reply.writeStr("[usb] PONG TB-J716F\r\n");
    } else if (strEq(raw, "features")) {
        s.reply.writeStr("FEATURES: ping status info uptime bootargs display usb touch gpu.status wifi.status bt.status replay screen.snapshot screen.start screen.stop screen.configure reboot.fastboot\r\n");
    } else if (strEq(raw, "help")) {
        s.reply.writeStr("Commands: ping features info uptime bootargs status display usb gpu.status wifi bt input.status replay screen.snapshot screen.start screen.stop screen.configure reboot.fastboot\r\n");
    } else if (strEq(raw, "info") || strEq(raw, "uptime") || strEq(raw, "status")) {
        const uint32_t cpus = &armcpu_smp_running ? armcpu_smp_running : 0u;
        const uint64_t ms = nowMs();
        snprintf(buf, sizeof(buf),
                 "[usb] board=TB-J716F soc=SM8250 uptime=%llu.%03llus cpus=%u usb_events=%u resets=%u\r\n",
                 ms / 1000ULL, ms % 1000ULL, cpus, s.hw.usbEvents, s.hw.usbResets);
        s.reply.writeStr(buf);
        if (&native_touch_stage) {
            snprintf(buf, sizeof(buf), "[usb] TOUCH: stage=%u strokes=%u %s\r\n",
                     native_touch_stage, &native_touch_stroke_count ? native_touch_stroke_count : 0u,
                     &native_touch_rows ? native_touch_rows[0] : "");
            s.reply.writeStr(buf);
        }
    } else if (strEq(raw, "bootargs")) {
        snprintf(buf, sizeof(buf), "[usb] bootargs=%s\r\n", PE_boot_args());
        s.reply.writeStr(buf);
    } else if (strEq(raw, "display")) {
        snprintf(buf, sizeof(buf),
                 "[usb] FB: stage=%u %ux%u phys=0x%llx virt=0x%llx xfer=%u retired=%u gpu_offers=%u gpu_completions=%u\r\n",
                 &native_legacy_stage ? native_legacy_stage : 0u,
                 &native_legacy_gop_width ? native_legacy_gop_width : 0u,
                 &native_legacy_gop_height ? native_legacy_gop_height : 0u,
                 &native_legacy_gop_physical ? (unsigned long long)native_legacy_gop_physical : 0ULL,
                 &native_legacy_gop_virtual ? (unsigned long long)native_legacy_gop_virtual : 0ULL,
                 &native_legacy_transfer_calls ? native_legacy_transfer_calls : 0u,
                 &native_legacy_retirement_calls ? native_legacy_retirement_calls : 0u,
                 &native_gop_gpu_offers ? native_gop_gpu_offers : 0u,
                 &native_gop_gpu_completions ? native_gop_gpu_completions : 0u);
        s.reply.writeStr(buf);
        if (&native_backlight_stats) {
            snprintf(buf, sizeof(buf),
                     "[usb] BACKLIGHT: probe=%d/%d stage=%u calls=%u level=%u/%u scale=%u/256 fade=%ums ramp=%u/%u\r\n",
                     &sheng_backlight_probe_stage ? sheng_backlight_probe_stage : 0,
                     &sheng_backlight_probe_result ? sheng_backlight_probe_result : 0,
                     native_backlight_stats[0], native_backlight_stats[1],
                     native_backlight_stats[2], native_backlight_stats[3],
                     &native_backlight_scale ? native_backlight_scale : 256u,
                     native_backlight_stats[5], native_backlight_stats[6], native_backlight_stats[7]);
            s.reply.writeStr(buf);
        }
        if (&native_mdss_rows) {
            for (unsigned i = 0; i < 31; ++i) {
                if (!native_mdss_rows[i][0]) continue;
                snprintf(buf, sizeof(buf), "[usb] MDSS[%u]: %s\r\n", i, native_mdss_rows[i]);
                s.reply.writeStr(buf);
            }
        }
        if (&native_flip_rows) {
            for (unsigned i = 0; i < 16; ++i) {
                if (!native_flip_rows[i][0]) continue;
                snprintf(buf, sizeof(buf), "[usb] FLIP[%u]: %s\r\n", i, native_flip_rows[i]);
                s.reply.writeStr(buf);
            }
        }
    } else if (strEq(raw, "mdss") || strEq(raw, "mdss.status")) {
        snprintf(buf, sizeof(buf), "[usb] MDSS: step=%u\r\n", &native_mdss_step ? native_mdss_step : 0u);
        s.reply.writeStr(buf);
        if (&native_mdss_rows) {
            for (unsigned i = 0; i < 31; ++i) {
                if (!native_mdss_rows[i][0]) continue;
                snprintf(buf, sizeof(buf), "[usb] MDSS[%u]: %s\r\n", i, native_mdss_rows[i]);
                s.reply.writeStr(buf);
            }
        }
    } else if (strEq(raw, "backlight") || strEq(raw, "backlight.status")) {
        if (&native_backlight_stats) {
            snprintf(buf, sizeof(buf),
                     "[usb] BACKLIGHT: probe=%d/%d stage=%u calls=%u level=%u/%u scale=%u/256 fade=%ums ramp=%u/%u\r\n",
                     &sheng_backlight_probe_stage ? sheng_backlight_probe_stage : 0,
                     &sheng_backlight_probe_result ? sheng_backlight_probe_result : 0,
                     native_backlight_stats[0], native_backlight_stats[1],
                     native_backlight_stats[2], native_backlight_stats[3],
                     &native_backlight_scale ? native_backlight_scale : 256u,
                     native_backlight_stats[5], native_backlight_stats[6], native_backlight_stats[7]);
            s.reply.writeStr(buf);
        } else {
            s.reply.writeStr("[usb] BACKLIGHT: unavailable\r\n");
        }
    } else if (strEq(raw, "touch") || strEq(raw, "touch.status")) {
        snprintf(buf, sizeof(buf), "[usb] TOUCH: stage=%u strokes=%u\r\n",
                 &native_touch_stage ? native_touch_stage : 0u,
                 &native_touch_stroke_count ? native_touch_stroke_count : 0u);
        s.reply.writeStr(buf);
        if (&native_touch_rows) {
            for (unsigned i = 0; i < 4; ++i) {
                if (!native_touch_rows[i][0]) continue;
                snprintf(buf, sizeof(buf), "[usb] TOUCH[%u]: %s\r\n", i, native_touch_rows[i]);
                s.reply.writeStr(buf);
            }
        }
    } else if (strEq(raw, "battery") || strEq(raw, "battery.status")) {
        snprintf(buf, sizeof(buf), "[usb] BATTERY: pub=%u err=%d %s\r\n",
                 &native_battery_published ? native_battery_published : 0u,
                 &native_battery_error ? native_battery_error : 0,
                 (&native_battery_row && native_battery_row[0]) ? native_battery_row : "(empty)");
        s.reply.writeStr(buf);
    } else if (strEq(raw, "usb") || strEq(raw, "usb.status")) {
        snprintf(buf, sizeof(buf),
                 "[usb] USB: cfg=%u mps=%u dtr=%u dsts=0x%08x gctl=0x%08x dctl=0x%08x phy2=0x%08x qhs=0x%08x qss=0x%08x events=%u resets=%u\r\n",
                 s.hw.configured, s.hw.bulkMps, s.hw.lineState & 1u,
                 s.hw.rd(DWC3_DSTS), s.hw.rd(DWC3_GCTL), s.hw.rd(DWC3_DCTL),
                 s.hw.rd(DWC3_GUSB2PHYCFG(0)), s.hw.rd(QSCRATCH_HS_PHY_CTRL),
                 s.hw.rd(QSCRATCH_SS_PHY_CTRL),
                 s.hw.usbEvents, s.hw.usbResets);
        s.reply.writeStr(buf);
        if (&native_usb_stats) {
            snprintf(buf, sizeof(buf),
                     "[usb] XDCI: stage=%d gsnpsid=0x%08x smmu=0x%x fsr=0x%x pwrOn=%u pwrOff=%u rst=0x%x start=%u goOnBus=0x%x kext=0x%x irqs=%u polls=%u dsts=0x%08x dalep=0x%08x evcnt=0x%x evsiz=0x%x ep0cmd=0x%x dcfg=0x%08x gctl=0x%08x usb2=0x%08x\r\n",
                     static_cast<int>(native_usb_stats[0]), native_usb_stats[1],
                     native_usb_stats[7], native_usb_stats[25],
                     native_usb_stats[12], native_usb_stats[13], native_usb_stats[14],
                     native_usb_stats[19], native_usb_stats[20], native_usb_stats[21],
                     native_usb_stats[11], native_usb_stats[30],
                     native_usb_stats[26], native_usb_stats[27], native_usb_stats[28],
                     native_usb_stats[31], native_usb_stats[29],
                     native_usb_stats[16], native_usb_stats[17], native_usb_stats[18]);
            s.reply.writeStr(buf);
            snprintf(buf, sizeof(buf),
                     "[usb] XDCI-EV: total=%u setup=%08x_%08x ev=[%08x %08x %08x %08x %08x %08x %08x %08x]\r\n",
                     native_usb_stats[40], native_usb_stats[41], native_usb_stats[42],
                     native_usb_stats[32], native_usb_stats[33], native_usb_stats[34], native_usb_stats[35],
                     native_usb_stats[36], native_usb_stats[37], native_usb_stats[38], native_usb_stats[39]);
            s.reply.writeStr(buf);
        }
    } else if (strEq(raw, "usb.xdci")) {
        s.screen.streaming = false;
        s.pendingXdci      = true;
        s.pendingXdciSec   = 0u;
        s.reply.writeStr("[usb] handing off DWC3 controller to AppleUSBXDCI (QcomDwc3Xdci) permanently\r\n");
    } else if (strEq(raw, "usb.xdci.30")) {
        s.screen.streaming = false;
        s.pendingXdci      = true;
        s.pendingXdciSec   = 30u;
        s.reply.writeStr("[usb] handing off DWC3 controller to AppleUSBXDCI (QcomDwc3Xdci) for 30s\r\n");
    } else if (strEq(raw, "gpu") || strEq(raw, "gpu.status") || strEq(raw, "gpu.observe")) {
        snprintf(buf, sizeof(buf), "[usb] GPU: step=%u\r\n", &native_gpu_step ? native_gpu_step : 0u);
        s.reply.writeStr(buf);
        if (&native_gpu_rows) {
            for (unsigned i = 0; i < 31; ++i) {
                if (!native_gpu_rows[i][0]) continue;
                snprintf(buf, sizeof(buf), "[usb] GPU[%u]: %s\r\n", i, native_gpu_rows[i]);
                s.reply.writeStr(buf);
            }
        }
    } else if (strEq(raw, "wifi") || strEq(raw, "wifi.status") || strEq(raw, "mhi")) {
        if (&native_wifi_stats) {
            snprintf(buf, sizeof(buf),
                     "[usb] WIFI: stage=%d vote8=0x%x gdsc=0x%x halt=0x%x gpio=0x%x pwr=0x%x pcs=0x%x ltssm=0x%x rp=0x%08x ep=0x%08x link_ms=%u\r\n",
                     (int)native_wifi_stats[0], native_wifi_stats[1], native_wifi_stats[3],
                     native_wifi_stats[4], native_wifi_stats[10], native_wifi_stats[11],
                     native_wifi_stats[12], native_wifi_stats[13],
                     native_wifi_stats[7], native_wifi_stats[15], native_wifi_stats[16]);
            s.reply.writeStr(buf);
        }
        if (&native_wifi_ep && &native_wifi_mhi) {
            snprintf(buf, sizeof(buf),
                     "[usb] MHI: ep_stage=%u bar0=0x%08x mhi_step=%d smmu=0x%08x amss=%u segs=%u phase=0x%06x ee=%u state=%u bhi_st=0x%x bhi_err=0x%x bhie_st=0x%x mhi_st=0x%08x ms=%u\r\n",
                     native_wifi_ep[0], native_wifi_ep[2], (int)native_wifi_mhi[0],
                     native_wifi_mhi[1], native_wifi_mhi[3], native_wifi_mhi[4],
                     native_wifi_mhi[5], native_wifi_mhi[6], native_wifi_mhi[7],
                     native_wifi_mhi[8], native_wifi_mhi[9], native_wifi_mhi[15],
                     native_wifi_mhi[13], native_wifi_mhi[14]);
            s.reply.writeStr(buf);
        }
        if (&native_wifi_note && &native_wifi_note_n) {
            const uint32_t total = native_wifi_note_n;
            const uint32_t count = total < 16u ? total : 16u;
            for (uint32_t i = total - count; i < total; ++i) {
                const char *line = native_wifi_note[i % 128u];
                if (!line[0]) continue;
                snprintf(buf, sizeof(buf), "[usb] WIFI_NOTE[%u]: %s\r\n", i, line);
                s.reply.writeStr(buf);
            }
        }
    } else if (strEq(raw, "bt") || strEq(raw, "bt.status") || strEq(raw, "bluetooth")) {
        snprintf(buf, sizeof(buf), "[usb] BT: step=%u\r\n", &native_bt_step ? native_bt_step : 0u);
        s.reply.writeStr(buf);
        if (&native_bt_stats && native_bt_stats) {
            const char *st = native_bt_stats();
            if (st && st[0]) {
                s.reply.writeStr(st);
                s.reply.writeStr("\r\n");
            }
        }
        if (&native_bt_uart && native_bt_uart) {
            const char *ua = native_bt_uart();
            if (ua && ua[0]) {
                s.reply.writeStr(ua);
                s.reply.writeStr("\r\n");
            }
        }
        if (&native_bt_rows) {
            for (unsigned i = 0; i < 16; ++i) {
                if (!native_bt_rows[i][0]) continue;
                snprintf(buf, sizeof(buf), "[usb] BT[%u]: %s\r\n", i, native_bt_rows[i]);
                s.reply.writeStr(buf);
            }
        }
    } else if (strEq(raw, "input.status")) {
        s.reply.writeStr("GIULIA INPUT: available=0 touch=0 keyboard=0 buttons=0\r\n");
    } else if (strEq(raw, "replay")) {
        replayRamoops(s, false);
    } else if (strEq(raw, "screen.snapshot")) {
        (void)usbCaptureAndQueueGsc1(s.screen, s.reply, true);
    } else if (strEq(raw, "screen.start")) {
        s.screen.streaming = true;
        s.screen.nextFrameTick = 0;
        s.reply.writeStr("GIULIA SCREEN: streaming enabled\r\n");
    } else if (strEq(raw, "screen.stop")) {
        s.screen.streaming = false;
        usbQueueGsc1Stop(s.screen, s.reply);
        s.reply.writeStr("GIULIA SCREEN: streaming stopped\r\n");
    } else if (strStarts(raw, "screen.configure")) {
        const char *p = raw + 16;
        bool ok1 = false, ok2 = false;
        const uint32_t edge = parseUint(p, &ok1);
        const uint32_t fps  = parseUint(p, &ok2);
        if (ok1 && edge >= 160u && edge <= 2780u) s.screen.maxEdge = edge;
        if (ok2 && fps >= 1u && fps <= 30u) s.screen.maxFps = fps;
        snprintf(buf, sizeof(buf), "GIULIA SCREEN: configured max_size=%u max_fps=%u\r\n",
                 s.screen.maxEdge, s.screen.maxFps);
        s.reply.writeStr(buf);
    } else if (strEq(raw, "reboot.status")) {
        snprintf(buf, sizeof(buf), "GIULIA REBOOT: pending=%u\r\n", s.pendingFastboot ? 1u : 0u);
        s.reply.writeStr(buf);
    } else if (strEq(raw, "reboot.fastboot")) {
        s.screen.streaming = false;
        s.hw.needReplay    = false;
        s.reply.reset();
        s.pendingFastboot  = true;
        logStage("usb: reboot.fastboot requested by host");
        s.reply.writeStr("GIULIA REBOOT: resetting target=fastboot disks=shutdown-complete\r\n");
    } else {
        snprintf(buf, sizeof(buf), "[usb] unsupported command: %s\r\n", raw);
        s.reply.writeStr(buf);
    }
}

static void handleDepEvent(TerminalSession &s, uint32_t ev) {
    UsbHw &hw = s.hw;
    const unsigned dep    = (ev >> 1) & 0x1Fu;
    const unsigned kind   = (ev >> 6) & 0x0Fu;
    const unsigned status = (ev >> 12) & 0x0Fu;

    if (dep <= 1u) {
        if (kind == DWC3_DEPEVT_XFERNOTREADY) {
            if (status == DEPEVT_STATUS_CONTROL_STATUS && (hw.ep0Stage == 2 || hw.ep0Stage == 1)) {
                ep0StartStatus(hw, dep <= 1u ? dep : hw.ep0StatusDep);
            }
            return;
        }
        if (kind != DWC3_DEPEVT_XFERCOMPLETE) return;
        if (hw.ep0Stage == 0) {
            usbCacheRange(hw.dmaVirt + DMA_EP0_TRB_OFF, 64u, 2);
            usbCacheRange(hw.dmaVirt + DMA_EP0_BUF_OFF, 64u, 2);
            handleEp0Setup(hw);
        } else if (hw.ep0Stage == 1) {
            usbCacheRange(hw.dmaVirt + DMA_EP0_TRB_OFF, 64u, 2);
            bool setupPending = false;
            for (unsigned i = 0; i < hw.ep0Trbs; ++i) {
                if (DWC3_TRB_SIZE_TRBSTS(ep0TrbPtr(hw, i)[2]) == DWC3_TRBSTS_SETUP_PENDING) {
                    setupPending = true;
                }
            }
            if (!hw.ep0In && hw.ep0Armed == 7u) {
                usbCacheRange(hw.dmaVirt + DMA_EP0_BUF_OFF, 64u, 2);
                memcpy(hw.lineCoding, hw.dmaVirt + DMA_EP0_BUF_OFF, 7);
            }
            if (setupPending) {
                ep0ArmSetup(hw);
                return;
            }
            hw.ep0Stage = 2;
        } else if (hw.ep0Stage == 3) {
            ep0ArmSetup(hw);
        }
        return;
    }

    if (dep == 2u && kind == DWC3_DEPEVT_XFERCOMPLETE) {
        hw.ep2Armed = false;
        hw.ep2RscIdx = 0u;
        usbCacheRange(hw.dmaVirt + DMA_EP2_TRB_OFF, 64u, 2);
        usbCacheRange(hw.dmaVirt + DMA_EP2_BUF_OFF, hw.bulkMps, 2);
        volatile uint32_t *t = reinterpret_cast<volatile uint32_t *>(hw.dmaVirt + DMA_EP2_TRB_OFF);
        const uint32_t rem = t[2] & DWC3_TRB_SIZE_MASK;
        const uint32_t got = (rem <= hw.bulkMps) ? (hw.bulkMps - rem) : 0u;
        const uint8_t *rx = hw.dmaVirt + DMA_EP2_BUF_OFF;
        hw.hostReady = true;
        for (uint32_t i = 0; i < got; ++i) {
            const char c = static_cast<char>(rx[i]);
            if (c == '\r' || c == '\n') {
                if (s.cmdLen > 0) {
                    s.cmdLine[s.cmdLen] = '\0';
                    handleCommand(s, s.cmdLine);
                    s.cmdLen = 0;
                }
            } else if (c >= 0x20 && c < 0x7F && s.cmdLen + 1u < sizeof(s.cmdLine)) {
                s.cmdLine[s.cmdLen++] = c;
            }
        }
        armBulkOut(hw);
        return;
    }

    if (dep == 3u && kind == DWC3_DEPEVT_XFERCOMPLETE) {
        hw.ep3Busy   = false;
        hw.ep3RscIdx = 0u;
        return;
    }
}

static void pollUsbEvents(TerminalSession &s) {
    UsbHw &hw = s.hw;
    volatile uint32_t *ring = reinterpret_cast<volatile uint32_t *>(hw.dmaVirt + DMA_EVT_OFF);
    for (unsigned guard = 0; guard < 64; ++guard) {
        const uint32_t count = hw.rd(DWC3_GEVNTCOUNT(0)) & 0xFFFCu;
        if (!count) break;
        if (count > 4096u) {
            hw.evtOff = (hw.evtOff + count) % 4096u;
            hw.wr(DWC3_GEVNTCOUNT(0), count);
            continue;
        }
        usbCacheRange(hw.dmaVirt + DMA_EVT_OFF, 4096u, 2);
        uint32_t left = count;
        while (left >= 4u) {
            const uint32_t ev = ring[hw.evtOff / 4u];
            ++hw.usbEvents;
            if (ev & 1u) {
                const unsigned type = (ev >> 8) & 0x0Fu;
                if (type == DWC3_DEV_EVENT_RESET) {
                    ++hw.usbResets;
                    if (hw.usbResets == 1u) {
                        logStage("usb: first host USB_RESET received");
                    }
                    endDepTransfer(hw, 2, hw.ep2RscIdx, hw.ep2Armed);
                    endDepTransfer(hw, 3, hw.ep3RscIdx, hw.ep3Busy);
                    hw.configured = 0;
                    hw.hostReady  = false;
                    hw.wr(DWC3_DCFG, hw.rd(DWC3_DCFG) & ~DWC3_DCFG_DEVADDR_MASK);
                    ep0Restart(hw);
                } else if (type == DWC3_DEV_EVENT_CONNECT_DONE) {
                    const uint32_t spd = hw.rd(DWC3_DSTS) & DWC3_DSTS_CONNECTSPD_MASK;
                    hw.bulkMps = (spd == DWC3_DCFG_HIGHSPEED) ? 512u : 64u;
                } else if (type == DWC3_DEV_EVENT_DISCONNECT) {
                    endDepTransfer(hw, 2, hw.ep2RscIdx, hw.ep2Armed);
                    endDepTransfer(hw, 3, hw.ep3RscIdx, hw.ep3Busy);
                    hw.configured = 0;
                    hw.hostReady  = false;
                }
            } else {
                handleDepEvent(s, ev);
            }
            hw.evtOff = (hw.evtOff + 4u) % 4096u;
            left -= 4u;
        }
        hw.wr(DWC3_GEVNTCOUNT(0), count);
    }
}

__attribute__((noreturn)) static void executeWarmFastbootReboot(UsbHw &hw) {
    // Cleanly drop USB pull-up so host pyserial closes immediately and enters fastboot wait
    hw.wr(DWC3_DCTL, hw.rd(DWC3_DCTL) & ~DWC3_DCTL_RUN_STOP);
    IOSleep(100);

    // PM8150 PON_SOFT_RB_SPARE (0x8F) is already programmed to PON_REASON_BOOTLOADER (0x04)
    // with warm-reset retention by prepare_warm_fastboot() in BootRamoops.c.
    // On Lenovo TB-J716F, IMEM 0x146BF65C is TZ XPU-protected and SCM_HALT_PMIC_ARB hangs;
    // PSCI_SYSTEM_RESET (0x84000009) performs a direct warm reset into Fastboot.
    __asm__ volatile("msr daifset, #3" ::: "memory");
    {
        register uint64_t x0 __asm__("x0") = J716F_PSCI_SYSTEM_RESET;
        register uint64_t x1 __asm__("x1") = 0;
        register uint64_t x2 __asm__("x2") = 0;
        register uint64_t x3 __asm__("x3") = 0;
        __asm__ volatile("smc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3)
                         : : "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11",
                             "x12", "x13", "x14", "x15", "x16", "x17", "memory");
    }
    for (;;) __asm__ volatile("wfe");
}

static bool bringUpController(TerminalSession &s) {
    UsbHw &hw = s.hw;
    if (!hw.dmaDesc) {
        hw.dmaDesc = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
            kernel_task,
            static_cast<IOOptionBits>(kIODirectionInOut) | static_cast<IOOptionBits>(kIOMemoryPhysicallyContiguous),
            DMA_TOTAL_BYTES, 0xFFFFFFF000ULL);
        if (!hw.dmaDesc || hw.dmaDesc->prepare() != kIOReturnSuccess) {
            logStage("usb: dmaDesc alloc/prepare failed");
            return false;
        }
        addr64_t segLen = 0;
        hw.dmaPhys = hw.dmaDesc->getPhysicalSegment(0, &segLen);
        hw.dmaVirt = static_cast<uint8_t *>(hw.dmaDesc->getBytesNoCopy());
        if (!hw.dmaVirt || !hw.dmaPhys || segLen < DMA_TOTAL_BYTES) {
            logStage("usb: dmaVirt/dmaPhys invalid");
            return false;
        }
    }
    memset(hw.dmaVirt, 0, DMA_TOTAL_BYTES);
    usbCacheRange(hw.dmaVirt, DMA_TOTAL_BYTES, 1);
    logStage("usb: dma cacheable buffer ready");

    if (!hw.smmu) {
        (void)usbConfigureSmmuBypass(hw);
    }
    if (!usbGccAndPhyInit(hw)) {
        logStage("usb: gcc/phy init failed");
        return false;
    }

    if (!hw.core) {
        hw.core = reinterpret_cast<volatile uint32_t *>(
            mapRangeUncached(J716F_USB0_CORE_BASE, J716F_USB0_CORE_SIZE, hw.coreMap));
        if (!hw.core) {
            logStage("usb: core map failed");
            return false;
        }
    }

    const uint32_t gsnpsid = hw.rd(DWC3_GSNPSID);
    const uint32_t sid = gsnpsid & DWC3_GSNPSID_MASK;
    if (sid != DWC3_GSNPSID_USB3 && sid != DWC3_GSNPSID_USB31 && sid != DWC3_GSNPSID_USB31_3331) {
        char msg[96];
        snprintf(msg, sizeof(msg), "usb: unexpected GSNPSID=0x%08x", gsnpsid);
        logStage(msg);
        return false;
    }

    // 1. Switch QSCRATCH_GENERAL_CFG PIPE_UTMI_CLK_SEL if QMP SS PHY pipe_clk is off OR if forced into HS-only fallback (phyInitMode == 1).
    //    During normal UEFI boot, SSUsb1InitSSPhy (0x9094) succeeds and leaves pipe_clk running with QSCRATCH_GENERAL_CFG = 0.
    const bool pipeClkOff = (hw.grd(GCC_USB3_PRIM_PHY_PIPE_CLK) & GCC_BRANCH_CLK_OFF) != 0u;
    if (pipeClkOff || hw.phyInitMode == 1u) {
        hw.wr(QSCRATCH_GENERAL_CFG, hw.rd(QSCRATCH_GENERAL_CFG) | QSCRATCH_PIPE_UTMI_CLK_DIS);
        IODelay(100);
        hw.wr(QSCRATCH_GENERAL_CFG,
              (hw.rd(QSCRATCH_GENERAL_CFG) & ~0x200u) |
              QSCRATCH_PIPE_UTMI_CLK_SEL | QSCRATCH_PIPE3_PHYSTATUS_SW);
        IODelay(100);
        hw.wr(QSCRATCH_GENERAL_CFG, hw.rd(QSCRATCH_GENERAL_CFG) & ~QSCRATCH_PIPE_UTMI_CLK_DIS);
    }
    hw.wr(QSCRATCH_CGCTL, hw.rd(QSCRATCH_CGCTL) | 0x02u);

    // 2. OEM UsbfnDwc3Dxe VBUS + power-present override (0xee48..0xee94):
    //    Set QSCRATCH_HS_PHY_CTRL bits 20 & 28 and QSCRATCH_SS_PHY_CTRL bit 24 (LANE0_PWR_PRESENT) before DwcInitialize!
    hw.wr(QSCRATCH_HS_PHY_CTRL, hw.rd(QSCRATCH_HS_PHY_CTRL) | QSCRATCH_UTMI_OTG_VBUS_VALID);
    hw.wr(QSCRATCH_HS_PHY_CTRL, hw.rd(QSCRATCH_HS_PHY_CTRL) | QSCRATCH_SW_SESSVLD_SEL);
    hw.wr(QSCRATCH_SS_PHY_CTRL, hw.rd(QSCRATCH_SS_PHY_CTRL) | QSCRATCH_LANE0_PWR_PRESENT);

    // 3. OEM UsbfnDwc3Dxe DwcInitialize (0x39cc..0x40e0): device CSFTRST, then configure DWC3 core
    hw.wr(DWC3_DCTL, (hw.rd(DWC3_DCTL) & ~DWC3_DCTL_RUN_STOP) | DWC3_DCTL_CSFTRST);
    for (unsigned i = 0; i < 100000; ++i) {
        if ((hw.rd(DWC3_DCTL) & DWC3_DCTL_CSFTRST) == 0u) break;
        IODelay(5);
    }

    hw.wr(DWC3_DCTL, (hw.rd(DWC3_DCTL) & ~(0x1F000000u | DWC3_DCTL_RUN_STOP)) | 0x10F00000u);
    hw.wr(DWC3_GSBUSCFG0, hw.rd(DWC3_GSBUSCFG0) | 0x0Eu);
    hw.wr(DWC3_GSBUSCFG1, (hw.rd(DWC3_GSBUSCFG1) & ~0xF00u) | 0xE00u);

    // Clear ENBLSLPM (bit 8) and SUSPHY (bit 6) ONLY; preserve hardware PHYIF/USBTRDTIM straps (0x3aa0..0x3ad4)
    hw.wr(DWC3_GUSB2PHYCFG(0), hw.rd(DWC3_GUSB2PHYCFG(0)) & ~DWC3_GUSB2PHYCFG_ENBLSLPM);
    hw.wr(DWC3_GUSB2PHYCFG(0), hw.rd(DWC3_GUSB2PHYCFG(0)) & ~DWC3_GUSB2PHYCFG_SUSPHY);
    // Clear ELASTIC_BUFFER_MODE (bit 0) (0x3ae0..0x3b00)
    hw.wr(DWC3_GUSB3PIPECTL(0), hw.rd(DWC3_GUSB3PIPECTL(0)) & ~1u);

    // OEM UsbfnDwc3Dxe DwcInitialize (0x3b10..0x3b7c): partition TX FIFOs 1..15
    {
        uint32_t fifoAddr = hw.rd(DWC3_GTXFIFOSIZ(0)) & 0xFFFFu;
        for (unsigned ep = 1; ep <= 15; ++ep) {
            const uint32_t depth = (ep < 7u) ? 0x0184u : 0x0082u;
            hw.wr(DWC3_GTXFIFOSIZ(ep), (fifoAddr << 16) | depth);
            fifoAddr += depth;
        }
    }

    // Exact UsbfnDwc3Dxe GCTL sequence (0x3cf0..0x3d3c): clear bit 16 (U2RSTECN), PRTCAPDIR=2 (Device), PWRDNSCALE=2
    hw.wr(DWC3_GCTL, hw.rd(DWC3_GCTL) & ~0x00010000u);
    hw.wr(DWC3_GCTL, (hw.rd(DWC3_GCTL) & ~0x00003000u) | DWC3_GCTL_PRTCAPDIR(DWC3_GCTL_PRTCAP_DEVICE));
    hw.wr(DWC3_GCTL, (hw.rd(DWC3_GCTL) & 0x0007FFFFu) | 0x00100000u);

    // Exact UsbfnDwc3Dxe DCFG + DEVTEN + DEPSTARTCFG + Event Buffer order (0x3d44..0x40e0)
    hw.wr(DWC3_DCFG,
          (hw.rd(DWC3_DCFG) & ~(DWC3_DCFG_SPEED_MASK | DWC3_DCFG_DEVADDR_MASK)) |
          (1u << 22) | DWC3_DCFG_HIGHSPEED);
    hw.wr(DWC3_DEVTEN, 0x07u);       // DISCONNEVTEN | USBRSTEN | CONNECTDONEEN

    // UsbfnDwc3Dxe 0x3dfc..0x3e58 issues DEPSTARTCFG(0) before programming GEVNTADR/SIZ/COUNT
    (void)epCmd(hw, 0, DWC3_DEPCMD_DEPSTARTCFG, 0, 0, 0);

    const uint64_t evtPa = hw.dmaPhys + DMA_EVT_OFF;
    hw.wr(DWC3_GEVNTADRLO(0), static_cast<uint32_t>(evtPa));
    hw.wr(DWC3_GEVNTADRHI(0), static_cast<uint32_t>(evtPa >> 32));
    hw.wr(DWC3_GEVNTSIZ(0), 4096u);  // bit 31 clear: event generation unmasked
    hw.wr(DWC3_GEVNTCOUNT(0), 0u);
    hw.evtOff = 0u;

    programEp0(hw);

    // Hold SE0 disconnect cleanly for 30ms before asserting D+ pull-up via RUN_STOP (0x6480..0x64ac)
    IOSleep(30);
    hw.wr(DWC3_DCTL, hw.rd(DWC3_DCTL) | DWC3_DCTL_RUN_STOP);
    {
        char msg[128];
        snprintf(msg, sizeof(msg),
                 "usb: DWC3 CDC-ACM running gsnpsid=0x%08x dsts=0x%08x qss=0x%08x",
                 gsnpsid, hw.rd(DWC3_DSTS), hw.rd(QSCRATCH_SS_PHY_CTRL));
        logStage(msg);
    }
    return true;
}

static TerminalSession *gSession = nullptr;

static void usbDebugThread(void *arg, int waitResult) {
    (void)arg;
    (void)waitResult;
    IOSleep(200);
    logStage("usb: debug thread started");

    void *rawMem = IOMalloc(sizeof(TerminalSession));
    if (!rawMem) return;
    memset(rawMem, 0, sizeof(TerminalSession));
    TerminalSession *s = new (rawMem, UsbPlaceTag{}) TerminalSession();
    s->hw.initDefaults();

    const uint32_t kReplyBytes = 3u * 1024u * 1024u;
    const uint32_t kRgbBytes   = 2u * 1024u * 1024u;
    s->replyMem = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, kIODirectionInOut, kReplyBytes, 4096);
    s->rgbMem   = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, kIODirectionInOut, kRgbBytes, 4096);
    if (!s->replyMem || !s->rgbMem ||
        s->replyMem->prepare() != kIOReturnSuccess ||
        s->rgbMem->prepare() != kIOReturnSuccess) {
        logStage("usb: reply/rgb buffer alloc failed");
        return;
    }

    s->reply.buf = static_cast<uint8_t *>(s->replyMem->getBytesNoCopy());
    s->reply.cap = kReplyBytes;
    s->reply.reset();
    s->screen.init(static_cast<uint8_t *>(s->rgbMem->getBytesNoCopy()), kRgbBytes);
    s->ramoops = reinterpret_cast<volatile const uint8_t *>(
        mapRangeUncached(J716F_RAMOOPS_BASE, J716F_RAMOOPS_SIZE, s->ramoopsMap));

    if (!bringUpController(*s)) return;
    gSession = s;

    uint8_t *txScratch = static_cast<uint8_t *>(IOMalloc(DMA_EP3_MAX_CHUNK));
    if (!txScratch) return;

    uint64_t bootTick = nowMs();
    uint64_t hudTick  = 0;
    unsigned replugs  = 0;

    for (;;) {
        pollUsbEvents(*s);

        const uint64_t now = nowMs();
        if (now >= hudTick) {
            hudTick = now + 250ULL;
            const uint32_t dsts = s->hw.rd(DWC3_DSTS);
            const uint32_t dctl = s->hw.rd(DWC3_DCTL);
            const uint32_t gctl = s->hw.rd(DWC3_GCTL);
            const uint32_t qcfg = s->hw.rd(QSCRATCH_GENERAL_CFG);
            const uint32_t qhs  = s->hw.rd(QSCRATCH_HS_PHY_CTRL);
            const uint32_t pclk = s->hw.grd(GCC_USB3_PRIM_PHY_PIPE_CLK);
            const uint32_t gcnt = s->hw.rd(DWC3_GEVNTCOUNT(0));
            snprintf(gUsbHudLine0, sizeof(gUsbHudLine0),
                     "USB: ev=%u rst=%u cfg=%u ep0=%d cmd=%d phy=%u smmu=%u(smr=%08x/%02x) rplg=%u",
                     s->hw.usbEvents, s->hw.usbResets, s->hw.configured,
                     s->hw.ep0Stage, s->hw.lastCmdSt, s->hw.phyInitMode,
                     s->hw.smmuBank, s->hw.smmuOrigSmr, s->hw.smmuOrigS2cr & 0xFFu, replugs);
            snprintf(gUsbHudLine1, sizeof(gUsbHudLine1),
                     "REG: dsts=%08x dctl=%08x gctl=%08x qcfg=%08x qhs=%08x pclk=%08x gcnt=%u",
                     dsts, dctl, gctl, qcfg, qhs, pclk, gcnt);
            renderHudOverlay();
            if (&boot_progress_ext) {
                snprintf(boot_progress_ext[1], sizeof(boot_progress_ext[1]),
                         "usb e=%u r=%u c=%u d=%x q=%x",
                         s->hw.usbEvents, s->hw.usbResets, s->hw.configured, dsts, qcfg);
            }
        }
        if (s->hw.ep3Busy && now > s->hw.ep3ArmTick + 2000ULL) {
            endDepTransfer(s->hw, 3, s->hw.ep3RscIdx, s->hw.ep3Busy);
        }

        // Fallback Stage 1 at t+4s: if no events arrived on UEFI-preserved PHY state,
        // run full Femto v2 HS PHY re-init + glitch-free QSCRATCH UTMI clock mux + DwcInitialize!
        if (replugs == 0 && s->hw.usbEvents == 0 && now > bootTick + 4000ULL) {
            ++replugs;
            s->hw.phyInitMode = 1u;
            s->hw.wr(DWC3_DCTL, s->hw.rd(DWC3_DCTL) & ~DWC3_DCTL_RUN_STOP);
            IOSleep(50);
            (void)bringUpController(*s);
            logStage("usb: fallback Femto v2 HS PHY + UTMI mux replug issued");
        }

        if (s->hw.configured && s->hw.hostReady) {
            if (s->hw.needReplay) {
                s->hw.needReplay = false;
                replayRamoops(*s, true);
            }
            if (s->screen.streaming && s->reply.used == 0 && now >= s->screen.nextFrameTick) {
                const uint32_t interval = 1000u / (s->screen.maxFps ? s->screen.maxFps : 1u);
                s->screen.nextFrameTick = now + interval;
                (void)usbCaptureAndQueueGsc1(s->screen, s->reply, false);
            }
            if (!s->hw.ep3Busy) {
                if (s->reply.used > 0) {
                    const uint32_t n = s->reply.pop(txScratch, DMA_EP3_MAX_CHUNK);
                    (void)startBulkIn(s->hw, txScratch, n);
                } else if (s->ramoops) {
                    volatile const uint32_t *hdr = reinterpret_cast<volatile const uint32_t *>(s->ramoops);
                    const uint32_t sig = hdr[0], start = hdr[1];
                    if (sig == J716F_RAMOOPS_SIG && start < J716F_RAMOOPS_DATA && start != s->lastRamoopsPos) {
                        const volatile uint8_t *ring = s->ramoops + J716F_RAMOOPS_HEAD;
                        uint32_t avail = (start >= s->lastRamoopsPos)
                            ? (start - s->lastRamoopsPos)
                            : (J716F_RAMOOPS_DATA - s->lastRamoopsPos);
                        if (avail > DMA_EP3_MAX_CHUNK) avail = DMA_EP3_MAX_CHUNK;
                        for (uint32_t i = 0; i < avail; ++i) {
                            txScratch[i] = ring[s->lastRamoopsPos + i];
                        }
                        if (startBulkIn(s->hw, txScratch, avail)) {
                            s->lastRamoopsPos = (s->lastRamoopsPos + avail) % J716F_RAMOOPS_DATA;
                        }
                    }
                }
            }
        }

        if (s->pendingFastboot && (!s->hw.configured || !s->hw.hostReady || (s->reply.used == 0 && !s->hw.ep3Busy))) {
            IOSleep(50);
            executeWarmFastbootReboot(s->hw);
        }

        if (s->pendingXdci && (!s->hw.configured || !s->hw.hostReady || (s->reply.used == 0 && !s->hw.ep3Busy))) {
            const uint32_t holdSec = s->pendingXdciSec;
            s->pendingXdci = false;
            s->pendingXdciSec = 0u;
            IOSleep(50);
            endDepTransfer(s->hw, 2, s->hw.ep2RscIdx, s->hw.ep2Armed);
            endDepTransfer(s->hw, 3, s->hw.ep3RscIdx, s->hw.ep3Busy);
            s->hw.configured = 0;
            s->hw.hostReady  = false;
            s->hw.wr(DWC3_DCTL, s->hw.rd(DWC3_DCTL) & ~DWC3_DCTL_RUN_STOP);
            IOSleep(100);
            if (&native_usb_xdci_activate && native_usb_xdci_activate) {
                native_usb_xdci_activate();
            }
            if (holdSec == 0u) {
                for (;;) {
                    IOSleep(10000);
                }
            } else {
                IOSleep(holdSec * 1000u);
                if (&native_usb_xdci_deactivate && native_usb_xdci_deactivate) {
                    native_usb_xdci_deactivate();
                }
                IOSleep(150);
                (void)bringUpController(*s);
                logStage("usb: returned from AppleUSBXDCI handoff to CDC-ACM");
            }
        }

        if (!s->hw.configured) {
            IODelay(200);  // fast poll (~0.2ms) during USB enumeration
        } else if (s->hw.ep3Busy || s->reply.used > 0 || s->screen.streaming) {
            IOSleep(1);
        } else {
            IOSleep(5);
        }
    }
}

} // namespace

extern "C" void usb_debug_draw_hud(void) {
    renderHudOverlay();
}

extern "C" void native_usb_debug_probe(void) {
    static int started = 0;
    if (__atomic_exchange_n(&started, 1, __ATOMIC_ACQ_REL)) return;
    unsigned enable = 1;
    (void)PE_parse_boot_argn("j716f-usb", &enable, sizeof(enable));
    if (!enable) return;
    usbVoteRpmhRailsOnce();
    thread_t th = nullptr;
    if (kernel_thread_start(usbDebugThread, nullptr, &th) == KERN_SUCCESS && th) {
        thread_deallocate(th);
    }
}
