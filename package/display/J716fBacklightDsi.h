// SPDX-License-Identifier: BSD-2-Clause-Patent
// Pure hardware protocol & register logic for TB-J716F (SM8250) EDO RM692E6
// dual-DSI AMOLED panel backlight via DSI0 (0x0AE94000, qcom,dcs-cmd-by-left)
// using the Qualcomm DSI v2.4 TPG DMA FIFO path (pure MMIO, no RAM DMA / SMMU).
//
// Verified against stock TB-J716F dtbo-overlay-01 (fragment@37,
// qcom,mdss_dsi_rm692e6_edo_amoled_cmd) and SM8250 techpack/display dsi_ctrl:
//   - Panel mode: dsi_cmd_mode, dual DSI (ctrl 0 + 1), qcom,dcs-cmd-by-left
//   - Brightness: bl_ctrl_dcs, 11-bit big-endian DCS 0x51 (0x0000..0x07FF)
//     on-command:        39 01 00 00 00 00 03 51 07 ff
//     highlight-off-cmd: 39 01 00 00 00 00 03 51 07 ff
//   - Embedded FIFO packet (8 bytes = 2 DWORDs, even -> no extra zero DWORD):
//     word0 = 0xC0390003  (WC=3, DT=0x39 DCS Long Write, BIT(7) LAST | BIT(6) LONG)
//     word1 = 0xFF000051 | ((level >> 8) << 8) | ((level & 0xFF) << 16)
#ifndef J716F_BACKLIGHT_DSI_H
#define J716F_BACKLIGHT_DSI_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define J716F_DSI0_PHYS_BASE                 0x0AE94000ULL
#define J716F_DSI0_PHYS_SIZE                 0x400u

/* SM8250 Qualcomm DSI Controller v2.4 register offsets (dsi_ctrl_reg.h). */
#define J716F_DSI_HW_VERSION                 0x0000u
#define J716F_DSI_CTRL                       0x0004u
#define J716F_DSI_STATUS                     0x0008u
#define J716F_DSI_FIFO_STATUS                0x000Cu
#define J716F_DSI_COMMAND_MODE_DMA_CTRL      0x003Cu
#define J716F_DSI_DMA_CMD_OFFSET             0x0048u
#define J716F_DSI_DMA_CMD_LENGTH             0x004Cu
#define J716F_DSI_TRIG_CTRL                  0x0084u
#define J716F_DSI_CMD_MODE_DMA_SW_TRIGGER    0x0090u
#define J716F_DSI_INT_CTRL                   0x0110u
#define J716F_DSI_CLK_CTRL                   0x011Cu
#define J716F_DSI_TEST_PATTERN_GEN_CTRL      0x015Cu
#define J716F_DSI_TEST_PATTERN_GEN_DMA_VAL   0x017Cu
#define J716F_DSI_TPG_DMA_FIFO_STATUS        0x01DCu
#define J716F_DSI_TPG_DMA_FIFO_RESET         0x01ECu

/* Register bitfields. */
#define J716F_DSI_CTRL_EN                    (1u << 0)
#define J716F_DSI_CTRL_VIDEO_MODE_EN         (1u << 1)
#define J716F_DSI_CTRL_CMD_MODE_EN           (1u << 2)
#define J716F_DSI_CTRL_ACTIVE_CMD_MASK       (J716F_DSI_CTRL_EN | J716F_DSI_CTRL_CMD_MODE_EN)

#define J716F_DSI_STATUS_CMD_ENGINE_BUSY     (1u << 0)
#define J716F_DSI_STATUS_CMD_DMA_BUSY        (1u << 1)
#define J716F_DSI_STATUS_CMD_MDP_BUSY        (1u << 2)
#define J716F_DSI_STATUS_TX_BUSY_MASK        (J716F_DSI_STATUS_CMD_DMA_BUSY | J716F_DSI_STATUS_CMD_MDP_BUSY)

#define J716F_DSI_DMA_CTRL_BROADCAST_EN      (1u << 31)
#define J716F_DSI_DMA_CTRL_IS_MASTER         (1u << 30)
#define J716F_DSI_DMA_CTRL_EMBEDDED_MODE     (1u << 28)
#define J716F_DSI_DMA_CTRL_USE_LPM           (1u << 26)

