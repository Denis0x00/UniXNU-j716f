// SPDX-License-Identifier: BSD-2-Clause-Patent
// TB-J716F (SM8250-AC, Adreno 650 v2.0 chipid 0x06050002) staged GPU power, clock,
// KGSL SMMU, GMU clock, and AOP cmd-db state machine (Phase 1: zero MMIO / zero IOKit).
// Verified against TB-J716F stock DTB:
//   qcom,kgsl-3d0@3d00000 (size 0x40000, RSCC 0x3de0000/0x10000, SPI 300, UBWC 4, HBB 16)
//   qcom,gmu@3d6a000      (size 0x30000, PDC 0xb290000/0x10000, PDC seq 0xb490000/0x10000)
//   qcom,gpucc@3d90000    (size 0x9000)
//   kgsl-smmu@3da0000     (size 0x10000)
//   pil_gpu_region@8681a000 (size 0x2000), cmd-db @ 0x80860000 (size 0x20000)
// Host-tested under ASan/UBSan by package/gpu/tests/J716fGpuPowerBoardTest.c.

#ifndef J716F_GPU_POWER_BOARD_H
#define J716F_GPU_POWER_BOARD_H

#include <stddef.h>
#include <stdint.h>

#define GPB_CHIPID_A650_V2   0x06050002u
#define GPB_HIGHEST_BANK_BIT 16u
#define GPB_UBWC_MODE        4u
#define GPB_GPU_IRQ_SPI      300u
#define GPB_PIL_GPU_BASE     0x8681A000ULL
#define GPB_PIL_GPU_SIZE     0x00002000ULL

#define GPB_GCC_GPU_PAGE     0x00171000ULL // GCC + 0x71000: GPU branches
#define GPB_GCC_VOTE_PAGE    0x00152000ULL // GCC + 0x52000: GPLL0 branch votes
#define GPB_GPUCC_BASE       0x03D90000ULL
#define GPB_GPUCC_SIZE       0x00009000ULL
#define GPB_SMMU_BASE        0x03DA0000ULL
#define GPB_SMMU_GR0_SIZE    0x00001000ULL // global space 0 only in Phase 1
#define GPB_GPU_BASE         0x03D00000ULL
#define GPB_GPU_WINDOW       0x00040000ULL
#define GPB_GMU_BASE         0x03D6A000ULL
#define GPB_GMU_SIZE         0x00030000ULL
#define GPB_RSCC_BASE        0x03DE0000ULL
#define GPB_RSCC_SIZE        0x00010000ULL

// GCC GPU page offsets (relative to GPB_GCC_GPU_PAGE / GPB_GCC_VOTE_PAGE)
#define GPB_GCC_GPU_CFG_AHB       0x004u
#define GPB_GCC_GPU_MEMNOC_GFX    0x00Cu
#define GPB_GCC_GPU_SNOC_DVM_GFX  0x018u
#define GPB_GCC_GPU_MISC          0x028u
#define GPB_GCC_DDRSS_GPU_AXI     0x154u
#define GPB_GCC_PLL_VOTE          0x000u
#define GPB_GPLL0_VOTES           ((1u << 15) | (1u << 16))
#define GPB_CBCR_CLK_OFF          (1u << 31)
#define GPB_CBCR_ENABLE           (1u << 0)

// GPUCC offsets (relative to GPB_GPUCC_BASE)
#define GPB_CC_PLL1_MODE          0x0100u
#define GPB_CC_CXO_AON_CLK        0x1004u
#define GPB_CC_GX_GDSCR           0x100Cu
#define GPB_CC_CX_GDSCR           0x106Cu
#define GPB_CC_AHB_CLK            0x1078u
#define GPB_CC_CX_GMU_CLK         0x1098u
#define GPB_CC_CXO_CLK            0x109Cu
#define GPB_CC_GMU_RCGR           0x1120u
#define GPB_CC_CX_GDS_HW_CTRL     0x1540u
#define GPB_CC_SMMU_VOTE_CLK      0x5000u
#define GPB_CX_GMU_WAKE_SLEEP     (0xFFu << 4)
#define GPB_GDSC_POLL_US          2000u
#define GPB_GDSC_PWR_ON           (1u << 31)
#define GPB_GDSC_SW_COLLAPSE      (1u << 0)

