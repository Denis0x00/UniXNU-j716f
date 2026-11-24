/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* TB-J716F (SM8250-AC, Adreno 650 v2.0) A6XX Command Processor Ring 0 implementing
 * upstream drivers/Adreno740/NativeAdrenoRing.h for full ABI compatibility with
 * NativeAdrenoSubmit, NativeAdrenoUserClient, and NativeAdrenoPublish.
 *
 * All PM4 opcodes and event IDs are pinned to Linux drivers/gpu/drm/msm/adreno/a6xx_gpu.c
 * and Mesa src/freedreno/registers/adreno/{adreno_pm4.xml,a6xx.xml}:
 *   CP_EVENT_WRITE (0x46): CACHE_FLUSH_TS (0x04) | IRQ (1<<31), CACHE_INVALIDATE (0x31),
 *                          PC_CCU_INVALIDATE_DEPTH (0x18), PC_CCU_INVALIDATE_COLOR (0x19)
 *   CP_SMMU_TABLE_UPDATE (0x53), CP_SET_PROTECTED_MODE (0x5f), CP_SET_MARKER (0x65),
 *   CP_WAIT_FOR_IDLE (0x26), CP_WAIT_FOR_ME (0x13), CP_WAIT_REG_MEM (0x3c), CP_REG_TO_MEM (0x3e). */
#include "NativeAdrenoRing.h"
#include "AdrenoPM4.h"
#include "NativeAdrenoPreempt.h"
#include <IOKit/IOLib.h>

enum : uint32_t {
    RingBase = 0x0800,           /* 64-bit CP_RB_BASE */
    RingControl = 0x0802,        /* CP_RB_CNTL */
    RingRptrAddress = 0x0804,    /* 64-bit CP_RB_RPTR_ADDR */
    RingRptr = 0x0806,
    RingWptr = 0x0807,
    RingScratch = 0x0885,        /* CP_SCRATCH(2) */
    RingEventWrite = 0x46,       /* CP_EVENT_WRITE on A6XX */
    RingCacheFlushTs = 0x04,     /* CACHE_FLUSH_TS */
    RingCcuInvDepth = 0x18,      /* PC_CCU_INVALIDATE_DEPTH */
    RingCcuInvColor = 0x19,      /* PC_CCU_INVALIDATE_COLOR */
    RingCacheInvalidate = 0x31,  /* CACHE_INVALIDATE (UCHE) */
    RingEventIrq = 1u << 31,
    RingBlockBytes = 32,
    RingWaitForMe = 0x13,
    RingWaitForIdle = 0x26,
    RingWaitRegMem = 0x3c,
    RingWaitWriteEq = 3,
    RingRegToMem = 0x3e,
    RingSmmuTableUpdate = 0x53,
    RingSetProtectedMode = 0x5f,
    RingSetMarker = 0x65,
    RingAlwaysOnCounter = 0x0980,
    RingPerfctrSramInit = 0x050e,
    RingPerfctrSramInitStatus = 0x050f,
    RingStampOffset = 512,
    RingStampWords = 4,
    /* Word counts for A650 framed submissions:
     *   Prologue base: CCU_INV_DEPTH(2) + CCU_INV_COLOR(2) + SET_MARKER(2) = 6 words
     *   Context reset: WAIT_FOR_IDLE(1) + WAIT_FOR_ME(1) + PROT_OFF(2) + SRAM_INIT(2) +
     *                  CACHE_INV(2) + WAIT_SRAM(7) + PROT_ON(2) + WAIT_IDLE(1) + WAIT_ME(1) = 19 words
     *   Table switch:  CP_SMMU_TABLE_UPDATE(5) = 5 words
     *   Framed fence:  SET_MARKER(2) + SCRATCH(2) + EVENT_WRITE CACHE_FLUSH_TS(5) = 9 words
     *   Plain fence:   SCRATCH(2) + EVENT_WRITE CACHE_FLUSH_TS(5) = 7 words */
    RingPrologueWords = 6,
    RingResetWords = 19,
    RingTableWords = 5,
    RingFenceWords = 7,
    RingFramedFenceWords = 9,
    RingTableSnapshotOffset = 128,
    RingPostambleOffset = 64,
    RingBvFenceOffset = 12
};

