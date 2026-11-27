// SPDX-License-Identifier: BSD-2-Clause-Patent
// J716F board glue for unmodified upstream GopFramebuffer
// (NativeLegacyDisplay.cpp + NativeGOPPresenter.cpp) on iPadOS 17.7.11 (21H461).
// Publishes the UEFI GOP framebuffer (0x9C000000, 2560x1600 BGRX32) as an
// IOMobileFramebufferLegacy provider for CPU scanout (phase 4) and also triggers
// j716f_keystore_probe() during bsd_autoconf().
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <kern/clock.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSMetaClass.h>
#include <libkern/c++/OSString.h>
#include <mach/kmod.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <string.h>
#include <sys/proc.h>

extern "C" kmod_info_t g_kernel_kmod_info;
extern "C" kern_return_t OSKextLoadKextWithIdentifier(const char *identifier);
extern "C" void j716f_legacy_display_static_init(void) __asm__("__GLOBAL__sub_I_NativeLegacyDisplay.cpp");
extern "C" IOService *native_legacy_display_allocate(void);
extern "C" void j716f_gop_presenter_static_init(void) __asm__("__GLOBAL__sub_I_NativeGOPPresenter.cpp");
extern "C" IOService *native_gop_presenter_create(void);
extern "C" bool native_gop_gpu_offer(IOService *presenter, void *display,
                                     IOMemoryDescriptor *memory, uint64_t stride,
                                     uint32_t width, uint32_t height, uint32_t format);
extern "C" void native_gop_presenter_set_source(IOService *presenter, IOService *nub);
extern "C" void j716f_keystore_probe(void);
extern "C" void j716f_hid_input_probe(void);
extern "C" void j716f_usb_debug_probe(void);
extern "C" void j716f_touch_probe(void);
extern "C" void j716f_buttons_probe(void);
extern "C" void j716f_cpufreq_probe(void);
extern "C" void j716f_battery_probe(void);
extern "C" void j716f_gpu_probe(void);
extern "C" void j716f_backlight_probe(void);
extern "C" void j716f_dpu_probe(void);
extern "C" void j716f_wcn_probe(void);

namespace {
IOService *g_pending_display = nullptr;
IOService *g_active_display = nullptr;
IOService *g_active_nub = nullptr;
IOService *g_active_presenter = nullptr;
IOService *g_gpu_handoff_presenter = nullptr;

IORegistryEntry *g_unit_scale_chosen = nullptr;
bool g_unit_scale_added = false;
bool g_unit_rotation_added = false;

void finishUnitScalePolicy(bool keep) {
  if (g_unit_scale_chosen && !keep) {
    if (g_unit_scale_added) g_unit_scale_chosen->removeProperty("display-scale");
    if (g_unit_rotation_added) g_unit_scale_chosen->removeProperty("display-rotation");
  }
  OSSafeReleaseNULL(g_unit_scale_chosen);
  g_unit_scale_added = false;
  g_unit_rotation_added = false;
}

bool publishOrMatchWord(IORegistryEntry *entry, const char *key, uint32_t value, bool &added) {
  OSObject *previous = entry->getProperty(key);
  if (previous) {
    OSData *data = OSDynamicCast(OSData, previous);
    uint32_t word = 0;
    if (!data || data->getLength() != sizeof(word)) return false;
    memcpy(&word, data->getBytesNoCopy(), sizeof(word));
    return word == value;
  }
  OSData *data = OSData::withBytes(&value, sizeof(value));
  const bool ok = data && entry->setProperty(key, data);
  OSSafeReleaseNULL(data);
  added = ok;
  return ok;
}

bool validGopVideo(const PE_Video &video) {
  return video.v_baseAddr != 0 && video.v_width != 0 && video.v_height != 0 &&
         video.v_depth == 32 && video.v_rotate == 0 &&
         video.v_width <= 16384 && video.v_height <= 16384 &&
         video.v_rowBytes >= video.v_width * 4 &&
         video.v_rowBytes <= UINT64_MAX / video.v_height;
}
}  // namespace

