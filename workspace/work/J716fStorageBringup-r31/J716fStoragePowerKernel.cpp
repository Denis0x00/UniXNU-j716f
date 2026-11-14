// SPDX-License-Identifier: BSD-2-Clause-Patent
// Experimental TB-J716F-only GCC reads: no advance grant and no abort recovery.
#include "J716fStoragePower.h"
#include "J716fStorageBringup.h"
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <kern/task.h>
#include <kern/thread.h>
#include <kern/sched_prim.h>
#include <machine/machine_routines.h>
#include <libkern/libkern.h>

// Exact XNU osfmk/arm/cpu_data.h C ABI; that header hides this declaration
// behind MACH_KERNEL_PRIVATE, which this IOKit archive does not globally enable.
extern "C" int get_preemption_level(void);

namespace {
using namespace j716f_power;
State power_state;
struct Mapping { IOMemoryMap *object = nullptr; uintptr_t cpu = 0; };

Context actualContext(void *) {
  Context result;
  result.thread_present = current_thread() != THREAD_NULL;
  result.interrupt_context = ml_at_interrupt_context();
  result.interrupts_enabled = ml_get_interrupts_enabled();
  result.preemption_level = result.thread_present ? static_cast<unsigned>(get_preemption_level()) : ~0U;
  result.kernel_task_present = kernel_task != nullptr;
  j716f_storage::Report bridge;
  result.registered = j716f_storage_bringup_status(&bridge) == 0 &&
                      bridge.stage == j716f_storage::Stage::Registered;
  sm8250_ufs::RuntimeReport runtime;
  result.runtime_cold = sm8250_ufs_status(&runtime) == 0 &&
                        runtime.phase == sm8250_ufs::RuntimePhase::Cold;
  return result;
}
bool mapPage(void *opaque, uint64_t physical, uint64_t bytes) {
  auto &state = *static_cast<Mapping *>(opaque);
  if (physical != Page || bytes != PageBytes || state.object) return false;
  IODeviceMemory *memory = IODeviceMemory::withRange(physical, bytes);
  if (!memory) { printf("[J716F power] descriptor allocation failed; mmio=0\n"); return false; }
  printf("[J716F power] descriptor acquired page=0x177000 bytes=4096\n");
  IOMemoryMap *mapping = memory->map(kIOMapAnywhere | kIOMapReadOnly | kIOMapInhibitCache);
  memory->release();
  printf("[J716F power] descriptor released; mapping-created=%u\n", mapping ? 1U : 0U);
  if (!mapping) return false;
  const uintptr_t cpu = static_cast<uintptr_t>(mapping->getVirtualAddress());
  const IOOptionBits options = mapping->getMapOptions();
  if (!cpu || (cpu & (PageBytes - 1)) || cpu > UINTPTR_MAX - PageBytes ||
      mapping->getLength() != PageBytes || !(options & kIOMapReadOnly) ||
      (options & kIOMapCacheMask) != kIOMapInhibitCache) {
    mapping->release();
    printf("[J716F power] mapping rejected and released; mmio=0\n");
    return false;
  }
  state.object = mapping;
  state.cpu = cpu;
  printf("[J716F power] map verified readonly=1 inhibit-cache=1; bus-readability=unproven\n");
  return true;
}
bool readRegister(void *opaque, uint64_t physical, uint32_t *out) {
  auto &state = *static_cast<Mapping *>(opaque);
  if (!state.object || !out) return false;
  bool allowed = false;
  for (unsigned i = 0; i < RegisterCount; ++i) if (physical == address(i)) allowed = true;
  if (!allowed) return false;
  const uintptr_t cpu = state.cpu + static_cast<uintptr_t>(physical - Page);
  // ml_probe_read[_64] in the ACTUAL ARM64 XNU is an unimplemented panic stub.
  // The single volatile LDR and barriers can external-abort or stall. A before
  // log without after is an incomplete experiment, never successful evidence.
  __asm__ volatile("dsb sy" ::: "memory");
  const uint32_t value = *reinterpret_cast<const volatile uint32_t *>(cpu);
  __asm__ volatile("dsb sy" ::: "memory");
  *out = value;
  return true;
}
void unmapPage(void *opaque) {
  auto &state = *static_cast<Mapping *>(opaque);
  if (state.object) state.object->release();
  state = {};
}
void logEvent(void *, Event event, unsigned index, const Report &r) {
  switch (event) {
    case Event::Begin:
      printf("[J716F power] begin experimental-fixed-gcc-read board=TB-J716F fault-contained=0 retry=0\n"); break;
    case Event::Context:
      printf("[J716F power] context thread=%u irq=%u irq-enabled=%u preempt=%u kernel-task=%u registered=%u cold=%u\n",
        r.context.thread_present ? 1U : 0U, r.context.interrupt_context ? 1U : 0U,
        r.context.interrupts_enabled ? 1U : 0U, r.context.preemption_level,
        r.context.kernel_task_present ? 1U : 0U, r.context.registered ? 1U : 0U,
        r.context.runtime_cold ? 1U : 0U); break;
    case Event::MapBefore:
      printf("[J716F power] map-before page=0x177000 bytes=4096 readonly=1\n"); break;
    case Event::MapAfter:
      printf("[J716F power] map-after maps=%u\n", r.maps); break;
    case Event::ReadBefore:
      printf("[J716F power] read-before index=%u name=%s pa=0x%llx\n", index, registerName(index),
        static_cast<unsigned long long>(address(index))); break;
    case Event::ReadAfter:
      printf("[J716F power] read-after index=%u name=%s pa=0x%llx value=0x%08x\n", index, registerName(index),
        static_cast<unsigned long long>(address(index)), r.values[index]); break;
    case Event::UnmapBefore:
      printf("[J716F power] unmap-before maps=%u\n", r.maps); break;
    case Event::UnmapAfter:
      printf("[J716F power] unmap-after unmaps=%u\n", r.unmaps); break;
    case Event::Summary:
      printf("[J716F power] result=%s code=%u attempted=%u completed=%u valid=0x%x maps=%u unmaps=%u\n",
        resultName(r.result), static_cast<unsigned>(r.result), r.reads_attempted, r.reads_completed,
        r.valid_mask, r.maps, r.unmaps);
      if (r.result == Result::Captured)
        printf("[J716F power] hints gdsc-on=%u reset-asserted=%u axi-readable=%u ahb-readable=%u; lane-clock-state=uninterpreted\n",
          r.power_on ? 1U : 0U, r.reset_asserted ? 1U : 0U,
          r.axi_readable_hint ? 1U : 0U, r.ahb_readable_hint ? 1U : 0U);
      printf("[J716F power] snapshot-only storage-stage-unchanged hci=0 smmu=0 dma=0 writes=0 next=review-power-snapshot\n");
      break;
  }
}
}
extern "C" unsigned j716f_storage_power_probe(void) {
  Mapping mapping;
  const Backend backend = {&mapping, actualContext, mapPage, readRegister, unmapPage, logEvent};
  Report report;
  return static_cast<unsigned>(run(power_state, backend, report));
}
