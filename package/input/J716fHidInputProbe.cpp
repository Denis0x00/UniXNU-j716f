// SPDX-License-Identifier: BSD-2-Clause-Patent
// TB-J716F board probe for unmodified upstream ShengHidInput (NativeHidInput.cpp).
// Loads IOHIDFamily + IOHIDEventDriver, registers NativeHidInput via late-init staging,
// and publishes Built-In/DisplayIntegrated/Digitizer properties for backboardd.
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <libkern/c++/OSBoolean.h>
#include <libkern/c++/OSIterator.h>
#include <libkern/c++/OSKext.h>
#include <libkern/c++/OSMetaClass.h>
#include <libkern/c++/OSSymbol.h>
#include <mach/kmod.h>
#include <pexpert/pexpert.h>

extern kmod_info_t g_kernel_kmod_info;
extern "C" void j716f_hid_static_init(void) __asm__("__GLOBAL__sub_I_NativeHidInput.cpp");
extern "C" IOService *native_hid_input_allocate(void);
extern "C" IOService *native_hid_touch_allocate(void);
extern "C" kern_return_t OSKextLoadKextWithIdentifier(const char *identifier);
extern "C" volatile uint32_t j716f_legacy_transfer_calls;

extern "C" {
volatile uint32_t j716f_hid_stage = 0;
volatile uint32_t j716f_hid_resource_found = 0;
volatile int j716f_hid_load_status = -1;
volatile int j716f_hid_driver_load_status = -1;
volatile int j716f_hid_meta_status = -1;
volatile uint64_t j716f_hid_object = 0;
volatile uint32_t j716f_hid_events[8] = {};
volatile int j716f_hid_results[8] = {-1, -1, -1, -1, -1, -1, -1, -1};

static unsigned g_auto_configured = 0;
static unsigned g_auto_first = 0;
static unsigned g_auto_count = 1;
static unsigned g_auto_interval = 20;
static unsigned g_auto_sent = 0;
static uint64_t g_auto_next = 0;
static uint64_t g_auto_freq = 0;

static uint64_t counterNow(void) {
  uint64_t v = 0;
  __asm__ volatile("isb\n\tmrs %0, cntpct_el0" : "=r"(v));
  return v;
}

bool native_hid_input_requested(void) {
  if (j716f_legacy_transfer_calls < 2) return false;
  if (!g_auto_configured) {
    g_auto_configured = 1;
    g_auto_first = 0;
    g_auto_count = 1;
    g_auto_interval = 20;
    PE_parse_boot_argn("offline-hid-auto-home", &g_auto_first, sizeof(g_auto_first));
    PE_parse_boot_argn("offline-hid-auto-home-count", &g_auto_count, sizeof(g_auto_count));
    PE_parse_boot_argn("offline-hid-auto-home-interval", &g_auto_interval, sizeof(g_auto_interval));
    if (g_auto_count > 8) g_auto_count = 8;
    if (g_auto_interval < 5) g_auto_interval = 5;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(g_auto_freq));
    if (g_auto_first && g_auto_freq) {
      g_auto_next = counterNow() + static_cast<uint64_t>(g_auto_first) * g_auto_freq;
    }
  }
  if (g_auto_first && g_auto_freq && g_auto_sent < g_auto_count && counterNow() >= g_auto_next) {
    ++g_auto_sent;
    g_auto_next += static_cast<uint64_t>(g_auto_interval) * g_auto_freq;
    return true;
  }
  return false;
}

static IORegistryEntry *findEventDriver(IORegistryEntry *parent, unsigned depth) {
  if (!depth || !parent) return nullptr;
  OSIterator *it = parent->getChildIterator(gIOServicePlane);
  if (!it) return nullptr;
  IORegistryEntry *found = nullptr;
  while (OSObject *child = it->getNextObject()) {
    IORegistryEntry *entry = OSDynamicCast(IORegistryEntry, child);
    if (!entry) continue;
    if (entry->metaCast("IOHIDEventDriver")) {
      found = entry;
      break;
    }
    if ((found = findEventDriver(entry, depth - 1))) break;
  }
  it->release();
  return found;
}