// ARM SMMUv2 global space 0 (KGSL SMMU)
#define GPB_SMMU_SCR0             0x000u
#define GPB_SMMU_ID0              0x020u
#define GPB_SMMU_ID1              0x024u
#define GPB_SMMU_ID2              0x028u
#define GPB_SMMU_GFSR             0x048u
#define GPB_SMMU_SMR(n)           (0x800u + 4u * (n))
#define GPB_SMMU_S2CR(n)          (0xC00u + 4u * (n))
#define GPB_SMMU_MAX_SMR          128u

// Adreno 650 RBBM / GMU / RSCC offsets
#define GPB_RBBM_STATUS_BYTES            (0x0210u * 4u)
#define GPB_GMU_REG(dword)               ((dword) * 4u)
#define GPB_GMU_CM3_SYSRESET             GPB_GMU_REG(0x5000u)
#define GPB_GMU_CM3_FW_INIT_RESULT       GPB_GMU_REG(0x501Cu)
#define GPB_GMU_SPTPRAC_PWR_CLK_STATUS   GPB_GMU_REG(0x50D0u)
#define GPB_GMU_HFI_CTRL_STATUS          GPB_GMU_REG(0x5180u)
#define GPB_GMU_GENERAL_7                GPB_GMU_REG(0x51CCu)
#define GPB_GMU_AO_HOST_INTERRUPT_STATUS GPB_GMU_REG(0x9305u)
#define GPB_GMU_AHB_FENCE_STATUS         GPB_GMU_REG(0x9313u)
#define GPB_RSCC_SEQ_BUSY_DRV0           GPB_GMU_REG(0x0101u)
#define GPB_RSCC_RSC_STATUS0_DRV0        GPB_GMU_REG(0x0004u)

// GMU RCG (200 MHz = GPLL0 div / 1.5 -> sel=6, hid=2)
#define GPB_RCG_UPDATE     (1u << 0)
#define GPB_RCG_ROOT_EN    (1u << 1)
#define GPB_RCG_ROOT_OFF   (1u << 31)
#define GPB_RCG_CFG_CLEAR  (0x1Fu | (7u << 8) | (3u << 12) | (1u << 20))
#define GPB_GMU_SRC_SEL    6u
#define GPB_GMU_SRC_HID    2u

// AOP cmd-db
#define GPB_CMD_DB_BASE    0x80860000ULL
#define GPB_CMD_DB_BYTES   0x00020000u
#define GPB_CMD_DB_MAGIC   0x0C0330DBu
#define GPB_CMD_DB_DATA    144u

enum gpb_state {
    GPB_NOT_RUN = 0,
    GPB_DONE = 1,
    GPB_SKIPPED_PREREQ = 2,
    GPB_SKIPPED_MODE = 3,
    GPB_NOMAP = 4,
};

enum {
    GPB_WIN_GCC_GPU = 0,
    GPB_WIN_GCC_VOTE = 1,
    GPB_WIN_GPUCC = 2,
    GPB_WIN_SMMU = 3,
    GPB_WIN_GPU = 4,
    GPB_WIN_GMU = 5,
    GPB_WIN_RSCC = 6,
    GPB_WINDOWS = 7,
};

struct gpb_io {
    void *cookie;
    int (*read32)(void *cookie, unsigned window, uint32_t offset, uint32_t *out);
    int (*write32)(void *cookie, unsigned window, uint32_t offset, uint32_t value);
    void (*delay_us)(void *cookie, unsigned us);
};

struct gpb_result {
    unsigned mode;
    enum gpb_state gcc, cc, smmu, gpu;
    uint32_t cfg_ahb, cfg_ahb_after, memnoc, dvm, misc, pll_vote;
    uint32_t cx_gdscr, gx_gdscr, cc_ahb, cc_smmu, cc_gmu;
    uint32_t scr0, id0, id1, id2, gfsr;
    uint32_t smr_valid, smr_first, smr_first_val, s2cr_first_val;
    uint32_t rbbm_status;
    uint32_t cx_before, cx_hwctrl, cc_smmu_before;
    unsigned powered_cx, powered_clocks;
    unsigned enabled_ahb;
    int io_error;
};

