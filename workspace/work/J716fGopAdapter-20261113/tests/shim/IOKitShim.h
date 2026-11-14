// Host-only behavioral model of the narrow IOKit APIs used by the real source.
#pragma once
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>
using IOByteCount = uint64_t;
using addr64_t = uint64_t;
// XNU kern/locks.h declares this global tag. Keep it visible so the host test
// detects accidental collisions with a provider's unqualified variable names.
struct gate;
namespace Shim {
inline std::string fail;
inline int live = 0, attached = 0, started = 0, detached = 0, stopped = 0;
inline uint64_t nextId = 1;
inline std::map<std::string, unsigned> args;
inline int consoleError = 0;
}
class OSObject {
public:
    int refs = 1;
    OSObject() { ++Shim::live; }
    virtual ~OSObject() { --Shim::live; }
    void retain() { ++refs; }
    void release() { if (--refs == 0) delete this; }
};
#define OSSafeReleaseNULL(value) do { if (value) { (value)->release(); (value) = nullptr; } } while (0)
class IODeviceMemory : public OSObject {
public:
    uint64_t base, size, segment;
    IODeviceMemory(uint64_t b, uint64_t n) : base(b), size(n), segment(n) {}
    static IODeviceMemory *withRange(uint64_t b, uint64_t n) {
        return Shim::fail == "memory" ? nullptr : new IODeviceMemory(b, n);
    }
    uint64_t getLength() const { return size; }
    addr64_t getPhysicalSegment(uint64_t, IOByteCount *length) const {
        *length = segment; return base;
    }
};
class OSArray : public OSObject {
public:
    std::vector<OSObject *> items;
    ~OSArray() override { for (auto *p : items) p->release(); }
    static OSArray *withCapacity(unsigned) { return Shim::fail == "array" ? nullptr : new OSArray; }
    bool setObject(OSObject *p) {
        if (Shim::fail == "range") return false;
        p->retain(); items.push_back(p); return true;
    }
};
class IOService : public OSObject {
public:
    inline static IOService *resources = nullptr;
    uint64_t id = Shim::nextId++;
    bool inactive = false, bsd = false;
    OSArray *memory = nullptr;
    IOService *parent = nullptr;
    static void *operator new(size_t n) noexcept {
        return Shim::fail == "service" ? nullptr : std::malloc(n);
    }
    static void operator delete(void *p) noexcept { std::free(p); }
    ~IOService() override { if (memory) memory->release(); }
    bool init() { return Shim::fail != "init"; }
    void setName(const char *) {}
    void setDeviceMemory(OSArray *a) {
        if (Shim::fail == "setMemory") return;
        if (memory) memory->release();
        memory = a; a->retain();
    }
    unsigned getDeviceMemoryCount() const { return memory ? unsigned(memory->items.size()) : 0; }
    IODeviceMemory *getDeviceMemoryWithIndex(unsigned n) const {
        return memory && n < memory->items.size() ? static_cast<IODeviceMemory *>(memory->items[n]) : nullptr;
    }
    bool isInactive() const { return inactive; }
    uint64_t getRegistryEntryID() const { return id; }
    bool attach(IOService *p) {
        if (Shim::fail == "attach") return false;
        parent = p; ++Shim::attached; return true;
    }
    bool start(IOService *) {
        if (Shim::fail == "start") return false;
        ++Shim::started; return true;
    }
    void stop(IOService *) { ++Shim::stopped; }
    void detach(IOService *) { parent = nullptr; ++Shim::detached; }
    OSObject *getProperty(const char *name) {
        return bsd && !std::strcmp(name, "IOBSD") ? this : nullptr;
    }
    static IOService *getResourceService() { return resources; }
};
struct PE_Video {
    uint64_t v_baseAddr, v_rowBytes, v_width, v_height, v_depth, v_rotate;
    uint64_t v_offset = 0, v_length = 0;
};
namespace Shim { inline PE_Video video = {0x9c000000, 10240, 2560, 1600, 32, 0}; }
inline int PE_current_console(PE_Video *v) { *v = Shim::video; return Shim::consoleError; }
inline bool PE_parse_boot_argn(const char *name, void *out, int size) {
    auto at = Shim::args.find(name);
    if (at == Shim::args.end() || size != static_cast<int>(sizeof(unsigned))) return false;
    std::memcpy(out, &at->second, sizeof(unsigned)); return true;
}
inline void IOLog(const char *, ...) {}
