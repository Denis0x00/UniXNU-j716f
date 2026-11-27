// SPDX-License-Identifier: BSD-2-Clause-Patent
// Offline C11 ASan/UBSan unit tests for J716fBacklightDsi.h (SM8250 DSI v2.4
// TPG DMA FIFO brightness protocol for Lenovo TB-J716F EDO RM692E6 AMOLED).
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../J716fBacklightDsi.h"

typedef struct FakeDsi {
    uint32_t regs[0x200u / 4u];
    uint32_t busy_reads_left;
    uint32_t done_reads_left;
    uint32_t fifo_words[8];
    uint32_t fifo_count;
    uint32_t trigger_count;
    uint32_t fifo_reset_pulses;
    uint32_t total_delay_us;
    uint32_t barrier_count;
    bool cleared_error_bit;
} FakeDsi;

static uint32_t fake_rd(void *ctx, uint32_t reg) {
    FakeDsi *d = (FakeDsi *)ctx;
    assert((reg & 3u) == 0u && reg < sizeof(d->regs));
    if (reg == J716F_DSI_STATUS) {
        if (d->busy_reads_left > 0u) {
            --d->busy_reads_left;
            return J716F_DSI_STATUS_CMD_MDP_BUSY;
        }
        return 0u;
    }
    if (reg == J716F_DSI_INT_CTRL) {
        uint32_t v = d->regs[reg / 4u];
        if (d->trigger_count > 0u) {
            if (d->done_reads_left > 0u) {
                --d->done_reads_left;
            } else {
                v |= J716F_DSI_INT_CMD_DMA_DONE;
                d->regs[reg / 4u] = v;
            }
        }
        return v;
    }
    return d->regs[reg / 4u];
}

static void fake_wr(void *ctx, uint32_t reg, uint32_t val) {
    FakeDsi *d = (FakeDsi *)ctx;
    assert((reg & 3u) == 0u && reg < sizeof(d->regs));
    if (reg == J716F_DSI_TEST_PATTERN_GEN_DMA_VAL) {
        assert(d->fifo_count < 8u);
        d->fifo_words[d->fifo_count++] = val;
        return;
    }
    if (reg == J716F_DSI_CMD_MODE_DMA_SW_TRIGGER && (val & 1u)) {
        ++d->trigger_count;
        return;
    }
    if (reg == J716F_DSI_TPG_DMA_FIFO_RESET) {
        if (val == 1u) ++d->fifo_reset_pulses;
        d->regs[reg / 4u] = val;
        return;
    }
    if (reg == J716F_DSI_INT_CTRL) {
        if (val & J716F_DSI_INT_ERROR_STATUS) d->cleared_error_bit = true;
        if (val & J716F_DSI_INT_CMD_DMA_DONE) {
            d->regs[reg / 4u] &= ~J716F_DSI_INT_CMD_DMA_DONE;
        }
        return;
    }
    d->regs[reg / 4u] = val;
}

static void fake_delay(void *ctx, uint32_t us) {
    FakeDsi *d = (FakeDsi *)ctx;
    d->total_delay_us += us;
}

static void fake_barrier(void *ctx) {
    FakeDsi *d = (FakeDsi *)ctx;
    ++d->barrier_count;
}

static void init_fake(FakeDsi *d) {
    memset(d, 0, sizeof(*d));
    d->regs[J716F_DSI_HW_VERSION / 4u] = 0x20040000u;
    d->regs[J716F_DSI_CTRL / 4u] = J716F_DSI_CTRL_ACTIVE_CMD_MASK | 0xF0u;
    d->regs[J716F_DSI_TRIG_CTRL / 4u] = (1u << 31) | J716F_DSI_TRIG_DMA_SW;
    d->regs[J716F_DSI_CLK_CTRL / 4u] = 0x00000001u;
    d->regs[J716F_DSI_COMMAND_MODE_DMA_CTRL / 4u] = J716F_DSI_DMA_CTRL_BROADCAST_EN;
    d->regs[J716F_DSI_INT_CTRL / 4u] = J716F_DSI_INT_ERROR_STATUS | (1u << 1);
}

static void test_encoding_and_math(void) {
    uint32_t w0 = 0u, w1 = 0u;

    j716f_dsi_encode_dcs51_11bit(0u, &w0, &w1);
    assert(w0 == 0xC0390003u);
    assert(w1 == 0xFF000051u);

    j716f_dsi_encode_dcs51_11bit(0x07FFu, &w0, &w1);
    assert(w0 == 0xC0390003u);
    assert(w1 == 0xFFFF0751u);

    /* Big-endian check: 0x0412 -> hi=0x04 at byte 1, lo=0x12 at byte 2. */
    j716f_dsi_encode_dcs51_11bit(0x0412u, &w0, &w1);
    assert(w0 == 0xC0390003u);
    assert(w1 == 0xFF120451u);

    /* Out-of-range clamp to 2047. */
    j716f_dsi_encode_dcs51_11bit(9999u, &w0, &w1);
    assert(w1 == 0xFFFF0751u);

    assert(j716f_bl_clamp_level(-10) == 0u);
    assert(j716f_bl_clamp_level(0) == 0u);
    assert(j716f_bl_clamp_level(1024) == 1024u);
    assert(j716f_bl_clamp_level(5000) == 2047u);

    assert(j716f_bl_level_for_nits(-1) == 0u);
    assert(j716f_bl_level_for_nits(0) == 0u);
    assert(j716f_bl_level_for_nits(1) == 1u); /* Positive nits never rounds to dark. */
    assert(j716f_bl_level_for_nits((int32_t)(J716F_BL_MAX_NITS << 16)) == 2047u);
    assert(j716f_bl_level_for_nits((int32_t)(250u << 16)) == 1024u);
}