__attribute__((noinline)) void native_hid_live_checkpoint(IOService *device) {
  if (!device) return;
  if (IORegistryEntry *driver = findEventDriver(device, 4)) {
    static IORegistryEntry *published_for = nullptr;
    if (device->getProperty("Built-In") && published_for != driver) {
      published_for = driver;
      driver->setProperty("Built-In", kOSBooleanTrue);
      driver->setProperty("DisplayIntegrated", kOSBooleanTrue);
      driver->setProperty("PrimaryUsagePage", static_cast<unsigned long long>(0x0d), 32);
      driver->setProperty("PrimaryUsage", static_cast<unsigned long long>(4), 32);
      if (IORegistryEntry *iface = driver->getParentEntry(gIOServicePlane)) {
        iface->setProperty("Built-In", kOSBooleanTrue);
        iface->setProperty("DisplayIntegrated", kOSBooleanTrue);
        iface->setProperty("PrimaryUsagePage", static_cast<unsigned long long>(0x0d), 32);
        iface->setProperty("PrimaryUsage", static_cast<unsigned long long>(4), 32);
      }
      IOLog("[J716F hid] IOHIDEventDriver bound with Built-In Digitizer properties\n");
    }
  }
}

void native_hid_input_observed(unsigned event, int result) {
  if (event < 8) {
    const uint32_t prev = j716f_hid_events[event];
    j716f_hid_events[event] = prev + 1U;
    j716f_hid_results[event] = result;
    if ((event == 4 || event == 6) && prev < 3) {
      IOLog("[J716F hid] report event=%u count=%u status=0x%x\n",
            event, prev + 1U, static_cast<unsigned>(result));
    }
  }
}

void j716f_hid_input_probe(void) {
  unsigned enabled = 0;
  if (!PE_parse_boot_argn("offline-hid-input", &enabled, sizeof(enabled)) || enabled != 1) {
    return;
  }
  if (j716f_hid_stage != 0) return;
  j716f_hid_stage = 1;

  j716f_hid_load_status = OSKextLoadKextWithIdentifier("com.apple.iokit.IOHIDFamily");
  if (j716f_hid_load_status == kOSReturnSuccess) {
    j716f_hid_driver_load_status = OSKext::loadKextWithIdentifier(
        "com.apple.iokit.IOHIDEventDriver", true, false,
        kOSKextExcludeNone, kOSKextExcludeNone, nullptr);
  }
  j716f_hid_stage = 2;

  uint32_t hid_dev_size = 0;
  if (const OSSymbol *sym = OSSymbol::withCString("IOHIDDevice")) {
    if (const OSMetaClass *meta = OSMetaClass::getMetaClassWithName(sym)) {
      hid_dev_size = meta->getClassSize();
    }
    sym->release();
  }

  if (j716f_hid_load_status != kOSReturnSuccess) {
    IOLog("[J716F hid] IOHIDFamily load failed rc=0x%x\n",
          static_cast<unsigned>(j716f_hid_load_status));
    return;
  }

  OSDictionary *match = IOService::serviceMatching("IOHIDResource");
  IOService *resource = match ? IOService::waitForMatchingService(match, 5000000000ULL) : nullptr;
  j716f_hid_resource_found = resource != nullptr ? 1U : 0U;

  unsigned send_home = 0;
  PE_parse_boot_argn("offline-hid-home", &send_home, sizeof(send_home));
  if (resource && send_home == 1 && hid_dev_size == 208) {
    void *handle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
    if (handle) {
      j716f_hid_static_init();
      j716f_hid_meta_status = OSMetaClass::postModLoad(handle);
      if (j716f_hid_meta_status == kOSReturnSuccess) {
        IOService *device = native_hid_input_allocate();
        j716f_hid_object = reinterpret_cast<uintptr_t>(device);
        const bool valid = device && device->metaCast("IOHIDDevice") && device->init();
        native_hid_input_observed(0, valid ? 0 : kIOReturnError);
        const bool attached = valid && device->attach(resource);
        native_hid_input_observed(1, attached ? 0 : kIOReturnError);
        const bool started = attached && device->start(resource);
        if (!started) {
          native_hid_input_observed(7, kIOReturnError);
          if (attached) device->detach(resource);
          OSSafeReleaseNULL(device);
          j716f_hid_object = 0;
          IOLog("[J716F hid] NativeHidInput start failed (valid=%u att=%u)\n",
                unsigned(valid), unsigned(attached));
        } else {
          IOLog("[J716F hid] NativeHidInput published (drv_rc=0x%x dev_sz=%u)\n",
                static_cast<unsigned>(j716f_hid_driver_load_status), hid_dev_size);
        }
      } else {
        IOLog("[J716F hid] postModLoad failed rc=%d\n", j716f_hid_meta_status);
      }
    }
  } else {
    IOLog("[J716F hid] skipped res=%u home=%u dev_sz=%u\n",
          j716f_hid_resource_found, send_home, hid_dev_size);
  }
  OSSafeReleaseNULL(resource);
  OSSafeReleaseNULL(match);
  j716f_hid_stage = 3;
}
}
