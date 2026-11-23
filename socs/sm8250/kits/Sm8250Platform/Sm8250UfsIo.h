// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef SM8250_UFS_IO_H
#define SM8250_UFS_IO_H
#include <stddef.h>
#include <stdint.h>

namespace sm8250_ufs {
enum class CacheOp : unsigned { Clean, Invalidate, CleanInvalidate };
// OS boundary: mapped-device access, monotonic time and cache maintenance.
// All callbacks are synchronous and bounded; false means the operation may
// have partially happened. MMIO arguments are physical addresses. Cache ranges
// are byte offsets within the one retained DmaView, never arbitrary pointers.
// barrier is a full system DMA/MMIO ordering barrier (DSB SY on this platform).
struct Io {
  void *context;
  bool (*read32)(void *, uint64_t physical, uint32_t *);
  bool (*write32)(void *, uint64_t physical, uint32_t);
  bool (*barrier)(void *);
  bool (*ticks)(void *, uint64_t *);
  bool (*cache)(void *, CacheOp, size_t offset, size_t bytes);
};
struct DmaView { uint8_t *cpu; uint64_t physical; size_t bytes; };
} // namespace sm8250_ufs
#endif
