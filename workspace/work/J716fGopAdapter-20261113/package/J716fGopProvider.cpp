// SPDX-License-Identifier: BSD-2-Clause-Patent
// Resource metadata only. No mapping, pixel access, display class or userclient.
#include <IOKit/IOService.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOLib.h>
#include <libkern/c++/OSArray.h>
#include <pexpert/pexpert.h>
#include "J716fDisplayCore.h"

using namespace J716fDisplay;
namespace {
volatile uint32_t j716fGopTransactionGate;
enum class Phase : uint32_t { Idle, Armed, Terminal };
Phase phase;
Video earlyVideo;
IOService *reservationOwner;
IODeviceMemory *platformReservation;
IOService *activeResource;
uint64_t reservationOwnerId;

void releaseReservation() {
    OSSafeReleaseNULL(platformReservation);
    OSSafeReleaseNULL(reservationOwner);
    reservationOwnerId = 0;
}
bool currentVideo(Video &out) {
    PE_Video v = {};
    if (PE_current_console(&v)) return false;
    out = {v.v_baseAddr, v.v_rowBytes, v.v_width, v.v_height, v.v_depth, v.v_rotate,
           v.v_offset, v.v_length};
    return true;
}
bool selected() {
    unsigned value = 0;
    return PE_parse_boot_argn("j716f-gop-resource", &value, sizeof(value)) && value == 1;
}
bool incompatibleOptions() {
    const char *const names[] = {"fb-gpu", "offline-legacy-cpu-present",
        "offline-legacy-retire", "offline-legacy-phase", "offline-legacy-unit-scale",
        "offline-gop-present", "offline-dpu", "offline-gpu"};
    for (const char *name : names) {
        unsigned value = 0;
        // Even a malformed value must not accidentally select an alternate path.
        if (PE_parse_boot_argn(name, &value, sizeof(value))) return true;
    }
    return false;
}
bool descriptorMatches(IOService *owner, IODeviceMemory *descriptor) {
    if (!owner || !descriptor || owner->isInactive()) return false;
    const unsigned count = owner->getDeviceMemoryCount();
    if (!count || count > 16) return false;
    bool found = false;
    for (unsigned i = 0; i < count; ++i)
        if (owner->getDeviceMemoryWithIndex(i) == descriptor) found = true;
    if (!found || descriptor->getLength() != kReservationBytes) return false;
    IOByteCount length = 0;
    const addr64_t base = descriptor->getPhysicalSegment(0, &length);
    return validReservation(base, length);
}
bool registeredReservationMatches() {
    return reservationOwner && reservationOwnerId &&
        reservationOwner->getRegistryEntryID() == reservationOwnerId &&
        descriptorMatches(reservationOwner, platformReservation);
}
struct KernelOps {
    IOService *parent;
    IOService *nub = nullptr;
    IODeviceMemory *memory = nullptr;
    OSArray *ranges = nullptr;
    explicit KernelOps(IOService *p) : parent(p) {}
    bool allocate(uint64_t base, uint64_t size) {
        nub = new IOService;
        memory = IODeviceMemory::withRange(base, size);
        ranges = OSArray::withCapacity(1);
        return nub && memory && ranges;
    }
    bool init() { return nub->init(); }
    bool installRange() {
        if (!ranges->setObject(memory)) return false;
        nub->setDeviceMemory(ranges);
        nub->setName("J716fGOPResourceOnly");
        return nub->getDeviceMemoryCount() == 1 &&
            nub->getDeviceMemoryWithIndex(0) == memory;
    }
    bool attach() { return nub->attach(parent); }
    bool start() { return nub->start(parent); }
    void stop() { nub->stop(parent); }
    void detach() { nub->detach(parent); }
    void finish(bool keep) {
        OSSafeReleaseNULL(ranges);
        OSSafeReleaseNULL(memory);
        if (keep) { activeResource = nub; nub = nullptr; }
        OSSafeReleaseNULL(nub);
    }
};
}

extern "C" {
volatile uint32_t j716f_gop_resource_result;
volatile uint32_t j716f_gop_resource_early_calls;
volatile uint32_t j716f_gop_resource_late_calls;
// These are capabilities, not configurable switches. Their zero values are
// independent of resource-registration success.
extern const uint32_t j716f_gop_display_service_available = 0;
extern const uint32_t j716f_gop_scanout_available = 0;

// Trusted platform-only integration API. The caller must own a current-boot
// reservation already excluded from the physical allocator, and keep it valid
// until reboot. A descriptor is checked for consistency; it is NOT evidence of
// allocator exclusion by itself. There is deliberately no caller in this build.
// A user boot argument cannot invoke this API or manufacture the reservation.
bool j716f_gop_register_platform_reservation(IOService *owner, IODeviceMemory *memory) {
    if (!acquire(&j716fGopTransactionGate)) return false;
    const bool valid = phase == Phase::Idle && !platformReservation && owner &&
        owner->getRegistryEntryID() &&
        descriptorMatches(owner, memory);
    if (valid) {
        owner->retain(); memory->retain();
        reservationOwner = owner; platformReservation = memory;
        reservationOwnerId = owner->getRegistryEntryID();
    }
    release(&j716fGopTransactionGate);
    return valid;
}
static void record(Result result) {
    j716f_gop_resource_result = static_cast<uint32_t>(result);
    IOLog("J716F GOP resource-only: result=%u display=0 scanout=0\n",
          static_cast<unsigned>(result));
}
void native_legacy_display_probe(void) {
    if (!selected()) return;
    if (!acquire(&j716fGopTransactionGate)) return;
    j716f_gop_resource_early_calls = j716f_gop_resource_early_calls + 1;
    if (phase != Phase::Idle) { release(&j716fGopTransactionGate); return; }
    Result result = Result::Armed;
    if (incompatibleOptions()) result = Result::UnsupportedOption;
    else if (!currentVideo(earlyVideo)) result = Result::ConsoleUnavailable;
    else if (!validVideo(earlyVideo)) result = Result::InvalidConsole;
    else if (!platformReservation) result = Result::NoPlatformReservation;
    else if (!registeredReservationMatches())
        result = Result::InvalidReservation;
    phase = result == Result::Armed ? Phase::Armed : Phase::Terminal;
    if (phase == Phase::Terminal) releaseReservation();
    record(result); release(&j716fGopTransactionGate);
}
void native_legacy_display_late_probe(void) {
    if (!selected()) return;
    if (!acquire(&j716fGopTransactionGate)) return;
    j716f_gop_resource_late_calls = j716f_gop_resource_late_calls + 1;
    if (phase == Phase::Terminal) { release(&j716fGopTransactionGate); return; }
    Result result = Result::BadOrder;
    if (phase == Phase::Armed) {
        Video video = {};
        IOService *parent = IOService::getResourceService();
        if (incompatibleOptions()) result = Result::UnsupportedOption;
        else if (!currentVideo(video)) result = Result::ConsoleUnavailable;
        else if (!validVideo(video) || !equalVideo(video, earlyVideo))
            result = Result::ChangedConsole;
        else if (!registeredReservationMatches())
            result = Result::InvalidReservation;
        else if (!parent || !parent->getProperty("IOBSD")) result = Result::NoIOBSD;
        else { KernelOps ops(parent); result = createResource(ops, video); }
    }
    phase = Phase::Terminal;
    if (result != Result::ResourceReady) releaseReservation();
    record(result); release(&j716fGopTransactionGate);
}
}
