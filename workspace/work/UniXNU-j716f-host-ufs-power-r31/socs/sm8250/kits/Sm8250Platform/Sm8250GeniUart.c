#include "Sm8250GeniUart.h"
#include <sys/errno.h>

enum {
    /* GCC_APCS_CLOCK_BRANCH_ENA_VOTE (0x52008) bits for QUPv3_WRAP0 core/2x/m_ahb/s_ahb + S6 */
    kQup0VoteMask = (1u << 6) | (1u << 7) | (1u << 8) | (1u << 9) | (1u << 16),
    /* gcc-sm8250.c gcc_parent_map_0: P_GPLL0_OUT_EVEN (300 MHz) has cfg = 6, mode = 2 (dual-edge MND).
     * SM8250 QUPv3 HW version is 0x20050000 (2.5.0), which uses 16x UART oversampling. */
    kCfgSrcGpll0Even = 6u << 8,
    kCfgModeDualEdge = 2u << 12,
};

struct rcg_freq_entry {
    uint32_t baud;
    uint32_t freq_hz;
    uint16_t hid_div;
    uint16_t m;
    uint16_t n;
};

/* SM8250 GPLL0_OUT_EVEN = 300 MHz (hid_div = 2*1 - 1 = 1) with 16x UART oversampling: */
static const struct rcg_freq_entry kFreqTable[] = {
    {  115200u,  7372800u, 1u, 384u, 15625u }, /* 300 MHz * 384 / 15625 = 7.3728 MHz (115200 * 16 * 4) */
    { 3000000u, 48000000u, 1u,   4u,    25u }, /* 300 MHz * 4 / 25      = 48.0 MHz   (3000000 * 16 * 1) */
    { 3200000u, 51200000u, 1u,  64u,   375u }, /* 300 MHz * 64 / 375    = 51.2 MHz   (3200000 * 16 * 1) */
    { 4000000u, 64000000u, 1u,  16u,    75u }, /* 300 MHz * 16 / 75     = 64.0 MHz   (4000000 * 16 * 1) */
};

bool sm8250_gcc_update32(volatile uint32_t *reg, uint32_t clear, uint32_t set) {
    if (!reg)
        return false;
    uint32_t val = (*reg & ~clear) | set;
    *reg = val;
    __asm__ volatile("dsb sy" ::: "memory");
    (void)*reg;
    return true;
}

bool sm8250_geni_uart6_vote(volatile uint32_t *vote_reg, bool enabled) {
    if (!vote_reg)
        return false;
    if (enabled)
        return sm8250_gcc_update32(vote_reg, 0, kQup0VoteMask);
    return sm8250_gcc_update32(vote_reg, kQup0VoteMask, 0);
}

int sm8250_geni_uart6_source(const struct sm8250_geni_clock_io *io, uint32_t baud, uint32_t *out_hz) {
    if (!io || !io->read || !io->write || !out_hz)
        return -EINVAL;

    const struct rcg_freq_entry *entry = NULL;
    for (size_t i = 0; i < sizeof(kFreqTable) / sizeof(kFreqTable[0]); i++) {
        if (kFreqTable[i].baud == baud) {
            entry = &kFreqTable[i];
            break;
        }
    }
    if (!entry)
        return -EINVAL;

    uint32_t cfg = kCfgSrcGpll0Even | (uint32_t)(entry->hid_div & 0x1fu);
    uint32_t m_val = 0, not_n_m = 0, not_2d = 0;
    if (entry->m && entry->n) {
        cfg |= kCfgModeDualEdge;
        m_val = entry->m;
        not_n_m = (~(uint32_t)(entry->n - entry->m)) & 0xffffu;
        not_2d = (~(uint32_t)entry->n) & 0xffffu;
    }

    io->write(io->context, SM8250_GCC_QUPV3_WRAP0_S6_M, m_val);
    io->write(io->context, SM8250_GCC_QUPV3_WRAP0_S6_N, not_n_m);
    io->write(io->context, SM8250_GCC_QUPV3_WRAP0_S6_D, not_2d);

    uint32_t old_cfg = io->read(io->context, SM8250_GCC_QUPV3_WRAP0_S6_CFG_RCGR);
    io->write(io->context, SM8250_GCC_QUPV3_WRAP0_S6_CFG_RCGR, (old_cfg & ~0x3fffu) | cfg);

    uint32_t cmd = io->read(io->context, SM8250_GCC_QUPV3_WRAP0_S6_CMD_RCGR);
    io->write(io->context, SM8250_GCC_QUPV3_WRAP0_S6_CMD_RCGR, cmd | 1u);

    for (unsigned int wait = 0; wait < 1000u; wait++) {
        if ((io->read(io->context, SM8250_GCC_QUPV3_WRAP0_S6_CMD_RCGR) & 1u) == 0) {
            *out_hz = entry->freq_hz;
            return 0;
        }
        if (io->delay)
            io->delay(io->context, 2);
    }
    return -ETIMEDOUT;
}
