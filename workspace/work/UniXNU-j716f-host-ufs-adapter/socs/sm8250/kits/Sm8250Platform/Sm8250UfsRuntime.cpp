// SPDX-License-Identifier: BSD-2-Clause-Patent
// TB-J716F ZUI 15.0.485 DTB0/DTBO1 supplies host/SMMU resources. The stock
// UFS node has no dma-coherent/iommus property: SID ownership is NOT inferred.
#include <IOKit/IOLib.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IODeviceMemory.h>
#include <libkern/libkern.h>
#include <kern/task.h>
#include "Sm8250UfsRuntime.h"

namespace {
using namespace sm8250_ufs;
namespace policy = shengufs::read_policy;
namespace restricted = shengufs::restricted;
constexpr size_t dma_bytes = 139264;
struct Map { uint64_t physical, bytes; IOMemoryMap *object; uintptr_t cpu; };
struct Runtime {
  RuntimePhase phase = RuntimePhase::Cold;
  RuntimeAccess access = {};
  RuntimeAllocation allocation = {};
  Map maps[3] = {};
  IOBufferMemoryDescriptor *buffer = nullptr;
  bool prepared = false, writes_allowed = false;
  uint32_t global_pages = 0;
  DmaView dma = {};
  HandoffTransaction transaction;
  HciPort hci;
  restricted::SingleSlot slot;
  policy::Context context = {};
  restricted::Budget budget = {};
  HandoffResult handoff = {};
  restricted::Result command = {};
  AppsSmmuClaim claim = {};
  AppsSmmuToken token = {};
};
// Explicit static workspace keeps full SMMU snapshots off the kernel stack.
// All members have constant initialization; there is no OSObject constructor.
Runtime runtime;
unsigned runtime_busy;
unsigned runtime_initialized;
struct Guard {
  bool held;
  Guard() : held(__atomic_exchange_n(&runtime_busy, 1, __ATOMIC_ACQUIRE) == 0) {}
  ~Guard() { if (held) __atomic_store_n(&runtime_busy, 0, __ATOMIC_RELEASE); }
};
unsigned report(RuntimeReport *out, RuntimeError error, AppsSmmuResult owner = APPS_SMMU_OK)
{
  if (out) {
    *out = RuntimeReport{};
    out->error = error; out->phase = runtime.phase;
    out->allocation = runtime.allocation; out->handoff = runtime.handoff;
    out->command = runtime.command; out->owner_error = owner;
  }
  return static_cast<unsigned>(error);
}
bool window(uint64_t address, uint64_t base, uint64_t bytes)
{ return !(address & 3U) && address >= base && bytes >= 4 && address - base <= bytes - 4; }
bool indexed(uint64_t offset, uint64_t first, unsigned count)
{ return offset >= first && offset - first < uint64_t(count) * 4 && !((offset - first) & 3); }
bool allowed_read(uint64_t address)
{
  const auto &p = *shengufs::platformProfile(shengufs::Platform::Sm8250);
  if (address == p.gcc + p.gdsc || address == p.gcc + p.reset) return true;
  for (unsigned i = 0; i < shengufs::ClockCount; ++i)
    if (address == p.gcc + p.clocks[i].offset) return true;
  if (window(address, p.host.base, p.host.bytes)) {
    switch (address - p.host.base) {
      case 0: case 8: case 0x20: case 0x24: case 0x30: case 0x34:
      case 0x50: case 0x54: case 0x58: case 0x60:
      case 0x70: case 0x74: case 0x78: case 0x80: return true;
      default: return false;
    }
  }
  if (!window(address, p.smmu.base, p.smmu.bytes)) return false;
  const uint64_t offset = address - p.smmu.base;
  if (offset == 0 || offset == 0x20 || offset == 0x24 || offset == 0x28 ||
      indexed(offset, 0x800, 128) || indexed(offset, 0xc00, 128) ||
      indexed(offset, 0x1000, 128) || indexed(offset, 0x1800, 128)) return true;
  const uint64_t cb = uint64_t(runtime.global_pages) * 4096;
  return runtime.global_pages && offset >= cb && offset - cb < 128 * 4096 &&
         !((offset - cb) & 4095);
}
bool allowed_write(uint64_t address)
{
  if (!runtime.writes_allowed) return false;
  const auto &p = *shengufs::platformProfile(shengufs::Platform::Sm8250);
  if (address == p.host.base + 0x20 || address == p.host.base + 0x50 ||
      address == p.host.base + 0x54 || address == p.host.base + 0x58 ||
      address == p.host.base + 0x60) return true;
  if (!window(address, p.smmu.base, p.smmu.bytes)) return false;
  const auto &c = runtime.claim;
  const uint64_t offset = address - p.smmu.base;
  for (unsigned i = 0; i < 2; ++i)
    if (offset == 0x800 + 4U * c.slots[i] || offset == 0xc00 + 4U * c.slots[i]) return true;
  return offset == 0x1000 + 4U * c.bank || offset == 0x1800 + 4U * c.bank ||
         (runtime.global_pages && offset == (uint64_t(runtime.global_pages) + c.bank) * 4096);
}
uintptr_t mapped(uint64_t address)
{
  for (const auto &m : runtime.maps)
    if (m.object && window(address, m.physical, m.bytes)) return m.cpu + address - m.physical;
  return 0;
}
bool read32(void *, uint64_t address, uint32_t *out)
{
  if (!out || !allowed_read(address)) return false;
  const uintptr_t va = mapped(address);
  if (!va) return false;
  // Synchronous external aborts are NOT caught. Board access evidence must
  // establish powered/readable registers before entering this OS boundary.
  __asm__ volatile("dsb sy" ::: "memory");
  *out = *reinterpret_cast<volatile uint32_t *>(va);
  __asm__ volatile("dsb sy" ::: "memory");
  if (address == 0x15000024) runtime.global_pages = 1U << (((*out >> 28) & 7U) + 1U);
  return true;
}
bool write32(void *, uint64_t address, uint32_t value)
{
  if (!allowed_write(address)) return false;
  const uintptr_t va = mapped(address);
  if (!va) return false;
  __asm__ volatile("dsb sy" ::: "memory");
  *reinterpret_cast<volatile uint32_t *>(va) = value;
  __asm__ volatile("dsb sy" ::: "memory");
  return true;
}
bool barrier(void *) { __asm__ volatile("dsb sy" ::: "memory"); return true; }
bool ticks(void *, uint64_t *value)
{
  if (!value) return false;
  __asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(*value) :: "memory");
  return true;
}
bool cache(void *, CacheOp op, size_t offset, size_t bytes)
{
  if (!runtime.dma.cpu || !bytes || (offset & 63) || (bytes & 63) ||
      offset > runtime.dma.bytes || bytes > runtime.dma.bytes - offset) return false;
  if (op != CacheOp::Clean && op != CacheOp::Invalidate && op != CacheOp::CleanInvalidate) return false;
  const uintptr_t first = reinterpret_cast<uintptr_t>(runtime.dma.cpu) + offset;
  __asm__ volatile("dsb sy" ::: "memory");
  for (size_t at = 0; at < bytes; at += 64) {
    const uintptr_t address = first + at;
    if (op == CacheOp::Clean) __asm__ volatile("dc cvac, %0" :: "r"(address) : "memory");
    else if (op == CacheOp::Invalidate) __asm__ volatile("dc ivac, %0" :: "r"(address) : "memory");
    else __asm__ volatile("dc civac, %0" :: "r"(address) : "memory");
  }
  __asm__ volatile("dsb sy" ::: "memory");
  return true;
}
Io io() { return {nullptr, read32, write32, barrier, ticks, cache}; }
void release_unused()
{
  // Called only before any owner installation or hardware publication.
  for (auto &m : runtime.maps) {
    if (m.object) m.object->release();
    m = {};
  }
  if (runtime.buffer) {
    if (runtime.prepared) runtime.buffer->complete(kIODirectionInOut);
    runtime.buffer->release();
  }
  runtime.buffer = nullptr; runtime.prepared = false; runtime.dma = {};
  runtime.allocation = {}; runtime.phase = RuntimePhase::Failed;
}
bool map_range(unsigned index, uint64_t physical, uint64_t bytes)
{
  IODeviceMemory *memory = IODeviceMemory::withRange(physical, bytes);
  if (!memory) return false;
  IOMemoryMap *mapping = memory->map(kIOMapAnywhere | kIOMapInhibitCache);
  memory->release();
  if (!mapping) return false;
  const uintptr_t cpu = static_cast<uintptr_t>(mapping->getVirtualAddress());
  if (!cpu || mapping->getLength() < bytes || cpu > UINTPTR_MAX - bytes) {
    mapping->release(); return false;
  }
  runtime.maps[index] = {physical, bytes, mapping, cpu};
  return true;
}
void retain_failure()
{
  runtime.phase = RuntimePhase::Quarantined;
  // A failed quarantine call (e.g. metadata BUSY) still cannot free anything.
  if (runtime.token.lease_id) (void)apps_smmu_owner_quarantine(&runtime.token, 0x52554e54);
}
policy::Request command(policy::Command kind, uint64_t lba = 0)
{
  policy::Request r = {};
  r.upiu[3] = runtime.context.expected_tag;
  if (kind == policy::Command::PowerMode) {
    r.upiu[0] = 0x16; r.upiu[5] = 1; r.upiu[12] = 3; r.upiu[13] = 2;
  } else if (kind != policy::Command::Nop) {
    r.upiu[0] = 1; r.upiu[2] = runtime.context.selected_lun;
    if (kind == policy::Command::ReadCapacity10) { r.upiu[16] = 0x25; r.data_bytes = 8; }
    if (kind == policy::Command::Read10) {
      r.upiu[16] = 0x28; r.data_bytes = 4096;
      for (unsigned i = 0; i < 4; ++i) r.upiu[18 + i] = uint8_t(lba >> (24 - 8 * i));
      r.upiu[24] = 1;
    }
  }
  if (r.data_bytes) {
    r.direction = policy::Direction::FromDevice; r.prd_count = 1; r.upiu[1] = 0x40;
    for (unsigned i = 0; i < 4; ++i) {
      r.upiu[12 + i] = uint8_t(r.data_bytes >> (24 - 8 * i));
      r.prd[i] = uint8_t(runtime.context.dma.data.address >> (8 * i));
      r.prd[4 + i] = uint8_t(runtime.context.dma.data.address >> (32 + 8 * i));
      r.prd[12 + i] = uint8_t((r.data_bytes - 1) >> (8 * i));
    }
  }
  return r;
}
bool execute(policy::Command kind, uint64_t lba = 0)
{
  const auto request = command(kind, lba);
  runtime.command = runtime.slot.execute(request, runtime.token, runtime.hci.port(), runtime.budget);
  return runtime.command.error == restricted::Error::None;
}
}

