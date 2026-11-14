// SPDX-License-Identifier: BSD-2-Clause-Patent
// Current-kernel allocator exclusion and metadata registration only.
#include <IOKit/IOService.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOLib.h>
#include <libkern/c++/OSArray.h>
#include <pexpert/pexpert.h>
#include <mach/vm_param.h>
#include <arm/pmap_public.h>
#include "J716fDisplayHandoffCore.h"
#include "J716fDisplayCore.h"

#if defined(KERNEL_INTEGRITY_KTRR) || defined(KERNEL_INTEGRITY_CTRR)
#error This target qualifies only the dynamic avail_start boot allocator.
#endif

extern "C" {
extern unsigned long gPhysBase, gPhysSize;
extern pmap_paddr_t avail_start, avail_end, vm_first_phys, vm_last_phys;
// Exact pinned osfmk/arm/pmap/pmap.h declaration; the implementation is C.
// Use the public type header instead of its internal NBBY macro definitions.
boolean_t pmap_valid_page(ppnum_t pn);
bool j716f_gop_register_platform_reservation(IOService *, IODeviceMemory *);
volatile uint32_t j716f_display_handoff_result;
volatile uint32_t j716f_display_handoff_calls;
volatile uint32_t j716f_display_handoff_pages_checked;
// Metadata authority does not authorize display operation or a cache mapping.
extern const uint32_t j716f_display_handoff_scanout_available = 0;
}

namespace J716fHandoff {
namespace {
volatile uint32_t transactionGate;
bool attempted;
IOService *keptOwner;
Bounds acceptedBounds;

Bounds currentBounds() {
    return {gPhysBase, gPhysSize, avail_start, avail_end,
            vm_first_phys, vm_last_phys, static_cast<uint64_t>(PAGE_SIZE), first_avail_phys};
}
bool readVideo(J716fDisplay::Video &out) {
    PE_Video v = {};
    if (PE_current_console(&v)) return false;
    out = {v.v_baseAddr, v.v_rowBytes, v.v_width, v.v_height, v.v_depth,
           v.v_rotate, v.v_offset, v.v_length};
    return true;
}
bool noManagedPages() {
    for (uint64_t pa = kBase; pa < kEnd; pa += kPage) {
        if (pmap_valid_page(static_cast<ppnum_t>(pa / kPage))) return false;
        j716f_display_handoff_pages_checked = j716f_display_handoff_pages_checked + 1;
    }
    return true;
}
bool wantsValidation() {
    unsigned value = 0;
    return PE_parse_boot_argn("j716f-gop-resource", &value, sizeof(value)) && value == 1;
}
Result registerCurrent(IOService *platform) {
    if (!platform || platform->isInactive() || !platform->getRegistryEntryID())
        return Result::InvalidPlatform;
    const uint64_t platformId = platform->getRegistryEntryID();
    const Bounds bounds = currentBounds();
    if (!validBounds(bounds)) return Result::InvalidBounds;
    J716fDisplay::Video video = {};
    if (!readVideo(video)) return Result::ConsoleUnavailable;
    if (!J716fDisplay::validVideo(video)) return Result::InvalidConsole;
    if (!noManagedPages()) return Result::ManagedPage;

    IOService *owner = new IOService;
    IODeviceMemory *memory = IODeviceMemory::withRange(kBase, kBytes);
    OSArray *ranges = OSArray::withCapacity(1);
    bool attached = false, started = false;
    Result result = Result::AllocationFailed;
    if (owner && memory && ranges) {
        result = Result::InitFailed;
        if (owner->init()) {
            result = Result::RangeFailed;
            if (ranges->setObject(memory)) {
                owner->setName("J716fDisplayReservationMetadata");
                owner->setDeviceMemory(ranges);
                if (owner->getDeviceMemoryCount() == 1 &&
                    owner->getDeviceMemoryWithIndex(0) == memory) {
                    result = Result::AttachFailed;
                    attached = owner->attach(platform);
                    if (attached) {
                        result = Result::StartFailed;
                        started = owner->start(platform);
                        if (started) {
                            J716fDisplay::Video later = {};
                            result = Result::ChangedEvidence;
                            if (!platform->isInactive() &&
                                platform->getRegistryEntryID() == platformId &&
                                equalBounds(bounds, currentBounds()) &&
                                readVideo(later) && J716fDisplay::equalVideo(video, later) &&
                                noManagedPages() &&
                                !platform->isInactive() &&
                                platform->getRegistryEntryID() == platformId &&
                                equalBounds(bounds, currentBounds()) &&
                                readVideo(later) && J716fDisplay::equalVideo(video, later)) {
                                result = Result::RegisterRejected;
                                if (j716f_gop_register_platform_reservation(owner, memory)) {
                                    acceptedBounds = bounds;
                                    keptOwner = owner;
                                    owner = nullptr; // Own one permanent reference until reboot.
                                    result = Result::Registered;
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    if (owner && started) owner->stop(platform);
    if (owner && attached) owner->detach(platform);
    OSSafeReleaseNULL(ranges);
    OSSafeReleaseNULL(memory);
    OSSafeReleaseNULL(owner);
    return result;
}
}
}

extern "C" void j716f_display_handoff_platform_ready(IOService *platform) {
    using namespace J716fHandoff;
    // The patch calls only after the final virtual configure() succeeded.
    // A request flag does not bypass any evidence check. Disabled is silent.
    if (!wantsValidation() || !J716fDisplay::acquire(&transactionGate)) return;
    if (attempted) { J716fDisplay::release(&transactionGate); return; }
    attempted = true;
    j716f_display_handoff_calls = j716f_display_handoff_calls + 1;
    const J716fHandoff::Result result = registerCurrent(platform);
    j716f_display_handoff_result = static_cast<uint32_t>(result);
    IOLog("J716F display handoff: result=%u checked=%u display=0 scanout=0\n",
          static_cast<unsigned>(result), j716f_display_handoff_pages_checked);
    J716fDisplay::release(&transactionGate);
}
