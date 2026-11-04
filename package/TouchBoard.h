// SPDX-License-Identifier: BSD-2-Clause-Patent
// Lenovo Xiaoxin Pad Pro 2021 (TB-J716F, SM8250 "kona") Goodix GT7386 touch board constants.
// Verified from j716f.dtb (i2c@a94000 -> goodix-gt738x@5d) and stock kernel disassembly.
#ifndef J716F_TOUCH_BOARD_H
#define J716F_TOUCH_BOARD_H
#include <stdint.h>

// QUPv3 Wrapper 1 (qcom,qupv3_1_geni_se@ac0000) Serial Engine 5 (i2c@a94000)
#define TB_QUP_WRAPPER_BASE 0x00ac0000ULL
#define TB_SE_BASE          0x00a94000ULL
#define TB_SE_SIZE          0x4000ULL

// apps_smmu@15000000 stream 0x43 for QUPv3 Wrapper 1 (used only if FIFO_IF_DISABLE is set)
#define TB_QUP_SMMU_SID     0x0043u
#define TB_APPS_SMMU_BASE   0x15000000ULL
#define TB_APPS_SMMU_SIZE   0x100000ULL

// RPMh AOP cmd-db & apps_rsc drv-2 (j716f.dtb: cmd_db_mem@80860000, rsc@18200000 drv-2@18220000)
#define TB_CMD_DB_BASE      0x80860000ULL
#define TB_CMD_DB_SIZE      0x20000ULL
#define TB_RSC_DRV_BASE     0x18220000ULL
#define TB_RSC_DRV_SIZE     0x10000ULL
#define TB_RSC_TCS_OFFSET   0x0d00u

// GCC (qcom,gcc@100000): QUPv3 Wrap 1 votes in 0x52008, GPLL0 in 0x52018
#define TB_GCC_BASE         0x00100000ULL
#define TB_GCC_VOTE_ENABLE  0x52008u
#define TB_GCC_GPLL0_ENABLE 0x52018u
#define TB_GCC_GPLL0_MODE   0x00000u

// gcc-sm8250.c QUPv3 Wrapper 1 SE5 branch vote bits & CBCR halt registers
#define TB_VOTE_WRAP1_M_AHB   (1u << 18)  // halt 0x18004
#define TB_VOTE_WRAP1_S_AHB   (1u << 19)  // halt 0x18008
#define TB_VOTE_WRAP1_CORE    (1u << 20)  // halt 0x23138
#define TB_VOTE_WRAP1_CORE_2X (1u << 21)  // halt 0x23140
#define TB_VOTE_WRAP1_S5      (1u << 27)  // halt 0x185fc
#define TB_VOTE_ALL (TB_VOTE_WRAP1_M_AHB | TB_VOTE_WRAP1_S_AHB | TB_VOTE_WRAP1_CORE | TB_VOTE_WRAP1_CORE_2X | TB_VOTE_WRAP1_S5)

#define TB_HALT_M_AHB       0x18004u
#define TB_HALT_S_AHB       0x18008u
#define TB_HALT_S5          0x185fcu
#define TB_HALT_CORE        0x23138u
#define TB_HALT_CORE_2X     0x23140u
#define TB_CBCR_CLK_OFF     (1u << 31)

// gcc_qupv3_wrap1_s5_clk_src RCG at 0x18600
#define TB_S5_RCG           0x18600u
#define TB_RCG_CMD          0x00u
#define TB_RCG_CFG          0x04u
#define TB_RCG_DFSR_CMD     0x14u
#define TB_RCG_PERF_CFG(l)  (0x1cu + 4u * (l))
#define TB_RCG_PERF_M(l)    (0x5cu + 4u * (l))
#define TB_RCG_PERF_N(l)    (0x9cu + 4u * (l))
#define TB_DFS_LEVELS       8u

// TLMM SOUTH tile (0x0f000000 + 0x500000 = 0x0f500000): GPIO 36..39
#define TB_TLMM_SOUTH_BASE  0x0f500000ULL
#define TB_GPIO_STRIDE      0x1000u
#define TB_GPIO_I2C_SDA     36u  // func 1 = qup13
#define TB_GPIO_I2C_SCL     37u  // func 1 = qup13
#define TB_GPIO_RESET       38u  // active-low reset output
#define TB_GPIO_IRQ         39u  // active-low / falling-edge attention input

// Goodix GT7385P / GT7386 ("phoneix" / gtx8) I2C addresses & registers
#define GTX8_ADDR_PRIMARY   0x5du
#define GTX8_ADDR_FALLBACK  0x14u
#define ST_FTS_ADDR_PROBE   0x49u

#define GTX8_REG_CMD        0x8040u
#define GTX8_REG_HANDSHAKE  0x8043u
#define GTX8_REG_VERSION    0x8240u  // 12 bytes: [4..7] ASCII PID e.g. "7386"
#define GTX8_REG_TOUCH_DATA 0x824du  // [0] bit7 ready, [1] low4 count & bit5 stylus, [2..] 8B records + checksum

#define TB_PANEL_MAX_X      2560u
#define TB_PANEL_MAX_Y      1600u
#define TB_HID_MAX_X        30479u
#define TB_HID_MAX_Y        20319u

static inline uint64_t tb_src_hz(unsigned sel) {
    return sel == 0 ? 19200000ULL : sel == 1 ? 600000000ULL : sel == 6 ? 300000000ULL : 0ULL;
}

static inline uint64_t tb_calc_rate(uint64_t prate, uint32_t m, uint32_t n, unsigned mode, unsigned hid) {
    if (hid) { prate *= 2ULL; prate /= (hid + 1ULL); }
    if (mode && n) { prate *= m; prate /= n; }
    return prate;
}

static inline uint64_t tb_dfs_level_hz(uint32_t cfg, uint32_t mreg, uint32_t nreg) {
    unsigned src = (cfg >> 8) & 7u, mode = (cfg >> 12) & 3u, hid = cfg & 0x1fu;
    uint32_t m = 0, n = 0;
    if (mode) { m = mreg & 0xffffu; n = ((~nreg) & 0xffffu) + m; }
    return tb_calc_rate(tb_src_hz(src), m, n, mode, hid);
}

#endif
