// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef SM8250_UFS_DIAGNOSTICS_H
#define SM8250_UFS_DIAGNOSTICS_H
#include "UfsObservation.h"

namespace sm8250_ufs {
enum class DiagnosticStage : unsigned { None = 0, Power = 1, Host = 2, Geometry = 3 };
enum DiagnosticAccessFact : uint32_t {
  DiagnosticBoardVerified = 1U << 0,
  DiagnosticReadAccessEstablished = 1U << 1,
  DiagnosticSleepableContext = 1U << 2,
  RequiredDiagnosticFacts = 7,
};
// Trusted kernel-platform caller only. authorization names external evidence of
// current powered, readable access to exactly these pages. Values/flags do not
// manufacture evidence, exclude firmware/TZ, or grant queue/DMA/SMMU ownership.
// Request and output storage must be kernel-owned; request stays stable during
// the call. No DMA-memory attestation is needed because no DMA is allocated.
struct DiagnosticAccess {
  uint32_t version;
  uint32_t board; // 0x4a373136 = TB-J716F, not a measured identity probe.
  uint64_t authorization;
  uint32_t facts;
  unsigned range_count;
  shengufs::Range ranges[3]; // Exact stage pages, in GCC/HCI/SMMU-ID order.
};
struct DiagnosticRequest {
  DiagnosticStage stage;
  DiagnosticAccess access;
};
enum class DiagnosticError : unsigned {
  None, Busy, NotInitialized, WrongPhase, InvalidRequest, InvalidIo,
  Mapping, Read, PowerNotReady, ResetAsserted, ClockNotReady, GeometryUnknown,
};
struct DiagnosticReport {
  DiagnosticError error = DiagnosticError::None;
  DiagnosticStage requested_stage = DiagnosticStage::None;
  DiagnosticStage completed_stage = DiagnosticStage::None;
  shengufs::UfsObservation observation = {};
  unsigned attempted_reads = 0, mapped_pages = 0, released_pages = 0;
  uint64_t failed_mapping = 0;
};
// Deliberately no write, DMA, cache or owner callbacks. mapReadOnly must create
// only the exact requested read-only mapping; false must retain no new mapping.
// unmap releases a successful mapping and must not fail. All callbacks are
// synchronous/bounded; read32 must not follow register values as pointers.
struct DiagnosticIo {
  void *context;
  bool (*mapReadOnly)(void *, uint64_t physical, uint64_t bytes);
  void (*unmap)(void *, uint64_t physical, uint64_t bytes);
  bool (*read32)(void *, uint64_t physical, uint32_t *value);
};
shengufs::Range diagnosticRange(unsigned index);
bool diagnosticAddressAllowed(DiagnosticStage, uint64_t address);
// Each call is an independent, non-atomic fixed-register snapshot. Power reads
// up to 11 GCC registers; Host adds 14 HCI registers; Geometry adds 3 SMMU IDs.
// Earlier failure stops the call. No automatic next stage, stream/context scan,
// ownership inference, HandoffGrant, firmware pointer access or retained state.
DiagnosticReport observeDiagnostics(const DiagnosticRequest &, const DiagnosticIo &);
} // namespace sm8250_ufs
#endif