extern "C" {
volatile uint32_t j716f_legacy_stage = 0;
volatile int j716f_legacy_meta_status = -1;
volatile uint32_t j716f_legacy_init_result = 0;
volatile uint32_t j716f_legacy_start_result = 0;
volatile uint32_t j716f_legacy_probe_match = 0;
volatile uint32_t j716f_legacy_parent_match = 0;
volatile uint32_t j716f_legacy_dt_attached = 0;
volatile int j716f_legacy_surface_status = -1;
volatile uint32_t j716f_legacy_surface_found = 0;
volatile int j716f_legacy_unit_scale_status = -1;
volatile uint64_t j716f_legacy_object = 0;
volatile uint64_t j716f_legacy_video_base = 0;
volatile uint64_t j716f_legacy_video_bytes = 0;
volatile uint32_t j716f_legacy_frame_count = 0;
volatile uint32_t j716f_legacy_swap_accepted = 0;
volatile uint32_t j716f_legacy_swap_rejected = 0;
volatile uint32_t j716f_legacy_swap_last_reason = 0;
volatile uint32_t j716f_legacy_transfer_calls = 0;
volatile int j716f_legacy_transfer_status = -1;
volatile uint32_t j716f_legacy_client_calls = 0;
volatile int j716f_legacy_client_status = -1;
volatile uint32_t j716f_legacy_gpu_handoff_ready = 0;
volatile uint32_t j716f_legacy_gpu_fallbacks = 0;
volatile int j716f_legacy_gpu_last_status = 0;
volatile uint32_t j716f_legacy_vsync_hz = 0;
volatile uint32_t j716f_legacy_vsync_ticks = 0;
volatile uint32_t j716f_legacy_copy_count = 0;
volatile uint64_t j716f_legacy_copy_ticks_total = 0;
volatile uint64_t j716f_legacy_copy_ticks_max = 0;

bool native_legacy_prepare_scanout(void) {
  return !ml_at_interrupt_context() && ml_get_interrupts_enabled();
}

void native_legacy_pm_observed(unsigned stage, IOReturn status, unsigned long ordinal) {
  if (stage <= 3) {
    IOLog("[J716F fb] pm stage=%u status=0x%x ordinal=%lu\n",
          stage, static_cast<unsigned>(status), ordinal);
  }
}

void native_legacy_surface_rejected(uint64_t width, uint64_t height, uint32_t format,
                                    uint32_t planes, uint64_t stride, uint64_t allocation,
                                    uint64_t bytes) {
  if (j716f_legacy_swap_rejected <= 8) {
    IOLog("[J716F fb] surface rejected %llux%llu fmt=0x%x planes=%u stride=%llu alloc=%llu bytes=%llu\n",
          static_cast<unsigned long long>(width), static_cast<unsigned long long>(height),
          format, planes, static_cast<unsigned long long>(stride),
          static_cast<unsigned long long>(allocation), static_cast<unsigned long long>(bytes));
  }
}

void native_legacy_frame_observed(unsigned kind, uint64_t count, uint64_t minx, uint64_t miny,
                                  uint64_t maxx, uint64_t maxy) {
  const uint32_t n = __atomic_fetch_add(&j716f_legacy_frame_count, 1U, __ATOMIC_RELAXED);
  if (n < 16) {
    IOLog("[J716F fb] frame %u kind=%u lit=%llu box=(%llu,%llu)-(%llu,%llu)\n",
          n, kind, static_cast<unsigned long long>(count),
          static_cast<unsigned long long>(minx), static_cast<unsigned long long>(miny),
          static_cast<unsigned long long>(maxx), static_cast<unsigned long long>(maxy));
  }
}

void native_legacy_frame_colors_observed(const uint32_t *, const uint64_t *, uint64_t) {}

void native_legacy_swap_stage(unsigned) {}

void native_legacy_vsync_observed(unsigned event, unsigned hz) {
  if (hz) j716f_legacy_vsync_hz = hz;
  if (event == 1u) {
    __atomic_fetch_add(&j716f_legacy_vsync_ticks, 1U, __ATOMIC_RELAXED);
  }
}

void native_legacy_swap_validation_observed(bool accepted, unsigned reason, const void *,
                                            const void *) {
  if (accepted) {
    __atomic_fetch_add(&j716f_legacy_swap_accepted, 1U, __ATOMIC_RELAXED);
  } else {
    const uint32_t r = __atomic_fetch_add(&j716f_legacy_swap_rejected, 1U, __ATOMIC_RELAXED) + 1U;
    j716f_legacy_swap_last_reason = reason;
    if (r <= 8) {
      IOLog("[J716F fb] swap rejected reason=%u count=%u\n", reason, r);
    }
  }
}

void native_legacy_copy_timing_observed(uint64_t ticks, uint64_t) {
  __atomic_fetch_add(&j716f_legacy_copy_count, 1U, __ATOMIC_RELAXED);
  __atomic_fetch_add(&j716f_legacy_copy_ticks_total, ticks, __ATOMIC_RELAXED);
  for (uint64_t cur = __atomic_load_n(&j716f_legacy_copy_ticks_max, __ATOMIC_RELAXED);
       ticks > cur && !__atomic_compare_exchange_n(&j716f_legacy_copy_ticks_max, &cur, ticks,
                                                   true, __ATOMIC_RELAXED, __ATOMIC_RELAXED);) {
  }
}

void native_legacy_transfer_observed(IOReturn status, const void *, uint64_t bytes) {
  const uint32_t calls = __atomic_fetch_add(&j716f_legacy_transfer_calls, 1U, __ATOMIC_RELAXED) + 1U;
  j716f_legacy_transfer_status = status;
  if (status != kIOReturnSuccess && calls <= 8) {
    IOLog("[J716F fb] transfer failed status=0x%x bytes=%llu\n",
          static_cast<unsigned>(status), static_cast<unsigned long long>(bytes));
  }
}

bool native_legacy_gpu_offer(void *display, IOMemoryDescriptor *memory, uint64_t stride,
                             uint32_t width, uint32_t height, uint32_t format) {
  IOService *presenter = __atomic_load_n(&g_gpu_handoff_presenter, __ATOMIC_ACQUIRE);
  return presenter != nullptr &&
         native_gop_gpu_offer(presenter, display, memory, stride, width, height, format);
}

void native_legacy_gpu_fallback_observed(int status) {
  __atomic_fetch_add(&j716f_legacy_gpu_fallbacks, 1U, __ATOMIC_RELAXED);
  j716f_legacy_gpu_last_status = status;
}

void native_legacy_retirement_observed(IOService *) {}

void native_legacy_registration_observed(IOService *) {
  IOLog("[J716F fb] NativeLegacyDisplay registered in IORegistry\n");
}

void native_legacy_client_enter(task_t, uint32_t) {
  __atomic_fetch_add(&j716f_legacy_client_calls, 1U, __ATOMIC_RELAXED);
}

void native_legacy_client_observed(IOReturn status, IOUserClient *client) {
  j716f_legacy_client_status = status;
  if (j716f_legacy_client_calls <= 8) {
    IOLog("[J716F fb] user client status=0x%x cls=%s\n",
          static_cast<unsigned>(status),
          client ? client->getMetaClass()->getClassName() : "-");
  }
}

bool native_legacy_gop_mapping_observed(IOService *object, IOMemoryMap *mapping,
                                        uint32_t width, uint32_t height) {
  if (reinterpret_cast<uintptr_t>(object) != j716f_legacy_object) {
    panic("[J716F fb] wrong mapping owner");
  }
  IOLog("[J716F fb] GOP mapped pa=0x%llx len=%llu %ux%u\n",
        static_cast<unsigned long long>(mapping->getPhysicalAddress()),
        static_cast<unsigned long long>(mapping->getLength()), width, height);

  unsigned enabled = 0;
  PE_parse_boot_argn("offline-legacy-unit-scale", &enabled, sizeof(enabled));
  if (!enabled) return true;

  j716f_legacy_unit_scale_status = 1;
  unsigned phase = 0;
  PE_parse_boot_argn("offline-legacy-phase", &phase, sizeof(phase));
  PE_Video video = {};
  if (enabled < 1 || enabled > 3 || phase != 4 || g_unit_scale_chosen ||
      PE_current_console(&video) || video.v_width != width || video.v_height != height ||
      video.v_rotate != 0 || mapping->getPhysicalAddress() != video.v_baseAddr) {
    return false;
  }

  g_unit_scale_chosen = IORegistryEntry::fromPath("/chosen", gIODTPlane);
  if (!g_unit_scale_chosen ||
      !publishOrMatchWord(g_unit_scale_chosen, "display-scale", enabled, g_unit_scale_added) ||
      !publishOrMatchWord(g_unit_scale_chosen, "display-rotation",
                          static_cast<uint32_t>(video.v_rotate), g_unit_rotation_added)) {
    finishUnitScalePolicy(false);
    return false;
  }
  j716f_legacy_unit_scale_status = 0;
  return true;
}

__attribute__((noreturn)) void native_legacy_backend_guard(unsigned slot, void *, void *caller) {
  panic("[J716F fb] NativeLegacyDisplay guarded leaf=%u caller=0x%llx stage=%u",
        slot, static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(caller)),
        j716f_legacy_stage);
}