static void test_hs_and_lp_transfer(void) {
    FakeDsi d;
    init_fake(&d);
    d.busy_reads_left = 3u; /* Simulate MDP busy for 2 polls after initial probe. */
    d.done_reads_left = 2u; /* Simulate DMA finishing on 3rd status read. */

    const J716fDsiIo io = {fake_rd, fake_wr, fake_delay, fake_barrier, &d};
    J716fDsiDiag diag = {0};

    /* HS transfer of 0x05A3 (hi=0x05, lo=0xA3). */
    int rc = j716f_dsi_send_brightness(&io, 0x05A3u, false, &diag);
    assert(rc == J716F_BL_OK);
    assert(d.fifo_count == 2u);
    assert(d.fifo_words[0] == 0xC0390003u);
    assert(d.fifo_words[1] == 0xFFA30551u);
    assert(d.trigger_count == 1u);
    assert(d.barrier_count == 1u);
    assert(d.fifo_reset_pulses == 2u); /* Pre-reset + post-reset cleanup. */
    assert(!d.cleared_error_bit);
    assert((diag.dma_ctrl & J716F_DSI_DMA_CTRL_BROADCAST_EN) == 0u);
    assert((diag.dma_ctrl & J716F_DSI_DMA_CTRL_IS_MASTER) != 0u);
    assert((diag.dma_ctrl & J716F_DSI_DMA_CTRL_EMBEDDED_MODE) != 0u);
    assert((diag.dma_ctrl & J716F_DSI_DMA_CTRL_USE_LPM) == 0u);
    assert(d.regs[J716F_DSI_DMA_CMD_LENGTH / 4u] == 8u);
    assert(d.regs[J716F_DSI_TEST_PATTERN_GEN_CTRL / 4u] == 0u);
    assert(d.regs[J716F_DSI_CLK_CTRL / 4u] == 0x00000001u); /* Restored. */
    assert(diag.busy_polls == 2u);
    assert(diag.done_polls == 2u);

    /* LP transfer + non-SW initial TRIG_CTRL restoration. */
    init_fake(&d);
    d.regs[J716F_DSI_TRIG_CTRL / 4u] = (1u << 31) | 0x0u;
    rc = j716f_dsi_send_brightness(&io, 0x0100u, true, &diag);
    assert(rc == J716F_BL_OK);
    assert((diag.dma_ctrl & J716F_DSI_DMA_CTRL_USE_LPM) != 0u);
    assert(d.fifo_words[1] == 0xFF000151u);
    assert(d.regs[J716F_DSI_TRIG_CTRL / 4u] == ((1u << 31) | 0x0u)); /* Restored. */
}

static void test_error_paths(void) {
    FakeDsi d;
    const J716fDsiIo io = {fake_rd, fake_wr, fake_delay, fake_barrier, &d};
    J716fDsiDiag diag = {0};

    /* Bad HW version. */
    init_fake(&d);
    d.regs[J716F_DSI_HW_VERSION / 4u] = 0xFFFFFFFFu;
    assert(j716f_dsi_send_brightness(&io, 1000u, false, &diag) == J716F_BL_ERR_BAD_HW_VER);
    assert(d.trigger_count == 0u);

    /* DSI disabled. */
    init_fake(&d);
    d.regs[J716F_DSI_CTRL / 4u] = J716F_DSI_CTRL_EN; /* CMD_MODE_EN missing. */
    assert(j716f_dsi_send_brightness(&io, 1000u, false, &diag) == J716F_BL_ERR_DSI_DISABLED);
    assert(d.trigger_count == 0u);

    /* MDP busy timeout. */
    init_fake(&d);
    d.busy_reads_left = J716F_DSI_WAIT_POLLS + 10u;
    assert(j716f_dsi_send_brightness(&io, 1000u, false, &diag) == J716F_BL_ERR_BUSY_TIMEOUT);
    assert(d.trigger_count == 0u);
    assert(d.regs[J716F_DSI_CLK_CTRL / 4u] == 0x00000001u);

    /* DMA done timeout still cleans up TPG FIFO & restores clocks. */
    init_fake(&d);
    d.done_reads_left = J716F_DSI_WAIT_POLLS + 10u;
    assert(j716f_dsi_send_brightness(&io, 1000u, false, &diag) == J716F_BL_ERR_DMA_TIMEOUT);
    assert(d.trigger_count == 1u);
    assert(d.regs[J716F_DSI_TEST_PATTERN_GEN_CTRL / 4u] == 0u);
    assert(d.regs[J716F_DSI_CLK_CTRL / 4u] == 0x00000001u);
}

int main(void) {
    test_encoding_and_math();
    test_hs_and_lp_transfer();
    test_error_paths();
    printf("J716fBacklightDsiTest: PASS\n");
    return 0;
}
