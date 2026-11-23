// SPDX-License-Identifier: BSD-2-Clause-Patent
// Included in the r41 storage kernel translation unit.
#include <IOKit/IOBSD.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IORegistryEntry.h>
#include <IOKit/IOService.h>
#include <kern/task.h>
#include <kern/thread.h>
#include <libkern/OSKextLib.h>
#include <libkern/c++/OSBoolean.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>
#include <libkern/c++/OSSymbol.h>
#include <libkern/crypto/sha2.h>
#include <libkern/libkern.h>
#include <mach/kmod.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <sys/param.h>
#include <sys/types.h>

#include "J716fSkipSetupData.h"
#include "KnownBlockTrial.h"
#include "R41RootWindowDevice.h"
#include "R41UfsTransport.h"
#include "bootstrap-domain/J716fStorageDomain.h"

extern "C" int get_preemption_level(void);
extern kmod_info_t g_kernel_kmod_info;
extern "C" void j716f_r41_base_init(void) __asm__("__GLOBAL__sub_I_IOBlockStorageDevice.cpp");
extern "C" void j716f_r41_block_init(void) __asm__("__GLOBAL__sub_I_R41RootWindowDevice.cpp");
extern "C" void j716f_ram_block_init(void) __asm__("__GLOBAL__sub_I_NativeRamBlock.cpp");
extern "C" __attribute__((weak)) void (*arm64_platform_sleep_hook)(boolean_t) = nullptr;

namespace {
const OSMetaClass *g_storage_parent = nullptr;
constexpr char kSystemOsUuid[] = "8846FE78-2910-4297-AF97-D59BB6F8FC16";
constexpr char kOsRootUuid[] = "04769D26-2CA0-4825-B443-E2E5F88300F2";
}  // namespace

extern "C" const OSMetaClass *j716f_r41_storage_parent(void) {
  if (!g_storage_parent) {
    panic("J716F r41: storage metaclass initialized before IOStorageFamily");
  }
  return g_storage_parent;
}

