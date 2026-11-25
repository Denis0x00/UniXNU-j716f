// SPDX-License-Identifier: BSD-2-Clause-Patent
// Live XNU USB High-Speed CDC-ACM diagnostic terminal for Lenovo TB-J716F (SM8250).
// Self-contained bare-metal DWC3 USB 2.0 device driver on USB0 (0x0A600000):
//   - Enumerates on Windows 10/11 via built-in usbser.sys (VID 0x1209 / PID 0x0001)
//   - Replays and live-streams the multi-core 256 KiB kernel log ring (PE_j716f_klog_read)
//   - Provides interactive diagnostics: status, cpufreq, smp, display, touch, usb, log, screen.snapshot, reboot
//   - Strictly opt-in via boot-arg j716f-usb=1; never draws on the SpringBoard framebuffer.

#include "J716fUsbDebugCdc.h"
#include "J716fUsbDebugWire.h"
#include "J716fQcomRpmh.h"
#include "J716fSmmuBypass.h"

#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <pexpert/pexpert.h>
#include <string.h>

extern "C" {
extern uint32_t PE_j716f_klog_read(uint64_t from_seq, char *dst, uint32_t max_len,
                                   uint64_t *out_next_seq) __attribute__((weak_import));
extern void j716f_cpufreq_snapshot(char *out, size_t cap) __attribute__((weak_import));
extern void j716f_battery_snapshot(char *out, size_t cap) __attribute__((weak_import));
extern void j716f_gpu_snapshot(char *out, size_t cap) __attribute__((weak_import));
extern uint64_t sheng_cpu_idle_mach(int cpu) __attribute__((weak_import));

extern volatile uint32_t armcpu_smp_running __attribute__((weak_import));
extern volatile uint32_t armcpu_cpu_init_completed __attribute__((weak_import));
extern volatile uint32_t armcpu_smp_step[8] __attribute__((weak_import));
extern volatile uint32_t armcpu_smp_fail[8] __attribute__((weak_import));

extern volatile uint32_t j716f_cpufreq_stage __attribute__((weak_import));
extern volatile uint32_t j716f_cpufreq_mhz[4] __attribute__((weak_import));

extern volatile uint32_t j716f_legacy_stage __attribute__((weak_import));
extern volatile uint64_t j716f_legacy_video_base __attribute__((weak_import));
extern volatile uint64_t j716f_legacy_video_bytes __attribute__((weak_import));
extern volatile uint32_t j716f_legacy_frame_count __attribute__((weak_import));
extern volatile uint32_t j716f_legacy_swap_accepted __attribute__((weak_import));
extern volatile uint32_t j716f_legacy_swap_rejected __attribute__((weak_import));
extern volatile uint32_t j716f_legacy_transfer_calls __attribute__((weak_import));
extern volatile uint32_t j716f_legacy_client_calls __attribute__((weak_import));

extern volatile uint32_t j716f_hid_events[8] __attribute__((weak_import));
extern volatile unsigned native_touch_stage __attribute__((weak_import));
extern volatile unsigned native_touch_stroke_count __attribute__((weak_import));
extern char native_touch_rows[4][120] __attribute__((weak_import));
extern volatile uint32_t j716f_buttons_changes __attribute__((weak_import));
extern volatile uint32_t j716f_buttons_mask __attribute__((weak_import));
extern volatile int j716f_buttons_error __attribute__((weak_import));

volatile uint32_t j716f_usb_stage = 0;
volatile uint32_t j716f_usb_events = 0;
volatile uint32_t j716f_usb_configured = 0;
}

