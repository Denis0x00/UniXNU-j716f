// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <stdint.h>

namespace J716fDisplay {
static constexpr uint64_t kReservationBase = 0x9c000000ULL;
static constexpr uint64_t kReservationBytes = 0x02400000ULL;
static constexpr uint64_t kVisibleBytes = 0x00fa0000ULL;

struct Video {
    uint64_t base, rowBytes, width, height, depth, rotation;
    uint64_t offset = 0, length = 0;
};
enum class Result : uint32_t {
    Untouched = 0, Disabled, Armed, Busy, BadOrder, UnsupportedOption,
    ConsoleUnavailable, InvalidConsole, NoPlatformReservation,
    InvalidReservation, ChangedConsole, NoIOBSD, AllocationFailed,
    InitFailed, RangeFailed, AttachFailed, StartFailed, ResourceReady
};

inline bool equalVideo(const Video &a, const Video &b) {
    return a.base == b.base && a.rowBytes == b.rowBytes && a.width == b.width &&
        a.height == b.height && a.depth == b.depth && a.rotation == b.rotation &&
        a.offset == b.offset && a.length == b.length;
}
inline bool validReservation(uint64_t base, uint64_t length) {
    return base == kReservationBase && length == kReservationBytes &&
        base <= UINT64_MAX - length;
}
inline bool validVideo(const Video &v) {
    if (!v.width || !v.height || v.width > UINT64_MAX / 4 ||
        v.rowBytes < v.width * 4 || v.rowBytes > UINT64_MAX / v.height)
        return false;
    const uint64_t bytes = v.rowBytes * v.height;
    if (v.base > UINT64_MAX - bytes) return false;
    return v.base == kReservationBase && v.width == 2560 && v.height == 1600 &&
        v.depth == 32 && !v.rotation && v.rowBytes == 10240 &&
        bytes == kVisibleBytes && !v.offset &&
        (!v.length || (v.length >= bytes && v.length <= kReservationBytes)) &&
        v.base + bytes <= kReservationBase + kReservationBytes;
}

// One non-spinning transaction gate; a concurrent caller returns immediately.
// This is not an interrupt lock: callers may allocate while owning the gate.
inline bool acquire(volatile uint32_t *gate) {
    uint32_t expected = 0;
    return __atomic_compare_exchange_n(gate, &expected, 1, false,
                                      __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
}
inline void release(volatile uint32_t *gate) {
    __atomic_store_n(gate, 0, __ATOMIC_RELEASE);
}

// The kernel implementation and host failure-injection tests use this same
// transaction. Ops owns its local references; success transfers only the nub.
template <class Ops> Result createResource(Ops &ops, const Video &video) {
    bool attached = false, started = false, keep = false;
    Result result = Result::AllocationFailed;
    if (ops.allocate(video.base, video.rowBytes * video.height)) {
        result = Result::InitFailed;
        if (ops.init()) {
            result = Result::RangeFailed;
            if (ops.installRange()) {
                result = Result::AttachFailed;
                attached = ops.attach();
                if (attached) {
                    result = Result::StartFailed;
                    started = ops.start();
                    if (started) { result = Result::ResourceReady; keep = true; }
                }
            }
        }
    }
    // Failed start does not establish start ownership. Only a successful start
    // would warrant stop; a future post-start operation must use this cleanup.
    if (!keep && started) ops.stop();
    if (!keep && attached) ops.detach();
    ops.finish(keep);
    return result;
}
}