namespace j716f_r41_kernel {
using namespace j716f_r41;
namespace domain = j716f_storage_domain;

struct PageMap {
  uint64_t pa = 0;
  uint64_t bytes = 0;
  IOMemoryMap *map = nullptr;
  uintptr_t va = 0;
  bool writable = false;
};

enum MapSlot : unsigned {
  MapRamSlot = 0,
  MapOwnerPage,
  MapGcc,
  MapHci,
  MapSmmuGr0,
  MapSmmuGr1,
  MapSmmuCb2,
  MapSmmuCb3,
  MapCount
};

struct KernelContext {
  PageMap maps[MapCount] = {};
  IOBufferMemoryDescriptor *dma_buf = nullptr;
  sm8250_ufs::DmaView dma = {};
  IOLock *io_lock = nullptr;
  size_t cache_line = 64;
  unsigned live = 0;
};

struct WindowPublishState {
  bool kext_loaded = false;
  OSReturn kext_status = 0;
  bool parent_matched = false;
  uint32_t parent_size = 0;
  uint32_t source_size = 0;
  bool classes_registered = false;
  bool sysos_started = false;
  bool sysos_ready = false;
  uint32_t sysos_ms = 0;
  char sysos_bsd[20] = {};
  bool os_started = false;
  bool os_ready = false;
  uint32_t os_ms = 0;
  char os_bsd[20] = {};
};

KernelContext kctx;
Transport transport;
InitReport g_init_report;
WindowPublishState g_pub;
IOService *g_sysos_device = nullptr;
IOService *g_os_device = nullptr;

bool contextLive(const KernelContext &c) {
  return __atomic_load_n(&c.live, __ATOMIC_ACQUIRE) != 0 &&
         !ml_at_interrupt_context();
}

void copyStr(char *dst, size_t cap, const char *src) {
  if (!dst || !cap) return;
  if (!src) {
    dst[0] = '\0';
    return;
  }
  size_t i = 0;
  for (; i + 1 < cap && src[i]; ++i) dst[i] = src[i];
  dst[i] = '\0';
}

bool strEq(const char *a, const char *b) {
  if (!a || !b) return false;
  while (*a && *b) {
    if (*a != *b) return false;
    ++a;
    ++b;
  }
  return *a == *b;
}

bool mapOne(PageMap &slot, uint64_t pa, uint64_t bytes, bool writable) {
  if (slot.map) return slot.pa == pa && slot.bytes == bytes && slot.writable == writable;
  IODeviceMemory *mem = IODeviceMemory::withRange(pa, bytes);
  if (!mem) return false;
  IOOptionBits opts = kIOMapAnywhere | kIOMapInhibitCache;
  if (!writable) opts |= kIOMapReadOnly;
  IOMemoryMap *m = mem->map(opts);
  mem->release();
  if (!m) return false;
  const uintptr_t va = static_cast<uintptr_t>(m->getVirtualAddress());
  IOByteCount seg = 0;
  if (!va || m->getLength() < bytes || va > UINTPTR_MAX - bytes ||
      m->getAddressTask() != kernel_task ||
      (m->getMapOptions() & kIOMapCacheMask) != kIOMapInhibitCache ||
      m->getPhysicalSegment(0, &seg, 0) != pa || seg < bytes) {
    m->release();
    return false;
  }
  if (!writable && !(m->getMapOptions() & kIOMapReadOnly)) {
    m->release();
    return false;
  }
  if (writable && (m->getMapOptions() & kIOMapReadOnly)) {
    m->release();
    return false;
  }
  slot = {pa, bytes, m, va, writable};
  return true;
}

uintptr_t locate(const KernelContext &c, uint64_t pa, size_t width, bool writing) {
  if (!contextLive(c) || (pa & (width - 1))) return 0;
  for (unsigned i = 0; i < MapCount; ++i) {
    const auto &m = c.maps[i];
    if (!m.map) continue;
    if (pa >= m.pa && m.bytes >= width && pa - m.pa <= m.bytes - width) {
      if (writing && (!m.writable || i != MapHci)) return 0;
      return m.va + static_cast<uintptr_t>(pa - m.pa);
    }
  }
  return 0;
}

bool readRamSlot(void *opaque, R35Record *out_record, uint32_t *out_jown_magic,
                 uint32_t *out_jown_seq) {
  auto &c = *static_cast<KernelContext *>(opaque);
  if (!out_record || !out_jown_magic || !out_jown_seq || !contextLive(c)) return false;
  if (!mapOne(c.maps[MapRamSlot], kRamSlotPhysical, kRamSlotBytes, false) ||
      !mapOne(c.maps[MapOwnerPage], kOwnerPagePhysical, 4096, false))
    return false;
  __asm__ volatile("dsb sy" ::: "memory");
  const volatile uint32_t *owner = reinterpret_cast<const volatile uint32_t *>(
      c.maps[MapOwnerPage].va + kOwnerOffsetInPage);
  *out_jown_magic = owner[0];
  *out_jown_seq = owner[1];
  const volatile uint32_t *src =
      reinterpret_cast<const volatile uint32_t *>(c.maps[MapRamSlot].va);
  uint32_t *dst = reinterpret_cast<uint32_t *>(out_record);
  for (size_t i = 0; i < sizeof(R35Record) / 4; ++i) dst[i] = src[i];
  __asm__ volatile("dsb sy" ::: "memory");
  return true;
}

bool read32(void *opaque, uint64_t pa, uint32_t *out) {
  auto &c = *static_cast<KernelContext *>(opaque);
  if (!out) return false;
  const uintptr_t va = locate(c, pa, 4, false);
  if (!va) return false;
  __asm__ volatile("dsb sy" ::: "memory");
  *out = *reinterpret_cast<const volatile uint32_t *>(va);
  __asm__ volatile("dsb sy" ::: "memory");
  return true;
}

bool read64(void *opaque, uint64_t pa, uint64_t *out) {
  auto &c = *static_cast<KernelContext *>(opaque);
  if (!out) return false;
  const uintptr_t va = locate(c, pa, 8, false);
  if (!va) return false;
  __asm__ volatile("dsb sy" ::: "memory");
  *out = *reinterpret_cast<const volatile uint64_t *>(va);
  __asm__ volatile("dsb sy" ::: "memory");
  return true;
}

bool write32(void *opaque, uint64_t pa, uint32_t val) {
  auto &c = *static_cast<KernelContext *>(opaque);
  if (pa != R35_HCI_BASE + 0x20 && pa != R35_HCI_BASE + 0x50 &&
      pa != R35_HCI_BASE + 0x54 && pa != R35_HCI_BASE + 0x58 &&
      pa != R35_HCI_BASE + 0x60)
    return false;
  const uintptr_t va = locate(c, pa, 4, true);
  if (!va) return false;
  __asm__ volatile("dsb sy" ::: "memory");
  *reinterpret_cast<volatile uint32_t *>(va) = val;
  __asm__ volatile("dsb sy" ::: "memory");
  return true;
}

bool barrier(void *opaque) {
  auto &c = *static_cast<KernelContext *>(opaque);
  if (!contextLive(c)) return false;
  __asm__ volatile("dsb sy" ::: "memory");
  return true;
}

bool ticks(void *opaque, uint64_t *out) {
  auto &c = *static_cast<KernelContext *>(opaque);
  if (!out || !contextLive(c)) return false;
  __asm__ volatile("isb\n\tmrs %0, cntpct_el0" : "=r"(*out) :: "memory");
  return true;
}

bool cache(void *opaque, sm8250_ufs::CacheOp op, size_t off, size_t bytes) {
  auto &c = *static_cast<KernelContext *>(opaque);
  if (!contextLive(c) || !c.dma.cpu || !bytes || off > c.dma.bytes ||
      bytes > c.dma.bytes - off || !c.cache_line || (c.cache_line & (c.cache_line - 1)))
    return false;
  if (op != sm8250_ufs::CacheOp::Clean && op != sm8250_ufs::CacheOp::Invalidate &&
      op != sm8250_ufs::CacheOp::CleanInvalidate)
    return false;
  const uintptr_t start =
      (reinterpret_cast<uintptr_t>(c.dma.cpu) + off) & ~(c.cache_line - 1);
  const uintptr_t end =
      (reinterpret_cast<uintptr_t>(c.dma.cpu) + off + bytes + c.cache_line - 1) &
      ~(c.cache_line - 1);
  const uintptr_t base = reinterpret_cast<uintptr_t>(c.dma.cpu);
  if (start < base || end > base + c.dma.bytes || end <= start) return false;
  __asm__ volatile("dsb sy" ::: "memory");
  for (uintptr_t a = start; a < end; a += c.cache_line) {
    if (op == sm8250_ufs::CacheOp::Clean)
      __asm__ volatile("dc cvac, %0" :: "r"(a) : "memory");
    else if (op == sm8250_ufs::CacheOp::Invalidate)
      __asm__ volatile("dc ivac, %0" :: "r"(a) : "memory");
    else
      __asm__ volatile("dc civac, %0" :: "r"(a) : "memory");
  }
  __asm__ volatile("dsb sy" ::: "memory");
  return true;
}

bool allocateDma(void *opaque, sm8250_ufs::DmaView *out) {
  auto &c = *static_cast<KernelContext *>(opaque);
  if (!out || !contextLive(c) || c.dma_buf || !kernel_task) return false;
  IOBufferMemoryDescriptor *buf = IOBufferMemoryDescriptor::inTaskWithOptions(
      kernel_task,
      static_cast<IOOptionBits>(kIODirectionInOut) |
          static_cast<IOOptionBits>(kIOMemoryPhysicallyContiguous) |
          static_cast<IOOptionBits>(kIOMemoryMapperNone),
      kDmaBytes, 4096);
  if (!buf) return false;
  if (buf->prepare(kIODirectionInOut) != kIOReturnSuccess) {
    buf->release();
    return false;
  }
  IOByteCount seg_len = 0;
  const addr64_t pa = buf->getPhysicalSegment(0, &seg_len, kIOMemoryMapperNone);
  uint8_t *cpu = static_cast<uint8_t *>(buf->getBytesNoCopy());
  if (!pa || (pa & 4095U) || seg_len < kDmaBytes || buf->getLength() != kDmaBytes ||
      !cpu || (reinterpret_cast<uintptr_t>(cpu) & 4095U)) {
    buf->complete(kIODirectionInOut);
    buf->release();
    return false;
  }
  bzero(cpu, kDmaBytes);
  c.dma_buf = buf;
  c.dma = {cpu, static_cast<uint64_t>(pa), kDmaBytes};
  if (!cache(&c, sm8250_ufs::CacheOp::CleanInvalidate, 0, kDmaBytes)) {
    buf->complete(kIODirectionInOut);
    buf->release();
    c.dma_buf = nullptr;
    c.dma = {};
    return false;
  }
  *out = c.dma;
  return true;
}

void lockIo(void *opaque) {
  auto &c = *static_cast<KernelContext *>(opaque);
  if (c.io_lock) IOLockLock(c.io_lock);
}

void unlockIo(void *opaque) {
  auto &c = *static_cast<KernelContext *>(opaque);
  if (c.io_lock) IOLockUnlock(c.io_lock);
}

void pausePoll(void *opaque) {
  auto &c = *static_cast<KernelContext *>(opaque);
  if (contextLive(c)) IOSleep(1);
}

void logFault(void *, const char *stage, uint32_t is, uint32_t db,
              uint32_t tdb, uint32_t rsr, uint32_t polls, uint64_t dt) {
  printf("[J716F r41] UFS fault stage=%s is=%08x db=%08x tdb=%08x rsr=%x polls=%u dt=%llu\n",
         stage ? stage : "-", is, db, tdb, rsr, polls, static_cast<unsigned long long>(dt));
}

bool findUuidMediaInSubtree(IOService *root, const char *wanted_uuid, char *out_bsd, size_t bsd_cap) {
  IORegistryIterator *iter = IORegistryIterator::iterateOver(
      root, gIOServicePlane, kIORegistryIterateRecursively);
  if (!iter) return false;
  bool found = false;
  IORegistryEntry *entry = nullptr;
  while ((entry = iter->getNextObject()) != nullptr) {
    if (!entry->metaCast("IOMedia")) continue;
    OSString *uuid_obj = OSDynamicCast(OSString, entry->getProperty("UUID"));
    const char *uuid = uuid_obj ? uuid_obj->getCStringNoCopy() : nullptr;
    if (!uuid || !strEq(uuid, wanted_uuid)) continue;
    OSString *bsd_obj = OSDynamicCast(OSString, entry->getProperty(kIOBSDNameKey));
    const char *bsd = bsd_obj ? bsd_obj->getCStringNoCopy() : nullptr;
    if (bsd && bsd[0]) {
      copyStr(out_bsd, bsd_cap, bsd);
      found = true;
      break;
    }
  }
  iter->release();
  return found;
}

void dumpSubtreeMedia(const char *label, IOService *root) {
  IORegistryIterator *iter = IORegistryIterator::iterateOver(
      root, gIOServicePlane, kIORegistryIterateRecursively);
  if (!iter) return;
  IORegistryEntry *entry = nullptr;
  unsigned shown = 0;
  while ((entry = iter->getNextObject()) != nullptr && shown < 8) {
    if (!entry->metaCast("IOMedia")) continue;
    OSString *uuid_obj = OSDynamicCast(OSString, entry->getProperty("UUID"));
    OSString *bsd_obj = OSDynamicCast(OSString, entry->getProperty(kIOBSDNameKey));
    printf("[J716F r41] media %s[%u] cls=%s bsd=%s uuid=%s\n",
           label, shown, entry->getMetaClass()->getClassName(),
           (bsd_obj && bsd_obj->getCStringNoCopy()) ? bsd_obj->getCStringNoCopy() : "-",
           (uuid_obj && uuid_obj->getCStringNoCopy()) ? uuid_obj->getCStringNoCopy() : "-");
    ++shown;
  }
  iter->release();
}

void publishRootWindows(WindowPublishState &p) {
  const auto &m = g_init_report.gpt.manifest;
  if (!m.valid || m.systemos_index < 0 || m.os_index < 0) return;

  p.kext_status = OSKextLoadKextWithIdentifier("com.apple.iokit.IOStorageFamily");
  if (p.kext_status != kOSReturnSuccess) return;
  p.kext_loaded = true;

  const OSSymbol *sym = OSSymbol::withCString("IOBlockStorageDevice");
  g_storage_parent = sym ? OSMetaClass::getMetaClassWithName(sym) : nullptr;
  if (sym) sym->release();
  p.parent_size = g_storage_parent ? g_storage_parent->getClassSize() : 0;
  p.source_size = static_cast<uint32_t>(j716f_r41_base_class_size());
  if (!g_storage_parent || p.parent_size != p.source_size) return;
  p.parent_matched = true;

  void *mod_handle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
  if (!mod_handle) return;
  j716f_r41_base_init();
  j716f_r41_block_init();
  j716f_ram_block_init();
  if (OSMetaClass::postModLoad(mod_handle) != kOSReturnSuccess) return;
  p.classes_registered = true;

  OSDictionary *matching = IOService::serviceMatching("IOPlatformExpertDevice");
  IOService *provider = matching ? IOService::waitForMatchingService(matching, 3000000000ULL)
                                 : nullptr;
  if (matching) matching->release();
  if (!provider) return;

  // 1. Publish SystemOS window first (bare APFS container) and wait for its volume UUID.
  const auto &sys_w = m.windows[m.systemos_index];
  IOService *sys_dev = j716f_r41_create_window_device(
      &transport, 0, "SystemOS", sys_w.abs_offset, sys_w.size_bytes);
  if (sys_dev && sys_dev->attach(provider) && sys_dev->start(provider)) {
    g_sysos_device = sys_dev;
    p.sysos_started = true;
    for (unsigned step = 0; step < 100; ++step) {
      IOSleep(100);
      p.sysos_ms = (step + 1) * 100;
      if (findUuidMediaInSubtree(g_sysos_device, kSystemOsUuid, p.sysos_bsd, sizeof(p.sysos_bsd))) {
        p.sysos_ready = true;
        break;
      }
    }
    if (!p.sysos_ready) dumpSubtreeMedia("SystemOS", g_sysos_device);
  } else if (sys_dev) {
    sys_dev->release();
  }

  // 2. Publish OS root window (512B GPT + APFS container) and wait for root UUID.
  const auto &os_w = m.windows[m.os_index];
  IOService *os_dev = j716f_r41_create_window_device(
      &transport, 1, "OS", os_w.abs_offset, os_w.size_bytes);
  if (os_dev && os_dev->attach(provider) && os_dev->start(provider)) {
    g_os_device = os_dev;
    p.os_started = true;
    for (unsigned step = 0; step < 100; ++step) {
      IOSleep(100);
      p.os_ms = (step + 1) * 100;
      if (findUuidMediaInSubtree(g_os_device, kOsRootUuid, p.os_bsd, sizeof(p.os_bsd))) {
        p.os_ready = true;
        break;
      }
    }
    if (!p.os_ready) dumpSubtreeMedia("OS", g_os_device);
  } else if (os_dev) {
    os_dev->release();
  }

  // 3. Optional Stage 7 RAM Data disk: start NativeRamBlock only when DarwinBoot
  // placed a non-zero PackedRAMDisk or RAMDisk range into /chosen/memory-map.
  IORegistryEntry *mem_map = IORegistryEntry::fromPath("/chosen/memory-map", gIODTPlane);
  bool has_data_seed = false;
  if (mem_map) {
    const char *const keys[] = {"PackedRAMDisk", "RAMDisk"};
    for (const char *key : keys) {
      OSData *range = OSDynamicCast(OSData, mem_map->getProperty(key));
      if (range && range->getLength() >= 16) {
        uint64_t words[2] = {};
        memcpy(words, range->getBytesNoCopy(), sizeof(words));
        if (words[0] != 0 && words[1] != 0) {
          has_data_seed = true;
          break;
        }
      }
    }
  }
  OSSafeReleaseNULL(mem_map);
  if (has_data_seed) {
    IOService *ram_disk =
        OSDynamicCast(IOService, OSMetaClass::allocClassWithName("NativeRamBlock"));
    if (ram_disk) {
      if (ram_disk->init() && ram_disk->attach(provider) && ram_disk->start(provider)) {
        printf("[J716F r41] NativeRamBlock Data disk started\n");
        // Give APFS a brief window to publish the Data volume IOMedia before load_init_program().
        for (unsigned step = 0; step < 50; ++step) {
          IOSleep(100);
          IORegistryIterator *it = IORegistryIterator::iterateOver(
              ram_disk, gIOServicePlane, kIORegistryIterateRecursively);
          bool data_ready = false;
          if (it) {
            IORegistryEntry *e = nullptr;
            while ((e = it->getNextObject()) != nullptr) {
              if (!e->metaCast("IOMedia")) continue;
              OSString *u = OSDynamicCast(OSString, e->getProperty("UUID"));
              OSString *b = OSDynamicCast(OSString, e->getProperty(kIOBSDNameKey));
              if (u && u->getLength() == 36 && b && b->getLength() > 0) {
                printf("[J716F r41] NativeRamBlock Data volume ready: bsd=%s uuid=%s (%ums)\n",
                       b->getCStringNoCopy(), u->getCStringNoCopy(), (step + 1) * 100);
                data_ready = true;
                break;
              }
            }
            it->release();
          }
          if (data_ready) break;
        }
      } else {
        printf("[J716F r41] NativeRamBlock Data disk start failed\n");
        ram_disk->release();
      }
    } else {
      printf("[J716F r41] NativeRamBlock class allocation failed\n");
    }
  }

  provider->release();
}

unsigned runPreRoot() {
  domain::Guard guard(domain::shared(), domain::Kind::Bootstrap);
  if (!guard.held()) {
    printf("[J716F r41] blocked=software-domain\n");
    return 1;
  }
  if (ml_at_interrupt_context() || !ml_get_interrupts_enabled() ||
      get_preemption_level() != 0 || current_thread() == nullptr) {
    printf("[J716F r41] blocked=context\n");
    return 2;
  }
  uint64_t freq = 0, ctr = 0;
  __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(freq));
  __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
  if (freq < 1000000 || freq > 1000000000) {
    printf("[J716F r41] blocked=timer-freq freq=%llu\n", (unsigned long long)freq);
    return 3;
  }
  const size_t dline = static_cast<size_t>(4U) << ((ctr >> 16) & 15U);
  if (dline < 16 || dline > 256 || (dline & (dline - 1))) {
    printf("[J716F r41] blocked=ctr-dline dline=%u\n", (unsigned)dline);
    return 4;
  }