#define J716F_DSI_TRIG_DMA_MASK              0x7u
#define J716F_DSI_TRIG_DMA_SW                0x4u

#define J716F_DSI_INT_CMD_DMA_DONE           (1u << 0)
#define J716F_DSI_INT_ERROR_STATUS           (1u << 24)

/* dsi_ctrl_hw_cmn.c: DSI_CTRL_DYNAMIC_FORCE_ON = (0x23F | BIT(8) | BIT(9) | BIT(11) | BIT(21)). */
#define J716F_DSI_CLK_DYNAMIC_FORCE_ON       0x00200B3Fu

/* dsi_ctrl_hw_cmn.c: CMD_DMA_TPG_EN (BIT(1)) | TPG_DMA_FIFO_MODE (BIT(2)) | CUSTOM_PATTERN (3 << 16). */
#define J716F_DSI_TPG_CMD_DMA_FIFO_CFG       ((1u << 1) | (1u << 2) | (3u << 16))

/* RM692E6 brightness parameters (dtbo-overlay-01 fragment@37). */
#define J716F_BL_MAX_LEVEL                   2047u
#define J716F_BL_DEFAULT_LEVEL               1280u
#define J716F_BL_MIN_NITS                    2u
#define J716F_BL_MID_NITS                    140u
#define J716F_BL_MAX_NITS                    500u

/* Return codes from j716f_dsi_probe_hw / j716f_dsi_send_brightness. */
#define J716F_BL_OK                          0
#define J716F_BL_ERR_BAD_ARG                -1
#define J716F_BL_ERR_BAD_HW_VER             -2
#define J716F_BL_ERR_DSI_DISABLED           -3
#define J716F_BL_ERR_BUSY_TIMEOUT           -4
#define J716F_BL_ERR_DMA_TIMEOUT            -5

/* Wait budget: 500 * 50 us = 25 ms (> 2 frames at 90 Hz). */
#define J716F_DSI_WAIT_POLLS                 500u
#define J716F_DSI_WAIT_STEP_US               50u

typedef struct J716fDsiIo {
    uint32_t (*rd)(void *ctx, uint32_t reg);
    void (*wr)(void *ctx, uint32_t reg, uint32_t val);
    void (*delay_us)(void *ctx, uint32_t us);
    void (*barrier)(void *ctx);
    void *ctx;
} J716fDsiIo;

typedef struct J716fDsiDiag {
    uint32_t hw_ver;
    uint32_t ctrl;
    uint32_t status;
    uint32_t trig_ctrl;
    uint32_t clk_ctrl;
    uint32_t dma_ctrl;
    uint32_t int_ctrl;
    uint32_t busy_polls;
    uint32_t done_polls;
} J716fDsiDiag;

static inline uint32_t j716f_bl_clamp_level(int32_t requested) {
    if (requested <= 0) return 0u;
    if ((uint32_t)requested > J716F_BL_MAX_LEVEL) return J716F_BL_MAX_LEVEL;
    return (uint32_t)requested;
}

/* Signed 16.16 nits -> 11-bit level; a positive nits request never rounds down to off. */
static inline uint32_t j716f_bl_level_for_nits(int32_t fixed_16_16) {
    if (fixed_16_16 <= 0) return 0u;
    const uint64_t scale = (uint64_t)J716F_BL_MAX_NITS << 16;
    const uint64_t scaled = ((uint64_t)(uint32_t)fixed_16_16 * J716F_BL_MAX_LEVEL + scale / 2u) / scale;
    if (scaled < 1u) return 1u;
    if (scaled > J716F_BL_MAX_LEVEL) return J716F_BL_MAX_LEVEL;
    return (uint32_t)scaled;
}

/* Encode 11-bit big-endian DCS 0x51 (WRDISBV) into two 32-bit embedded FIFO DWORDs. */
static inline void j716f_dsi_encode_dcs51_11bit(uint32_t level, uint32_t *word0, uint32_t *word1) {
    if (level > J716F_BL_MAX_LEVEL) level = J716F_BL_MAX_LEVEL;
    const uint32_t hi = (level >> 8) & 0x7u;
    const uint32_t lo = level & 0xFFu;
    /* Header bytes: [0]=WC_LSB(0x03), [1]=WC_MSB(0x00), [2]=DT(0x39), [3]=LAST|LONG(0xC0). */
    *word0 = 0xC0390003u;
    /* Payload bytes: [4]=0x51, [5]=hi, [6]=lo, [7]=0xFF (32-bit pad). */
    *word1 = 0xFF000051u | (hi << 8) | (lo << 16);
}