static uint32_t ilog2(size_t value)
{
    uint32_t result = 0;
    while (value > 1) { value >>= 1; ++result; }
    return result;
}

extern "C" {
volatile uint32_t native_adreno_ring_log[4][4 + 112];
volatile uint32_t native_adreno_ring_log_count;
volatile uint32_t native_adreno_table_switches[8];
volatile uint32_t native_adreno_wptr_fence[4];
}

IOReturn NativeAdrenoRing::poison()
{
    poisoned_ = true;
    return kIOReturnIOError;
}

IOReturn NativeAdrenoRing::writeWptr(uint32_t value)
{
    native_adreno_wptr_fence[0] = native_adreno_wptr_fence[0] + 1;
    if (io_.write32(io_.cookie, RingWptr, value)) return poison();
    if (!io_.fence_status) return kIOReturnSuccess;
    uint32_t status = 0;
    for (unsigned tries = 0;; ++tries) {
        if (io_.barrier(io_.cookie) || io_.fence_status(io_.fence_cookie, 0, &status)) return poison();
        if (!(status & 1u)) {
            if (tries) native_adreno_wptr_fence[1] = native_adreno_wptr_fence[1] + 1;
            return kIOReturnSuccess;
        }
        if (tries == 200) {
            native_adreno_wptr_fence[3] = native_adreno_wptr_fence[3] + 1;
            (void)io_.fence_status(io_.fence_cookie, 1, &status);
            return kIOReturnSuccess;
        }
        native_adreno_wptr_fence[2] = native_adreno_wptr_fence[2] + 1;
        (void)io_.fence_status(io_.fence_cookie, 1, &status);
        if (io_.write32(io_.cookie, RingWptr, value)) return poison();
    }
}

IOReturn NativeAdrenoRing::readRptr(uint32_t *value)
{
    uint32_t rptr = 0;
    if (secondary_ || preempt_) {
        if (cache_.acquire(cache_.cookie, fence_, fence_bytes_)) return poison();
        rptr = fence_[0];
    } else if (io_.read32(io_.cookie, RingRptr, &rptr)) {
        return poison();
    }
    if (rptr >= ring_bytes_ / 4) return poison();
    *value = rptr;
    rptr_shadow_ = rptr;
    return kIOReturnSuccess;
}

IOReturn NativeAdrenoRing::ibBefore(uint32_t rptr, uint64_t *iova, uint32_t *dwords) const
{
    if (!ring_ || !iova || !dwords) return kIOReturnBadArgument;
    const uint32_t mask = (uint32_t)(ring_bytes_ / 4 - 1);
    const uint32_t header = ring_[(rptr - 4) & mask];
    if ((header >> 28) != 7u || ((header >> 16) & 0x7fu) != 0x3fu || (header & 0x3fffu) != 3)
        return kIOReturnUnsupported;
    *iova = (uint64_t)ring_[(rptr - 2) & mask] << 32 | ring_[(rptr - 3) & mask];
    *dwords = ring_[(rptr - 1) & mask] & 0xfffffu;
    return kIOReturnSuccess;
}

IOReturn NativeAdrenoRing::stallState(uint32_t *out)
{
    /* Only A650-valid registers (no A7XX BV registers 0x0a66..0x0a74). */
    static const uint32_t regs[16] = {
        0x0210, 0x0211, 0x0212, 0x0213, 0x0201, 0x0821, 0x0825, 0x0824,
        RingRptr, RingWptr, 0x0928, 0x0929, 0x092a, 0x092b, 0x092c, 0x092d
    };
    if (!out) return kIOReturnBadArgument;
    for (uint32_t i = 0; i < 16; ++i) {
        if (io_.read32(io_.cookie, regs[i], &out[i])) return poison();
    }
    for (uint32_t i = 16; i < NativeAdrenoRingStallWords; ++i) out[i] = 0;
    return kIOReturnSuccess;
}

IOReturn NativeAdrenoRing::kick()
{
    if (!attached_ || poisoned_) return kIOReturnNotReady;
    if (!current_) { restore_wptr_ = true; return kIOReturnSuccess; }
    if (io_.barrier(io_.cookie)) return poison();
    return writeWptr(next_);
}