extern "C" void sm8250_ufs_adapter_init(void)
{ __atomic_store_n(&runtime_initialized, 1U, __ATOMIC_RELEASE); }

extern "C" unsigned sm8250_ufs_allocate(const RuntimeAccess *access, RuntimeReport *out)
{
  Guard guard;
  if (!guard.held) return static_cast<unsigned>(RuntimeError::Busy);
  if (!__atomic_load_n(&runtime_initialized, __ATOMIC_ACQUIRE)) return report(out, RuntimeError::NotInitialized);
  if (runtime.phase != RuntimePhase::Cold) return report(out, RuntimeError::WrongPhase);
  if (!access || !out) return report(out, RuntimeError::InvalidAccess);
  const RuntimeAccess a = *access;
  if (a.version != 1 || a.board != 0x4a373136 || !a.authorization ||
      a.facts != RequiredAccessFacts || !kernel_task) return report(out, RuntimeError::InvalidAccess);
  uint64_t ctr;
  __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
  if ((4U << ((ctr >> 16) & 15)) != 64) return report(out, RuntimeError::CacheGeometry);
  runtime.access = a;
  runtime.buffer = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task,
    static_cast<IOOptionBits>(kIODirectionInOut) | static_cast<IOOptionBits>(kIOMemoryPhysicallyContiguous) |
    static_cast<IOOptionBits>(kIOMemoryMapperNone), dma_bytes, 4096);
  if (!runtime.buffer) { release_unused(); return report(out, RuntimeError::Allocation); }
  if (runtime.buffer->prepare(kIODirectionInOut) != kIOReturnSuccess) {
    release_unused(); return report(out, RuntimeError::Allocation);
  }
  runtime.prepared = true;
  IOByteCount contiguous = 0;
  const uint64_t physical = runtime.buffer->getPhysicalSegment(0, &contiguous, kIOMemoryMapperNone);
  auto *cpu = static_cast<uint8_t *>(runtime.buffer->getBytesNoCopy());
  if (!cpu || !physical || (physical & 4095) || (reinterpret_cast<uintptr_t>(cpu) & 4095) ||
      physical > UINT64_MAX - dma_bytes || reinterpret_cast<uintptr_t>(cpu) > UINTPTR_MAX - dma_bytes ||
      contiguous < dma_bytes || runtime.buffer->getLength() != dma_bytes) {
    release_unused(); return report(out, RuntimeError::Allocation);
  }
  runtime.dma = {cpu, physical, dma_bytes};
  bzero(cpu, dma_bytes);
  if (!cache(nullptr, CacheOp::CleanInvalidate, 0, dma_bytes) ||
      !map_range(0, 0x177000, 4096) || !map_range(1, 0x1d84000, 0x3000) ||
      !map_range(2, 0x15000000, 0x100000)) {
    release_unused(); return report(out, RuntimeError::Mapping);
  }
  runtime.allocation = {1, physical, dma_bytes}; // One allocation lifetime per boot.
  runtime.phase = RuntimePhase::Allocated;
  return report(out, RuntimeError::None);
}

