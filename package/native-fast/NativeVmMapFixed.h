// SPDX-License-Identifier: BSD-2-Clause-Patent
// Pre-TTBR1-switch linear bootstrap PA->KVA resolver for j716f_native_vm_map().
//
// Root cause of Run 2 (44:PERM_MAP_ERR -> panic -> 41:EL1_SYNC_FLT):
// j716f_native_vm_map() executes AFTER arm_vm_physmap_init(args) overwrites
// _ptov_table with 2 MiB-aligned post-switch KVA mappings (+0x200000 slide for
// [topOfKernelData, real_avail_end)), BEFORE set_mmu_ttb_alternate(cpu_ttep)
// switches TTBR1_EL1 off bootstrap_pagetables (0xd2f7e000).
// Calling _phystokv(pa) in that window returns the post-switch KVA, which under
// bootstrap_pagetables translates linearly to (pa + 0x200000) in uninitialized
// DRAM (reading 0 instead of the L2/L3 PTEs of cpu_tte).
// Resolving via linear bootstrap translation (pa - gPhysBase + gVirtBase, i.e.
// _phystokv+0x168 at 0xfffffff0073f8548) resolves the exact active VA in
// bootstrap_pagetables before TTBR1_EL1 switches to cpu_ttep.
#ifndef J716F_NATIVE_VMMAP_FIXED_H
#define J716F_NATIVE_VMMAP_FIXED_H

#include "NativeLog.h"

static inline JL64 jlf_bootstrap_phystokv(
    JL64 pa, JL64 phys_base, JL64 phys_size, JL64 virt_base)
{
  if (pa < phys_base || (pa - phys_base) >= phys_size) return 0;
  return virt_base + (pa - phys_base);
}

#endif