IOReturn NativeAdrenoRing::drainBus(uint32_t *ack)
{
    enum : uint32_t { GbifHalt = 0x3c45, GbifHaltAck = 0x3c46, ClientHalt = 1u, ArbHalt = 2u };
    if (!attached_ || poisoned_ || !ack) return kIOReturnNotReady;
    ack[0] = ack[1] = 0;
    if (io_.write32(io_.cookie, GbifHalt, ClientHalt)) return poison();
    for (unsigned i = 0; i < 20000 && !(ack[0] & ClientHalt); ++i)
        if (io_.read32(io_.cookie, GbifHaltAck, &ack[0])) return poison();
    if (io_.write32(io_.cookie, GbifHalt, ClientHalt | ArbHalt)) return poison();
    for (unsigned i = 0; i < 20000 && !(ack[1] & ArbHalt); ++i)
        if (io_.read32(io_.cookie, GbifHaltAck, &ack[1])) return poison();
    if (io_.write32(io_.cookie, GbifHalt, 0)) return poison();
    return kIOReturnSuccess;
}

static size_t packet(uint32_t *words, size_t at, uint32_t opcode, const uint32_t *payload, uint32_t count)
{
    if (AdrenoPM4Type7(opcode, count, &words[at]) != AdrenoPM4Success) return 0;
    for (uint32_t i = 0; i < count; ++i) words[at + 1 + i] = payload[i];
    return at + 1 + count;
}

static size_t stamp(uint32_t *words, size_t at, uint64_t fence_iova, uint32_t seqno, bool end)
{
    const uint64_t to = fence_iova + RingStampOffset + (uint64_t)(seqno & 63) * 16 + (end ? 8 : 0);
    const uint32_t payload[3] = { RingAlwaysOnCounter | (2u << 18) | (1u << 30), (uint32_t)to, (uint32_t)(to >> 32) };
    return packet(words, at, RingRegToMem, payload, 3);
}

IOReturn NativeAdrenoRing::emitPrologue(uint32_t at, bool reset, bool table, uint32_t *end, uint32_t number)
{
    const uint32_t mask = (uint32_t)(ring_bytes_ / 4 - 1);
    uint32_t words[RingPrologueWords + RingResetWords + RingTableWords + RingStampWords] = {0};
    if (table && !reset) return poison();

    const uint32_t count = RingPrologueWords + (reset ? RingResetWords : 0) +
                           (table ? RingTableWords : 0) + (stamps_ ? RingStampWords : 0);
    size_t n = 0;

    if (reset) {
        const uint32_t prot_off = 0, prot_on = 1, inv = RingCacheInvalidate;
        const uint32_t sram[6] = { RingWaitWriteEq, RingPerfctrSramInitStatus, 0, 1, 1, 0 };
        n = packet(words, n, RingWaitForIdle, nullptr, 0);
        if (n) n = packet(words, n, RingWaitForMe, nullptr, 0);
        if (n) n = packet(words, n, RingSetProtectedMode, &prot_off, 1);
        if (n && AdrenoPM4Type4(RingPerfctrSramInit, 1, &words[n]) == AdrenoPM4Success) {
            words[n + 1] = 1;
            n += 2;
        } else {
            n = 0;
        }
        if (table) {
            const uint32_t update[4] = { (uint32_t)table_, (uint32_t)(table_ >> 32), 0, 0 };
            if (n) n = packet(words, n, RingSmmuTableUpdate, update, 4);
        }
        if (n) n = packet(words, n, RingEventWrite, &inv, 1);
        if (n) n = packet(words, n, RingWaitRegMem, sram, 6);
        if (n) n = packet(words, n, RingSetProtectedMode, &prot_on, 1);
        if (n) n = packet(words, n, RingWaitForIdle, nullptr, 0);
        if (n) n = packet(words, n, RingWaitForMe, nullptr, 0);
    }

    if (stamps_) {
        n = stamp(words, n, fence_iova_, number, false);
    }
    const uint32_t depth = RingCcuInvDepth, color = RingCcuInvColor, marker_off = 0x101;
    if (!reset && !stamps_) {
        n = packet(words, 0, RingEventWrite, &depth, 1);
    } else if (n) {
        n = packet(words, n, RingEventWrite, &depth, 1);
    }
    if (n) n = packet(words, n, RingEventWrite, &color, 1);
    if (n) n = packet(words, n, RingSetMarker, &marker_off, 1);
    if (n != count) return poison();

    for (uint32_t i = 0; i < count; ++i) ring_[(at + i) & mask] = words[i];
    *end = (at + count) & mask;
    return kIOReturnSuccess;
}