  kctx.io_lock = IOLockAlloc();
  if (!kctx.io_lock) {
    printf("[J716F r41] blocked=lock-alloc\n");
    return 5;
  }
  kctx.cache_line = dline;
  __atomic_store_n(&kctx.live, 1U, __ATOMIC_RELEASE);

  const bool mapped =
      mapOne(kctx.maps[MapGcc], 0x00177000ULL, 4096, false) &&
      mapOne(kctx.maps[MapHci], R35_HCI_BASE, 4096, true) &&
      mapOne(kctx.maps[MapSmmuGr0], R35_SMMU_BASE, 4096, false) &&
      mapOne(kctx.maps[MapSmmuGr1], R35_SMMU_BASE + 0x1000ULL, 4096, false) &&
      mapOne(kctx.maps[MapSmmuCb2], R35_SMMU_BASE + 0x82000ULL, 4096, false) &&
      mapOne(kctx.maps[MapSmmuCb3], R35_SMMU_BASE + 0x83000ULL, 4096, false);
  if (!mapped) {
    __atomic_store_n(&kctx.live, 0U, __ATOMIC_RELEASE);
    printf("[J716F r41] blocked=mmio-map\n");
    return 6;
  }

  printf("[J716F r41] begin ro-apfs-root core-next smmu-writes=0 tz=unknown\n");
  const Platform plat{&kctx, readRamSlot, read32, read64, write32,
                      barrier, ticks, cache, allocateDma, lockIo, unlockIo,
                      pausePoll, logFault};
  const Budget budget{freq * 10, 1000000, 4};
  g_init_report = transport.initialize(plat, budget, j716f_first_block::reference);
  const InitReport &r = g_init_report;

