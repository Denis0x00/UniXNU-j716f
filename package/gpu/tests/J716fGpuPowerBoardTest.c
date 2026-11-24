// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host ASan/UBSan unit test for package/gpu/J716fGpuPowerBoard.h.
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../J716fGpuPowerBoard.h"

struct mock_hw {
    uint32_t cfg_ahb, memnoc, dvm, misc, axi, pll_vote;
    uint32_t cx_gdscr, cx_hwctrl, gx_gdscr, cc_ahb, cc_smmu, cc_gmu, cc_cxo, pll1_mode;
    uint32_t gmu_cmd, gmu_cfg;
    uint32_t scr0, id0, id1, id2, gfsr, smr[4], s2cr[4];
    uint32_t rbbm_status;
    uint32_t gmu_sysreset, gmu_fw, gmu_gx, gmu_hfi, gmu_gen7, gmu_ao_irq, gmu_fence;
    uint32_t rscc_busy, rscc_status;
    unsigned reads, writes;
    int stall_rcg, stall_branch;
};

static int mock_read(void *cookie, unsigned w, uint32_t off, uint32_t *out) {
    struct mock_hw *m = (struct mock_hw *)cookie;
    ++m->reads;
    switch (w) {
    case GPB_WIN_GCC_GPU:
        if (off == GPB_GCC_GPU_CFG_AHB) { *out = m->cfg_ahb; return 0; }
        if (off == GPB_GCC_GPU_MEMNOC_GFX) { *out = m->memnoc; return 0; }
        if (off == GPB_GCC_GPU_SNOC_DVM_GFX) { *out = m->dvm; return 0; }
        if (off == GPB_GCC_GPU_MISC) { *out = m->misc; return 0; }
        if (off == GPB_GCC_DDRSS_GPU_AXI) { *out = m->axi; return 0; }
        return -1;
    case GPB_WIN_GCC_VOTE:
        if (off == GPB_GCC_PLL_VOTE) { *out = m->pll_vote; return 0; }
        return -1;
    case GPB_WIN_GPUCC:
        if (off == GPB_CC_CX_GDSCR) { *out = m->cx_gdscr; return 0; }
        if (off == GPB_CC_CX_GDS_HW_CTRL) { *out = m->cx_hwctrl; return 0; }
        if (off == GPB_CC_GX_GDSCR) { *out = m->gx_gdscr; return 0; }
        if (off == GPB_CC_AHB_CLK) { *out = m->cc_ahb; return 0; }
        if (off == GPB_CC_SMMU_VOTE_CLK) { *out = m->cc_smmu; return 0; }
        if (off == GPB_CC_CX_GMU_CLK) { *out = m->cc_gmu; return 0; }
        if (off == GPB_CC_CXO_CLK) { *out = m->cc_cxo; return 0; }
        if (off == GPB_CC_GMU_RCGR) { *out = m->gmu_cmd; return 0; }
        if (off == GPB_CC_GMU_RCGR + 4u) { *out = m->gmu_cfg; return 0; }
        if (off == GPB_CC_PLL1_MODE) { *out = m->pll1_mode; return 0; }
        return -1;
    case GPB_WIN_SMMU:
        if (off == GPB_SMMU_SCR0) { *out = m->scr0; return 0; }
        if (off == GPB_SMMU_ID0) { *out = m->id0; return 0; }
        if (off == GPB_SMMU_ID1) { *out = m->id1; return 0; }
        if (off == GPB_SMMU_ID2) { *out = m->id2; return 0; }
        if (off == GPB_SMMU_GFSR) { *out = m->gfsr; return 0; }
        for (unsigned i = 0; i < 4u; ++i) {
            if (off == GPB_SMMU_SMR(i)) { *out = m->smr[i]; return 0; }
            if (off == GPB_SMMU_S2CR(i)) { *out = m->s2cr[i]; return 0; }
        }
        return -1;
    case GPB_WIN_GPU:
        if (off == GPB_RBBM_STATUS_BYTES) { *out = m->rbbm_status; return 0; }
        return -1;
    case GPB_WIN_GMU:
        if (off == GPB_GMU_CM3_SYSRESET) { *out = m->gmu_sysreset; return 0; }
        if (off == GPB_GMU_CM3_FW_INIT_RESULT) { *out = m->gmu_fw; return 0; }
        if (off == GPB_GMU_SPTPRAC_PWR_CLK_STATUS) { *out = m->gmu_gx; return 0; }
        if (off == GPB_GMU_HFI_CTRL_STATUS) { *out = m->gmu_hfi; return 0; }
        if (off == GPB_GMU_GENERAL_7) { *out = m->gmu_gen7; return 0; }
        if (off == GPB_GMU_AO_HOST_INTERRUPT_STATUS) { *out = m->gmu_ao_irq; return 0; }
        if (off == GPB_GMU_AHB_FENCE_STATUS) { *out = m->gmu_fence; return 0; }
        return -1;
    case GPB_WIN_RSCC:
        if (off == GPB_RSCC_SEQ_BUSY_DRV0) { *out = m->rscc_busy; return 0; }
        if (off == GPB_RSCC_RSC_STATUS0_DRV0) { *out = m->rscc_status; return 0; }
        return -1;
    default:
        return -1;
    }
}