struct gpb_gmu_clock_on {
    unsigned step;       // 1..4 in progress, 5 when all passed
    int failed;          // 0 ok, 1 timeout/refusal, 2 I/O error
    unsigned rcg_polls, branch_polls;
    uint32_t vote, gmu_cmd, gmu_cfg, cx_gmu, ahb, cxo, axi, memnoc, pll1_mode;
};

struct gpb_gmu_state {
    uint32_t sysreset, fw_init, gx_status, hfi_status, general7, ao_irq, fence, rscc_busy, rscc_status;
};

struct gpb_cmd_db_entry {
    uint32_t address, slave;
    const uint8_t *aux;
    unsigned aux_bytes;
};

static inline int gpb_clock_on(uint32_t cbcr) { return (cbcr & GPB_CBCR_CLK_OFF) == 0u; }
static inline int gpb_cfg_ahb_enabled(uint32_t cbcr) { return (cbcr & GPB_CBCR_ENABLE) != 0u; }
static inline int gpb_gdsc_on(uint32_t gdscr) { return (gdscr & GPB_GDSC_PWR_ON) != 0u; }
static inline unsigned gpb_smmu_num_smr(uint32_t id0) { return id0 & 0xFFu; }

static inline int gpb_rd(const struct gpb_io *io, struct gpb_result *r, unsigned window, uint32_t offset, uint32_t *out) {
    uint32_t v = 0;
    if (io->read32(io->cookie, window, offset, &v)) {
        r->io_error = 1;
        *out = 0;
        return -1;
    }
    *out = v;
    return 0;
}

