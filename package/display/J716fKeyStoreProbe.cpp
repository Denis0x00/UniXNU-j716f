// SPDX-License-Identifier: BSD-2-Clause-Patent
// J716F board glue for unmodified upstream AppleKeyStoreState (NativeKeyStoreEvents.cpp).
// Publishes the ephemeral AppleKeyStore service when offline-key-events=1 is passed.
// With aks=1, answers libaks/MobileKeyBag state queries (unlocked, software-wrapped keys
// derived from /chosen/unique-chip-id; not hardware data protection).
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSMetaClass.h>
#include <libkern/c++/OSSymbol.h>
#include <mach/kmod.h>
#include <pexpert/pexpert.h>
#include <string.h>

extern "C" kmod_info_t g_kernel_kmod_info;
extern "C" void j716f_keyevents_static_init(void) __asm__("__GLOBAL__sub_I_NativeKeyStoreEvents.cpp");
extern "C" IOService *native_keyevents_allocate(void);

extern "C" {
volatile uint32_t j716f_keyevents_stage = 0;
volatile uint32_t j716f_keyevents_refused_opens = 0;
volatile int j716f_keyevents_status = -1;
volatile uint64_t j716f_keyevents_service = 0;
volatile uint32_t j716f_aks_opens = 0;
volatile uint32_t j716f_aks_calls = 0;

void native_keyevents_open_refused(void) {
  __atomic_fetch_add(&j716f_keyevents_refused_opens, 1U, __ATOMIC_RELAXED);
}

bool native_aks_state_enabled(void) {
  static int cached = -1;
  if (cached < 0) {
    unsigned value = 0;
    cached = (PE_parse_boot_argn("aks", &value, sizeof(value)) && value == 1) ? 1 : 0;
  }
  return cached == 1;
}

uint32_t native_aks_lock_state(void) {
  static int cached = -1;
  if (cached < 0) {
    unsigned value = 0;
    if (!PE_parse_boot_argn("aks-sls", &value, sizeof(value)) || value > 7) {
      value = 0;
    }
    cached = static_cast<int>(value);
  }
  return static_cast<uint32_t>(cached);
}

size_t native_aks_device_id(uint8_t *out, size_t capacity) {
  if (!out || capacity < 8) return 0;
  IORegistryEntry *chosen = IORegistryEntry::fromPath("/chosen", gIODTPlane);
  OSData *id = chosen ? OSDynamicCast(OSData, chosen->getProperty("unique-chip-id")) : nullptr;
  size_t n = 0;
  if (id && id->getLength() >= 8 && id->getLength() <= capacity) {
    const uint8_t *bytes = static_cast<const uint8_t *>(id->getBytesNoCopy());
    bool nonzero = false;
    for (unsigned i = 0; i < id->getLength(); ++i) {
      out[i] = bytes[i];
      nonzero |= (bytes[i] != 0);
    }
    if (nonzero) n = id->getLength();
  }
  OSSafeReleaseNULL(chosen);
  return n;
}

void native_aks_opened(void) {
  const uint32_t opens = __atomic_fetch_add(&j716f_aks_opens, 1U, __ATOMIC_RELAXED) + 1U;
  if (opens <= 4) {
    IOLog("[J716F aks] AppleKeyStore user client opened (%u)\n", opens);
  }
}

void native_aks_call(uint32_t selector, int32_t result) {
  const uint32_t calls = __atomic_fetch_add(&j716f_aks_calls, 1U, __ATOMIC_RELAXED) + 1U;
  if (result != 0 && calls <= 32) {
    IOLog("[J716F aks] selector=%u rc=0x%x (calls=%u)\n", selector, static_cast<unsigned>(result), calls);
  }
}

void j716f_keystore_probe(void) {
  unsigned enabled = 0;
  if (!PE_parse_boot_argn("offline-key-events", &enabled, sizeof(enabled)) || enabled != 1 ||
      j716f_keyevents_stage != 0) {
    return;
  }
  j716f_keyevents_stage = 1;
  j716f_keyevents_status = kIOReturnNotPermitted;

  IORegistryEntry *chosen = IORegistryEntry::fromPath("/chosen", gIODTPlane);
  if (!chosen) {
    j716f_keyevents_status = kIOReturnNoDevice;
    IOLog("[J716F aks] /chosen missing\n");
    return;
  }
  OSData *data = OSDynamicCast(OSData, chosen->getProperty("ephemeral-storage"));
  uint32_t ephemeral = 0;
  if (data && data->getLength() == sizeof(ephemeral)) {
    memcpy(&ephemeral, data->getBytesNoCopy(), sizeof(ephemeral));
  }
  OSSafeReleaseNULL(chosen);

  if (ephemeral != 1 && !native_aks_state_enabled()) {
    IOLog("[J716F aks] skipped (ephemeral=%u aks=%u)\n", ephemeral, unsigned(native_aks_state_enabled()));
    return;
  }

  const OSSymbol *name = OSSymbol::withCString("AppleKeyStore");
  const bool absent = name && !OSMetaClass::getMetaClassWithName(name);
  OSSafeReleaseNULL(name);
  if (!absent) {
    IOLog("[J716F aks] AppleKeyStore metaclass already present\n");
    return;
  }

  IOService *provider = IOService::getResourceService();
  if (!provider) {
    IOLog("[J716F aks] IOResources missing\n");
    return;
  }

  void *handle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
  if (!handle) return;
  j716f_keyevents_static_init();
  j716f_keyevents_status = OSMetaClass::postModLoad(handle);
  if (j716f_keyevents_status != kOSReturnSuccess) {
    IOLog("[J716F aks] postModLoad failed status=%d\n", j716f_keyevents_status);
    return;
  }

  j716f_keyevents_stage = 2;
  IOService *service = native_keyevents_allocate();
  const bool initialized = service && service->init();
  const bool attached = initialized && service->attach(provider);
  const bool started = attached && service->start(provider);
  if (!started) {
    if (attached) service->detach(provider);
    OSSafeReleaseNULL(service);
    j716f_keyevents_status = kIOReturnError;
    IOLog("[J716F aks] AppleKeyStore start failed\n");
    return;
  }

  j716f_keyevents_service = reinterpret_cast<uintptr_t>(service);
  j716f_keyevents_status = kIOReturnSuccess;
  j716f_keyevents_stage = 3;
  IOLog("[J716F aks] AppleKeyStore published (aks=%u sls=%u)\n",
        unsigned(native_aks_state_enabled()), native_aks_lock_state());
}
}
