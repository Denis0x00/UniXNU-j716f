// SPDX-License-Identifier: BSD-2-Clause-Patent
// Three-level/4 KiB mapper for XNU ARM64 T1SZ=26 (38-bit VA, 256-entry L1 root).
// Fixes the T1SZ=25 (& 511 -> root[448]) bug in package/native-log/NativeMap.h
// that caused 43:BOOT_MAP_ERR on real TB-J716F hardware (2026-10-29 Run 1).
#ifndef J716F_NATIVE_MAP_FIXED_H
#define J716F_NATIVE_MAP_FIXED_H
#include "NativeLog.h"
#define JLF_PAGE_MASK 0x0000fffffffff000ULL
#define JLF_BLOCK_MASK 0x0000ffffffe00000ULL
#define JLF_HINT 0x0010000000000000ULL
#define JLF_L1_MASK 255ULL

typedef struct {
  JL64 *(*resolve)(JL64 pa);
  JL64 *(*allocate)(JL64 *pa);
} JLFMapOps;

static inline int jlf_map_window_t1sz26(JL64 *root, JL64 va, JL64 original_pa,
    const JLFMapOps *ops, JL64 attributes)
{
  JL32 page, i;
  if ((va | original_pa) & 4095ULL) return 0;
  for (page = 0; page < JL_CONSOLE_BYTES / 4096U; ++page) {
    JL64 at = va + (JL64)page * 4096ULL, old, alloc_pa, expected;
    JL64 *l2, *l3, *entry;
    JL64 l1_idx = (at >> 30) & JLF_L1_MASK;
    if ((root[l1_idx] & 3ULL) != 3ULL) return 0;
    l2 = ops->resolve(root[l1_idx] & JLF_PAGE_MASK);
    if (!l2) return 0;
    entry = &l2[(at >> 21) & 511ULL];
    old = *entry;
    if ((old & 3ULL) == 1ULL) {
      l3 = ops->allocate(&alloc_pa);
      if (!l3 || (alloc_pa & 4095ULL) || (alloc_pa & ~JLF_PAGE_MASK)) return 0;
      for (i = 0; i < 512U; ++i)
        l3[i] = ((old & JLF_BLOCK_MASK) + (JL64)i * 4096ULL) |
                (old & ~(JLF_BLOCK_MASK | JLF_HINT)) | 2ULL;
      *entry = alloc_pa | 3ULL;
    } else if ((old & 3ULL) == 3ULL) {
      l3 = ops->resolve(old & JLF_PAGE_MASK);
      if (!l3) return 0;
    } else {
      return 0;
    }
    expected = original_pa + (JL64)page * 4096ULL;
    if ((l3[(at >> 12) & 511ULL] & (JLF_PAGE_MASK | 3ULL)) != (expected | 3ULL)) return 0;
    for (i = 0; i < 512U; ++i) l3[i] &= ~JLF_HINT;
    l3[(at >> 12) & 511ULL] = (JL_CONSOLE_PA + (JL64)page * 4096ULL) | attributes;
  }
  return 1;
}
#endif