static inline int j716f_dsi_probe_hw(const J716fDsiIo *io, J716fDsiDiag *diag) {
    if (!io || !io->rd || !io->wr || !io->delay_us) return J716F_BL_ERR_BAD_ARG;
    const uint32_t hw_ver   = io->rd(io->ctx, J716F_DSI_HW_VERSION);
    const uint32_t ctrl     = io->rd(io->ctx, J716F_DSI_CTRL);
    const uint32_t status   = io->rd(io->ctx, J716F_DSI_STATUS);
    const uint32_t trig     = io->rd(io->ctx, J716F_DSI_TRIG_CTRL);
    const uint32_t clk      = io->rd(io->ctx, J716F_DSI_CLK_CTRL);
    const uint32_t dma_ctrl = io->rd(io->ctx, J716F_DSI_COMMAND_MODE_DMA_CTRL);
    const uint32_t int_ctrl = io->rd(io->ctx, J716F_DSI_INT_CTRL);
    if (diag) {
        diag->hw_ver     = hw_ver;
        diag->ctrl       = ctrl;
        diag->status     = status;
        diag->trig_ctrl  = trig;
        diag->clk_ctrl   = clk;
        diag->dma_ctrl   = dma_ctrl;
        diag->int_ctrl   = int_ctrl;
        diag->busy_polls = 0u;
        diag->done_polls = 0u;
    }
    if (hw_ver == 0u || hw_ver == 0xFFFFFFFFu || ((hw_ver >> 28) != 2u)) {
        return J716F_BL_ERR_BAD_HW_VER;
    }
    if ((ctrl & J716F_DSI_CTRL_ACTIVE_CMD_MASK) != J716F_DSI_CTRL_ACTIVE_CMD_MASK) {
        return J716F_BL_ERR_DSI_DISABLED;
    }
    return J716F_BL_OK;
}

static inline void j716f_dsi_reset_tpg_fifo(const J716fDsiIo *io) {
    io->wr(io->ctx, J716F_DSI_TEST_PATTERN_GEN_CTRL, 0u);
    io->wr(io->ctx, J716F_DSI_TPG_DMA_FIFO_RESET, 1u);
    io->delay_us(io->ctx, 1u);
    io->wr(io->ctx, J716F_DSI_TPG_DMA_FIFO_RESET, 0u);
}

static inline void j716f_dsi_clear_dma_done(const J716fDsiIo *io) {
    /* Preserve interrupt enable bits and never clear BIT(24) error status. */
    uint32_t reg = io->rd(io->ctx, J716F_DSI_INT_CTRL);
    reg |= J716F_DSI_INT_CMD_DMA_DONE;
    reg &= ~J716F_DSI_INT_ERROR_STATUS;
    io->wr(io->ctx, J716F_DSI_INT_CTRL, reg);
}

