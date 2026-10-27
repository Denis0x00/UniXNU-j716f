// SPDX-License-Identifier: BSD-2-Clause-Patent
// Three-level/4 KiB mapper for INACTIVE roots only. No live break-before-make.
#ifndef J716F_NATIVE_MAP_H
#define J716F_NATIVE_MAP_H
#include "NativeLog.h"
#define JL_PAGE_MASK 0x0000fffffffff000ULL
#define JL_BLOCK_MASK 0x0000ffffffe00000ULL
#define JL_HINT 0x0010000000000000ULL
typedef struct {
  JL64 *(*resolve)(JL64 pa);
  JL64 *(*allocate)(JL64 *pa);
} JLMapOps;

static inline __attribute__((always_inline)) int jl_map_window(JL64 *root, JL64 va, JL64 original_pa,
    const JLMapOps *ops, JL64 attributes)
{
  JL32 page, i;
  if ((va | original_pa) & 4095ULL) return 0;
  for (page = 0; page < JL_CONSOLE_BYTES/4096; ++page) {
    JL64 at = va + (JL64)page*4096, old, alloc_pa, expected;
    JL64 *l2, *l3, *entry;
    if ((root[(at>>30)&511] & 3) != 3) return 0;
    l2 = ops->resolve(root[(at>>30)&511] & JL_PAGE_MASK);
    if (!l2) return 0;
    entry = &l2[(at>>21)&511]; old = *entry;
    if ((old & 3) == 1) {
      l3 = ops->allocate(&alloc_pa);
      if (!l3 || (alloc_pa & 4095) || (alloc_pa & ~JL_PAGE_MASK)) return 0;
      for (i = 0; i < 512; ++i)
        l3[i] = ((old & JL_BLOCK_MASK) + (JL64)i*4096) |
                (old & ~(JL_BLOCK_MASK | JL_HINT)) | 2ULL;
      // Block AP/XN are now inherited by every child. Target pages are the
      // explicitly reserved dummy window, not neighbouring live kernel data.
      *entry = alloc_pa | 3ULL;
    } else if ((old & 3) == 3) {
      l3 = ops->resolve(old & JL_PAGE_MASK);
      if (!l3) return 0;
    } else return 0;
    expected = original_pa + (JL64)page*4096;
    if ((l3[(at>>12)&511] & (JL_PAGE_MASK | 3ULL)) != (expected | 3ULL)) return 0;
    // An old contiguous group cannot retain HINT after only some PAs change.
    for (i = 0; i < 512; ++i) l3[i] &= ~JL_HINT;
    l3[(at>>12)&511] = (JL_CONSOLE_PA + (JL64)page*4096) | attributes;
  }
  return 1;
}
#endif