static inline void gpb_run(const struct gpb_io *io, unsigned mode, unsigned have, struct gpb_result *r) {
    __builtin_memset(r, 0, sizeof(*r));
    r->mode = mode;
    if (!mode || !(have & (1u << GPB_WIN_GCC_GPU))) {
        r->gcc = mode ? GPB_NOMAP : GPB_SKIPPED_MODE;
        return;
    }
    if (gpb_rd(io, r, GPB_WIN_GCC_GPU, GPB_GCC_GPU_CFG_AHB, &r->cfg_ahb)) return;
    (void)gpb_rd(io, r, GPB_WIN_GCC_GPU, GPB_GCC_GPU_MEMNOC_GFX, &r->memnoc);
    (void)gpb_rd(io, r, GPB_WIN_GCC_GPU, GPB_GCC_GPU_SNOC_DVM_GFX, &r->dvm);
    (void)gpb_rd(io, r, GPB_WIN_GCC_GPU, GPB_GCC_GPU_MISC, &r->misc);
    if (have & (1u << GPB_WIN_GCC_VOTE)) {
        (void)gpb_rd(io, r, GPB_WIN_GCC_VOTE, GPB_GCC_PLL_VOTE, &r->pll_vote);
    }
    r->cfg_ahb_after = r->cfg_ahb;
    if (!gpb_cfg_ahb_enabled(r->cfg_ahb) && mode >= 2u) {
        if (!io->write32(io->cookie, GPB_WIN_GCC_GPU, GPB_GCC_GPU_CFG_AHB, r->cfg_ahb | GPB_CBCR_ENABLE)) {
            r->enabled_ahb = 1u;
            (void)gpb_rd(io, r, GPB_WIN_GCC_GPU, GPB_GCC_GPU_CFG_AHB, &r->cfg_ahb_after);
        } else {
            r->io_error = 1;
        }
    }
    r->gcc = GPB_DONE;

    if (!(have & (1u << GPB_WIN_GPUCC))) {
        r->cc = GPB_NOMAP;
    } else if (!gpb_cfg_ahb_enabled(r->cfg_ahb_after)) {
        r->cc = GPB_SKIPPED_PREREQ;
    } else {
        (void)gpb_rd(io, r, GPB_WIN_GPUCC, GPB_CC_CX_GDSCR, &r->cx_gdscr);
        (void)gpb_rd(io, r, GPB_WIN_GPUCC, GPB_CC_GX_GDSCR, &r->gx_gdscr);
        (void)gpb_rd(io, r, GPB_WIN_GPUCC, GPB_CC_AHB_CLK, &r->cc_ahb);
        (void)gpb_rd(io, r, GPB_WIN_GPUCC, GPB_CC_SMMU_VOTE_CLK, &r->cc_smmu);
        (void)gpb_rd(io, r, GPB_WIN_GPUCC, GPB_CC_CX_GMU_CLK, &r->cc_gmu);
        r->cc = GPB_DONE;
    }

    r->cx_before = r->cx_gdscr;
    r->cc_smmu_before = r->cc_smmu;
    if (mode >= 3u && r->cc == GPB_DONE) {
        int bad = 0;
        if (!gpb_gdsc_on(r->cx_gdscr)) {
            uint32_t st = 0;
            bad = io->write32(io->cookie, GPB_WIN_GPUCC, GPB_CC_CX_GDSCR, r->cx_gdscr & ~GPB_GDSC_SW_COLLAPSE);
            if (!bad) {
                unsigned waited = 0;
                if (io->delay_us) io->delay_us(io->cookie, 1u);
                for (;;) {
                    if (gpb_rd(io, r, GPB_WIN_GPUCC, GPB_CC_CX_GDS_HW_CTRL, &st)) { bad = 1; break; }
                    if (gpb_gdsc_on(st) || waited >= GPB_GDSC_POLL_US) break;
                    if (io->delay_us) io->delay_us(io->cookie, 10u);
                    waited += 10u;
                }
                r->cx_hwctrl = st;
                (void)gpb_rd(io, r, GPB_WIN_GPUCC, GPB_CC_CX_GDSCR, &r->cx_gdscr);
                r->powered_cx = (!bad && gpb_gdsc_on(st)) ? 1u : 0u;
            }
        } else {
            r->powered_cx = 2u;
        }
        if (bad) r->io_error = 1;
        if (r->powered_cx) {
            uint32_t a = 0, c = 0;
            int cbad = gpb_rd(io, r, GPB_WIN_GPUCC, GPB_CC_AHB_CLK, &a) |
                       gpb_rd(io, r, GPB_WIN_GPUCC, GPB_CC_SMMU_VOTE_CLK, &c);
            if (!cbad && !(a & GPB_CBCR_ENABLE)) {
                cbad |= io->write32(io->cookie, GPB_WIN_GPUCC, GPB_CC_AHB_CLK, a | GPB_CBCR_ENABLE);
            }
            if (!cbad && !(c & GPB_CBCR_ENABLE)) {
                cbad |= io->write32(io->cookie, GPB_WIN_GPUCC, GPB_CC_SMMU_VOTE_CLK, c | GPB_CBCR_ENABLE);
            }
            unsigned waited = 0;
            while (!cbad) {
                if (gpb_rd(io, r, GPB_WIN_GPUCC, GPB_CC_SMMU_VOTE_CLK, &r->cc_smmu)) { cbad = 1; break; }
                if (gpb_clock_on(r->cc_smmu) || waited >= 500u) break;
                if (io->delay_us) io->delay_us(io->cookie, 10u);
                waited += 10u;
            }
            (void)gpb_rd(io, r, GPB_WIN_GPUCC, GPB_CC_AHB_CLK, &r->cc_ahb);
            r->powered_clocks = (!cbad && gpb_clock_on(r->cc_smmu)) ? 1u : 0u;
            if (cbad) r->io_error = 1;
        }
    }

    if (!(have & (1u << GPB_WIN_SMMU))) {
        r->smmu = GPB_NOMAP;
    } else if (r->cc != GPB_DONE || !gpb_gdsc_on(r->cx_gdscr) || !gpb_clock_on(r->cc_smmu)) {
        r->smmu = GPB_SKIPPED_PREREQ;
    } else {
        (void)gpb_rd(io, r, GPB_WIN_SMMU, GPB_SMMU_SCR0, &r->scr0);
        (void)gpb_rd(io, r, GPB_WIN_SMMU, GPB_SMMU_ID0, &r->id0);
        (void)gpb_rd(io, r, GPB_WIN_SMMU, GPB_SMMU_ID1, &r->id1);
        (void)gpb_rd(io, r, GPB_WIN_SMMU, GPB_SMMU_ID2, &r->id2);
        (void)gpb_rd(io, r, GPB_WIN_SMMU, GPB_SMMU_GFSR, &r->gfsr);
        unsigned n = gpb_smmu_num_smr(r->id0);
        if (n > GPB_SMMU_MAX_SMR) n = GPB_SMMU_MAX_SMR;
        r->smr_first = 0xFFFFFFFFu;
        for (unsigned i = 0; i < n; ++i) {
            uint32_t smr = 0;
            if (gpb_rd(io, r, GPB_WIN_SMMU, GPB_SMMU_SMR(i), &smr)) break;
            if (smr & (1u << 31)) {
                ++r->smr_valid;
                if (r->smr_first == 0xFFFFFFFFu) {
                    r->smr_first = i;
                    r->smr_first_val = smr;
                    (void)gpb_rd(io, r, GPB_WIN_SMMU, GPB_SMMU_S2CR(i), &r->s2cr_first_val);
                }
            }
        }
        r->smmu = GPB_DONE;
    }

    if (!(have & (1u << GPB_WIN_GPU))) {
        r->gpu = GPB_NOMAP;
    } else if (r->cc != GPB_DONE || !gpb_gdsc_on(r->cx_gdscr) || !gpb_gdsc_on(r->gx_gdscr)) {
        r->gpu = GPB_SKIPPED_PREREQ;
    } else {
        (void)gpb_rd(io, r, GPB_WIN_GPU, GPB_RBBM_STATUS_BYTES, &r->rbbm_status);
        r->gpu = GPB_DONE;
    }
}