IOReturn NativeAdrenoRing::emitFence(uint32_t at, uint32_t seqno, uint32_t *end)
{
    const uint32_t mask = (uint32_t)(ring_bytes_ / 4 - 1);
    uint32_t words[RingFenceWords] = {0};
    if (AdrenoPM4Type4(RingScratch, 1, &words[0]) != AdrenoPM4Success ||
        AdrenoPM4Type7(RingEventWrite, 4, &words[2]) != AdrenoPM4Success)
        return poison();
    words[1] = seqno;
    words[3] = RingCacheFlushTs | RingEventIrq;
    const uint64_t at4 = fence_iova_ + 4;
    words[4] = (uint32_t)at4;
    words[5] = (uint32_t)(at4 >> 32);
    words[6] = seqno;
    for (uint32_t i = 0; i < RingFenceWords; ++i) ring_[(at + i) & mask] = words[i];
    *end = (at + RingFenceWords) & mask;
    return kIOReturnSuccess;
}

IOReturn NativeAdrenoRing::emitFramedFence(uint32_t at, uint32_t seqno, uint32_t *end)
{
    return emitFramedFence(at, seqno, end, false);
}

IOReturn NativeAdrenoRing::emitFramedFence(uint32_t at, uint32_t seqno, uint32_t *end, bool /*yield*/)
{
    const uint32_t mask = (uint32_t)(ring_bytes_ / 4 - 1);
    uint32_t words[RingFramedFenceWords + RingStampWords] = {0};
    const uint32_t total = RingFramedFenceWords + (stamps_ ? RingStampWords : 0);
    size_t n = 0;
    if (stamps_) {
        n = stamp(words, 0, fence_iova_, seqno, true);
        if (!n) return poison();
    }
    const uint32_t marker_on = 0x100;
    n = packet(words, n, RingSetMarker, &marker_on, 1);
    if (!n || AdrenoPM4Type4(RingScratch, 1, &words[n]) != AdrenoPM4Success) return poison();
    words[n + 1] = seqno;
    n += 2;
    const uint64_t at4 = fence_iova_ + 4;
    const uint32_t fence_pkt[4] = { RingCacheFlushTs | RingEventIrq, (uint32_t)at4, (uint32_t)(at4 >> 32), seqno };
    n = packet(words, n, RingEventWrite, fence_pkt, 4);
    if (n != total) return poison();
    for (uint32_t i = 0; i < total; ++i) ring_[(at + i) & mask] = words[i];
    *end = (at + total) & mask;
    return kIOReturnSuccess;
}

IOReturn NativeAdrenoRing::attach(const NativeAdrenoRingIo *io, const NativeAdrenoRingCache *cache,
    void *ring, size_t ring_bytes, uint64_t ring_iova, void *fence, size_t fence_bytes, uint64_t fence_iova)
{
    if (attached_ || poisoned_ || ring_ || fence_) return kIOReturnBusy;
    if (!io || !io->write32 || !io->read32 || !io->barrier || !cache ||
        !cache->publish || !cache->acquire || !ring || !fence ||
        ring_bytes < 8192 || ring_bytes > (1u << 20) || (ring_bytes & (ring_bytes - 1)) ||
        ((ring_iova | ring_bytes) & (ring_bytes - 1)) || (ring_iova & 4095) ||
        fence_bytes < 64 || (fence_iova & 7) || fence_bytes >= (1u << 24))
        return kIOReturnBadArgument;
    io_ = *io;
    cache_ = *cache;
    ring_ = static_cast<volatile uint32_t *>(ring);
    ring_bytes_ = ring_bytes;
    ring_iova_ = ring_iova;
    fence_ = static_cast<volatile uint32_t *>(fence);
    fence_bytes_ = fence_bytes;
    fence_iova_ = fence_iova;
    for (size_t i = 0; i < fence_bytes_ / 4; ++i) fence_[i] = 0;
    control_ = (uint32_t)(ilog2(ring_bytes_ / 8) & 0x3f) | (ilog2(RingBlockBytes / 8) << 8);
    IOReturn status = program();
    if (status) return status;
    seqno_ = 0;
    completed_ = 0;
    submissions_ = 0;
    attached_ = true;
    return kIOReturnSuccess;
}

