// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "Sm8250UfsHci.h"

namespace sm8250_ufs {
namespace p = shengufs::read_policy;
namespace r = shengufs::restricted;
namespace {
// TB-J716F ZUI 15.0.485 / selected DTB0+DTBO1: HCI 0x1d84000/0x3000.
// PHY 0x1d87000/0xe00 and ICE 0x1d90000/0x8000 are intentionally outside
// this adapter. INTID 297 remains masked; this path polls. The two SIDs checked
// in bind are Linux SM8250 source expectations, never a stock ownership grant.
constexpr uint64_t kHost = 0x01d84000;
constexpr size_t kBytes = 139264;
constexpr uint32_t kFatal = 0x71880;
struct Guard {
  unsigned &word; bool held;
  explicit Guard(unsigned &w) : word(w), held(__atomic_exchange_n(&w, 1, __ATOMIC_ACQUIRE) == 0) {}
  ~Guard() { if (held) __atomic_store_n(&word, 0, __ATOMIC_RELEASE); }
};
void clear(uint8_t *p, size_t n) { for (size_t i = 0; i < n; ++i) p[i] = 0; }
void copy(uint8_t *d, const uint8_t *s, size_t n) { for (size_t i = 0; i < n; ++i) d[i] = s[i]; }
void le32(uint8_t *p, uint32_t v)
{ for (unsigned i = 0; i < 4; ++i) p[i] = uint8_t(v >> (8 * i)); }
bool empty(const r::Probe &s)
{ return !s.doorbell && !s.task_doorbell && !(s.interrupt_status & kFatal); }
bool ready(const r::Probe &s, uint64_t list)
{ return empty(s) && s.hce == 1 && (s.hcs & 3) == 3 && !s.interrupt_enable &&
         s.run_stop == 1 && s.request_list == list; }
}

bool HciPort::fail()
{
  __atomic_store_n(&failed_, 1, __ATOMIC_RELEASE);
  // Retain allocation/mapping forever after any ambiguous operation. No free,
  // rebind, reset, unpublish, cancellation or retry is hidden in this adapter.
  if (token_.lease_id) (void)apps_smmu_owner_quarantine(&token_, 0x484349);
  return false;
}
bool HciPort::validOwner()
{
  return !failed() && stage_ != Stage::Unbound &&
         apps_smmu_owner_validate(&token_, &claim_) == APPS_SMMU_OK;
}
bool HciPort::barrier() { return io_.barrier(io_.context) || fail(); }
bool HciPort::read(unsigned offset, uint32_t &value)
{
  // This port cannot be used to reach UIC, controller reset, task-management,
  // PHY, vendor registers or an arbitrary caller-supplied physical range.
  switch (offset) {
    case 0: case 0x20: case 0x24: case 0x30: case 0x34:
    case 0x50: case 0x54: case 0x58: case 0x60: case 0x78: break;
    default: return fail();
  }
  return io_.read32(io_.context, kHost + offset, &value) || fail();
}
bool HciPort::write(unsigned offset, uint32_t value)
{
  if (!((offset == 0x20 && value == 1) || (offset == 0x58 && value == 1) ||
        (offset == 0x60 && value <= 1) ||
        (offset == 0x50 && value == uint32_t(dma_.physical)) ||
        (offset == 0x54 && value == uint32_t(dma_.physical >> 32)))) return fail();
  return barrier() && (io_.write32(io_.context, kHost + offset, value) || fail()) && barrier();
}
bool HciPort::cache(CacheOp op, size_t offset, size_t bytes)
{
  if (!bytes || offset >= dma_.bytes || bytes > dma_.bytes - offset) return fail();
  // Entire 64-byte lines always remain inside the dedicated allocation.
  const size_t first = offset & ~size_t(63);
  const size_t end = (offset + bytes + 63) & ~size_t(63);
  if (end > dma_.bytes || end <= first) return fail();
  return barrier() && (io_.cache(io_.context, op, first, end - first) || fail()) && barrier();
}
bool HciPort::bind(const Io &io, const DmaView &dma, const AppsSmmuClaim &claim,
                   const AppsSmmuToken &token)
{
  Guard guard(busy_);
  if (!guard.held || stage_ != Stage::Unbound || failed()) return false;
  if (!io.read32 || !io.write32 || !io.barrier || !io.ticks || !io.cache || !dma.cpu ||
      (reinterpret_cast<uintptr_t>(dma.cpu) & 63) || dma.bytes != kBytes ||
      !dma.physical || (dma.physical & 4095) || dma.physical > UINT64_MAX - kBytes ||
      reinterpret_cast<uintptr_t>(dma.cpu) > UINTPTR_MAX - kBytes ||
      claim.controller_base != kHost || claim.smmu_base != 0x15000000 ||
      claim.sid_count != 2 || claim.slot_count != 2 || claim.sids[0] != 0xe0 || claim.sids[1] != 0x4e0 ||
      claim.dma_base != dma.physical || claim.dma_bytes != dma.bytes ||
      apps_smmu_owner_validate(&token, &claim) != APPS_SMMU_OK) return false;
  io_ = io; dma_ = dma; claim_ = claim; token_ = token; stage_ = Stage::Bound;
  uint32_t cap = 0;
  if (!read(0, cap)) return false;
  wide_ = (cap & (1U << 24)) != 0;
  if (!wide_ && dma.physical + dma.bytes - 1 > UINT32_MAX) return fail();
  return true;
}
bool HciPort::snapshot(r::Probe &out)
{
  out = {};
  if (!validOwner()) return fail();
  uint32_t low = 0, high = 0;
  return barrier() && read(0x34, out.hce) && read(0x30, out.hcs) &&
    read(0x24, out.interrupt_enable) && read(0x20, out.interrupt_status) &&
    read(0x58, out.doorbell) && read(0x78, out.task_doorbell) && read(0x60, out.run_stop) &&
    read(0x50, low) && read(0x54, high) &&
    ((out.request_list = uint64_t(low) | (uint64_t(high) << 32)), true) &&
    (io_.ticks(io_.context, &out.ticks) || fail()) && barrier();
}
bool HciPort::setupQueue()
{
  Guard guard(busy_);
  if (!guard.held || stage_ != Stage::Bound || !validOwner()) return false;
  r::Probe before{};
  if (!snapshot(before) || !empty(before) || before.hce != 1 || (before.hcs & 3) != 3 ||
      before.interrupt_enable || before.run_stop) return fail();
  if (!write(0x50, uint32_t(dma_.physical)) || !write(0x54, uint32_t(dma_.physical >> 32))) return false;
  r::Probe installed{};
  if (!snapshot(installed) || !empty(installed) || installed.run_stop || installed.interrupt_enable ||
      installed.request_list != dma_.physical || installed.hce != 1 || (installed.hcs & 3) != 3) return fail();
  if (!write(0x60, 1)) return false;
  r::Probe started{};
  if (!snapshot(started) || !ready(started, dma_.physical) ||
      apps_smmu_owner_queue_started(&token_) != APPS_SMMU_OK) return fail();
  return true;
}
bool HciPort::prepare(const p::Request &request, const p::Context &context)
{
  if (stage_ != Stage::Bound && stage_ != Stage::Collected) return fail();
  p::Accepted accepted{};
  if (p::validateRequest(request, context, accepted) != p::Error::None ||
      context.dma.allocation.address != dma_.physical || context.dma.allocation.bytes != dma_.bytes ||
      !validOwner() || apps_smmu_owner_validate_ready(&token_, &claim_) != APPS_SMMU_NOT_QUIESCENT)
    return fail();
  r::Probe before{};
  if (!snapshot(before) || !ready(before, dma_.physical)) return fail();
  // First CPU descriptor/data writes occur only after the final packet gate,
  // exact live owner, addressability and empty-queue checks above.
  context_ = context; accepted_ = accepted; completed_ocs_ = 15;
  clear(dma_.cpu, 64);
  clear(dma_.cpu + 4096, 1088);
  clear(response_, sizeof(response_));
  le32(dma_.cpu, 0x10000000U | (uint32_t(request.direction) << 25));
  le32(dma_.cpu + 8, 15);
  le32(dma_.cpu + 16, uint32_t(dma_.physical + 4096));
  le32(dma_.cpu + 20, uint32_t((dma_.physical + 4096) >> 32));
  le32(dma_.cpu + 24, 128U | (128U << 16));
  le32(dma_.cpu + 28, request.prd_count | (256U << 16));
  copy(dma_.cpu + 4096, request.upiu, 32);
  copy(dma_.cpu + 5120, request.prd, 16);
  if (accepted.data_bytes) clear(dma_.cpu + size_t(accepted.data_address - dma_.physical), accepted.data_bytes);
  if (!cache(CacheOp::CleanInvalidate, 0, 64) || !cache(CacheOp::CleanInvalidate, 4096, 1088)) return false;
  if (accepted.data_bytes && !cache(CacheOp::CleanInvalidate,
      size_t(accepted.data_address - dma_.physical), accepted.data_bytes)) return false;
  stage_ = Stage::Prepared;
  return true;
}
bool HciPort::ack()
{
  if (stage_ != Stage::Prepared || !validOwner()) return fail();
  r::Probe before{};
  if (!snapshot(before) || !ready(before, dma_.physical) || !write(0x20, 1)) return fail();
  stage_ = Stage::Acknowledged;
  return true;
}
bool HciPort::ring()
{
  if (stage_ != Stage::Acknowledged || !validOwner()) return fail();
  r::Probe before{};
  if (!snapshot(before) || !ready(before, dma_.physical)) return fail();
  stage_ = Stage::Rung; // A failing write may nevertheless have reached hardware.
  return write(0x58, 1);
}
bool HciPort::poll(r::Poll &out)
{
  out = {}; out.ocs = 15;
  if (stage_ != Stage::Rung || !validOwner()) return fail();
  r::Probe live{};
  if (!snapshot(live)) return false;
  out = {live.ticks, live.doorbell, live.task_doorbell, live.interrupt_status, 15};
  if (live.hce != 1 || (live.hcs & 3) != 3 || live.interrupt_enable ||
      live.run_stop != 1 || live.request_list != dma_.physical) return fail();
  if (!live.doorbell && !live.task_doorbell && !(live.interrupt_status & kFatal)) {
    if (!cache(CacheOp::Invalidate, 0, 64)) return false;
    const uint32_t word = p::loadLe32(dma_.cpu + 8);
    if (word & ~15U) return fail();
    out.ocs = word;
    if (word != 15) { completed_ocs_ = word; stage_ = Stage::Completed; }
  }
  return true;
}
bool HciPort::collect(r::Completion &out)
{
  out = {};
  if (stage_ != Stage::Completed || !validOwner()) return fail();
  r::Probe live{};
  if (!snapshot(live) || !ready(live, dma_.physical)) return fail();
  if (!cache(CacheOp::Invalidate, 4608, 512)) return false;
  if (accepted_.data_bytes && !cache(CacheOp::Invalidate,
      size_t(accepted_.data_address - dma_.physical), accepted_.data_bytes)) return false;
  copy(response_, dma_.cpu + 4608, 512);
  copy(out.response, response_, 512); out.response_bytes = 512;
  if (accepted_.command == p::Command::ReadCapacity10) {
    copy(out.capacity, dma_.cpu + size_t(accepted_.data_address - dma_.physical), 8);
    out.capacity_bytes = 8;
  }
  stage_ = Stage::Collected;
  return true;
}
bool HciPort::copyReadBlock(uint8_t *out, size_t bytes)
{
  Guard guard(busy_);
  if (!guard.held || !out || bytes != 4096 || stage_ != Stage::Collected ||
      accepted_.command != p::Command::Read10 || !validOwner() ||
      apps_smmu_owner_validate_ready(&token_, &claim_) != APPS_SMMU_OK) return false;
  p::ResponseResult result{};
  if (p::validateResponse(accepted_, context_, completed_ocs_, response_, 512, result) != p::Error::None ||
      result.status != p::ResponseStatus::Completed) return false;
  // The destination must not alias the retained DMA allocation: copying to it
  // would violate the no-write-after-completion ownership discipline.
  const uintptr_t dst = reinterpret_cast<uintptr_t>(out), base = reinterpret_cast<uintptr_t>(dma_.cpu);
  if (dst > UINTPTR_MAX - bytes || (dst < base + dma_.bytes && base < dst + bytes)) return false;
  copy(out, dma_.cpu + size_t(accepted_.data_address - dma_.physical), bytes);
  return true;
}
bool HciPort::stopQueue(uint64_t timeout_ticks, uint32_t max_polls)
{
  Guard guard(busy_);
  if (!guard.held || !timeout_ticks || !max_polls || max_polls > 100000 ||
      (stage_ != Stage::Bound && stage_ != Stage::Collected) || !validOwner() ||
      apps_smmu_owner_validate_ready(&token_, &claim_) != APPS_SMMU_OK) return false;
  r::Probe before{};
  if (!snapshot(before) || !ready(before, dma_.physical) || !write(0x60, 0)) return fail();
  uint64_t last = before.ticks;
  for (uint32_t i = 0; i < max_polls; ++i) {
    r::Probe live{};
    if (!snapshot(live) || !empty(live) || live.hce != 1 || live.interrupt_enable ||
        live.request_list != dma_.physical || live.ticks < last ||
        live.ticks - before.ticks >= timeout_ticks) return fail();
    last = live.ticks;
    if (live.run_stop == 0) return true;
    if (live.run_stop != 1) return fail();
  }
  return fail();
}
bool HciPort::probeThunk(void *v, r::Probe *out)
{
  if (!v || !out) return false;
  auto &self = *static_cast<HciPort *>(v); Guard guard(self.busy_);
  return guard.held && self.snapshot(*out);
}
bool HciPort::prepareThunk(void *v, const p::Request &request, const p::Context &context)
{
  if (!v) return false;
  auto &self = *static_cast<HciPort *>(v); Guard guard(self.busy_);
  return guard.held && self.prepare(request, context);
}
bool HciPort::ackThunk(void *v)
{
  if (!v) return false;
  auto &self = *static_cast<HciPort *>(v); Guard guard(self.busy_);
  return guard.held && self.ack();
}
bool HciPort::ringThunk(void *v)
{
  if (!v) return false;
  auto &self = *static_cast<HciPort *>(v); Guard guard(self.busy_);
  return guard.held && self.ring();
}
bool HciPort::pollThunk(void *v, r::Poll *out)
{
  if (!v || !out) return false;
  auto &self = *static_cast<HciPort *>(v); Guard guard(self.busy_);
  return guard.held && self.poll(*out);
}
bool HciPort::collectThunk(void *v, r::Completion *out)
{
  if (!v || !out) return false;
  auto &self = *static_cast<HciPort *>(v); Guard guard(self.busy_);
  return guard.held && self.collect(*out);
}
r::Port HciPort::port()
{ return {this, probeThunk, prepareThunk, ackThunk, ringThunk, pollThunk, collectThunk}; }
} // namespace sm8250_ufs
