#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SM8250_GCC_BASE 0x00100000ULL
#define SM8250_GCC_SIZE 0x000a0000ULL

/* SM8250 QUPv3_0 Wrapper (0x009c0000) & SE6 UART (0x00998000) for QCA6390 Bluetooth */
#define SM8250_QUP0_BASE 0x009c0000ULL
#define SM8250_QUP0_SIZE 0x00004000ULL
#define SM8250_UART6_BASE 0x00998000ULL
#define SM8250_UART6_SIZE 0x00004000ULL
#define SM8250_QUP0_SID 0x05a3U
#define SM8250_UART6_INTID 639U

/* SM8250 GCC QUPv3_WRAP0_S6 clock registers */
#define SM8250_GCC_APCS_CLOCK_BRANCH_ENA_VOTE 0x00052008U
#define SM8250_GCC_APCS_GPLL_ENA_VOTE 0x00052018U
#define SM8250_GCC_QUPV3_WRAP0_S6_CBCR 0x0001772cU
#define SM8250_GCC_QUPV3_WRAP0_S6_CMD_RCGR 0x00017730U
#define SM8250_GCC_QUPV3_WRAP0_S6_CFG_RCGR 0x00017734U
#define SM8250_GCC_QUPV3_WRAP0_S6_M 0x00017738U
#define SM8250_GCC_QUPV3_WRAP0_S6_N 0x0001773cU
#define SM8250_GCC_QUPV3_WRAP0_S6_D 0x00017740U
#define SM8250_GCC_QUPV3_WRAP0_S6_DFS_SR 0x00017744U
#define SM8250_GCC_QUPV3_WRAP0_S6_PERF_DFSR(l) (0x0001774cU + 4U * (l))
#define SM8250_GCC_QUPV3_WRAP0_S6_PERF_M_DFSR(l) (0x0001778cU + 4U * (l))
#define SM8250_GCC_QUPV3_WRAP0_S6_PERF_N_DFSR(l) (0x000177ccU + 4U * (l))
#define SM8250_GCC_QUPV3_WRAP0_S6_PERF_D_DFSR(l) (0x0001780cU + 4U * (l))

struct sm8250_geni_clock_io {
    void *context;
    uint32_t (*read)(void *context, uint32_t reg);
    void (*write)(void *context, uint32_t reg, uint32_t value);
    void (*delay)(void *context, unsigned int us);
};

bool sm8250_gcc_update32(volatile uint32_t *reg, uint32_t clear, uint32_t set);
bool sm8250_geni_uart6_vote(volatile uint32_t *vote_reg, bool enabled);
int sm8250_geni_uart6_source(const struct sm8250_geni_clock_io *io, uint32_t baud, uint32_t *out_hz);

#ifdef __cplusplus
}
#endif