IOReturn NativeAdrenoRing::attachSecondary(const NativeAdrenoRingIo *, const NativeAdrenoRingCache *,
    void *, size_t, uint64_t, void *, size_t, uint64_t)
{
    return kIOReturnUnsupported;
}

IOReturn NativeAdrenoRing::setPreempt(uint64_t, uint64_t, uint64_t)
{
    return kIOReturnUnsupported;
}

void NativeAdrenoRing::initPreemptPage() { (void)bv_rptr_iova_; }

IOReturn NativeAdrenoRing::resetExecutedTable(uint64_t) { return kIOReturnUnsupported; }
IOReturn NativeAdrenoRing::executedTable(uint64_t *ttbr0) { if (ttbr0) *ttbr0 = 0; return kIOReturnUnsupported; }
IOReturn NativeAdrenoRing::writePreemptRecord(uint8_t *) const { return kIOReturnUnsupported; }
IOReturn NativeAdrenoRing::writePreemptWptr(uint8_t *) const { return kIOReturnUnsupported; }

IOReturn NativeAdrenoRing::restoreWptr()
{
    if (!attached_ || poisoned_) return kIOReturnNotReady;
    if (!restore_wptr_) return kIOReturnSuccess;
    if (writeWptr(next_)) return kIOReturnIOError;
    restore_wptr_ = false;
    return kIOReturnSuccess;
}

IOReturn NativeAdrenoRing::program()
{
    const uint32_t control = control_ ? control_ : (uint32_t)(ilog2(ring_bytes_ / 8) & 0x3f) | (ilog2(RingBlockBytes / 8) << 8);
    const uint32_t base_lo = (uint32_t)ring_iova_, base_hi = (uint32_t)(ring_iova_ >> 32);
    const uint32_t rptr_lo = (uint32_t)fence_iova_, rptr_hi = (uint32_t)(fence_iova_ >> 32);
    const uint32_t writes[] = {
        RingBase, base_lo, RingBase + 1, base_hi, RingControl, control,
        RingRptrAddress, rptr_lo, RingRptrAddress + 1, rptr_hi, RingWptr, 0, RingScratch, 0
    };
    for (size_t i = 0; i < sizeof(writes) / sizeof(writes[0]); i += 2)
        if (io_.write32(io_.cookie, writes[i], writes[i + 1])) return poison();
    uint32_t rptr = 0;
    IOReturn status = readRptr(&rptr);
    if (status) return status;
    next_ = rptr;
    return kIOReturnSuccess;
}

IOReturn NativeAdrenoRing::restart(uint32_t completed)
{
    if (!attached_) return kIOReturnNotReady;
    poisoned_ = false;
    for (size_t i = 0; i < fence_bytes_ / 4; ++i) fence_[i] = 0;
    fence_[1] = completed;
    fence_[RingBvFenceOffset / 4] = completed;
    for (size_t i = 0; i < ring_bytes_ / 4; ++i) ring_[i] = 0;
    if (cache_.publish(cache_.cookie, fence_, fence_bytes_) || cache_.publish(cache_.cookie, ring_, ring_bytes_))
        return poison();
    IOReturn status = program();
    if (status) return status;
    seqno_ = completed;
    completed_ = completed;
    context_ = 0;
    reset_pending_ = true;
    table_ = 0;
    table_pending_ = false;
    return kIOReturnSuccess;
}

IOReturn NativeAdrenoRing::restartSecondary(uint32_t) { return kIOReturnUnsupported; }

uint32_t NativeAdrenoRing::timeline(uint32_t fence, uint64_t *start, uint64_t *end)
{
    if (!start || !end) return 0;
    *start = *end = 0;
    uint32_t status = (attached_ ? 1u : 0u) | (stamps_ ? 2u : 0u) | (fence_bytes_ >= RingStampOffset + 64 * 16 ? 4u : 0u);
    if (status != 7) return status;
    if (cache_.acquire(cache_.cookie, fence_, fence_bytes_)) return status;
    status |= 8;
    const volatile uint32_t *slot = fence_ + (RingStampOffset + (fence & 63) * 16) / 4;
    *start = (uint64_t)slot[1] << 32 | slot[0];
    *end = (uint64_t)slot[3] << 32 | slot[2];
    return status | 16;
}