  g_pub = {};
  if (transport.online() && r.gpt.manifest.valid) {
    publishRootWindows(g_pub);
  }

  const PostCheck post = transport.postCheck();
  const Stats ts = transport.stats();
  uint64_t s_rd = 0, s_by = 0, s_wr = 0, s_er = 0;
  uint64_t o_rd = 0, o_by = 0, o_wr = 0, o_er = 0;
  j716f_r41_window_stats(0, &s_rd, &s_by, &s_wr, &s_er);
  j716f_r41_window_stats(1, &o_rd, &o_by, &o_wr, &o_er);

  printf("[J716F r41] pre ph=%s err=%s rec=%u hw=%u l4ok=%u shng=%u/%u ver=%u wins=%u img=%llu\n",
         phaseName(r.phase), errorName(r.error), unsigned(r.record_valid),
         unsigned(r.hardware_matched), unsigned(r.lun4_matched),
         unsigned(r.gpt.manifest.valid), r.gpt.manifest.status, r.gpt.manifest.version,
         r.gpt.manifest.window_count, (unsigned long long)r.gpt.manifest.image_bytes);
  printf("[J716F r41] pre kext=%u/%x par=%u/%u/%u reg=%u sysos=%u/%u/%ums/%s os=%u/%u/%ums/%s\n",
         unsigned(g_pub.kext_loaded), unsigned(g_pub.kext_status),
         unsigned(g_pub.parent_matched), g_pub.parent_size, g_pub.source_size,
         unsigned(g_pub.classes_registered),
         unsigned(g_pub.sysos_started), unsigned(g_pub.sysos_ready), g_pub.sysos_ms,
         g_pub.sysos_bsd[0] ? g_pub.sysos_bsd : "-",
         unsigned(g_pub.os_started), unsigned(g_pub.os_ready), g_pub.os_ms,
         g_pub.os_bsd[0] ? g_pub.os_bsd : "-");
  printf("[J716F r41] pre io sys=%llu/%lluB os=%llu/%lluB wr=%llu/%llu er=%llu/%llu tcmd=%llu tblk=%llu maxch=%u post=%u/%u/%x/%x\n",
         (unsigned long long)s_rd, (unsigned long long)s_by,
         (unsigned long long)o_rd, (unsigned long long)o_by,
         (unsigned long long)s_wr, (unsigned long long)o_wr,
         (unsigned long long)s_er, (unsigned long long)o_er,
         (unsigned long long)ts.read_commands, (unsigned long long)ts.blocks_read,
         ts.max_chunk_blocks, unsigned(post.checked), unsigned(post.display_slot4_unchanged),
         post.cb2_fsr, post.cb3_fsr);
  return (r.error == Error::None && g_pub.os_ready) ? 0U : 100U;
}