static inline int j716f_dsi_send_brightness(const J716fDsiIo *io, uint32_t level,
                                            bool use_lpm, J716fDsiDiag *diag) {
    J716fDsiDiag local = {0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u};
    int rc = j716f_dsi_probe_hw(io, &local);
    if (rc != J716F_BL_OK) {
        if (diag) *diag = local;
        return rc;
    }

    /* Force DSI clocks on for the short command transfer; restore on exit. */
    const uint32_t orig_clk = local.clk_ctrl;
    io->wr(io->ctx, J716F_DSI_CLK_CTRL, orig_clk | J716F_DSI_CLK_DYNAMIC_FORCE_ON);

    /* Wait until neither MDP nor a prior DMA transfer is using the DSI command bus. */
    uint32_t busy_polls = 0u;
    uint32_t st = io->rd(io->ctx, J716F_DSI_STATUS);
    while ((st & J716F_DSI_STATUS_TX_BUSY_MASK) != 0u && busy_polls < J716F_DSI_WAIT_POLLS) {
        io->delay_us(io->ctx, J716F_DSI_WAIT_STEP_US);
        ++busy_polls;
        st = io->rd(io->ctx, J716F_DSI_STATUS);
    }
    local.status = st;
    local.busy_polls = busy_polls;
    if ((st & J716F_DSI_STATUS_TX_BUSY_MASK) != 0u) {
        io->wr(io->ctx, J716F_DSI_CLK_CTRL, orig_clk);
        if (diag) *diag = local;
        return J716F_BL_ERR_BUSY_TIMEOUT;
    }

    /* Ensure DMA trigger source is SW trigger (4) while preserving MDP/TE bits. */
    const uint32_t orig_trig = local.trig_ctrl;
    const bool trig_changed = (orig_trig & J716F_DSI_TRIG_DMA_MASK) != J716F_DSI_TRIG_DMA_SW;
    if (trig_changed) {
        io->wr(io->ctx, J716F_DSI_TRIG_CTRL,
               (orig_trig & ~J716F_DSI_TRIG_DMA_MASK) | J716F_DSI_TRIG_DMA_SW);
    }

    j716f_dsi_clear_dma_done(io);
    j716f_dsi_reset_tpg_fifo(io);

    uint32_t word0 = 0u, word1 = 0u;
    j716f_dsi_encode_dcs51_11bit(level, &word0, &word1);

    /* Program TPG DMA FIFO with the 8-byte (2-DWORD, even) embedded DCS packet. */
    io->wr(io->ctx, J716F_DSI_TEST_PATTERN_GEN_CTRL, J716F_DSI_TPG_CMD_DMA_FIFO_CFG);
    io->wr(io->ctx, J716F_DSI_TEST_PATTERN_GEN_DMA_VAL, word0);
    io->wr(io->ctx, J716F_DSI_TEST_PATTERN_GEN_DMA_VAL, word1);

    /* qcom,dcs-cmd-by-left: broadcast OFF, master ON, embedded mode ON, HS or LP. */
    uint32_t dma_ctrl = io->rd(io->ctx, J716F_DSI_COMMAND_MODE_DMA_CTRL);
    dma_ctrl &= ~J716F_DSI_DMA_CTRL_BROADCAST_EN;
    dma_ctrl |= J716F_DSI_DMA_CTRL_IS_MASTER | J716F_DSI_DMA_CTRL_EMBEDDED_MODE;
    if (use_lpm) {
        dma_ctrl |= J716F_DSI_DMA_CTRL_USE_LPM;
    } else {
        dma_ctrl &= ~J716F_DSI_DMA_CTRL_USE_LPM;
    }
    io->wr(io->ctx, J716F_DSI_COMMAND_MODE_DMA_CTRL, dma_ctrl);
    local.dma_ctrl = dma_ctrl;

    io->wr(io->ctx, J716F_DSI_DMA_CMD_LENGTH, 8u);
    if (io->barrier) io->barrier(io->ctx);
    io->wr(io->ctx, J716F_DSI_CMD_MODE_DMA_SW_TRIGGER, 1u);

    /* Poll for CMD_MODE_DMA_DONE (BIT(0)) in DSI_INT_CTRL. */
    uint32_t done_polls = 0u;
    uint32_t intr = io->rd(io->ctx, J716F_DSI_INT_CTRL);
    while ((intr & J716F_DSI_INT_CMD_DMA_DONE) == 0u && done_polls < J716F_DSI_WAIT_POLLS) {
        io->delay_us(io->ctx, J716F_DSI_WAIT_STEP_US);
        ++done_polls;
        intr = io->rd(io->ctx, J716F_DSI_INT_CTRL);
    }
    local.int_ctrl = intr;
    local.done_polls = done_polls;
    local.status = io->rd(io->ctx, J716F_DSI_STATUS);

    const bool completed = (intr & J716F_DSI_INT_CMD_DMA_DONE) != 0u;
    if (completed) {
        j716f_dsi_clear_dma_done(io);
    }
    j716f_dsi_reset_tpg_fifo(io);
    if (trig_changed) {
        io->wr(io->ctx, J716F_DSI_TRIG_CTRL, orig_trig);
    }
    io->wr(io->ctx, J716F_DSI_CLK_CTRL, orig_clk);

    if (diag) *diag = local;
    return completed ? J716F_BL_OK : J716F_BL_ERR_DMA_TIMEOUT;
}

#ifdef __cplusplus
}
#endif

#endif /* J716F_BACKLIGHT_DSI_H */