static inline int gpb_rcg_set(const struct gpb_io *io, struct gpb_result *r, uint32_t rcgr, uint32_t sel, uint32_t hid,
                              uint32_t *cfg_after, unsigned *polls) {
    uint32_t cmd = 0, cfg = 0;
    int timeout = 1;
    if (gpb_rd(io, r, GPB_WIN_GPUCC, rcgr, &cmd) ||
        io->write32(io->cookie, GPB_WIN_GPUCC, rcgr, cmd | GPB_RCG_ROOT_EN)) {
        return -1;
    }
    for (unsigned i = 0; i < 500u; ++i) {
        if (gpb_rd(io, r, GPB_WIN_GPUCC, rcgr, &cmd)) return -1;
        if (!(cmd & GPB_RCG_ROOT_OFF)) { timeout = 0; break; }
        if (io->delay_us) io->delay_us(io->cookie, 1u);
    }
    if (timeout) return 1;
    if (gpb_rd(io, r, GPB_WIN_GPUCC, rcgr + 4u, &cfg) ||
        io->write32(io->cookie, GPB_WIN_GPUCC, rcgr + 4u, (cfg & ~GPB_RCG_CFG_CLEAR) | (sel << 8) | hid)) {
        return -1;
    }
    if (gpb_rd(io, r, GPB_WIN_GPUCC, rcgr, &cmd) ||
        io->write32(io->cookie, GPB_WIN_GPUCC, rcgr, cmd | GPB_RCG_UPDATE)) {
        return -1;
    }
    timeout = 1;
    for (unsigned i = 0; i < 500u; ++i) {
        ++(*polls);
        if (gpb_rd(io, r, GPB_WIN_GPUCC, rcgr, &cmd)) return -1;
        if (!(cmd & GPB_RCG_UPDATE)) { timeout = 0; break; }
        if (io->delay_us) io->delay_us(io->cookie, 1u);
    }
    if (timeout) return 1;
    if (io->write32(io->cookie, GPB_WIN_GPUCC, rcgr, cmd & ~GPB_RCG_ROOT_EN)) return -1;
    return gpb_rd(io, r, GPB_WIN_GPUCC, rcgr + 4u, cfg_after) ? -1 : 0;
}