IOReturn NativeAdrenoRing::fenceValue(uint32_t *value)
{
    if (!value) return kIOReturnBadArgument;
    *value = 0;
    if (!attached_) return kIOReturnNotReady;
    if (cache_.acquire(cache_.cookie, fence_, fence_bytes_)) return kIOReturnIOError;
    *value = fence_[1];
    return kIOReturnSuccess;
}

IOReturn NativeAdrenoRing::submitUnfenced(const uint32_t *words, size_t word_count)
{
    if (!attached_ || poisoned_) return kIOReturnNotReady;
    if (!words || !word_count) return kIOReturnBadArgument;
    const uint32_t size_words = (uint32_t)(ring_bytes_ / 4), mask = size_words - 1;
    if (word_count > size_words - 8) return kIOReturnNoResources;
    uint32_t rptr = 0;
    IOReturn status = readRptr(&rptr);
    if (status) return status;
    if (word_count + 8 > size_words - ((next_ - rptr) & mask)) return kIOReturnNoResources;
    for (size_t i = 0; i < word_count; ++i) ring_[(next_ + i) & mask] = words[i];
    const uint32_t end = (next_ + (uint32_t)word_count) & mask;
    if (cache_.publish(cache_.cookie, ring_, ring_bytes_) || io_.barrier(io_.cookie)) return poison();
    if (writeWptr(end)) return kIOReturnIOError;
    next_ = end;
    return kIOReturnSuccess;
}

IOReturn NativeAdrenoRing::idle(const NativeAdrenoRingClock *clock, uint32_t timeout_us)
{
    /* A6XX RBBM_STATUS bit 23 = GPU_BUSY_IGN_AHB, bit 31 = GPU_BUSY */
    enum : uint32_t { RbbmStatus = 0x0210, GpuBusyIgnAhb = 1u << 23 };
    if (!attached_ || poisoned_) return kIOReturnNotReady;
    if (!clock || !clock->now_us) return kIOReturnBadArgument;
    const uint64_t start = clock->now_us(clock->cookie);
    for (;;) {
        uint32_t rptr = 0, status = 0;
        IOReturn read = readRptr(&rptr);
        if (read) return read;
        if (rptr == next_) {
            if (io_.read32(io_.cookie, RbbmStatus, &status)) return poison();
            if (!(status & GpuBusyIgnAhb)) return kIOReturnSuccess;
        }
        if (clock->now_us(clock->cookie) - start >= timeout_us) return kIOReturnTimeout;
        IODelay(10);
    }
}

IOReturn NativeAdrenoRing::progressState(uint32_t out[6])
{
    enum : uint32_t { Ib1Base = 0x0928, Ib1Rem = 0x092a, Ib2Base = 0x092b, Ib2Rem = 0x092d, RoqAvailIb1 = 0x0949, RoqAvailIb2 = 0x094a };
    if (!out) return kIOReturnBadArgument;
    if (!attached_) return kIOReturnNotReady;
    uint32_t roq1 = 0, roq2 = 0;
    if (io_.read32(io_.cookie, Ib1Base, &out[0]) || io_.read32(io_.cookie, Ib1Base + 1, &out[1]) ||
        io_.read32(io_.cookie, Ib2Base, &out[2]) || io_.read32(io_.cookie, Ib2Base + 1, &out[3]) ||
        io_.read32(io_.cookie, Ib1Rem, &out[4]) || io_.read32(io_.cookie, Ib2Rem, &out[5]) ||
        io_.read32(io_.cookie, RoqAvailIb1, &roq1) || io_.read32(io_.cookie, RoqAvailIb2, &roq2))
        return kIOReturnIOError;
    out[4] += roq1 >> 16;
    out[5] += roq2 >> 16;
    return kIOReturnSuccess;
}

IOReturn NativeAdrenoRing::bvProgressState(uint32_t out[5])
{
    if (!out) return kIOReturnBadArgument;
    for (unsigned i = 0; i < 5; ++i) out[i] = 0;
    return attached_ ? kIOReturnSuccess : kIOReturnNotReady;
}