extern "C" unsigned sm8250_ufs_activate(const RuntimeStart *input, RuntimeReport *out)
{
  Guard guard;
  if (!guard.held) return static_cast<unsigned>(RuntimeError::Busy);
  if (runtime.phase != RuntimePhase::Allocated) return report(out, RuntimeError::WrongPhase);
  if (!input || !out) return report(out, RuntimeError::InvalidStart);
  RuntimeStart start = *input;
  const auto &p = *shengufs::platformProfile(shengufs::Platform::Sm8250);
  if (start.allocation_id != runtime.allocation.id ||
      start.handoff.register_access_authorization != runtime.access.authorization ||
      !shengufs::matches(p, start.handoff.identity) ||
      start.handoff.owner.claim.dma_base != runtime.dma.physical ||
      start.handoff.owner.claim.dma_bytes != dma_bytes || !policy::ordinaryLun(start.lun) ||
      start.known_block_count > 8 || !start.command_budget.timeout_ticks ||
      !start.command_budget.max_polls || start.command_budget.max_polls > 100000 ||
      start.command_budget.retries > 2) return report(out, RuntimeError::InvalidStart);
  for (unsigned i = 0; i < start.known_block_count; ++i) {
    if (start.known_blocks[i].lun != start.lun || start.known_blocks[i].lba > UINT32_MAX)
      return report(out, RuntimeError::InvalidStart);
    for (unsigned j = 0; j < i; ++j)
      if (start.known_blocks[j].lba == start.known_blocks[i].lba) return report(out, RuntimeError::InvalidStart);
  }
  // Do not retain caller-controlled text pointers across hardware callbacks.
  start.handoff.identity = {p.soc, p.controller, p.host, p.smmu};
  runtime.claim = start.handoff.owner.claim;
  runtime.context = {};
  auto &c = runtime.context;
  c.selected_lun = start.lun; c.expected_tag = start.tag;
  c.dma = {{runtime.dma.physical, dma_bytes}, {runtime.dma.physical, 32},
           {runtime.dma.physical + 4096, 32}, {runtime.dma.physical + 4608, 512},
           {runtime.dma.physical + 5120, 16}, {runtime.dma.physical + 8192, 131072}};
  c.known_block_count = start.known_block_count;
  for (unsigned i = 0; i < c.known_block_count; ++i) c.known_blocks[i] = start.known_blocks[i];
  if (runtime.slot.configure(c, runtime.claim) != restricted::Error::None)
    return report(out, RuntimeError::InvalidStart);
  runtime.budget = start.command_budget;
  runtime.writes_allowed = true;
  runtime.handoff = runtime.transaction.run(start.handoff, io(), runtime.dma, start.handoff_budget);
  runtime.token = runtime.handoff.token;
  if (runtime.handoff.error != HandoffError::None) {
    runtime.phase = (runtime.handoff.quarantined || runtime.handoff.lease_retained) ?
      RuntimePhase::Quarantined : RuntimePhase::Failed;
    // Even unpublished failures retain this one-shot context for examination.
    runtime.writes_allowed = false;
    return report(out, RuntimeError::Handoff);
  }
  if (!runtime.hci.bind(io(), runtime.dma, runtime.claim, runtime.token) || !runtime.hci.setupQueue()) {
    retain_failure(); return report(out, RuntimeError::Queue);
  }
  const policy::Command startup[] = {policy::Command::Nop, policy::Command::PowerMode,
                                    policy::Command::TestUnitReady, policy::Command::ReadCapacity10};
  for (auto kind : startup) {
    if (!execute(kind)) { retain_failure(); return report(out, RuntimeError::Command); }
  }
  runtime.phase = RuntimePhase::Active;
  return report(out, RuntimeError::None);
}