void reportPostRootAndRestart(const char *root_dev, const char *fs_type, int readonly,
                              int autherr, int launchd_err, unsigned long long launchd_size,
                              unsigned int launchd_magic, int sysver_err,
                              unsigned long long sysver_size) {
  const PostCheck post = transport.postCheck();
  const Stats ts = transport.stats();
  uint64_t s_rd = 0, s_by = 0, s_wr = 0, s_er = 0;
  uint64_t o_rd = 0, o_by = 0, o_wr = 0, o_er = 0;
  j716f_r41_window_stats(0, &s_rd, &s_by, &s_wr, &s_er);
  j716f_r41_window_stats(1, &o_rd, &o_by, &o_wr, &o_er);

  for (unsigned copy = 0; copy < 2; ++copy) {
    printf("[J716F r41] post c=%u rootdev=%s fstype=%s rdonly=%d autherr=%d\n",
           copy, root_dev ? root_dev : "-", fs_type ? fs_type : "-", readonly, autherr);
    printf("[J716F r41] post c=%u launchd_err=%d size=%llu magic=%08x sysver_err=%d sysver_size=%llu\n",
           copy, launchd_err, launchd_size, launchd_magic, sysver_err, sysver_size);
    printf("[J716F r41] post c=%u sysos=%s os=%s sys_io=%llu/%lluB os_io=%llu/%lluB wr=%llu/%llu er=%llu/%llu\n",
           copy, g_pub.sysos_bsd[0] ? g_pub.sysos_bsd : "-",
           g_pub.os_bsd[0] ? g_pub.os_bsd : "-",
           (unsigned long long)s_rd, (unsigned long long)s_by,
           (unsigned long long)o_rd, (unsigned long long)o_by,
           (unsigned long long)s_wr, (unsigned long long)o_wr,
           (unsigned long long)s_er, (unsigned long long)o_er);
    printf("[J716F r41] post c=%u tcmd=%llu tblk=%llu maxch=%u quar=%u post=%u disp4=%u fsr=%x/%x tlb=%x/%x rsr=%x db=%x\n",
           copy, (unsigned long long)ts.read_commands, (unsigned long long)ts.blocks_read,
           ts.max_chunk_blocks, unsigned(ts.quarantined),
           unsigned(post.checked), unsigned(post.display_slot4_unchanged),
           post.cb2_fsr, post.cb3_fsr, post.cb2_tlbstatus, post.cb3_tlbstatus,
           post.utrlrsr, post.utrldbr);
  }
  unsigned continue_boot = 0;
  if (PE_parse_boot_argn("j716f-r41-continue", &continue_boot, sizeof(continue_boot)) &&
      continue_boot == 1) {
    printf("[J716F r41] j716f-r41-continue=1: continuing past APFS root probe into Stage 7\n");
    return;
  }
  printf("[J716F r41] APFS root probe complete; restarting to UEFI menu for JXLG export\n");
  IOSleep(1000);
  PEHaltRestart(kPERestartCPU);
  panic("[J716F r41] PEHaltRestart returned after APFS root probe");
}