static inline int gpb_set_bits(const struct gpb_io *io, struct gpb_result *r, unsigned w, uint32_t off, uint32_t bits) {
    uint32_t v = 0;
    if (gpb_rd(io, r, w, off, &v)) return -1;
    return (v & bits) == bits ? 0 : io->write32(io->cookie, w, off, v | bits);
}

static inline void gpb_gmu_clocks_on(const struct gpb_io *io, struct gpb_result *r, struct gpb_gmu_clock_on *o) {
    __builtin_memset(o, 0, sizeof(*o));
    int e = 0;
    o->step = 1u;
    if (r->cc != GPB_DONE || !gpb_gdsc_on(r->cx_gdscr)) { o->failed = 1; return; }
    if (gpb_set_bits(io, r, GPB_WIN_GCC_VOTE, GPB_GCC_PLL_VOTE, GPB_GPLL0_VOTES)) goto io_err;
    if (io->delay_us) io->delay_us(io->cookie, 10u);
    o->step = 2u;
    e = gpb_rcg_set(io, r, GPB_CC_GMU_RCGR, GPB_GMU_SRC_SEL, GPB_GMU_SRC_HID, &o->gmu_cfg, &o->rcg_polls);
    if (e) { o->failed = e < 0 ? 2 : 1; goto out; }
    o->step = 3u;
    if (gpb_set_bits(io, r, GPB_WIN_GPUCC, GPB_CC_CX_GMU_CLK, GPB_CX_GMU_WAKE_SLEEP)) goto io_err;
    o->step = 4u;
    if (gpb_set_bits(io, r, GPB_WIN_GPUCC, GPB_CC_AHB_CLK, GPB_CBCR_ENABLE) ||
        gpb_set_bits(io, r, GPB_WIN_GPUCC, GPB_CC_CX_GMU_CLK, GPB_CBCR_ENABLE) ||
        gpb_set_bits(io, r, GPB_WIN_GPUCC, GPB_CC_CXO_CLK, GPB_CBCR_ENABLE) ||
        gpb_set_bits(io, r, GPB_WIN_GCC_GPU, GPB_GCC_DDRSS_GPU_AXI, GPB_CBCR_ENABLE) ||
        gpb_set_bits(io, r, GPB_WIN_GCC_GPU, GPB_GCC_GPU_MEMNOC_GFX, GPB_CBCR_ENABLE)) {
        goto io_err;
    }
    e = 1;
    for (unsigned i = 0; i < 500u; ++i) {
        ++o->branch_polls;
        if (gpb_rd(io, r, GPB_WIN_GPUCC, GPB_CC_CX_GMU_CLK, &o->cx_gmu)) goto io_err;
        if (gpb_clock_on(o->cx_gmu)) { e = 0; break; }
        if (io->delay_us) io->delay_us(io->cookie, 1u);
    }
    if (e) { o->failed = 1; goto out; }
    o->step = 5u;
    goto out;
io_err:
    o->failed = 2;
out:
    (void)gpb_rd(io, r, GPB_WIN_GCC_VOTE, GPB_GCC_PLL_VOTE, &o->vote);
    (void)gpb_rd(io, r, GPB_WIN_GPUCC, GPB_CC_GMU_RCGR, &o->gmu_cmd);
    (void)gpb_rd(io, r, GPB_WIN_GPUCC, GPB_CC_GMU_RCGR + 4u, &o->gmu_cfg);
    (void)gpb_rd(io, r, GPB_WIN_GPUCC, GPB_CC_CX_GMU_CLK, &o->cx_gmu);
    (void)gpb_rd(io, r, GPB_WIN_GPUCC, GPB_CC_AHB_CLK, &o->ahb);
    (void)gpb_rd(io, r, GPB_WIN_GPUCC, GPB_CC_CXO_CLK, &o->cxo);
    (void)gpb_rd(io, r, GPB_WIN_GCC_GPU, GPB_GCC_DDRSS_GPU_AXI, &o->axi);
    (void)gpb_rd(io, r, GPB_WIN_GCC_GPU, GPB_GCC_GPU_MEMNOC_GFX, &o->memnoc);
    (void)gpb_rd(io, r, GPB_WIN_GPUCC, GPB_CC_PLL1_MODE, &o->pll1_mode);
}

