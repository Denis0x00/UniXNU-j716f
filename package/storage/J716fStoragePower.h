// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef J716F_STORAGE_POWER_H
#define J716F_STORAGE_POWER_H
#include <stdint.h>

namespace j716f_power {
constexpr uint64_t Page = 0x177000;
constexpr uint64_t PageBytes = 4096;
constexpr unsigned RegisterCount = 11;
constexpr uint64_t HciPage = 0x1d84000;
constexpr uint64_t HciPageBytes = 4096;
constexpr unsigned HciPrimaryCount = 14;
constexpr unsigned HciRepeatCount = 10;
constexpr unsigned HciRegisterCount = HciPrimaryCount + HciRepeatCount;
constexpr uint64_t SmmuPage = 0x15000000;
constexpr uint64_t SmmuPageBytes = 4096;
constexpr unsigned SmmuHeaderCount = 4;
constexpr unsigned SmmuMaxGroups = 128;
constexpr unsigned SmmuBatchGroups = 4;
constexpr unsigned MaximumLoads = RegisterCount + HciRegisterCount + SmmuHeaderCount + 2 * SmmuMaxGroups;
enum class Phase : unsigned { Gcc, Hci, Smmu };
enum class Result : unsigned {
  Captured, Busy, AlreadyAttempted, InvalidBackend, ContextRejected,
  RegistrationRequired, RuntimeNotCold, MappingFailed, ReadFailed,
  PowerNotReady, ResetAsserted, ClockNotReady, HciMappingFailed, HciReadFailed,
  SmmuMappingFailed, SmmuReadFailed, SmmuFormatRejected,
};
enum class Event : unsigned {
  Begin, Context, MapBefore, MapAfter, ReadBefore, ReadAfter,
  UnmapBefore, UnmapAfter, Gate, Summary,
  GroupBefore, GroupAfter,
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
  Phase phase = Phase::Gcc;
  Context context = {};
  uint32_t values[RegisterCount] = {};
  uint32_t hci_values[HciRegisterCount] = {};
  unsigned valid_mask = 0;
  unsigned reads_attempted = 0, reads_completed = 0;
  unsigned hci_valid_mask = 0;
  unsigned hci_reads_attempted = 0, hci_reads_completed = 0;
  // Bit i compares repeat sample i with its fixed primary index. No equality
  // grants ownership or proves stable/coherent bases, queue stop or DMA drain.
  unsigned repeat_compared_mask = 0, repeat_changed_mask = 0;
  uint32_t smmu_header[SmmuHeaderCount] = {};
  uint32_t smmu_groups[SmmuMaxGroups][2] = {}; // SMR, S2CR: raw words, not pointers.
  unsigned smmu_header_valid_mask = 0;
  unsigned smmu_reads_attempted = 0, smmu_reads_completed = 0;
  // Successful group words form a contiguous prefix in SMR/S2CR order. A
  // partial last pair is not a valid complete group; no missing word is evidence.
  unsigned smmu_group_words_completed = 0;
  unsigned smmu_group_count = 0, smmu_context_count = 0, smmu_stage2_count = 0;
  unsigned smmu_global_pages = 0, smmu_sid_bits = 0;
  bool smmu_gate_passed = false;
  unsigned maps = 0, unmaps = 0;
  bool power_on = false, reset_asserted = false;
  bool axi_readable_hint = false, ahb_readable_hint = false;
  bool hci_gate_passed = false;
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
uint64_t hciAddress(unsigned index);
const char *hciRegisterName(unsigned index);
const char *hciSampleName(unsigned index);
unsigned hciRepeatPrimaryIndex(unsigned index);
uint64_t smmuAddress(unsigned index);
const char *smmuRegisterName(unsigned index);
uint64_t smmuGroupAddress(unsigned group, unsigned word);
const char *resultName(Result);
// Exactly one attempt per boot: GCC11, HCI14+10, then gated SMMU4+2*groups.
// At most 295 loads/3 mappings, no retry, writes, CB access or pointer following.
// Gate hints are not established HCI read authority or a controller handoff.
// Report is observation only: it never
// changes the old bridge state or creates RequiredDiagnosticFacts/ReadGrant.
Result run(State &, const Backend &, Report &);
}
extern "C" unsigned j716f_storage_power_probe(void);
#endif