namespace {

static const char *const kUsbStrings[4] = {
    nullptr,
    "Lenovo",
    "TB-J716F Debug CDC",
    "J716FDBG01",
};

static uint64_t nowMs() {
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
    return ns / 1000000ULL;
}

#include "J716fUsbDebugHw.h"
#include "J716fUsbDebugCommands.h"

static int epCmd(UsbHw &hw, unsigned dep, uint32_t cmd, uint32_t p0, uint32_t p1, uint32_t p2,
                 uint32_t *rawOut = nullptr) {
    const uint32_t cfg = hw.rd(DWC3_GUSB2PHYCFG(0));
    const bool drop = (cfg & DWC3_GUSB2PHYCFG_SUSPHY) != 0u;
    if (drop) hw.wr(DWC3_GUSB2PHYCFG(0), cfg & ~DWC3_GUSB2PHYCFG_SUSPHY);
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
    (void)epCmd(hw, 2, DWC3_DEPCMD_SETEPCONFIG,
                DWC3_DEPCFG_EP_TYPE(2) | DWC3_DEPCFG_MAX_PACKET_SIZE(hw.bulkMps) | DWC3_DEPCFG_FIFO_NUMBER(0),
                ev | DWC3_DEPCFG_EP_NUMBER(2), 0);
    (void)epCmd(hw, 2, DWC3_DEPCMD_SETTRANSFRESOURCE, DWC3_DEPXFERCFG_NUM_XFER_RES(1), 0, 0);
    (void)epCmd(hw, 3, DWC3_DEPCMD_SETEPCONFIG,
                DWC3_DEPCFG_EP_TYPE(2) | DWC3_DEPCFG_MAX_PACKET_SIZE(hw.bulkMps) | DWC3_DEPCFG_FIFO_NUMBER(1),
                ev | DWC3_DEPCFG_EP_NUMBER(3), 0);
    (void)epCmd(hw, 3, DWC3_DEPCMD_SETTRANSFRESOURCE, DWC3_DEPXFERCFG_NUM_XFER_RES(1), 0, 0);
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
        case 0x00:
            out[0] = 0; out[1] = 0; resp = 2; break;
        case 0x01:
        case 0x03:
        case 0x0B:
            break;
        case 0x05:
            hw.wr(DWC3_DCFG, (hw.rd(DWC3_DCFG) & ~DWC3_DCFG_DEVADDR_MASK) | DWC3_DCFG_DEVADDR(val & 0x7Fu));
            break;
        case 0x06: {
            const unsigned dtype = val >> 8;
            if (dtype == 1u) {
                memcpy(out, kUsbCdcDeviceDesc, sizeof(kUsbCdcDeviceDesc));
                resp = sizeof(kUsbCdcDeviceDesc);
            } else if (dtype == 2u) {
                resp = usb_cdc_build_config_desc(out, hw.bulkMps);
            } else if (dtype == 3u) {
                resp = buildStringDesc(val & 0xFFu, out);
                if (!resp) stall = true;
            } else if (dtype == 6u) {
                static const uint8_t kQual[10] = {10, 6, 0x00, 0x02, 0x02, 0x02, 0x00, 64, 1, 0};
                memcpy(out, kQual, 10);
                resp = 10;
            } else {
                stall = true;
            }
            break;
        }
        case 0x08:
            out[0] = hw.configured; resp = 1; break;
        case 0x09:
            hw.configured = static_cast<uint8_t>(val & 0xFFu);
            j716f_usb_configured = hw.configured;
            if (hw.configured) {
                ++hw.usbCfgCount;
                if (hw.usbCfgCount == 1u) {
                    IOLog("[J716F usb] host SET_CONFIGURATION=1 (mps=%u)\n", hw.bulkMps);
                }
                enableDataEndpoints(hw);
            }
            break;
        default:
            stall = true; break;
        }
    } else if ((rt & 0x60u) == 0x20u) {
        if (req == 0x21u) {
            memcpy(out, hw.lineCoding, 7);
            resp = 7;
        } else if (req == 0x20u) {
            resp = len > 64u ? 64u : len;
        } else if (req == 0x22u) {
            const bool prevDtr = (hw.lineState & 1u) != 0u;
            hw.lineState = val;
            if (val & 1u) {
                hw.hostReady = true;
                if (!prevDtr) hw.needReplay = true;
            } else {
                hw.hostReady = false;
            }
        } else if (req == 0x23u) {
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

enum RebootAction : uint8_t {
    kRebootNone       = 0,
    kRebootNormal     = 1,
    kRebootBootloader = 2,
    kRebootRecovery   = 3,
    kRebootPoweroff   = 4,
};

struct TerminalSession {
    UsbHw hw;
    UsbReplyRing reply;
    UsbScreenState screen;
    IOBufferMemoryDescriptor *replyMem = nullptr;
    IOBufferMemoryDescriptor *rgbMem   = nullptr;
    uint64_t klogCursor                = 0;
    uint64_t lastEnumTick              = 0;
    bool logStreamEnabled              = true;
    uint8_t pendingRebootAction        = kRebootNone;
    char cmdLine[256]                  = {};
    unsigned cmdLen                    = 0;
};

static uint64_t usbPsciCall(uint64_t fid, uint64_t a1) {
    register uint64_t x0 __asm__("x0") = fid;
    register uint64_t x1 __asm__("x1") = a1;
    register uint64_t x2 __asm__("x2") = 0;
    register uint64_t x3 __asm__("x3") = 0;
    __asm__ volatile("smc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3)
                     : : "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11",
                         "x12", "x13", "x14", "x15", "x16", "x17", "memory");
    return x0;
}

static int usbSpmiWaitDone(volatile uint32_t *engine) {
    for (uint32_t i = 0; i < 100000u; ++i) {
        const uint32_t st = engine[0x08u / 4u];
        if (st & 1u) return (st & 0xEu) ? 2 : 0;
    }
    return 1;
}

static int usbSpmiRead8(volatile uint32_t *obs, uint8_t reg, uint8_t *out) {
    if (!obs || !out) return 3;
    obs[0x00u / 4u] = (1u << 27) | (static_cast<uint32_t>(reg) << 4);
    const int rc = usbSpmiWaitDone(obs);
    if (!rc) *out = static_cast<uint8_t>(obs[0x18u / 4u]);
    return rc;
}

static int usbSpmiWrite8(volatile uint32_t *chnl, uint8_t reg, uint8_t val) {
    if (!chnl) return 3;
    chnl[0x10u / 4u] = val;
    chnl[0x00u / 4u] = static_cast<uint32_t>(reg) << 4;
    return usbSpmiWaitDone(chnl);
}

static void usbRebootModeOp(bool doWrite, uint8_t ponReason, uint32_t imemReason, char *out, size_t cap) {
    int ponRc = -1, imemRc = -1;
    int ponApid = -1, ponOwner = -1;
    uint8_t ponBefore = 0xFFu, ponAfter = 0xFFu;
    uint32_t imemBefore = 0xFFFFFFFFu, imemAfter = 0xFFFFFFFFu;

    IOMemoryMap *coreMap = nullptr;
    IOMemoryMap *cnfgMap = nullptr;
    volatile uint32_t *core = reinterpret_cast<volatile uint32_t *>(mapRangeUncached(J716F_SPMI_CORE_BASE, 0x2000u, coreMap));
    volatile uint32_t *cnfg = reinterpret_cast<volatile uint32_t *>(mapRangeUncached(J716F_SPMI_CNFG_BASE, 0x2000u, cnfgMap));
    if (core && cnfg) {
        const uint32_t ver = core[0];
        const uint32_t apids = core[1] & 0x7FFu;
        if (ver >= 0x50000000u && ver < 0x70000000u && apids > 0u && apids <= J716F_SPMI_MAX_APIDS) {
            for (uint32_t i = 0; i < apids; ++i) {
                const uint32_t m = core[(J716F_SPMI_APID_MAP_OFF + 4u * i) / 4u];
                if (!m || ((m >> 8) & 0xFFFu) != J716F_PM8150_PON_PPID) continue;
                const int own = static_cast<int>(cnfg[(J716F_SPMI_CNFG_OWNER_OFF + 4u * i) / 4u] & 7u);
                if (ponApid < 0 || own == 0) {
                    ponApid = static_cast<int>(i);
                    ponOwner = own;
                }
            }
        }
    }
    if (ponApid >= 0) {
        const uint64_t obsPhys = J716F_SPMI_OBSRVR_BASE + J716F_SPMI_OBS_APID_STRIDE * static_cast<uint64_t>(ponApid);
        IOMemoryMap *obsMap = nullptr;
        const uintptr_t obsPage = mapRangeUncached(obsPhys & ~0xFFFULL, 0x1000u, obsMap);
        volatile uint32_t *obs = obsPage ? reinterpret_cast<volatile uint32_t *>(obsPage + (obsPhys & 0xFFFu)) : nullptr;
        if (obs) {
            ponRc = usbSpmiRead8(obs, J716F_PON_SOFT_RB_SPARE, &ponBefore);
            ponAfter = ponBefore;
            if (doWrite && ponRc == 0) {
                if (ponOwner != 0) {
                    ponRc = -2;
                } else {
                    const uint8_t next = static_cast<uint8_t>(
                        (ponBefore & ~J716F_PON_REASON_MASK) |
                        ((ponReason << J716F_PON_REASON_SHIFT) & J716F_PON_REASON_MASK));
                    const uint64_t chPhys = J716F_SPMI_CHNLS_BASE + J716F_SPMI_CHNL_APID_STRIDE * static_cast<uint64_t>(ponApid);
                    IOMemoryMap *chMap = nullptr;
                    volatile uint32_t *ch = reinterpret_cast<volatile uint32_t *>(mapRangeUncached(chPhys, 0x1000u, chMap));
                    if (ch) {
                        ponRc = usbSpmiWrite8(ch, J716F_PON_SOFT_RB_SPARE, next);
                        if (ponRc == 0) {
                            ponRc = usbSpmiRead8(obs, J716F_PON_SOFT_RB_SPARE, &ponAfter);
                            if (ponRc == 0 && ponAfter != next) ponRc = -3;
                        }
                    } else {
                        ponRc = -4;
                    }
                    if (chMap) chMap->release();
                }
            }
        }
        if (obsMap) obsMap->release();
    }
    if (cnfgMap) cnfgMap->release();
    if (coreMap) coreMap->release();

    IOMemoryMap *imemMap = nullptr;
    const uintptr_t imemVa = mapRangeUncached(J716F_IMEM_BASE, J716F_IMEM_SIZE, imemMap);
    if (imemVa) {
        volatile uint32_t *rr = reinterpret_cast<volatile uint32_t *>(imemVa + J716F_IMEM_RESTART_REASON_OFF);
        imemBefore = *rr;
        imemAfter = imemBefore;
        imemRc = 0;
        if (doWrite) {
            *rr = imemReason;
            __asm__ volatile("dsb sy" ::: "memory");
            imemAfter = *rr;
            if (imemAfter != imemReason) imemRc = -3;
        }
    }
    if (imemMap) imemMap->release();

    if (doWrite) {
        snprintf(out, cap, "pon=%02x>%02x(rc=%d,apid=%d,own=%d) imem=%08x>%08x(rc=%d)",
                 ponBefore, ponAfter, ponRc, ponApid, ponOwner, imemBefore, imemAfter, imemRc);
    } else {
        const uint32_t psciVer = static_cast<uint32_t>(usbPsciCall(J716F_PSCI_VERSION, 0));
        const int64_t fReset = static_cast<int64_t>(usbPsciCall(J716F_PSCI_FEATURES, J716F_PSCI_SYSTEM_RESET));
        const int64_t fOff   = static_cast<int64_t>(usbPsciCall(J716F_PSCI_FEATURES, J716F_PSCI_SYSTEM_OFF));
        snprintf(out, cap,
                 "[usb] REBOOT.PROBE: psci=v%x reset=%lld off=%lld pon(apid=%d,own=%d,8f=%02x,rc=%d) imem(65c=%08x,rc=%d)\r\n",
                 psciVer, static_cast<long long>(fReset), static_cast<long long>(fOff),
                 ponApid, ponOwner, ponBefore, ponRc, imemBefore, imemRc);
    }
}

static void replayKernelLog(TerminalSession &s) {
    if (!&PE_j716f_klog_read) {
        s.reply.writeStr("[usb] klog ring unavailable\r\n");
        return;
    }
    s.reply.writeStr("\r\n[usb] === TB-J716F kernel log begin ===\r\n");
    uint64_t seq = 0;
    char chunk[1024];
    for (unsigned iter = 0; iter < 300; ++iter) {
        if (s.reply.freeBytes() < sizeof(chunk) + 256u) break;
        uint64_t nextSeq = seq;
        const uint32_t got = PE_j716f_klog_read(seq, chunk, sizeof(chunk), &nextSeq);
        seq = nextSeq;
        if (got == 0u) break;
        (void)s.reply.write(chunk, got);
    }
    s.klogCursor = seq;
    s.reply.writeStr("\r\n[usb] === TB-J716F kernel log end ===\r\n");
}

static void handleCommand(TerminalSession &s, const char *raw) {
    while (*raw == ' ' || *raw == '\t') ++raw;
    if (!*raw) return;
    char buf[512];

    if (strEq(raw, "ping")) {
        s.reply.writeStr("[usb] PONG TB-J716F\r\n");
    } else if (strEq(raw, "features")) {
        s.reply.writeStr("FEATURES: ping help status info uptime cpufreq battery gpu smp display touch input usb bootargs log replay log.on log.off screen.snapshot screen.start screen.stop screen.configure reboot reboot.probe poweroff\r\n");
    } else if (strEq(raw, "help")) {
        s.reply.writeStr("Commands: ping status cpufreq battery gpu smp display touch usb bootargs log log.on log.off screen.snapshot screen.start screen.stop screen.configure reboot [normal|bootloader|recovery|off] reboot.probe poweroff\r\n");
    } else if (strEq(raw, "info") || strEq(raw, "uptime") || strEq(raw, "status")) {
        const uint32_t cpus = &armcpu_smp_running ? armcpu_smp_running : 0u;
        const uint32_t edge = &armcpu_cpu_init_completed ? armcpu_cpu_init_completed : 0u;
        const uint64_t ms = nowMs();
        snprintf(buf, sizeof(buf),
                 "[usb] board=TB-J716F soc=SM8250-AC uptime=%llu.%03llus smp=%u/edge=%u usb_ev=%u rst=%u mps=%u\r\n",
                 static_cast<unsigned long long>(ms / 1000ULL),
                 static_cast<unsigned long long>(ms % 1000ULL),
                 cpus, edge, s.hw.usbEvents, s.hw.usbResets, s.hw.bulkMps);
        s.reply.writeStr(buf);
        if (&j716f_cpufreq_snapshot) {
            j716f_cpufreq_snapshot(buf, sizeof(buf));
            s.reply.writeStr(buf);
        }
        if (&j716f_battery_snapshot) {
            j716f_battery_snapshot(buf, sizeof(buf));
            s.reply.writeStr(buf);
        }
        snprintf(buf, sizeof(buf),
                 "[usb] FB: stage=%u frames=%u swap=%u/%u xfer=%u clients=%u\r\n",
                 &j716f_legacy_stage ? j716f_legacy_stage : 0u,
                 &j716f_legacy_frame_count ? j716f_legacy_frame_count : 0u,
                 &j716f_legacy_swap_accepted ? j716f_legacy_swap_accepted : 0u,
                 &j716f_legacy_swap_rejected ? j716f_legacy_swap_rejected : 0u,
                 &j716f_legacy_transfer_calls ? j716f_legacy_transfer_calls : 0u,
                 &j716f_legacy_client_calls ? j716f_legacy_client_calls : 0u);
        s.reply.writeStr(buf);
        snprintf(buf, sizeof(buf),
                 "[usb] INPUT: touch_stage=%u strokes=%u hid_touch=%u hid_btn=%u btn_mask=0x%x btn_chg=%u\r\n",
                 &native_touch_stage ? native_touch_stage : 0u,
                 &native_touch_stroke_count ? native_touch_stroke_count : 0u,
                 &j716f_hid_events ? j716f_hid_events[6] : 0u,
                 &j716f_hid_events ? j716f_hid_events[4] : 0u,
                 &j716f_buttons_mask ? j716f_buttons_mask : 0u,
                 &j716f_buttons_changes ? j716f_buttons_changes : 0u);
        s.reply.writeStr(buf);
    } else if (strEq(raw, "cpufreq") || strEq(raw, "cpu.freq")) {
        if (&j716f_cpufreq_snapshot) {
            j716f_cpufreq_snapshot(buf, sizeof(buf));
            s.reply.writeStr(buf);
        } else {
            snprintf(buf, sizeof(buf), "[usb] CPUFREQ: stage=%u d0=%u d1=%u d2=%u l3=%u MHz\r\n",
                     &j716f_cpufreq_stage ? j716f_cpufreq_stage : 0u,
                     &j716f_cpufreq_mhz ? j716f_cpufreq_mhz[0] : 0u,
                     &j716f_cpufreq_mhz ? j716f_cpufreq_mhz[1] : 0u,
                     &j716f_cpufreq_mhz ? j716f_cpufreq_mhz[2] : 0u,
                     &j716f_cpufreq_mhz ? j716f_cpufreq_mhz[3] : 0u);
            s.reply.writeStr(buf);
        }
    } else if (strEq(raw, "battery") || strEq(raw, "battery.status")) {
        if (&j716f_battery_snapshot) {
            j716f_battery_snapshot(buf, sizeof(buf));
            s.reply.writeStr(buf);
        } else {
            s.reply.writeStr("[usb] BATTERY: unavailable\r\n");
        }
    } else if (strEq(raw, "gpu") || strEq(raw, "gpu.status")) {
        if (&j716f_gpu_snapshot) {
            static char gbuf[6144];
            j716f_gpu_snapshot(gbuf, sizeof(gbuf));
            s.reply.writeStr(gbuf);
        } else {
            s.reply.writeStr("[usb] GPU: unavailable\r\n");
        }
    } else if (strEq(raw, "smp")) {
        snprintf(buf, sizeof(buf), "[usb] SMP: running=%u edge_init=%u\r\n",
                 &armcpu_smp_running ? armcpu_smp_running : 0u,
                 &armcpu_cpu_init_completed ? armcpu_cpu_init_completed : 0u);
        s.reply.writeStr(buf);
        if (&armcpu_smp_step && &armcpu_smp_fail) {
            snprintf(buf, sizeof(buf),
                     "[usb] SMP step=[%u %u %u %u %u %u %u %u] fail=[%u %u %u %u %u %u %u %u]\r\n",
                     armcpu_smp_step[0], armcpu_smp_step[1], armcpu_smp_step[2], armcpu_smp_step[3],
                     armcpu_smp_step[4], armcpu_smp_step[5], armcpu_smp_step[6], armcpu_smp_step[7],
                     armcpu_smp_fail[0], armcpu_smp_fail[1], armcpu_smp_fail[2], armcpu_smp_fail[3],
                     armcpu_smp_fail[4], armcpu_smp_fail[5], armcpu_smp_fail[6], armcpu_smp_fail[7]);
            s.reply.writeStr(buf);
        }
        if (&sheng_cpu_idle_mach) {
            snprintf(buf, sizeof(buf),
                     "[usb] IDLE(Mticks): [%llu %llu %llu %llu | %llu %llu %llu | %llu]\r\n",
                     static_cast<unsigned long long>(sheng_cpu_idle_mach(0) / 1000000ULL),
                     static_cast<unsigned long long>(sheng_cpu_idle_mach(1) / 1000000ULL),
                     static_cast<unsigned long long>(sheng_cpu_idle_mach(2) / 1000000ULL),
                     static_cast<unsigned long long>(sheng_cpu_idle_mach(3) / 1000000ULL),
                     static_cast<unsigned long long>(sheng_cpu_idle_mach(4) / 1000000ULL),
                     static_cast<unsigned long long>(sheng_cpu_idle_mach(5) / 1000000ULL),
                     static_cast<unsigned long long>(sheng_cpu_idle_mach(6) / 1000000ULL),
                     static_cast<unsigned long long>(sheng_cpu_idle_mach(7) / 1000000ULL));
            s.reply.writeStr(buf);
        }
    } else if (strEq(raw, "bootargs")) {
        snprintf(buf, sizeof(buf), "[usb] bootargs=%s\r\n", PE_boot_args());
        s.reply.writeStr(buf);
    } else if (strEq(raw, "display")) {
        PE_Video v = {};
        (void)PE_current_console(&v);
        snprintf(buf, sizeof(buf),
                 "[usb] FB: stage=%u gop=%lux%lu base=0x%lx row=%lu frames=%u swap=%u/%u xfer=%u clients=%u\r\n",
                 &j716f_legacy_stage ? j716f_legacy_stage : 0u,
                 v.v_width, v.v_height, v.v_baseAddr, v.v_rowBytes,
                 &j716f_legacy_frame_count ? j716f_legacy_frame_count : 0u,
                 &j716f_legacy_swap_accepted ? j716f_legacy_swap_accepted : 0u,
                 &j716f_legacy_swap_rejected ? j716f_legacy_swap_rejected : 0u,
                 &j716f_legacy_transfer_calls ? j716f_legacy_transfer_calls : 0u,
                 &j716f_legacy_client_calls ? j716f_legacy_client_calls : 0u);
        s.reply.writeStr(buf);
    } else if (strEq(raw, "touch") || strEq(raw, "input") || strEq(raw, "input.status")) {
        snprintf(buf, sizeof(buf),
                 "[usb] TOUCH: stage=%u strokes=%u btn_mask=0x%x btn_chg=%u btn_err=%d\r\n",
                 &native_touch_stage ? native_touch_stage : 0u,
                 &native_touch_stroke_count ? native_touch_stroke_count : 0u,
                 &j716f_buttons_mask ? j716f_buttons_mask : 0u,
                 &j716f_buttons_changes ? j716f_buttons_changes : 0u,
                 &j716f_buttons_error ? j716f_buttons_error : 0);
        s.reply.writeStr(buf);
        if (&j716f_hid_events) {
            snprintf(buf, sizeof(buf), "[usb] HID ev=[%u %u %u %u %u %u %u %u]\r\n",
                     j716f_hid_events[0], j716f_hid_events[1], j716f_hid_events[2], j716f_hid_events[3],
                     j716f_hid_events[4], j716f_hid_events[5], j716f_hid_events[6], j716f_hid_events[7]);
            s.reply.writeStr(buf);
        }
        if (&native_touch_rows) {
            for (unsigned i = 0; i < 4; ++i) {
                if (!native_touch_rows[i][0]) continue;
                snprintf(buf, sizeof(buf), "[usb] TOUCH[%u]: %s\r\n", i, native_touch_rows[i]);
                s.reply.writeStr(buf);
            }
        }
    } else if (strEq(raw, "usb") || strEq(raw, "usb.status")) {
        snprintf(buf, sizeof(buf),
                 "[usb] USB: cfg=%u mps=%u dtr=%u phy=%u smmu_bank=%u smr=0x%08x s2cr=0x%08x dsts=0x%08x gctl=0x%08x dctl=0x%08x qcfg=0x%08x qhs=0x%08x ev=%u rst=%u\r\n",
                 s.hw.configured, s.hw.bulkMps, s.hw.lineState & 1u, s.hw.phyInitMode,
                 s.hw.smmuBank, s.hw.smmuOrigSmr, s.hw.smmuOrigS2cr,
                 s.hw.rd(DWC3_DSTS), s.hw.rd(DWC3_GCTL), s.hw.rd(DWC3_DCTL),
                 s.hw.rd(QSCRATCH_GENERAL_CFG), s.hw.rd(QSCRATCH_HS_PHY_CTRL),
                 s.hw.usbEvents, s.hw.usbResets);
        s.reply.writeStr(buf);
    } else if (strEq(raw, "log") || strEq(raw, "replay")) {
        replayKernelLog(s);
    } else if (strEq(raw, "log.on")) {
        s.logStreamEnabled = true;
        s.reply.writeStr("[usb] live kernel log streaming enabled\r\n");
    } else if (strEq(raw, "log.off")) {
        s.logStreamEnabled = false;
        s.reply.writeStr("[usb] live kernel log streaming disabled\r\n");
    } else if (strEq(raw, "screen.snapshot")) {
        (void)usbCaptureAndQueueGsc1(s.screen, s.reply, true);
    } else if (strEq(raw, "screen.start")) {
        s.screen.streaming = true;
        s.screen.nextFrameTick = 0;
        s.reply.writeStr("[usb] SCREEN: streaming enabled\r\n");
    } else if (strEq(raw, "screen.stop")) {
        s.screen.streaming = false;
        usbQueueGsc1Stop(s.screen, s.reply);
        s.reply.writeStr("[usb] SCREEN: streaming stopped\r\n");
    } else if (strStarts(raw, "screen.configure")) {
        const char *p = raw + 16;
        bool ok1 = false, ok2 = false;
        const uint32_t edge = parseUint(p, &ok1);
        const uint32_t fps  = parseUint(p, &ok2);
        if (ok1 && edge >= 160u && edge <= 2780u) s.screen.maxEdge = edge;
        if (ok2 && fps >= 1u && fps <= 30u) s.screen.maxFps = fps;
        snprintf(buf, sizeof(buf), "[usb] SCREEN: configured max_size=%u max_fps=%u\r\n",
                 s.screen.maxEdge, s.screen.maxFps);
        s.reply.writeStr(buf);
    } else if (strEq(raw, "reboot.probe")) {
        usbRebootModeOp(false, 0u, 0u, buf, sizeof(buf));
        s.reply.writeStr(buf);
    } else if (strEq(raw, "reboot") || strEq(raw, "reboot normal") ||
               strEq(raw, "reboot bootloader") || strEq(raw, "reboot fastboot") ||
               strEq(raw, "reboot recovery") ||
               strEq(raw, "reboot off") || strEq(raw, "poweroff") || strEq(raw, "shutdown")) {
        uint8_t action = kRebootNormal;
        uint8_t ponReason = J716F_PON_REASON_NORMAL;
        uint32_t imemReason = J716F_IMEM_REASON_NORMAL;
        const char *modeName = "normal";
        if (strEq(raw, "reboot bootloader") || strEq(raw, "reboot fastboot")) {
            action = kRebootBootloader;
            ponReason = J716F_PON_REASON_BOOTLOADER;
            imemReason = J716F_IMEM_REASON_BOOTLOADER;
            modeName = "bootloader";
        } else if (strEq(raw, "reboot recovery")) {
            action = kRebootRecovery;
            ponReason = J716F_PON_REASON_RECOVERY;
            imemReason = J716F_IMEM_REASON_RECOVERY;
            modeName = "recovery";
        } else if (strEq(raw, "reboot off") || strEq(raw, "poweroff") || strEq(raw, "shutdown")) {
            action = kRebootPoweroff;
            modeName = "poweroff";
        }
        s.screen.streaming = false;
        s.hw.needReplay    = false;
        s.reply.reset();
        char diag[256] = {};
        if (action == kRebootPoweroff) {
            snprintf(buf, sizeof(buf), "[usb] REBOOT: mode=poweroff executing PSCI_SYSTEM_OFF\r\n");
        } else {
            usbRebootModeOp(true, ponReason, imemReason, diag, sizeof(diag));
            snprintf(buf, sizeof(buf), "[usb] REBOOT: mode=%s %s executing PSCI_SYSTEM_RESET\r\n", modeName, diag);
        }
        s.pendingRebootAction = action;
        IOLog("[J716F usb] %s\n", buf);
        s.reply.writeStr(buf);
    } else {
        snprintf(buf, sizeof(buf), "[usb] unknown command: %s (try 'help')\r\n", raw);
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
            j716f_usb_events = hw.usbEvents;
            if (ev & 1u) {
                const unsigned type = (ev >> 8) & 0x0Fu;
                if (type == DWC3_DEV_EVENT_RESET) {
                    ++hw.usbResets;
                    s.lastEnumTick = nowMs();
                    if (hw.usbResets == 1u) {
                        IOLog("[J716F usb] first host USB_RESET received\n");
                    }
                    endDepTransfer(hw, 2, hw.ep2RscIdx, hw.ep2Armed);
                    endDepTransfer(hw, 3, hw.ep3RscIdx, hw.ep3Busy);
                    hw.configured = 0;
                    j716f_usb_configured = 0;
                    hw.hostReady  = false;
                    hw.wr(DWC3_DCFG, hw.rd(DWC3_DCFG) & ~DWC3_DCFG_DEVADDR_MASK);
                    ep0Restart(hw);
                } else if (type == DWC3_DEV_EVENT_CONNECT_DONE) {
                    const uint32_t spd = hw.rd(DWC3_DSTS) & DWC3_DSTS_CONNECTSPD_MASK;
                    hw.bulkMps = (spd == DWC3_DCFG_HIGHSPEED) ? 512u : 64u;
                    s.lastEnumTick = nowMs();
                } else if (type == DWC3_DEV_EVENT_DISCONNECT) {
                    endDepTransfer(hw, 2, hw.ep2RscIdx, hw.ep2Armed);
                    endDepTransfer(hw, 3, hw.ep3RscIdx, hw.ep3Busy);
                    hw.configured = 0;
                    j716f_usb_configured = 0;
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

__attribute__((noreturn)) static void executePowerAction(UsbHw &hw, uint8_t action) {
    hw.wr(DWC3_DCTL, hw.rd(DWC3_DCTL) & ~DWC3_DCTL_RUN_STOP);
    IOSleep(100);
    __asm__ volatile("msr daifset, #3" ::: "memory");
    const uint64_t fid = (action == kRebootPoweroff) ? J716F_PSCI_SYSTEM_OFF : J716F_PSCI_SYSTEM_RESET;
    (void)usbPsciCall(fid, 0);
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
            IOLog("[J716F usb] dmaDesc alloc/prepare failed\n");
            return false;
        }
        addr64_t segLen = 0;
        hw.dmaPhys = hw.dmaDesc->getPhysicalSegment(0, &segLen);
        hw.dmaVirt = static_cast<uint8_t *>(hw.dmaDesc->getBytesNoCopy());
        if (!hw.dmaVirt || !hw.dmaPhys || segLen < DMA_TOTAL_BYTES) {
            IOLog("[J716F usb] dmaVirt/dmaPhys invalid\n");
            return false;
        }
    }
    memset(hw.dmaVirt, 0, DMA_TOTAL_BYTES);
    usbCacheRange(hw.dmaVirt, DMA_TOTAL_BYTES, 1);

    if (!hw.smmu) {
        (void)usbConfigureSmmuBypass(hw);
    }
    if (!usbGccAndPhyInit(hw)) {
        IOLog("[J716F usb] gcc/phy init failed\n");
        return false;
    }

    if (!hw.core) {
        hw.core = reinterpret_cast<volatile uint32_t *>(
            mapRangeUncached(J716F_USB0_CORE_BASE, J716F_USB0_CORE_SIZE, hw.coreMap));
        if (!hw.core) {
            IOLog("[J716F usb] core map failed\n");
            return false;
        }
    }

    const uint32_t gsnpsid = hw.rd(DWC3_GSNPSID);
    const uint32_t sid = gsnpsid & DWC3_GSNPSID_MASK;
    if (sid != DWC3_GSNPSID_USB3 && sid != DWC3_GSNPSID_USB31 && sid != DWC3_GSNPSID_USB31_3331) {
        IOLog("[J716F usb] unexpected GSNPSID=0x%08x\n", gsnpsid);
        return false;
    }

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

    hw.wr(QSCRATCH_HS_PHY_CTRL, hw.rd(QSCRATCH_HS_PHY_CTRL) | QSCRATCH_UTMI_OTG_VBUS_VALID);
    hw.wr(QSCRATCH_HS_PHY_CTRL, hw.rd(QSCRATCH_HS_PHY_CTRL) | QSCRATCH_SW_SESSVLD_SEL);
    hw.wr(QSCRATCH_SS_PHY_CTRL, hw.rd(QSCRATCH_SS_PHY_CTRL) | QSCRATCH_LANE0_PWR_PRESENT);

    hw.wr(DWC3_DCTL, (hw.rd(DWC3_DCTL) & ~DWC3_DCTL_RUN_STOP) | DWC3_DCTL_CSFTRST);
    for (unsigned i = 0; i < 100000; ++i) {
        if ((hw.rd(DWC3_DCTL) & DWC3_DCTL_CSFTRST) == 0u) break;
        IODelay(5);
    }

    hw.wr(DWC3_DCTL, (hw.rd(DWC3_DCTL) & ~(0x1F000000u | DWC3_DCTL_RUN_STOP)) | 0x10F00000u);
    hw.wr(DWC3_GSBUSCFG0, hw.rd(DWC3_GSBUSCFG0) | 0x0Eu);
    hw.wr(DWC3_GSBUSCFG1, (hw.rd(DWC3_GSBUSCFG1) & ~0xF00u) | 0xE00u);

    hw.wr(DWC3_GUSB2PHYCFG(0), hw.rd(DWC3_GUSB2PHYCFG(0)) & ~DWC3_GUSB2PHYCFG_ENBLSLPM);
    hw.wr(DWC3_GUSB2PHYCFG(0), hw.rd(DWC3_GUSB2PHYCFG(0)) & ~DWC3_GUSB2PHYCFG_SUSPHY);
    hw.wr(DWC3_GUSB3PIPECTL(0), hw.rd(DWC3_GUSB3PIPECTL(0)) & ~1u);

    {
        uint32_t fifoAddr = hw.rd(DWC3_GTXFIFOSIZ(0)) & 0xFFFFu;
        for (unsigned ep = 1; ep <= 15; ++ep) {
            const uint32_t depth = (ep < 7u) ? 0x0184u : 0x0082u;
            hw.wr(DWC3_GTXFIFOSIZ(ep), (fifoAddr << 16) | depth);
            fifoAddr += depth;
        }
    }

    hw.wr(DWC3_GCTL, hw.rd(DWC3_GCTL) & ~0x00010000u);
    hw.wr(DWC3_GCTL, (hw.rd(DWC3_GCTL) & ~0x00003000u) | DWC3_GCTL_PRTCAPDIR(DWC3_GCTL_PRTCAP_DEVICE));
    hw.wr(DWC3_GCTL, (hw.rd(DWC3_GCTL) & 0x0007FFFFu) | 0x00100000u);

    hw.wr(DWC3_DCFG,
          (hw.rd(DWC3_DCFG) & ~(DWC3_DCFG_SPEED_MASK | DWC3_DCFG_DEVADDR_MASK)) |
          (1u << 22) | DWC3_DCFG_HIGHSPEED);
    hw.wr(DWC3_DEVTEN, 0x07u);

    (void)epCmd(hw, 0, DWC3_DEPCMD_DEPSTARTCFG, 0, 0, 0);

    const uint64_t evtPa = hw.dmaPhys + DMA_EVT_OFF;
    hw.wr(DWC3_GEVNTADRLO(0), static_cast<uint32_t>(evtPa));
    hw.wr(DWC3_GEVNTADRHI(0), static_cast<uint32_t>(evtPa >> 32));
    hw.wr(DWC3_GEVNTSIZ(0), 4096u);
    hw.wr(DWC3_GEVNTCOUNT(0), 0u);
    hw.evtOff = 0u;

    programEp0(hw);

    IOSleep(30);
    hw.wr(DWC3_DCTL, hw.rd(DWC3_DCTL) | DWC3_DCTL_RUN_STOP);
    s.lastEnumTick = nowMs();
    IOLog("[J716F usb] DWC3 CDC-ACM running gsnpsid=0x%08x dsts=0x%08x smmu_bank=%u smr=0x%08x phy=%u\n",
          gsnpsid, hw.rd(DWC3_DSTS), hw.smmuBank, hw.smmuOrigSmr, hw.phyInitMode);
    return true;
}

static void usbDebugThread(void *arg, wait_result_t waitResult) {
    (void)arg;
    (void)waitResult;
    // Wait briefly so TouchProbe completes its SMMU/I2C setup without contention.
    IOSleep(350);
    j716f_usb_stage = 2;

    TerminalSession *s = static_cast<TerminalSession *>(IOMalloc(sizeof(TerminalSession)));
    if (!s) return;
    memset(s, 0, sizeof(TerminalSession));
    s->hw.initDefaults();
    s->logStreamEnabled = true;

    const uint32_t kReplyBytes = 3u * 1024u * 1024u;
    const uint32_t kRgbBytes   = 2u * 1024u * 1024u;
    s->replyMem = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, kIODirectionInOut, kReplyBytes, 4096);
    s->rgbMem   = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, kIODirectionInOut, kRgbBytes, 4096);
    if (!s->replyMem || !s->rgbMem ||
        s->replyMem->prepare() != kIOReturnSuccess ||
        s->rgbMem->prepare() != kIOReturnSuccess) {
        IOLog("[J716F usb] reply/rgb buffer alloc failed\n");
        return;
    }

    s->reply.buf = static_cast<uint8_t *>(s->replyMem->getBytesNoCopy());
    s->reply.cap = kReplyBytes;
    s->reply.reset();
    s->screen.init(static_cast<uint8_t *>(s->rgbMem->getBytesNoCopy()), kRgbBytes);

    if (!bringUpController(*s)) return;
    j716f_usb_stage = 3;

    uint8_t *txScratch = static_cast<uint8_t *>(IOMalloc(DMA_EP3_MAX_CHUNK));
    if (!txScratch) return;

    const uint64_t bootTick = nowMs();
    unsigned replugs = 0;

    for (;;) {
        pollUsbEvents(*s);

        const uint64_t now = nowMs();
        if (s->hw.ep3Busy && now > s->hw.ep3ArmTick + 2000ULL) {
            endDepTransfer(s->hw, 3, s->hw.ep3RscIdx, s->hw.ep3Busy);
        }

        // Fallback at t+4s: if no events arrived on UEFI-preserved PHY state,
        // run full Femto v2 HS PHY re-init + glitch-free QSCRATCH UTMI clock mux.
        if (replugs == 0 && s->hw.usbEvents == 0 && now > bootTick + 4000ULL) {
            ++replugs;
            s->hw.phyInitMode = 1u;
            s->hw.wr(DWC3_DCTL, s->hw.rd(DWC3_DCTL) & ~DWC3_DCTL_RUN_STOP);
            IOSleep(50);
            (void)bringUpController(*s);
            IOLog("[J716F usb] fallback Femto v2 HS PHY + UTMI mux replug issued\n");
        }

        if (s->hw.configured && s->hw.hostReady) {
            if (s->hw.needReplay) {
                s->hw.needReplay = false;
                replayKernelLog(*s);
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
                } else if (s->logStreamEnabled && &PE_j716f_klog_read) {
                    uint64_t nextSeq = s->klogCursor;
                    const uint32_t got = PE_j716f_klog_read(
                        s->klogCursor, reinterpret_cast<char *>(txScratch), 4096u, &nextSeq);
                    if (got > 0u) {
                        if (startBulkIn(s->hw, txScratch, got)) {
                            s->klogCursor = nextSeq;
                        }
                    } else {
                        s->klogCursor = nextSeq;
                    }
                }
            }
        }

        if (s->pendingRebootAction != kRebootNone &&
            (!s->hw.configured || !s->hw.hostReady || (s->reply.used == 0 && !s->hw.ep3Busy))) {
            IOSleep(50);
            executePowerAction(s->hw, s->pendingRebootAction);
        }

        // Fast-poll ONLY during active USB enumeration (~2s after pull-up or USB_RESET);
        // sleep cleanly when configured or unplugged so idle CPU cost is near zero.
        if (!s->hw.configured) {
            if (now <= s->lastEnumTick + 2000ULL) {
                IODelay(200);
            } else {
                IOSleep(10);
            }
        } else if (s->hw.ep3Busy || s->reply.used > 0 || s->screen.streaming) {
            IOSleep(1);
        } else {
            IOSleep(5);
        }
    }
}

} // namespace

extern "C" void j716f_usb_debug_probe(void) {
    static int started = 0;
    unsigned enable = 0;
    if (!PE_parse_boot_argn("j716f-usb", &enable, sizeof(enable)) || !enable) {
        return;
    }
    if (__atomic_exchange_n(&started, 1, __ATOMIC_ACQ_REL)) return;
    j716f_usb_stage = 1;
    usbVoteRpmhRailsOnce();
    thread_t th = nullptr;
    if (kernel_thread_start(usbDebugThread, nullptr, &th) == KERN_SUCCESS && th) {
        thread_deallocate(th);
    }
}
