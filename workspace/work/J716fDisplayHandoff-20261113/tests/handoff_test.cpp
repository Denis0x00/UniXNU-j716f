// Tests compile the actual new caller and the unchanged accepted provider.
#include <cassert>
#include <thread>
#include <atomic>
#include <IOKitShim.h>
using ppnum_t = uint32_t;
using pmap_paddr_t = uint64_t;
using boolean_t = int;
extern "C" {
unsigned long gPhysBase = 0xc0000000, gPhysSize = 0x10000000;
pmap_paddr_t avail_start = 0xc8000000, avail_end = 0xd0000000;
pmap_paddr_t vm_first_phys = 0xc0000000, vm_last_phys = 0xd0000000;
}
// mach/vm_param.h declares this before its __BEGIN_DECLS in the real header.
addr64_t first_avail_phys = 0xc4000000;
int PAGE_SHIFT_CONST = 12;
namespace Test {
inline uint64_t managedBad = UINT64_MAX;
inline unsigned queryCount;
inline unsigned changeAt;
inline int changeKind;
inline IOService *platform;
}
extern "C" int pmap_valid_page(ppnum_t ppn) {
    ++Test::queryCount;
    if (Test::queryCount == Test::changeAt) {
        switch (Test::changeKind) {
        case 1: --gPhysSize; break;
        case 2: ++Shim::video.v_rowBytes; break;
        case 3: Test::platform->inactive = true; break;
        case 4: ++Test::platform->id; break;
        }
    }
    return ppn == Test::managedBad;
}
#define PAGE_SIZE (1 << PAGE_SHIFT_CONST)
#include "../../J716fGopAdapter-20261113/package/J716fGopProvider.cpp"
#include "../package/J716fDisplayHandoff.cpp"