int authenticateWindowProvider(IOService *provider, const char *uuid, uint8_t *digest_out) {
  if (!provider || !uuid || strlen(uuid) != 36) return -1;
  const auto &m = g_init_report.gpt.manifest;
  if (!transport.online() || !m.valid) return -1;

  int win_idx = -1;
  const char *expected_uuid = nullptr;
  if (provider == g_os_device && m.os_index >= 0) {
    win_idx = m.os_index;
    expected_uuid = kOsRootUuid;
  } else if (provider == g_sysos_device && m.systemos_index >= 0) {
    win_idx = m.systemos_index;
    expected_uuid = kSystemOsUuid;
  } else {
    return -1;
  }

  if (!strEq(uuid, expected_uuid)) return 0;
  if (digest_out) {
    static const char kDomain[] = "J716F.SHNGROOT.Window.v1";
    const auto &w = m.windows[win_idx];
    SHA384_CTX ctx;
    SHA384_Init(&ctx);
    SHA384_Update(&ctx, kDomain, sizeof(kDomain));
    SHA384_Update(&ctx, w.sha256, sizeof(w.sha256));
    SHA384_Update(&ctx, &w.abs_offset, sizeof(w.abs_offset));
    SHA384_Update(&ctx, &w.size_bytes, sizeof(w.size_bytes));
    SHA384_Update(&ctx, expected_uuid, 36);
    SHA384_Final(digest_out, &ctx);
  }
  return 1;
}

int verifiedVolumeQuery(dev_t device, const char *expected_uuid, uint8_t *manifest_digest) {
  OSDictionary *matching = IOService::serviceMatching("IOMedia");
  OSDictionary *properties = OSDictionary::withCapacity(2);
  OSNumber *mjr = OSNumber::withNumber(
      static_cast<unsigned>(major(static_cast<uint32_t>(device))), 32);
  OSNumber *mnr = OSNumber::withNumber(
      static_cast<unsigned>(minor(static_cast<uint32_t>(device))), 32);
  bool ready = matching && properties && mjr && mnr &&
               properties->setObject(kIOBSDMajorKey, mjr) &&
               properties->setObject(kIOBSDMinorKey, mnr) &&
               matching->setObject("IOPropertyMatch", properties);
  OSSafeReleaseNULL(mnr);
  OSSafeReleaseNULL(mjr);
  OSSafeReleaseNULL(properties);
  OSIterator *iterator = ready ? IOService::getMatchingServices(matching) : nullptr;
  OSSafeReleaseNULL(matching);
  if (!iterator) return 0;

  bool authenticated = false;
  unsigned matches = 0;
  while (OSObject *object = iterator->getNextObject()) {
    IOService *media = OSDynamicCast(IOService, object);
    if (!media) continue;
    if (++matches != 1) {
      authenticated = false;
      break;
    }
    OSString *uuid = OSDynamicCast(OSString, media->getProperty("UUID"));
    if (!uuid || uuid->getLength() != 36 ||
        (expected_uuid && strcmp(expected_uuid, uuid->getCStringNoCopy()))) {
      continue;
    }
    unsigned depth = 0;
    for (IOService *prov = media; prov && depth < 64; prov = prov->getProvider(), ++depth) {
      const int auth = authenticateWindowProvider(prov, uuid->getCStringNoCopy(), manifest_digest);
      if (auth >= 0) {
        authenticated = (auth == 1);
        break;
      }
    }
  }
  iterator->release();
  if (!authenticated || matches != 1) {
    if (manifest_digest) bzero(manifest_digest, SHA384_DIGEST_LENGTH);
    return 0;
  }
  return 1;
}

}  // namespace j716f_r41_kernel

