// Compile the actual kernel provider against narrowly modeled host APIs.
#include <cassert>
#include <thread>
#include <atomic>
#include "../package/J716fGopProvider.cpp"

static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); std::abort(); } } while (0)
static void reset() {
    if (activeResource) {
        activeResource->stop(IOService::resources);
        activeResource->detach(IOService::resources);
        OSSafeReleaseNULL(activeResource);
    }
    releaseReservation();
    OSSafeReleaseNULL(IOService::resources);
    CHECK(Shim::live == 0);
    phase = Phase::Idle; j716fGopTransactionGate = 0; earlyVideo = {};
    Shim::fail.clear(); Shim::args.clear(); Shim::consoleError = 0;
    Shim::video = {0x9c000000, 10240, 2560, 1600, 32, 0};
    Shim::attached = Shim::started = Shim::detached = Shim::stopped = 0;
    j716f_gop_resource_result = 0;
    j716f_gop_resource_early_calls = 0;
    j716f_gop_resource_late_calls = 0;
}
static IOService *newOwner(bool validMember = true) {
    IOService *owner = new IOService;
    IODeviceMemory *memory = IODeviceMemory::withRange(kReservationBase, kReservationBytes);
    OSArray *array = OSArray::withCapacity(1);
    if (validMember) CHECK(array->setObject(memory));
    owner->setDeviceMemory(array); array->release(); memory->release();
    return owner;
}
static void registered() {
    auto *owner = newOwner();
    CHECK(j716f_gop_register_platform_reservation(owner, owner->getDeviceMemoryWithIndex(0)));
    owner->release();
    IOService::resources = new IOService; IOService::resources->bsd = true;
    Shim::args["j716f-gop-resource"] = 1;
}
static void expect(Result result) { CHECK(j716f_gop_resource_result == static_cast<uint32_t>(result)); }
int main() {
    reset();
    native_legacy_display_probe(); native_legacy_display_late_probe();
    CHECK(phase == Phase::Idle && Shim::live == 0); // Default off.
    Shim::args["j716f-gop-resource"] = 1;
    native_legacy_display_probe(); expect(Result::NoPlatformReservation);
    native_legacy_display_late_probe(); expect(Result::NoPlatformReservation);
    CHECK(Shim::live == 0);
    reset(); registered();
    CHECK(!j716f_gop_register_platform_reservation(reservationOwner, platformReservation));
    native_legacy_display_probe(); expect(Result::Armed);
    native_legacy_display_probe(); expect(Result::Armed);
    CHECK(!j716f_gop_register_platform_reservation(reservationOwner, platformReservation));
    native_legacy_display_late_probe(); expect(Result::ResourceReady);
    native_legacy_display_late_probe(); expect(Result::ResourceReady);
    CHECK(Shim::attached == 1 && Shim::started == 1 && activeResource);
    CHECK(activeResource->getDeviceMemoryWithIndex(0)->size == kVisibleBytes);
    CHECK(platformReservation->size == kReservationBytes);
    CHECK(j716f_gop_display_service_available == 0 && j716f_gop_scanout_available == 0);
    reset(); registered(); native_legacy_display_late_probe(); expect(Result::BadOrder);
    CHECK(!reservationOwner && !platformReservation);
    for (const char *fault : {"service", "memory", "array", "init", "range", "setMemory", "attach", "start"}) {
        reset(); registered(); native_legacy_display_probe(); expect(Result::Armed);
        Shim::fail = fault; native_legacy_display_late_probe();
        CHECK(phase == Phase::Terminal && !activeResource && !reservationOwner && !platformReservation);
        CHECK(Shim::live == 1); // Only resources service remains.
        CHECK(Shim::attached == Shim::detached);
        CHECK(Shim::started == 0 && Shim::stopped == 0);
        CHECK(j716f_gop_resource_result != static_cast<uint32_t>(Result::ResourceReady));
    }
    for (const char *fault : {"generation", "inactive", "member", "length", "segment", "base", "console", "mode", "bsd", "options"}) {
        reset(); registered(); native_legacy_display_probe(); expect(Result::Armed);
        const std::string name = fault;
        if (name == "generation") ++reservationOwner->id;
        if (name == "inactive") reservationOwner->inactive = true;
        if (name == "member") { reservationOwner->memory->release(); reservationOwner->memory = nullptr; }
        if (name == "length") --platformReservation->size;
        if (name == "segment") --platformReservation->segment;
        if (name == "base") ++platformReservation->base;
        if (name == "console") Shim::consoleError = 1;
        if (name == "mode") ++Shim::video.v_rowBytes;
        if (name == "bsd") IOService::resources->bsd = false;
        if (name == "options") Shim::args["fb-gpu"] = 0; // Presence is rejected.
        native_legacy_display_late_probe();
        CHECK(phase == Phase::Terminal && !activeResource && !reservationOwner && !platformReservation);
        CHECK(Shim::live == 1 && Shim::attached == 0);
    }
    reset();
    auto *owner = newOwner(); auto *other = newOwner();
    CHECK(!j716f_gop_register_platform_reservation(owner, other->getDeviceMemoryWithIndex(0)));
    CHECK(!j716f_gop_register_platform_reservation(nullptr, other->getDeviceMemoryWithIndex(0)));
    owner->inactive = true;
    CHECK(!j716f_gop_register_platform_reservation(owner, owner->getDeviceMemoryWithIndex(0)));
    owner->inactive = false; owner->id = 0;
    CHECK(!j716f_gop_register_platform_reservation(owner, owner->getDeviceMemoryWithIndex(0)));
    owner->release(); other->release();
    Video good = {kReservationBase,10240,2560,1600,32,0};
    CHECK(validVideo(good));
    for (unsigned member = 0; member < 6; ++member) {
        Video bad = good;
        uint64_t *fields[] = {&bad.base,&bad.rowBytes,&bad.width,&bad.height,&bad.depth,&bad.rotation};
        *fields[member] = UINT64_MAX; CHECK(!validVideo(bad));
        *fields[member] = 0; CHECK(member == 5 ? validVideo(bad) : !validVideo(bad));
    }
    CHECK(!validReservation(UINT64_MAX-1, kReservationBytes));
    CHECK(!validReservation(kReservationBase, kVisibleBytes));
    reset();
    CHECK(acquire(&j716fGopTransactionGate)); CHECK(!acquire(&j716fGopTransactionGate)); release(&j716fGopTransactionGate);
    std::atomic<unsigned> inside{0}, races{0};
    auto worker = [&] { for (unsigned n=0;n<20000;++n) if (acquire(&j716fGopTransactionGate)) {
        if (inside.fetch_add(1)) ++races;
        inside.fetch_sub(1); release(&j716fGopTransactionGate);
    }};
    std::thread a(worker), b(worker); a.join(); b.join();
    CHECK(races == 0 && j716fGopTransactionGate == 0);
    reset();
    std::printf("provider checks=%u: PASS (real provider source, host shim only)\n", checks);
}