void native_legacy_display_probe(void) {
  if (j716f_legacy_stage != 0) return;
  j716f_legacy_stage = 1;

  void *handle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
  if (!handle) {
    IOLog("[J716F fb] preModLoad failed\n");
    return;
  }
  j716f_legacy_display_static_init();
  j716f_gop_presenter_static_init();
  j716f_legacy_meta_status = OSMetaClass::postModLoad(handle);
  if (j716f_legacy_meta_status != kOSReturnSuccess) {
    IOLog("[J716F fb] postModLoad failed status=%d\n", j716f_legacy_meta_status);
    return;
  }

  PE_Video video = {};
  j716f_legacy_stage = 2;
  if (PE_current_console(&video) || !validGopVideo(video)) {
    IOLog("[J716F fb] invalid GOP video base=0x%lx %lux%lu depth=%lu row=%lu rot=%u\n",
          video.v_baseAddr, video.v_width, video.v_height, video.v_depth, video.v_rowBytes,
          static_cast<unsigned>(video.v_rotate));
    return;
  }

  IOService *display = native_legacy_display_allocate();
  j716f_legacy_object = reinterpret_cast<uintptr_t>(display);
  if (!display) return;

  j716f_legacy_parent_match =
      (display->metaCast("IOMobileFramebufferLegacy") &&
       display->metaCast("IOMobileFramebuffer") &&
       display->metaCast("IOService")) ? 1U : 0U;
  j716f_legacy_stage = 3;
  j716f_legacy_init_result = display->init() ? 1U : 0U;

  unsigned phase = 1;
  PE_parse_boot_argn("offline-legacy-phase", &phase, sizeof(phase));
  if (j716f_legacy_init_result && (phase == 2 || phase == 3 || phase == 4)) {
    g_pending_display = display;
    IOLog("[J716F fb] NativeLegacyDisplay initialized (phase=%u parent=%u %lux%lu @ 0x%lx)\n",
          phase, j716f_legacy_parent_match, video.v_width, video.v_height, video.v_baseAddr);
    return;
  }

  j716f_legacy_stage = 7;
  display->release();
  j716f_legacy_stage = 8;
}