IOReturn NativeAdrenoRing::busHalt(bool gx_off, unsigned spins)
{
    enum : uint32_t { RbbmGbifHalt = 0x0016, RbbmGbifHaltAck = 0x0017, GbifHalt = 0x3c45, GbifHaltAck = 0x3c46, ClientHalt = 1u, ArbHalt = 2u };
    if (!attached_) return kIOReturnNotReady;
    uint32_t ack = 0;
    unsigned i = 0;
    if (gx_off) {
        if (io_.write32(io_.cookie, RbbmGbifHalt, 1)) return kIOReturnIOError;
        for (i = 0; i < spins; ++i) { if (io_.read32(io_.cookie, RbbmGbifHaltAck, &ack)) return kIOReturnIOError; if (ack & 1) break; }
        if (i == spins) return kIOReturnTimeout;
    }
    if (io_.write32(io_.cookie, GbifHalt, ClientHalt)) return kIOReturnIOError;
    for (i = 0; i < spins; ++i) { if (io_.read32(io_.cookie, GbifHaltAck, &ack)) return kIOReturnIOError; if ((ack & ClientHalt) == ClientHalt) break; }
    if (i == spins) return kIOReturnTimeout;
    if (io_.write32(io_.cookie, GbifHalt, ArbHalt)) return kIOReturnIOError;
    for (i = 0; i < spins; ++i) { if (io_.read32(io_.cookie, GbifHaltAck, &ack)) return kIOReturnIOError; if ((ack & ArbHalt) == ArbHalt) break; }
    if (i == spins) return kIOReturnTimeout;
    return io_.write32(io_.cookie, GbifHalt, 0) ? kIOReturnIOError : kIOReturnSuccess;
}

IOReturn NativeAdrenoRing::detach()
{
    if (poisoned_) return kIOReturnBusy;
    if (!attached_) return kIOReturnNotReady;
    if (outstanding()) return kIOReturnBusy;
    attached_ = false;
    ring_ = nullptr;
    fence_ = nullptr;
    ring_bytes_ = fence_bytes_ = 0;
    ring_iova_ = fence_iova_ = 0;
    next_ = rptr_shadow_ = seqno_ = completed_ = submissions_ = 0;
    return kIOReturnSuccess;
}

IOReturn NativeAdrenoRing::submit(const uint32_t *words, size_t word_count, uint32_t *fence)
{
    if (!fence) return kIOReturnBadArgument;
    return submitWords(words, word_count, seqno_ + 1, false, true, fence);
}

IOReturn NativeAdrenoRing::submitFramed(const uint32_t *words, size_t word_count, uint32_t *fence)
{
    if (!fence) return kIOReturnBadArgument;
    return submitWords(words, word_count, seqno_ + 1, true, current_, fence);
}

IOReturn NativeAdrenoRing::submitFramed(const uint32_t *words, size_t word_count, uint32_t number, uint32_t *issued)
{
    if (!issued) return kIOReturnBadArgument;
    *issued = 0;
    if (!number || (int32_t)(number - seqno_) <= 0) return kIOReturnBadArgument;
    return submitWords(words, word_count, number, true, current_, issued);
}

