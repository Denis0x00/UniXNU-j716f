// SPDX-License-Identifier: BSD-2-Clause-Patent
#pragma once
#include <stdint.h>

namespace J716fHandoff {
static constexpr uint64_t kBase = 0x9c000000ULL;
static constexpr uint64_t kBytes = 0x02400000ULL;
static constexpr uint64_t kEnd = kBase + kBytes;
static constexpr uint64_t kPage = 4096;
struct Bounds {
    uint64_t physicalBase, physicalSize, availableStart, availableEnd;
    uint64_t managedFirst, managedLast, pageSize, earlyFirst;
};
enum class Result : uint32_t {
    Untouched = 0, InvalidPlatform, InvalidBounds, ConsoleUnavailable,
    InvalidConsole, ManagedPage, AllocationFailed, InitFailed, RangeFailed,
    AttachFailed, StartFailed, ChangedEvidence, RegisterRejected, Registered
};
inline bool equalBounds(const Bounds &a, const Bounds &b) {
    return a.physicalBase == b.physicalBase && a.physicalSize == b.physicalSize &&
        a.availableStart == b.availableStart && a.availableEnd == b.availableEnd &&
        a.managedFirst == b.managedFirst && a.managedLast == b.managedLast &&
        a.pageSize == b.pageSize && a.earlyFirst == b.earlyFirst;
}
// Deliberately accept only the high-span branch. A disjoint span below GOP is
// not enough for this proof of the monotonic early allocator's lower bound.
// There is no internal-hole support or authority boolean in this interface.
inline bool validBounds(const Bounds &b) {
    if (b.pageSize != kPage || b.physicalBase < kEnd || !b.physicalSize ||
        b.physicalBase > UINT64_MAX - b.physicalSize) return false;
    const uint64_t end = b.physicalBase + b.physicalSize;
    if ((b.physicalBase | b.physicalSize | b.availableStart | b.availableEnd |
         b.managedFirst | b.managedLast | b.earlyFirst) & (kPage - 1)) return false;
    return b.earlyFirst >= b.physicalBase && b.earlyFirst <= b.availableStart &&
        b.availableStart >= b.physicalBase &&
        b.availableStart <= b.availableEnd && b.availableEnd <= end &&
        b.availableEnd > b.physicalBase &&
        b.managedFirst == b.physicalBase && b.managedLast == b.availableEnd;
}
}
