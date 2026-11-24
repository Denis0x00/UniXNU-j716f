/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* TB-J716F (SM8250-AC, Adreno 650 v2.0) A6XX RBBM/CP interrupt decoder implementing
 * upstream drivers/Adreno740/NativeAdrenoIrq.h with exact Linux A6XX_INT_MASK (0x03D0E242). */
#include "NativeAdrenoIrq.h"
#include <IOKit/IOLib.h>

/* Exact A6XX register dword offsets and bit positions from pinned Mesa a6xx.xml & Linux a6xx_gpu.c:
 *   RBBM_INT_0_STATUS = 0x0201, RBBM_INT_CLEAR_CMD = 0x0037, RBBM_INT_0_MASK = 0x0038,
 *   CP_INTERRUPT_STATUS = 0x0823, CP_PROTECT_STATUS = 0x0824. */
enum : uint32_t {
    IrqRbbmStatus = 0x0201,
    IrqRbbmClear = 0x0037,
    IrqRbbmMask = 0x0038,
    IrqCpStatus = 0x0823,
    IrqCpProtect = 0x0824,
    IrqBitIdle = 1u << 0,
    IrqBitAhb = 1u << 1,
    IrqBitAtbAsync = 1u << 6,
    IrqBitGpc = 1u << 7,
    IrqBitCpSw = 1u << 8,
    IrqBitCpHw = 1u << 9,
    IrqBitCpCcuDepth = 1u << 10,
    IrqBitCpCcuColor = 1u << 11,
    IrqBitCpCcuResolve = 1u << 12,
    IrqBitCpIb2 = 1u << 13,
    IrqBitCpIb1 = 1u << 14,
    IrqBitCpRb = 1u << 15,
    IrqBitCpRbDone = 1u << 17,
    IrqBitCpWtDone = 1u << 18,
    IrqBitCpFence = 1u << 20,
    IrqBitAtbBus = 1u << 22,
    IrqBitHang = 1u << 23,
    IrqBitUcheOob = 1u << 24,
    IrqBitUcheTrap = 1u << 25,
    IrqBitIsdbCpu = 1u << 30,
    IrqBitIsdbDebug = 1u << 31
};

static constexpr uint32_t kKnown = IrqBitIdle | IrqBitAhb | IrqBitAtbAsync | IrqBitGpc |
    IrqBitCpSw | IrqBitCpHw | IrqBitCpCcuDepth | IrqBitCpCcuColor | IrqBitCpCcuResolve |
    IrqBitCpIb2 | IrqBitCpIb1 | IrqBitCpRb | IrqBitCpRbDone | IrqBitCpWtDone |
    IrqBitCpFence | IrqBitAtbBus | IrqBitHang | IrqBitUcheOob | IrqBitUcheTrap |
    IrqBitIsdbCpu | IrqBitIsdbDebug;

/* Exact Linux A6XX_INT_MASK (0x03D0E242): includes CP_CACHE_FLUSH_TS (bit 20). */
static constexpr uint32_t kEnable = IrqBitAhb | IrqBitAtbAsync | IrqBitCpHw |
    IrqBitCpIb2 | IrqBitCpIb1 | IrqBitCpRb | IrqBitCpFence | IrqBitAtbBus |
    IrqBitHang | IrqBitUcheOob | IrqBitUcheTrap;

/* A6XX CP_INTERRUPT_STATUS error bits (OPCODE, UCODE, HW_FAULT, PROTECT, AHB, etc.). */
static constexpr uint32_t kCpErrors = 0x3FF7u;

IOReturn NativeAdrenoIrq::poison()
{
    poisoned_ = true;
    return kIOReturnIOError;
}

uint32_t NativeAdrenoIrq::mask() { return kEnable; }
uint32_t NativeAdrenoIrq::cpErrorMask() { return kCpErrors; }

IOReturn NativeAdrenoIrq::attach(const NativeAdrenoRingIo *io)
{
    if (attached_ || poisoned_) return kIOReturnBusy;
    if (!io || !io->read32 || !io->write32 || !io->barrier) return kIOReturnBadArgument;
    io_ = *io;
    attached_ = true;
    enabled_ = 0;
    return kIOReturnSuccess;
}

IOReturn NativeAdrenoIrq::detach()
{
    if (poisoned_ || !attached_) return kIOReturnNotReady;
    attached_ = false;
    io_ = {};
    enabled_ = 0;
    return kIOReturnSuccess;
}

IOReturn NativeAdrenoIrq::programMask(uint32_t *programmed)
{
    if (!attached_ || poisoned_) return kIOReturnNotReady;
    if (programmed) *programmed = 0;
    uint32_t readback = 0;
    if (io_.write32(io_.cookie, IrqRbbmMask, kEnable)) return poison();
    if (io_.read32(io_.cookie, IrqRbbmMask, &readback)) return poison();
    if (readback != kEnable) return poison();
    enabled_ = readback;
    if (programmed) *programmed = readback;
    return kIOReturnSuccess;
}

IOReturn NativeAdrenoIrq::handle(NativeAdrenoIrqResult *result)
{
    if (!result) return kIOReturnBadArgument;
    *result = {};
    if (!attached_ || poisoned_) return kIOReturnNotReady;
    uint32_t status = 0;
    if (io_.read32(io_.cookie, IrqRbbmStatus, &status)) return poison();
    if (io_.write32(io_.cookie, IrqRbbmClear, status)) return poison();
    if (io_.barrier(io_.cookie)) return poison();
    uint32_t flags = 0, cp_status = 0, protect = 0;
    if (status & IrqBitCpFence) flags |= NativeAdrenoIrqFence;
    if (status & IrqBitCpSw) flags |= NativeAdrenoIrqPreempt;
    if (status & IrqBitIdle) flags |= NativeAdrenoIrqIdle;
    if (status & IrqBitHang) flags |= NativeAdrenoIrqHang;
    if (status & (IrqBitUcheOob | IrqBitUcheTrap)) flags |= NativeAdrenoIrqUche;
    if (status & (IrqBitAhb | IrqBitGpc | IrqBitAtbAsync | IrqBitAtbBus)) flags |= NativeAdrenoIrqBusError;
    if (status & IrqBitCpHw) {
        if (io_.read32(io_.cookie, IrqCpStatus, &cp_status)) return poison();
        if (cp_status & kCpErrors) {
            if (io_.read32(io_.cookie, IrqCpProtect, &protect)) return poison();
            flags |= NativeAdrenoIrqCpError;
        }
    }
    const uint32_t unknown = status & ~kKnown;
    if (unknown) flags |= NativeAdrenoIrqUnknown;
    if (flags & (NativeAdrenoIrqHang | NativeAdrenoIrqCpError | NativeAdrenoIrqUche |
                 NativeAdrenoIrqBusError | NativeAdrenoIrqSecurity | NativeAdrenoIrqUnknown)) {
        flags |= NativeAdrenoIrqFatal;
    }
    result->flags = flags;
    result->status = status;
    result->unknown = unknown;
    result->cp_status = cp_status;
    result->protect_status = protect;
    return kIOReturnSuccess;
}
