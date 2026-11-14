// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef J716F_STORAGE_POWER_H
#define J716F_STORAGE_POWER_H
#include <stdint.h>

namespace j716f_power {
constexpr uint64_t Page = 0x177000;
constexpr uint64_t PageBytes = 4096;
constexpr unsigned RegisterCount = 11;
enum class Result : unsigned {
  Captured, Busy, AlreadyAttempted, InvalidBackend, ContextRejected,
  RegistrationRequired, RuntimeNotCold, MappingFailed, ReadFailed,
};
enum class Event : unsigned {
  Begin, Context, MapBefore, MapAfter, ReadBefore, ReadAfter,
  UnmapBefore, UnmapAfter, Summary,
};
struct Context {
  bool thread_present = false;
  bool interrupt_context = false;
  bool interrupts_enabled = false;
  unsigned preemption_level = 0;
  bool kernel_task_present = false;
  bool registered = false;
  bool runtime_cold = false;
};
struct Report {
  Result result = Result::InvalidBackend;
  Context context = {};
  uint32_t values[RegisterCount] = {};
  unsigned valid_mask = 0;
  unsigned reads_attempted = 0, reads_completed = 0;
  unsigned maps = 0, unmaps = 0;
  bool power_on = false, reset_asserted = false;
  bool axi_readable_hint = false, ahb_readable_hint = false;
};
// Backend is a synchronous trusted kernel binding. Its map returns no retained
// resource on false; unmap must complete release before returning. No writes,
// DMA, runtime handoff or caller-selected address is present in this interface.
// read32 is explicitly EXPERIMENTAL and NOT fault-contained on this ARM64 XNU.
struct Backend {
  void *opaque;
  Context (*context)(void *);
  bool (*map)(void *, uint64_t, uint64_t);
  bool (*read32)(void *, uint64_t, uint32_t *);
  void (*unmap)(void *);
  void (*log)(void *, Event, unsigned, const Report &);
};
struct State { unsigned busy = 0; bool attempted = false; };
uint64_t address(unsigned index);
const char *registerName(unsigned index);
const char *resultName(Result);
// Exactly one attempt per boot, no retry. Report is observation only: it never
// changes the old bridge state or creates RequiredDiagnosticFacts/ReadGrant.
Result run(State &, const Backend &, Report &);
}
extern "C" unsigned j716f_storage_power_probe(void);
#endif