static int mock_write(void *cookie, unsigned w, uint32_t off, uint32_t val) {
    struct mock_hw *m = (struct mock_hw *)cookie;
    ++m->writes;
    if (w == GPB_WIN_GCC_GPU && off == GPB_GCC_GPU_CFG_AHB) {
        m->cfg_ahb = val & ~GPB_CBCR_CLK_OFF;
        return 0;
    }
    if (w == GPB_WIN_GCC_GPU && off == GPB_GCC_DDRSS_GPU_AXI) { m->axi = val & ~GPB_CBCR_CLK_OFF; return 0; }
    if (w == GPB_WIN_GCC_GPU && off == GPB_GCC_GPU_MEMNOC_GFX) { m->memnoc = val & ~GPB_CBCR_CLK_OFF; return 0; }
    if (w == GPB_WIN_GCC_VOTE && off == GPB_GCC_PLL_VOTE) { m->pll_vote = val; return 0; }
    if (w == GPB_WIN_GPUCC && off == GPB_CC_CX_GDSCR) {
        m->cx_gdscr = val;
        if (!(val & GPB_GDSC_SW_COLLAPSE)) {
            m->cx_gdscr |= GPB_GDSC_PWR_ON;
            m->cx_hwctrl |= GPB_GDSC_PWR_ON;
        }
        return 0;
    }
    if (w == GPB_WIN_GPUCC && off == GPB_CC_AHB_CLK) { m->cc_ahb = val & ~GPB_CBCR_CLK_OFF; return 0; }
    if (w == GPB_WIN_GPUCC && off == GPB_CC_SMMU_VOTE_CLK) { m->cc_smmu = val & ~GPB_CBCR_CLK_OFF; return 0; }
    if (w == GPB_WIN_GPUCC && off == GPB_CC_CXO_CLK) { m->cc_cxo = val & ~GPB_CBCR_CLK_OFF; return 0; }
    if (w == GPB_WIN_GPUCC && off == GPB_CC_CX_GMU_CLK) {
        m->cc_gmu = val;
        if ((val & GPB_CBCR_ENABLE) && !m->stall_branch) m->cc_gmu &= ~GPB_CBCR_CLK_OFF;
        return 0;
    }
    if (w == GPB_WIN_GPUCC && off == GPB_CC_GMU_RCGR) {
        m->gmu_cmd = val;
        if ((val & GPB_RCG_ROOT_EN) && !m->stall_rcg) m->gmu_cmd &= ~GPB_RCG_ROOT_OFF;
        if ((val & GPB_RCG_UPDATE) && !m->stall_rcg) m->gmu_cmd &= ~GPB_RCG_UPDATE;
        return 0;
    }
    if (w == GPB_WIN_GPUCC && off == GPB_CC_GMU_RCGR + 4u) { m->gmu_cfg = val; return 0; }
    return -1;
}

static void put_le16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put_le32(uint8_t *p, uint32_t v) { put_le16(p, (uint16_t)v); put_le16(p + 2, (uint16_t)(v >> 16)); }

