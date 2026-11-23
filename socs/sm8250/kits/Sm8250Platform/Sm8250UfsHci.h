// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef SM8250_UFS_HCI_H
#define SM8250_UFS_HCI_H
#include "Sm8250UfsIo.h"
#include "AppsSmmuOwner.h"
#include "RestrictedUfs.h"

namespace sm8250_ufs {
// Exactly one retained allocation and queue slot. No reset, free, UIC, task
// management, interrupts, persistence or old NativeUfs submission path.
class HciPort {
public:
  HciPort() = default;
  HciPort(const HciPort &) = delete;
  HciPort &operator=(const HciPort &) = delete;
  bool bind(const Io &, const DmaView &, const AppsSmmuClaim &, const AppsSmmuToken &);
  shengufs::restricted::Port port();
  // Caller must first publish and validate the owned SMMU mapping. This method
  // verifies stopped/empty HCI state, installs ONLY the retained UTRLBA and
  // starts the queue. It records queue_started after hardware readback.
  bool setupQueue();
  // No command cancellation: refuses active/unknown DMA. The caller still
  // uses SingleSlot::confirmStopped for its state/owner attestation.
  bool stopQueue(uint64_t timeout_ticks, uint32_t max_polls);
  // Only after a successful read completion and owner state returns ready.
  bool copyReadBlock(uint8_t *out, size_t bytes);
  bool failed() const { return __atomic_load_n(&failed_, __ATOMIC_ACQUIRE) != 0; }
private:
  enum class Stage : unsigned { Unbound, Bound, Prepared, Acknowledged, Rung, Completed, Collected };
  Io io_ = {};
  DmaView dma_ = {};
  AppsSmmuClaim claim_ = {};
  AppsSmmuToken token_ = {};
  shengufs::read_policy::Context context_ = {};
  shengufs::read_policy::Accepted accepted_ = {};
  uint8_t response_[512] = {};
  Stage stage_ = Stage::Unbound;
  uint32_t completed_ocs_ = 15;
  unsigned failed_ = 0, busy_ = 0;
  bool wide_ = false;
  bool fail();
  bool validOwner();
  bool read(unsigned offset, uint32_t &);
  bool write(unsigned offset, uint32_t);
  bool cache(CacheOp, size_t offset, size_t bytes);
  bool barrier();
  bool snapshot(shengufs::restricted::Probe &);
  bool prepare(const shengufs::read_policy::Request &, const shengufs::read_policy::Context &);
  bool ack();
  bool ring();
  bool poll(shengufs::restricted::Poll &);
  bool collect(shengufs::restricted::Completion &);
  static bool probeThunk(void *, shengufs::restricted::Probe *);
  static bool prepareThunk(void *, const shengufs::read_policy::Request &, const shengufs::read_policy::Context &);
  static bool ackThunk(void *);
  static bool ringThunk(void *);
  static bool pollThunk(void *, shengufs::restricted::Poll *);
  static bool collectThunk(void *, shengufs::restricted::Completion *);
};
} // namespace sm8250_ufs
#endif