static inline int gpb_gmu_regs_readable(const struct gpb_result *r, uint32_t cx_gmu, uint32_t cc_ahb) {
    return r->cc == GPB_DONE && gpb_gdsc_on(r->cx_gdscr) && gpb_clock_on(cc_ahb) && gpb_clock_on(cx_gmu);
}

static inline void gpb_read_gmu_state(const struct gpb_io *io, struct gpb_result *r, struct gpb_gmu_state *s) {
    (void)gpb_rd(io, r, GPB_WIN_GMU, GPB_GMU_CM3_SYSRESET, &s->sysreset);
    (void)gpb_rd(io, r, GPB_WIN_GMU, GPB_GMU_CM3_FW_INIT_RESULT, &s->fw_init);
    (void)gpb_rd(io, r, GPB_WIN_GMU, GPB_GMU_SPTPRAC_PWR_CLK_STATUS, &s->gx_status);
    (void)gpb_rd(io, r, GPB_WIN_GMU, GPB_GMU_HFI_CTRL_STATUS, &s->hfi_status);
    (void)gpb_rd(io, r, GPB_WIN_GMU, GPB_GMU_GENERAL_7, &s->general7);
    (void)gpb_rd(io, r, GPB_WIN_GMU, GPB_GMU_AO_HOST_INTERRUPT_STATUS, &s->ao_irq);
    (void)gpb_rd(io, r, GPB_WIN_GMU, GPB_GMU_AHB_FENCE_STATUS, &s->fence);
    (void)gpb_rd(io, r, GPB_WIN_RSCC, GPB_RSCC_SEQ_BUSY_DRV0, &s->rscc_busy);
    (void)gpb_rd(io, r, GPB_WIN_RSCC, GPB_RSCC_RSC_STATUS0_DRV0, &s->rscc_status);
}

static inline uint32_t gpb_le16(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}

static inline uint32_t gpb_le32(const uint8_t *p) {
    return gpb_le16(p) | (gpb_le16(p + 2) << 16);
}

static inline int gpb_cmd_db_id_is(const uint8_t *id, const char *name) {
    for (unsigned k = 0; k < 8u; ++k) {
        if (id[k] != (uint8_t)name[k]) return 0;
        if (!name[k]) return 1;
    }
    return 1;
}

static inline int gpb_cmd_db_find(const uint8_t *db, size_t bytes, const char *name, struct gpb_cmd_db_entry *out) {
    if (!db || !name || !out || bytes < GPB_CMD_DB_DATA || gpb_le32(db + 4) != GPB_CMD_DB_MAGIC) {
        return -1;
    }
    for (unsigned s = 0; s < 8u; ++s) {
        const uint8_t *h = db + 8u + 16u * s;
        const uint32_t slave = gpb_le16(h);
        const uint32_t heads = gpb_le16(h + 2);
        const uint32_t data = gpb_le16(h + 4);
        const uint32_t count = gpb_le16(h + 6);
        if (!slave) break;
        const size_t at = (size_t)GPB_CMD_DB_DATA + heads;
        if (at + (size_t)count * 24u > bytes) return -1;
        for (uint32_t i = 0; i < count; ++i) {
            const uint8_t *e = db + at + (size_t)i * 24u;
            if (!gpb_cmd_db_id_is(e, name)) continue;
            const size_t aux = (size_t)GPB_CMD_DB_DATA + data + gpb_le16(e + 22);
            const unsigned len = (unsigned)gpb_le16(e + 20);
            if (aux + len > bytes) return -1;
            out->address = gpb_le32(e + 16);
            out->slave = slave;
            out->aux = db + aux;
            out->aux_bytes = len;
            return 0;
        }
    }
    return 1;
}

static inline const char *gpb_state_text(enum gpb_state s) {
    return s == GPB_DONE ? "ok" :
           s == GPB_SKIPPED_PREREQ ? "skip" :
           s == GPB_SKIPPED_MODE ? "off" :
           s == GPB_NOMAP ? "nomap" : "-";
}

#endif // J716F_GPU_POWER_BOARD_H