static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr,"%s:%d: %s\n",__FILE__,__LINE__,#x); std::abort(); } } while(0)
static void reset() {
    if (activeResource) {
        activeResource->stop(IOService::resources); activeResource->detach(IOService::resources);
        OSSafeReleaseNULL(activeResource);
    }
    releaseReservation();
    if (J716fHandoff::keptOwner) {
        J716fHandoff::keptOwner->stop(Test::platform);
        J716fHandoff::keptOwner->detach(Test::platform);
        OSSafeReleaseNULL(J716fHandoff::keptOwner);
    }
    OSSafeReleaseNULL(Test::platform); OSSafeReleaseNULL(IOService::resources);
    CHECK(Shim::live == 0);
    phase = Phase::Idle; j716fGopTransactionGate = 0; earlyVideo = {};
    J716fHandoff::attempted = false; J716fHandoff::transactionGate = 0;
    J716fHandoff::acceptedBounds = {};
    j716f_display_handoff_calls = j716f_display_handoff_result = j716f_display_handoff_pages_checked = 0;
    j716f_gop_resource_result = j716f_gop_resource_early_calls = j716f_gop_resource_late_calls = 0;
    Shim::fail.clear(); Shim::args.clear(); Shim::consoleError = 0;
    Shim::video = {0x9c000000,10240,2560,1600,32,0};
    Shim::attached = Shim::started = Shim::detached = Shim::stopped = 0;
    gPhysBase = vm_first_phys = 0xc0000000; gPhysSize = 0x10000000;
    avail_start = 0xc8000000; avail_end = vm_last_phys = 0xd0000000;
    first_avail_phys = 0xc4000000;
    PAGE_SHIFT_CONST = 12;
    Test::managedBad = UINT64_MAX; Test::queryCount = Test::changeAt = Test::changeKind = 0;
}
static void setup() {
    reset(); Test::platform = new IOService;
    IOService::resources = new IOService; IOService::resources->bsd = true;
    Shim::args["j716f-gop-resource"] = 1;
}
static void expect(J716fHandoff::Result r) {
    CHECK(j716f_display_handoff_result == static_cast<uint32_t>(r));
}
int main() {
    using HResult = J716fHandoff::Result;
    reset(); j716f_display_handoff_platform_ready(nullptr);
    CHECK(!j716f_display_handoff_calls && !Test::queryCount && !Shim::live);
    for (unsigned flag : {0u,2u,UINT32_MAX}) {
        Shim::args["j716f-gop-resource"] = flag;
        j716f_display_handoff_platform_ready(nullptr);
        CHECK(!j716f_display_handoff_calls && !Test::queryCount);
    }
    setup(); j716f_display_handoff_platform_ready(Test::platform); expect(HResult::Registered);
    CHECK(j716f_display_handoff_pages_checked == 18432 && Test::queryCount == 18432);
    CHECK(J716fHandoff::keptOwner && reservationOwner == J716fHandoff::keptOwner);
    CHECK(J716fHandoff::keptOwner->parent == Test::platform);
    CHECK(platformReservation->size == 0x2400000);
    j716f_display_handoff_platform_ready(Test::platform);
    CHECK(j716f_display_handoff_calls == 1 && Test::queryCount == 18432);
    native_legacy_display_probe(); native_legacy_display_late_probe();
    CHECK(j716f_gop_resource_result == static_cast<uint32_t>(J716fDisplay::Result::ResourceReady));
    CHECK(j716f_gop_display_service_available == 0 && j716f_gop_scanout_available == 0 &&
          j716f_display_handoff_scanout_available == 0);
    CHECK(activeResource->getDeviceMemoryWithIndex(0)->size == 0xfa0000);

    for (int which = 0; which < 3; ++which) {
        setup();
        if (which == 1) Test::platform->inactive = true;
        if (which == 2) Test::platform->id = 0;
        j716f_display_handoff_platform_ready(which == 0 ? nullptr : Test::platform);
        expect(HResult::InvalidPlatform); CHECK(!Test::queryCount && !Shim::attached);
    }
    for (int which = 0; which < 13; ++which) {
        setup();
        switch(which) {
        case 0: gPhysBase = 0x80000000; break;
        case 1: gPhysBase = 0x9c000000; break;
        case 2: gPhysSize = 0; break;
        case 3: gPhysSize = UINT64_MAX; break;
        case 4: ++avail_start; break;
        case 5: avail_start = avail_end + 4096; break;
        case 6: avail_end = 0xe0000000; break;
        case 7: vm_first_phys -= 4096; break;
        case 8: vm_last_phys -= 4096; break;
        case 9: first_avail_phys = 0x9c000000; break;
        case 10: first_avail_phys = avail_start+4096; break;
        case 11: ++first_avail_phys; break;
        case 12: PAGE_SHIFT_CONST = 14; break;
        }
        j716f_display_handoff_platform_ready(Test::platform); expect(HResult::InvalidBounds);
        CHECK(!Test::queryCount && !Shim::attached);
    }
    setup(); Shim::consoleError = 1;
    j716f_display_handoff_platform_ready(Test::platform); expect(HResult::ConsoleUnavailable);
    setup(); Shim::video.v_baseAddr += 4096;
    j716f_display_handoff_platform_ready(Test::platform); expect(HResult::InvalidConsole);
    // Exercise every reservation page as the one unexpectedly managed page.
    for (uint64_t page=0x9c000; page<0x9e400; ++page) {
        setup(); Test::managedBad = page;
        j716f_display_handoff_platform_ready(Test::platform); expect(HResult::ManagedPage);
        CHECK(!Shim::attached && !J716fHandoff::keptOwner);
    }
    for (const char *fault : {"service","memory","array","init","range","setMemory","attach","start"}) {
        setup(); Shim::fail = fault;
        j716f_display_handoff_platform_ready(Test::platform);
        CHECK(j716f_display_handoff_result != static_cast<uint32_t>(HResult::Registered));
        CHECK(!J716fHandoff::keptOwner && !reservationOwner && Shim::live == 2);
        CHECK(Shim::attached == Shim::detached && Shim::started == Shim::stopped);
    }
    for (unsigned at : {9216u,18432u}) for (int change=1; change<=4; ++change) {
        setup(); Test::changeAt=at; Test::changeKind=change;
        j716f_display_handoff_platform_ready(Test::platform); expect(HResult::ChangedEvidence);
        CHECK(!J716fHandoff::keptOwner && !reservationOwner && Shim::live == 2);
        CHECK(Shim::started == 1 && Shim::stopped == 1 && Shim::detached == 1);
    }
    setup(); native_legacy_display_probe(); // Provider already terminal: caller cannot resurrect it.
    j716f_display_handoff_platform_ready(Test::platform); expect(HResult::RegisterRejected);
    CHECK(Shim::live == 2 && Shim::started == Shim::stopped && Shim::attached == Shim::detached);
    setup(); CHECK(J716fDisplay::acquire(&J716fHandoff::transactionGate));
    j716f_display_handoff_platform_ready(Test::platform); CHECK(!j716f_display_handoff_calls);
    J716fDisplay::release(&J716fHandoff::transactionGate);
    const J716fHandoff::Bounds good = {0x9e400000,0x10000000,0x9e800000,0xae400000,
                                      0x9e400000,0xae400000,4096,0x9e600000};
    CHECK(J716fHandoff::validBounds(good));
    auto bad=good; bad.pageSize=16384; CHECK(!J716fHandoff::validBounds(bad));
    bad=good; bad.physicalBase-=4096; CHECK(!J716fHandoff::validBounds(bad));
    bad=good; bad.physicalBase=UINT64_MAX-4095; CHECK(!J716fHandoff::validBounds(bad));
    reset();
    std::printf("handoff checks=%u: PASS (actual caller and frozen provider, host IOKit model)\n",checks);
}