extern "C" unsigned sm8250_ufs_read_known(uint64_t lba, uint8_t *output, size_t bytes, RuntimeReport *out)
{
  Guard guard;
  if (!guard.held) return static_cast<unsigned>(RuntimeError::Busy);
  if (runtime.phase != RuntimePhase::Active) return report(out, RuntimeError::WrongPhase);
  if (!output || !out || bytes != 4096 || lba > UINT32_MAX) return report(out, RuntimeError::InvalidStart);
  if (!execute(policy::Command::Read10, lba)) {
    // Policy refusal before callbacks leaves the active session available.
    if (runtime.command.error != restricted::Error::PolicyRejected) retain_failure();
    return report(out, RuntimeError::Command);
  }
  if (!runtime.hci.copyReadBlock(output, bytes)) { retain_failure(); return report(out, RuntimeError::Read); }
  return report(out, RuntimeError::None);
}

extern "C" unsigned sm8250_ufs_stop(RuntimeReport *out)
{
  Guard guard;
  if (!guard.held) return static_cast<unsigned>(RuntimeError::Busy);
  if (!out || runtime.phase != RuntimePhase::Active) return report(out, RuntimeError::WrongPhase);
  if (!runtime.hci.stopQueue(runtime.budget.timeout_ticks, runtime.budget.max_polls)) {
    retain_failure(); return report(out, RuntimeError::Stop);
  }
  runtime.command = runtime.slot.confirmStopped(runtime.token, runtime.hci.port());
  if (runtime.command.error != restricted::Error::None) { retain_failure(); return report(out, RuntimeError::Stop); }
  const auto owner = apps_smmu_owner_release(&runtime.token);
  if (owner != APPS_SMMU_OK) { retain_failure(); return report(out, RuntimeError::Owner, owner); }
  runtime.phase = RuntimePhase::Stopped; runtime.writes_allowed = false;
  // Mapping remains published: do not complete/release DMA or unmap registers.
  return report(out, RuntimeError::None);
}

extern "C" unsigned sm8250_ufs_discard_unused(RuntimeReport *out)
{
  Guard guard;
  if (!guard.held) return static_cast<unsigned>(RuntimeError::Busy);
  if (!out || runtime.phase != RuntimePhase::Allocated || runtime.writes_allowed)
    return report(out, RuntimeError::WrongPhase);
  release_unused(); return report(out, RuntimeError::None);
}
extern "C" unsigned sm8250_ufs_status(RuntimeReport *out)
{
  Guard guard;
  if (!guard.held) return static_cast<unsigned>(RuntimeError::Busy);
  return report(out, out ? RuntimeError::None : RuntimeError::InvalidStart);
}