extern "C" unsigned j716f_storage_power_probe(void) {
  return j716f_r41_kernel::runPreRoot();
}

extern "C" void j716f_r41_post_root_probe(const char *root_dev, const char *fs_type,
                                          int readonly, int autherr, int launchd_err,
                                          unsigned long long launchd_size,
                                          unsigned int launchd_magic, int sysver_err,
                                          unsigned long long sysver_size) {
  j716f_r41_kernel::reportPostRootAndRestart(
      root_dev, fs_type, readonly, autherr, launchd_err, launchd_size,
      launchd_magic, sysver_err, sysver_size);
}

extern "C" int native_verified_block_root_authorized(dev_t device) {
  return j716f_r41_kernel::verifiedVolumeQuery(device, nullptr, nullptr);
}

extern "C" int native_verified_block_manifest_sha384(dev_t device, const char *uuid,
                                                     uint8_t digest[48]) {
  if (!digest) return 0;
  bzero(digest, SHA384_DIGEST_LENGTH);
  if (!uuid || strlen(uuid) != 36) return 0;
  return j716f_r41_kernel::verifiedVolumeQuery(device, uuid, digest);
}

extern "C" int native_verified_block_volume_device(const char *uuid, dev_t *device,
                                                   char *path, size_t size) {
  if (!uuid || !device || !path || size < 6 || strlen(uuid) != 36) return 0;
  *device = NODEV;
  path[0] = '\0';
  OSDictionary *matching = IOService::serviceMatching("IOMedia");
  OSDictionary *properties = OSDictionary::withCapacity(1);
  OSString *wanted = OSString::withCString(uuid);
  bool ready = matching && properties && wanted &&
               properties->setObject("UUID", wanted) &&
               matching->setObject("IOPropertyMatch", properties);
  OSSafeReleaseNULL(wanted);
  OSSafeReleaseNULL(properties);
  OSIterator *iterator = ready ? IOService::getMatchingServices(matching) : nullptr;
  OSSafeReleaseNULL(matching);
  if (!iterator) return 0;

  unsigned matches = 0;
  bool authenticated = false;
  dev_t candidate = NODEV;
  char candidate_path[64] = {};
  while (OSObject *object = iterator->getNextObject()) {
    IOService *media = OSDynamicCast(IOService, object);
    if (!media) continue;
    if (++matches != 1) {
      authenticated = false;
      break;
    }
    OSString *identity = OSDynamicCast(OSString, media->getProperty("UUID"));
    OSString *name = OSDynamicCast(OSString, media->getProperty(kIOBSDNameKey));
    OSNumber *mjr = OSDynamicCast(OSNumber, media->getProperty(kIOBSDMajorKey));
    OSNumber *mnr = OSDynamicCast(OSNumber, media->getProperty(kIOBSDMinorKey));
    if (!identity || strcmp(identity->getCStringNoCopy(), uuid) || !name || !mjr || !mnr ||
        !name->getLength() || name->getLength() >= sizeof(candidate_path) - 6 ||
        name->getLength() >= size - 6 || mjr->unsigned64BitValue() > 255 ||
        mnr->unsigned64BitValue() > 0xffffff) {
      continue;
    }
    candidate = static_cast<dev_t>(makedev(mjr->unsigned32BitValue(), mnr->unsigned32BitValue()));
    if (!native_verified_block_root_authorized(candidate)) continue;
    snprintf(candidate_path, sizeof(candidate_path), "/dev/%s", name->getCStringNoCopy());
    authenticated = true;
  }
  iterator->release();
  if (!authenticated || matches != 1) return 0;
  *device = candidate;
  strlcpy(path, candidate_path, size);
  return 1;
}

// Volatile 2-level sparse RAM-backed store for NativeRamBlock when a >2 GiB
// APFS Data seed (e.g. 16 GiB data-21H461.dbr) runs in direct mode without
// persistent UFS writeback. Strictly RAM-only: zero UFS writes can ever occur.
namespace j716f_ram_store {
constexpr uint32_t kBlockBytes = 4096;
constexpr uint32_t kL2Shift = 10;
constexpr uint32_t kL2Count = 1U << kL2Shift;
constexpr uint32_t kL2Mask = kL2Count - 1U;
constexpr uint32_t kMaxL1 = 4112;  // Covers up to 4,210,688 blocks (>16 GiB + overlay header/bitmap)
constexpr uint32_t kMaxResidentPages = 131072;  // 512 MiB hard cap

IOLock *g_lock = nullptr;
uint8_t ***g_l1 = nullptr;
uint64_t g_total_blocks = 0;
uint32_t g_resident_pages = 0;
bool g_open = false;

bool isAllZero(const uint8_t *p) {
  for (size_t i = 0; i < kBlockBytes; ++i) {
    if (p[i] != 0) return false;
  }
  return true;
}

bool enabled() {
  unsigned v = 0;
  if (PE_parse_boot_argn("j716f-ram-store", &v, sizeof(v))) {
    return v == 1;
  }
  return PE_parse_boot_argn("data-reset", &v, sizeof(v)) && (v == 1 || v == 2);
}
}  // namespace j716f_ram_store

