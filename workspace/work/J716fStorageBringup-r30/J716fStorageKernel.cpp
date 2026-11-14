// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "J716fStorageBringup.h"
#include <libkern/libkern.h>

namespace {
// Constant/BSS initialization only. Not a constructor or matching provider.
j716f_storage::State storage_state;
void storage_log(j716f_storage::Event event, const j716f_storage::Report &r) {
  printf("[J716F storage] seq=%u event=%u stage=%s err=%u backend=%u next=%s\n",
    r.sequence, static_cast<unsigned>(event), j716f_storage::stageName(r.stage),
    static_cast<unsigned>(r.error), r.backend_result, j716f_storage::gateName(r.gate));
  if (event == j716f_storage::Event::ObserveReturn)
    printf("[J716F storage] diag=%u completed=%u reads=%u map=%u unmap=%u fail=0x%llx\n",
      static_cast<unsigned>(r.diagnostic.error), static_cast<unsigned>(r.diagnostic.completed_stage),
      r.diagnostic.attempted_reads, r.diagnostic.mapped_pages, r.diagnostic.released_pages,
      static_cast<unsigned long long>(r.diagnostic.failed_mapping));
  if (event == j716f_storage::Event::ActivateReturn || event == j716f_storage::Event::ReadReturn ||
      event == j716f_storage::Event::StopReturn)
    printf("[J716F storage] rt=%u handoff=%u owner=%u cmd=%u policy=%u blocks=%llu blockbytes=%u compared=%u match=%u\n",
      static_cast<unsigned>(r.runtime.error), static_cast<unsigned>(r.runtime.handoff.error),
      static_cast<unsigned>(r.runtime.owner_error), static_cast<unsigned>(r.runtime.command.error),
      static_cast<unsigned>(r.runtime.command.policy_error),
      static_cast<unsigned long long>(r.capacity.blocks),
      r.capacity.block_bytes, r.compared_bytes, r.reference_matched ? 1U : 0U);
}
const j716f_storage::Backend storage_backend = {
  sm8250_ufs_adapter_init, sm8250_ufs_observe, sm8250_ufs_allocate, sm8250_ufs_activate,
  sm8250_ufs_read_known, sm8250_ufs_stop, sm8250_ufs_discard_unused, storage_log,
};
} // namespace
extern "C" void j716f_storage_bringup_init(void) {
  (void)j716f_storage::initialize(storage_state, storage_backend, nullptr);
}
extern "C" unsigned j716f_storage_bringup_status(j716f_storage::Report *out) {
  return static_cast<unsigned>(j716f_storage::status(storage_state, out));
}
extern "C" unsigned j716f_storage_bringup_observe(const sm8250_ufs::DiagnosticRequest *input, j716f_storage::Report *out) {
  return static_cast<unsigned>(j716f_storage::observe(storage_state, storage_backend, input, out));
}
extern "C" unsigned j716f_storage_bringup_allocate(const sm8250_ufs::RuntimeAccess *input, j716f_storage::Report *out) {
  return static_cast<unsigned>(j716f_storage::allocate(storage_state, storage_backend, input, out));
}
extern "C" unsigned j716f_storage_bringup_activate(const sm8250_ufs::RuntimeStart *input, j716f_storage::Report *out) {
  return static_cast<unsigned>(j716f_storage::activate(storage_state, storage_backend, input, out));
}
extern "C" unsigned j716f_storage_bringup_read_known(uint64_t lba, const uint8_t *reference, uint8_t *output,
                                                   size_t bytes, j716f_storage::Report *out) {
  return static_cast<unsigned>(j716f_storage::readKnown(storage_state, storage_backend, lba, reference, output, bytes, out));
}
extern "C" unsigned j716f_storage_bringup_stop(j716f_storage::Report *out) {
  return static_cast<unsigned>(j716f_storage::stop(storage_state, storage_backend, out));
}
extern "C" unsigned j716f_storage_bringup_discard_unused(j716f_storage::Report *out) {
  return static_cast<unsigned>(j716f_storage::discardUnused(storage_state, storage_backend, out));
}
