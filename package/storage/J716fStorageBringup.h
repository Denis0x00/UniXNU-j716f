// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef J716F_STORAGE_BRINGUP_H
#define J716F_STORAGE_BRINGUP_H
#include "Sm8250UfsRuntime.h"

namespace j716f_storage {
// A board caller supplies live evidence. No boot argument, ROM constant or
// successful snapshot is converted into a diagnostic or ownership grant here.
enum class Stage : unsigned {
  Unregistered, Registered, PowerObserved, HostObserved, GeometryObserved,
  Allocated, TransportReady, KnownBlockRead, Stopped, Failed, Quarantined,
};
enum class Error : unsigned {
  None, Busy, NotRegistered, InvalidArgument, WrongStage, DiagnosticAccessRequired,
  RuntimeAccessRequired, HandoffRequired, BackendFailure, BackendContract,
  KnownBlockMismatch,
};
enum class Gate : unsigned {
  Registration, PowerReadAuthority, HostReadAuthority, GeometryReadAuthority,
  AllocationAuthority, ExclusiveHandoff, KnownBlockReference, StopProof, Terminal,
};
enum class Event : unsigned {
  Registered, ObserveEnter, ObserveReturn, AllocateEnter, AllocateReturn,
  ActivateEnter, ActivateReturn, ReadEnter, ReadReturn, StopEnter, StopReturn,
  DiscardEnter, DiscardReturn, Refused,
};
struct Report {
  Stage stage = Stage::Unregistered;
  Error error = Error::None;
  Gate gate = Gate::Registration;
  unsigned sequence = 0;
  unsigned backend_result = 0;
  sm8250_ufs::DiagnosticReport diagnostic = {};
  sm8250_ufs::RuntimeReport runtime = {};
  shengufs::read_policy::Capacity capacity = {}; // Retained actual READ CAPACITY result.
  uint64_t last_lba = 0;
  unsigned compared_bytes = 0;
  bool reference_matched = false;
};
// Tests bind an instrumented backend; the kernel wrapper binds ONLY the real
// frozen Runtime functions. Logging must be bounded and must not start I/O.
struct Backend {
  void (*init)();
  unsigned (*observe)(const sm8250_ufs::DiagnosticRequest *, sm8250_ufs::DiagnosticReport *);
  unsigned (*allocate)(const sm8250_ufs::RuntimeAccess *, sm8250_ufs::RuntimeReport *);
  unsigned (*activate)(const sm8250_ufs::RuntimeStart *, sm8250_ufs::RuntimeReport *);
  unsigned (*read)(uint64_t, uint8_t *, size_t, sm8250_ufs::RuntimeReport *);
  unsigned (*stop)(sm8250_ufs::RuntimeReport *);
  unsigned (*discard)(sm8250_ufs::RuntimeReport *);
  void (*log)(Event, const Report &);
};
struct State {
  unsigned busy = 0;
  Report report = {};
  sm8250_ufs::RuntimeAccess allocation_access = {};
};
// State/output/input buffers are trusted kernel-owned and must not alias each
// other. State has one lifetime per boot; do not reset, copy or free it after
// registration. Calls are synchronous and non-reentrant. No global constructors.
Error initialize(State &, const Backend &, Report *);
Error status(State &, Report *);
Error observe(State &, const Backend &, const sm8250_ufs::DiagnosticRequest *, Report *);
Error allocate(State &, const Backend &, const sm8250_ufs::RuntimeAccess *, Report *);
Error activate(State &, const Backend &, const sm8250_ufs::RuntimeStart *, Report *);
// Exactly one allowed 4096-byte READ(10). Reference is an independently captured
// immutable block for this LUN/LBA, never data just returned by this read. No
// broad device read API or content-derived grant is exposed.
Error readKnown(State &, const Backend &, uint64_t lba, const uint8_t *reference,
                uint8_t *output, size_t bytes, Report *);
Error stop(State &, const Backend &, Report *);
Error discardUnused(State &, const Backend &, Report *);
const char *stageName(Stage);
const char *gateName(Gate);
} // namespace j716f_storage

extern "C" void j716f_storage_bringup_init(void);
extern "C" unsigned j716f_storage_bringup_status(j716f_storage::Report *);
extern "C" unsigned j716f_storage_bringup_observe(const sm8250_ufs::DiagnosticRequest *, j716f_storage::Report *);
extern "C" unsigned j716f_storage_bringup_allocate(const sm8250_ufs::RuntimeAccess *, j716f_storage::Report *);
extern "C" unsigned j716f_storage_bringup_activate(const sm8250_ufs::RuntimeStart *, j716f_storage::Report *);
extern "C" unsigned j716f_storage_bringup_read_known(uint64_t, const uint8_t *, uint8_t *, size_t, j716f_storage::Report *);
extern "C" unsigned j716f_storage_bringup_stop(j716f_storage::Report *);
extern "C" unsigned j716f_storage_bringup_discard_unused(j716f_storage::Report *);
#endif