void native_legacy_display_late_probe(void) {
  // Always publish AppleKeyStore, USB debug, input, and power probes when enabled by boot-args.
  // Run j716f_usb_debug_probe() BEFORE j716f_touch_probe() so synchronous RPMh TCS 0 votes never race.
  j716f_keystore_probe();
  j716f_hid_input_probe();
  j716f_usb_debug_probe();
  j716f_touch_probe();
  j716f_buttons_probe();
  j716f_cpufreq_probe();
  j716f_battery_probe();
  j716f_gpu_probe();
  j716f_backlight_probe();
  j716f_dpu_probe();
  j716f_wcn_probe();

  IOService *display = g_pending_display;
  if (!display) return;
  g_pending_display = nullptr;

  j716f_legacy_surface_status = OSKextLoadKextWithIdentifier("com.apple.iokit.IOSurface");
  OSDictionary *matching = OSDictionary::withCapacity(1);
  {
    const OSObject *names[] = {
        OSString::withCString("IOCoreSurfaceRoot"),
        OSString::withCString("IOSurfaceRoot"),
    };
    OSArray *list = (names[0] && names[1]) ? OSArray::withObjects(names, 2) : nullptr;
    if (!matching || !list || !matching->setObject(gIONameMatchKey, list)) {
      OSSafeReleaseNULL(matching);
    }
    OSSafeReleaseNULL(list);
    for (const OSObject *n : names) {
      if (n) n->release();
    }
  }
  IOService *surface = matching ? IOService::waitForMatchingService(matching, 5000000000ULL) : nullptr;
  j716f_legacy_surface_found = surface != nullptr ? 1U : 0U;
  OSSafeReleaseNULL(surface);
  OSSafeReleaseNULL(matching);

  PE_Video video = {};
  if (j716f_legacy_surface_status == kOSReturnSuccess && j716f_legacy_surface_found &&
      !PE_current_console(&video) && validGopVideo(video)) {
    j716f_legacy_video_base = video.v_baseAddr;
    j716f_legacy_video_bytes = video.v_rowBytes * video.v_height;
    j716f_legacy_stage = 4;

    IOService *nub = new IOService;
    bool nub_attached = false;
    bool nub_started = false;
    bool attached = false;
    bool started = false;
    IORegistryEntry *dt_parent = nullptr;
    bool dt_attached = false;

    IOService *parent = IOService::getResourceService();
    IODeviceMemory *memory =
        IODeviceMemory::withRange(video.v_baseAddr, video.v_rowBytes * video.v_height);
    OSArray *ranges = OSArray::withCapacity(1);

    if (nub && memory && ranges && parent && nub->init()) {
      ranges->setObject(memory);
      nub->setDeviceMemory(ranges);
      nub->setName("NativeGOPFramebuffer");
      nub_attached = nub->attach(parent);
      if (nub_attached) nub_started = nub->start(parent);

      if (nub_started) {
        unsigned phase = 0;
        PE_parse_boot_argn("offline-legacy-phase", &phase, sizeof(phase));
        if (phase == 4) {
          dt_parent = IORegistryEntry::fromPath("/arm-io", gIODTPlane);
          const char type[] = "display-subsystem";
          OSData *type_data = OSData::withBytes(type, sizeof(type));
          if (dt_parent && type_data && nub->setProperty("device_type", type_data)) {
            nub->setName("gop-display", gIODTPlane);
            dt_attached = nub->attachToParent(dt_parent, gIODTPlane);
          }
          OSSafeReleaseNULL(type_data);
          j716f_legacy_dt_attached = dt_attached ? 1U : 0U;
        }

        if (phase == 3) {
          IOService *presenter = native_gop_presenter_create();
          bool presenter_attached = false;
          if (presenter && presenter->init() &&
              (presenter_attached = presenter->attach(nub)) && presenter->start(nub)) {
            g_active_presenter = presenter;
            g_active_nub = nub;
            nub = nullptr;
            nub_attached = false;
            nub_started = false;
          } else {
            if (presenter_attached) presenter->detach(nub);
            OSSafeReleaseNULL(presenter);
          }
        } else {
          SInt32 score = 0;
          j716f_legacy_probe_match =
              ((phase != 4 || dt_attached) && display->probe(nub, &score) == display) ? 1U : 0U;
          if (j716f_legacy_probe_match) attached = display->attach(nub);
          if (attached) {
            j716f_legacy_stage = 5;
            started = display->start(nub);
            j716f_legacy_start_result = started ? 1U : 0U;
          }
          if (started && phase == 4) {
            g_active_display = display;
            g_active_nub = nub;
            j716f_legacy_stage = 10;
            finishUnitScalePolicy(true);
            unsigned fb_gpu = 0;
            if (PE_parse_boot_argn("fb-gpu", &fb_gpu, sizeof(fb_gpu)) && fb_gpu == 1) {
              IOService *handoff = native_gop_presenter_create();
              bool handoff_attached = false;
              if (handoff && handoff->init()) {
                native_gop_presenter_set_source(handoff, nub);
                // Attach away from nub (IOService::getResourceService()) so
                // IOMobileFramebufferLegacy / SpringBoard never enumerates it as a display child.
                handoff_attached = handoff->attach(parent);
                if (handoff_attached && handoff->start(parent)) {
                  __atomic_store_n(&g_gpu_handoff_presenter, handoff, __ATOMIC_RELEASE);
                  j716f_legacy_gpu_handoff_ready = 1U;
                  IOLog("[J716F fb] NativeGOPPresenter GPU handoff active (fb-gpu=1)\n");
                } else {
                  if (handoff_attached) handoff->detach(parent);
                  OSSafeReleaseNULL(handoff);
                  IOLog("[J716F fb] NativeGOPPresenter GPU handoff start failed (CPU fallback)\n");
                }
              } else {
                OSSafeReleaseNULL(handoff);
              }
            }
            OSSafeReleaseNULL(dt_parent);
            OSSafeReleaseNULL(ranges);
            OSSafeReleaseNULL(memory);
            IOLog("[J716F fb] NativeLegacyDisplay published (stage=10 dt=%u scale=%d fb_gpu=%u)\n",
                  j716f_legacy_dt_attached, j716f_legacy_unit_scale_status,
                  j716f_legacy_gpu_handoff_ready);
            return;
          }
        }
      }
    }

    j716f_legacy_stage = 6;
    finishUnitScalePolicy(false);
    if (started) display->stop(nub);
    if (attached) display->detach(nub);
    if (dt_attached && dt_parent) nub->detachFromParent(dt_parent, gIODTPlane);
    if (nub_started) nub->stop(parent);
    if (nub_attached) nub->detach(parent);
    OSSafeReleaseNULL(ranges);
    OSSafeReleaseNULL(memory);
    OSSafeReleaseNULL(nub);
    OSSafeReleaseNULL(dt_parent);
  }

  IOLog("[J716F fb] not published: stage=%u meta=%d init=%u surface=%d/%u probe=%u start=%u dt=%u scale=%d\n",
        j716f_legacy_stage, j716f_legacy_meta_status, j716f_legacy_init_result,
        j716f_legacy_surface_status, j716f_legacy_surface_found, j716f_legacy_probe_match,
        j716f_legacy_start_result, j716f_legacy_dt_attached, j716f_legacy_unit_scale_status);
  j716f_legacy_stage = 7;
  display->release();
  j716f_legacy_stage = 8;
}

// Weak fallback stubs for optional GPU/DPU/trace weak_import hooks referenced
// by unmodified NativeLegacyDisplay.cpp when Adreno/DPU direct scanout is absent.
__attribute__((weak)) bool native_adreno_fences(uint32_t *, uint32_t *) { return false; }
__attribute__((weak)) bool native_adreno_fences_detail(uint32_t *, uint32_t *, uint32_t *) {
  return false;
}
__attribute__((weak)) uint32_t native_ufs_dpu_map(unsigned, const uint64_t *, unsigned) {
  return 0;
}
__attribute__((weak)) volatile uint32_t native_ufs_dpu_state = 0;
__attribute__((weak)) int native_gpu_trace_enabled(void) { return 0; }
__attribute__((weak)) void native_gpu_trace_record(uint32_t, uint32_t, const uint64_t *, uint32_t) {}
}