IOReturn NativeAdrenoRing::submitWords(const uint32_t *words, size_t word_count, uint32_t number, bool framed, bool give_wptr, uint32_t *fence)
{
    if (!fence) return kIOReturnBadArgument;
    *fence = 0;
    if (!attached_ || poisoned_) return kIOReturnNotReady;
    if (!words || !word_count || word_count > SIZE_MAX / sizeof(uint32_t)) return kIOReturnBadArgument;
    const uint32_t size_words = (uint32_t)(ring_bytes_ / 4), mask = size_words - 1;
    const bool reset = framed && reset_pending_, table = reset && table_pending_;
    const uint32_t extra = framed ? RingPrologueWords + (reset ? RingResetWords : 0) +
        (table ? RingTableWords : 0) + RingFramedFenceWords + (stamps_ ? 2 * RingStampWords : 0)
        : RingFenceWords;
    if (word_count > size_words - extra - 8) return kIOReturnNoResources;
    uint32_t rptr = 0;
    IOReturn status = readRptr(&rptr);
    if (status) return status;
    const uint32_t used = (next_ - rptr) & mask;
    if (word_count + extra + 8 > size_words - used) return kIOReturnNoResources;
    uint32_t at = next_;
    if (framed) {
        status = emitPrologue(at, reset, table, &at, number);
        if (status) return status;
    }
    for (size_t i = 0; i < word_count; ++i) ring_[(at + i) & mask] = words[i];
    at = (at + (uint32_t)word_count) & mask;
    uint32_t end = 0;
    status = framed ? emitFramedFence(at, number, &end, false) : emitFence(at, number, &end);
    if (status) return status;
    {
        const uint32_t slot = native_adreno_ring_log_count & 3;
        const uint32_t total = (end - next_) & mask;
        volatile uint32_t *record = native_adreno_ring_log[slot];
        record[0] = number;
        record[1] = next_;
        record[2] = end;
        record[3] = total;
        for (uint32_t i = 0; i < 112; ++i) record[4 + i] = i < total ? ring_[(next_ + i) & mask] : 0;
        native_adreno_ring_log_count = native_adreno_ring_log_count + 1;
    }
    if (cache_.publish(cache_.cookie, ring_, ring_bytes_)) return poison();
    if (io_.barrier(io_.cookie)) return poison();
    if (give_wptr) {
        if (writeWptr(end)) return kIOReturnIOError;
    } else {
        restore_wptr_ = true;
    }
    next_ = end;
    seqno_ = number;
    *fence = number;
    ++submissions_;
    if (reset) reset_pending_ = false;
    if (table) {
        table_pending_ = false;
        native_adreno_table_switches[0] = native_adreno_table_switches[0] + 1;
    }
    return kIOReturnSuccess;
}

NativeAdrenoRingFence NativeAdrenoRing::observeFence(uint32_t fence,
    const NativeAdrenoRingClock *clock, uint32_t timeout_us, uint32_t *observed)
{
    if (observed) *observed = 0;
    if (!attached_ || poisoned_) return NativeAdrenoRingFencePoisoned;
    if (!fence || fence > seqno_) return NativeAdrenoRingFenceArgument;
    uint64_t start = clock && clock->now_us ? clock->now_us(clock->cookie) : 0;
    for (;;) {
        if (cache_.acquire(cache_.cookie, fence_, fence_bytes_)) { poison(); return NativeAdrenoRingFencePoisoned; }
        const uint32_t value = fence_[1];
        if (observed) *observed = value;
        if (value > seqno_) { poison(); return NativeAdrenoRingFencePoisoned; }
        if (value >= fence) return NativeAdrenoRingFenceComplete;
        if (!clock || !clock->now_us) return NativeAdrenoRingFenceTimeout;
        if (clock->now_us(clock->cookie) - start >= timeout_us) return NativeAdrenoRingFenceTimeout;
        if (clock->idle && clock->idle(clock->cookie)) return NativeAdrenoRingFenceTimeout;
    }
}

NativeAdrenoRingFence NativeAdrenoRing::wait(uint32_t fence, const NativeAdrenoRingClock *clock,
    uint32_t timeout_us, uint32_t *observed)
{
    if (observed) *observed = 0;
    if (!attached_ || poisoned_) return NativeAdrenoRingFencePoisoned;
    if (!fence || fence > seqno_) return NativeAdrenoRingFenceArgument;
    uint64_t start = clock && clock->now_us ? clock->now_us(clock->cookie) : 0;
    for (;;) {
        if (cache_.acquire(cache_.cookie, fence_, fence_bytes_)) { poison(); return NativeAdrenoRingFencePoisoned; }
        const uint32_t value = fence_[1];
        if (observed) *observed = value;
        if (value >= fence) {
            if (value > seqno_) { poison(); return NativeAdrenoRingFencePoisoned; }
            completed_ = value;
            return NativeAdrenoRingFenceComplete;
        }
        if (!clock || !clock->now_us) return NativeAdrenoRingFenceTimeout;
        if (clock->now_us(clock->cookie) - start >= timeout_us) return NativeAdrenoRingFenceTimeout;
        if (clock->idle && clock->idle(clock->cookie)) return NativeAdrenoRingFenceTimeout;
    }
}
