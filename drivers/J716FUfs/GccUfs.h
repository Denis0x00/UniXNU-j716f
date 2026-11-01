// SPDX-License-Identifier: BSD-2-Clause-Patent
// SM8250 GCC registers used by UFS, transcribed from Linux gcc-sm8250.c.
#ifndef J716F_UFS_GCC_UFS_H
#define J716F_UFS_GCC_UFS_H

#define GCC_BASE                      0x00100000ULL
#define GCC_UFS_BASE                  0x00177000ULL
#define UFS_HC_BASE                   0x01d84000ULL
#define UFS_HC_SIZE                   0x3000ULL

#define GCC_UFS_PHY_GDSCR             0x004
#define GCC_UFS_PHY_AXI_ENABLE        0x010
#define GCC_UFS_PHY_AHB_ENABLE        0x018
#define GCC_UFS_PHY_PHY_AUX_ENABLE    0x09c

#define GCC_UFS_BRANCH_ENABLE_BIT     0x1U
#endif