extern "C" {
__attribute__((weak)) bool ml_static_mfree_prepare(vm_offset_t, vm_size_t) { return false; }

__attribute__((weak)) unsigned native_ufs_store_mode(void) {
  return j716f_ram_store::enabled() ? 2U : 0U;
}

__attribute__((weak)) int native_ufs_store_open(uint64_t blocks, uint64_t *first,
                                                const char **why) {
  using namespace j716f_ram_store;
  if (!enabled()) {
    if (why) *why = "read-only-j716f-storage";
    return 0;
  }
  if (!blocks || blocks > static_cast<uint64_t>(kMaxL1) * kL2Count) {
    if (why) *why = "ram-store-too-large";
    return 0;
  }
  if (!g_lock) g_lock = IOLockAlloc();
  if (!g_lock) {
    if (why) *why = "ram-store-lock";
    return 0;
  }
  IOLockLock(g_lock);
  if (!g_l1) {
    g_l1 = static_cast<uint8_t ***>(IOMallocZero(static_cast<size_t>(kMaxL1) * sizeof(uint8_t **)));
  }
  if (!g_l1) {
    IOLockUnlock(g_lock);
    if (why) *why = "ram-store-oom";
    return 0;
  }
  g_total_blocks = blocks;
  g_open = true;
  if (first) *first = 0;
  IOLockUnlock(g_lock);
  printf("[J716F r41] volatile RAM Data store opened: blocks=%llu cap=%uMiB (0 UFS writes)\n",
         static_cast<unsigned long long>(blocks), (kMaxResidentPages * kBlockBytes) >> 20);
  return 1;
}

__attribute__((weak)) int native_ufs_store_read(uint64_t block, uint32_t count, void *dst) {
  using namespace j716f_ram_store;
  if (!dst || !count || !g_open || !g_lock || block > g_total_blocks ||
      count > g_total_blocks - block) {
    return 0;
  }
  uint8_t *out = static_cast<uint8_t *>(dst);
  IOLockLock(g_lock);
  for (uint32_t i = 0; i < count; ++i) {
    const uint64_t b = block + i;
    const uint32_t l1_idx = static_cast<uint32_t>(b >> kL2Shift);
    const uint32_t l2_idx = static_cast<uint32_t>(b & kL2Mask);
    uint8_t **l2 = g_l1[l1_idx];
    uint8_t *page = l2 ? l2[l2_idx] : nullptr;
    if (page) {
      memcpy(out + static_cast<size_t>(i) * kBlockBytes, page, kBlockBytes);
    } else {
      bzero(out + static_cast<size_t>(i) * kBlockBytes, kBlockBytes);
    }
  }
  IOLockUnlock(g_lock);
  return 1;
}

__attribute__((weak)) int native_ufs_store_write(uint64_t block, uint32_t count, const void *src) {
  using namespace j716f_ram_store;
  if (!src || !count || !g_open || !g_lock || block > g_total_blocks ||
      count > g_total_blocks - block) {
    return 0;
  }
  const uint8_t *in = static_cast<const uint8_t *>(src);
  IOLockLock(g_lock);
  for (uint32_t i = 0; i < count; ++i) {
    const uint64_t b = block + i;
    const uint32_t l1_idx = static_cast<uint32_t>(b >> kL2Shift);
    const uint32_t l2_idx = static_cast<uint32_t>(b & kL2Mask);
    const uint8_t *src_page = in + static_cast<size_t>(i) * kBlockBytes;
    uint8_t **l2 = g_l1[l1_idx];
    uint8_t *page = l2 ? l2[l2_idx] : nullptr;
    if (!page) {
      if (isAllZero(src_page)) continue;
      if (g_resident_pages >= kMaxResidentPages) {
        IOLockUnlock(g_lock);
        printf("[J716F r41] volatile RAM Data store full (%u pages)\n", g_resident_pages);
        return 0;
      }
      if (!l2) {
        l2 = static_cast<uint8_t **>(IOMallocZero(static_cast<size_t>(kL2Count) * sizeof(uint8_t *)));
        if (!l2) {
          IOLockUnlock(g_lock);
          return 0;
        }
        g_l1[l1_idx] = l2;
      }
      page = static_cast<uint8_t *>(IOMallocZero(kBlockBytes));
      if (!page) {
        IOLockUnlock(g_lock);
        return 0;
      }
      l2[l2_idx] = page;
      ++g_resident_pages;
    }
    memcpy(page, src_page, kBlockBytes);
  }
  IOLockUnlock(g_lock);
  return 1;
}

__attribute__((weak)) int native_ufs_store_write_cached(uint64_t block, uint32_t count,
                                                        const void *src) {
  return native_ufs_store_write(block, count, src);
}

__attribute__((weak)) int native_ufs_store_sync(void) {
  return j716f_ram_store::g_open ? 1 : 0;
}

__attribute__((weak)) unsigned native_ufs_store_gpt_check(void) { return 0; }

int j716f_skip_setup_plist(unsigned index, const char **path, const uint8_t **data,
                           unsigned *size) {
  constexpr unsigned kCount =
      static_cast<unsigned>(sizeof(kJ716fSkipSetupPlists) / sizeof(kJ716fSkipSetupPlists[0]));
  if (index >= kCount) return 0;
  if (path) *path = kJ716fSkipSetupPlists[index].path;
  if (data) *data = kJ716fSkipSetupPlists[index].data;
  if (size) *size = kJ716fSkipSetupPlists[index].size;
  return 1;
}
}