static void test_staged_modes(void) {
    struct mock_hw hw;
    memset(&hw, 0, sizeof(hw));
    hw.cfg_ahb = GPB_CBCR_CLK_OFF;
    hw.cx_gdscr = GPB_GDSC_SW_COLLAPSE;
    hw.gx_gdscr = GPB_GDSC_SW_COLLAPSE;
    hw.cc_ahb = GPB_CBCR_CLK_OFF;
    hw.cc_smmu = GPB_CBCR_CLK_OFF;
    hw.cc_gmu = GPB_CBCR_CLK_OFF;
    hw.gmu_cmd = GPB_RCG_ROOT_OFF;
    hw.id0 = 4u;
    hw.smr[1] = 0x80000405u;
    hw.s2cr[1] = 0x00000001u;
    hw.rbbm_status = 0x00000010u;
    hw.gmu_sysreset = 1u;
    hw.rscc_status = 0x00000002u;

    const struct gpb_io io = {&hw, mock_read, mock_write, NULL};
    const unsigned all_win = (1u << GPB_WINDOWS) - 1u;
    struct gpb_result r;

    // Mode 0: completely dormant
    gpb_run(&io, 0u, all_win, &r);
    assert(r.gcc == GPB_SKIPPED_MODE && hw.reads == 0u && hw.writes == 0u);

    // Mode 1: pure read-only, skips GPUCC/SMMU/GPU when cfg_ahb is off
    gpb_run(&io, 1u, all_win, &r);
    assert(r.gcc == GPB_DONE && r.cc == GPB_SKIPPED_PREREQ && r.smmu == GPB_SKIPPED_PREREQ && r.gpu == GPB_SKIPPED_PREREQ);
    assert(hw.writes == 0u && r.io_error == 0);

    // Mode 2: enables cfg_ahb only, reads GPUCC, skips SMMU (CX off) and GPU (GX off)
    gpb_run(&io, 2u, all_win, &r);
    assert(r.gcc == GPB_DONE && r.enabled_ahb == 1u && r.cc == GPB_DONE);
    assert(r.smmu == GPB_SKIPPED_PREREQ && r.gpu == GPB_SKIPPED_PREREQ);
    assert(hw.writes == 1u && r.io_error == 0);

    // Mode 3: enables CX GDSC + SMMU clocks, reads SMMU global space 0, leaves GX untouched
    gpb_run(&io, 3u, all_win, &r);
    assert(r.powered_cx == 1u && r.powered_clocks == 1u && r.smmu == GPB_DONE);
    assert(r.smr_valid == 1u && r.smr_first == 1u && r.smr_first_val == 0x80000405u && r.s2cr_first_val == 1u);
    assert(r.gpu == GPB_SKIPPED_PREREQ && r.io_error == 0);

    // Mode 6: brings up GMU 200 MHz clocks and reads GMU/RSCC state
    struct gpb_gmu_clock_on clk;
    gpb_gmu_clocks_on(&io, &r, &clk);
    assert(clk.step == 5u && clk.failed == 0 && r.io_error == 0);
    assert((clk.vote & GPB_GPLL0_VOTES) == GPB_GPLL0_VOTES);
    assert((clk.gmu_cfg & 0x71Fu) == ((GPB_GMU_SRC_SEL << 8) | GPB_GMU_SRC_HID));
    assert(gpb_gmu_regs_readable(&r, clk.cx_gmu, clk.ahb));

    struct gpb_gmu_state gs;
    memset(&gs, 0, sizeof(gs));
    gpb_read_gmu_state(&io, &r, &gs);
    assert(gs.sysreset == 1u && gs.rscc_status == 2u && r.io_error == 0);

    // Failure path: RCG timeout stops cleanly at step 2
    hw.gmu_cmd = GPB_RCG_ROOT_OFF;
    hw.stall_rcg = 1;
    gpb_gmu_clocks_on(&io, &r, &clk);
    assert(clk.step == 2u && clk.failed == 1);
}

static void test_cmd_db(void) {
    uint8_t db[512];
    memset(db, 0, sizeof(db));
    put_le32(db + 0, 1u);
    put_le32(db + 4, GPB_CMD_DB_MAGIC);
    // Slave 0: slv_id=3 (ARC), header_offset=0, data_offset=64, cnt=1
    put_le16(db + 8, 3u);
    put_le16(db + 10, 0u);
    put_le16(db + 12, 64u);
    put_le16(db + 14, 1u);
    // Entry 0 at GPB_CMD_DB_DATA + 0: "gfx.lvl\0", addr=0x30010, len=4, aux_off=0
    memcpy(db + GPB_CMD_DB_DATA, "gfx.lvl", 7u);
    put_le32(db + GPB_CMD_DB_DATA + 16, 0x30010u);
    put_le16(db + GPB_CMD_DB_DATA + 20, 4u);
    put_le16(db + GPB_CMD_DB_DATA + 22, 0u);
    put_le16(db + GPB_CMD_DB_DATA + 64, 64u);
    put_le16(db + GPB_CMD_DB_DATA + 66, 256u);

    struct gpb_cmd_db_entry ent;
    assert(gpb_cmd_db_find(db, sizeof(db), "gfx.lvl", &ent) == 0);
    assert(ent.address == 0x30010u && ent.slave == 3u && ent.aux_bytes == 4u);
    assert(gpb_le16(ent.aux) == 64u && gpb_le16(ent.aux + 2) == 256u);
    assert(gpb_cmd_db_find(db, sizeof(db), "cx.lvl", &ent) == 1);
    assert(gpb_cmd_db_find(db, 64u, "gfx.lvl", &ent) == -1);
    db[4] = 0;
    assert(gpb_cmd_db_find(db, sizeof(db), "gfx.lvl", &ent) == -1);
}

int main(void) {
    test_staged_modes();
    test_cmd_db();
    printf("J716fGpuPowerBoardTest: PASS\n");
    return 0;
}
