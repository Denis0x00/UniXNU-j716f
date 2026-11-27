// SPDX-License-Identifier: BSD-2-Clause-Patent
// Pure C11 register definitions, read-only observation helpers, and ARMv8 Stage-1
// page-table builders for SM8250 DPU 6.0 (MDSS 0x0AE00000 / MDP 0x0AE01000) and
// Display APPS SMMU (0x15000000, qcom,smmu_sde_unsec_cb SID 0x820 mask 0x402)
// on Lenovo TB-J716F.
#ifndef J716F_DPU_SMMU_H
#define J716F_DPU_SMMU_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Physical MMIO windows (verified against stock boot-base-00.dtb.json). */
#define J716F_MDSS_PHYS_BASE            0x0AE00000ULL
#define J716F_MDSS_PHYS_SIZE            0x00085000u
#define J716F_MDP_SUB_OFF               0x00001000u

#define J716F_APPS_SMMU_PHYS_BASE       0x15000000ULL
#define J716F_APPS_SMMU_PHYS_SIZE       0x000D0000u

/* Stock TB-J716F boot-base-00.dtb.json: qcom,smmu_sde_unsec_cb iommus = <apps_smmu 0x820 0x402>. */
#define J716F_DPU_SMMU_SID_BASE         0x0820u
#define J716F_DPU_SMMU_SID_MASK         0x0402u

/* SM8250 DPU 6.0 MDP sub-block offsets (relative to MDP base 0x0AE01000). */
#define J716F_MDP_HW_VERSION            0x0000u
#define J716F_MDP_CTL_COUNT             6u
#define J716F_MDP_CTL_BASE              0x1000u
#define J716F_MDP_CTL_STRIDE            0x0200u

#define J716F_MDP_CTL_LAYER0            0x0000u
#define J716F_MDP_CTL_LAYER1            0x0004u
#define J716F_MDP_CTL_TOP               0x0014u
#define J716F_MDP_CTL_FLUSH             0x0018u
#define J716F_MDP_CTL_START             0x001Cu
#define J716F_MDP_CTL_INTF_ACTIVE       0x00F4u
#define J716F_MDP_CTL_FETCH_PIPE_ACTIVE 0x00FCu
#define J716F_MDP_CTL_FLUSH_ACTIVE      0x0110u

#define J716F_MDP_SSPP_COUNT            8u
#define J716F_SSPP_SRC_SIZE             0x0000u
#define J716F_SSPP_SRC_IMG_SIZE         0x0004u
#define J716F_SSPP_SRC_XY               0x0008u
#define J716F_SSPP_OUT_SIZE             0x000Cu
#define J716F_SSPP_OUT_XY               0x0010u
#define J716F_SSPP_SRC0_ADDR            0x0014u
#define J716F_SSPP_SRC_YSTRIDE0         0x0024u
#define J716F_SSPP_SRC_FORMAT           0x0030u
#define J716F_SSPP_SRC_OP_MODE          0x0038u

#define J716F_MDP_INTF_DSI0             0x6A800u
#define J716F_MDP_INTF_DSI1             0x6B000u
#define J716F_INTF_TIMING_ENGINE_EN     0x0000u
#define J716F_INTF_PANEL_FORMAT         0x0030u

/* Boot GOP framebuffer defaults on TB-J716F (2560x1600 BGRX32). */
#define J716F_DPU_BOOT_IOVA             0x9C000000u
#define J716F_DPU_BOOT_STRIDE           0x2800u

/* Display SMMU Stage-1 4 KiB window layout (matches NativeLegacyDisplay direct scanout). */
#define J716F_DPU_WINDOW                0x10000000u
#define J716F_DPU_SLOTS                 4u
#define J716F_DPU_SLOT_BYTES            (32u << 20)
#define J716F_DPU_SLOT_TABLES           (J716F_DPU_SLOT_BYTES >> 21) /* 16 L3 pages per 32 MiB slot */
#define J716F_DPU_SLOT_MAX_PAGES        (J716F_DPU_SLOT_BYTES >> 12) /* 8192 pages per slot */
#define J716F_DPU_TOTAL_TABLE_PAGES     (2u + J716F_DPU_SLOTS * J716F_DPU_SLOT_TABLES) /* 66 pages = 264 KiB */
#define J716F_DPU_CB_FAULTS             0xC00001FEu

/* Normal non-cacheable (MAIR attr0=0x44), AF=1, AP[1]=1 (EL0/EL1 RW). */
#define J716F_DPU_PTE_ATTRS             ((1ULL << 10) | (1ULL << 6))

/* APPS SMMU register offsets. */
#define J716F_SMMU_SCR0                 0x0000u
#define J716F_SMMU_IDR0                 0x0020u
#define J716F_SMMU_IDR1                 0x0024u
#define J716F_SMMU_IDR2                 0x0028u
#define J716F_SMMU_SGFSR                0x0048u
#define J716F_SMMU_SGFSYNR0             0x0050u
#define J716F_SMMU_SGFSYNR1             0x0054u
#define J716F_SMMU_SMR(i)               (0x0800u + 4u * (i))
#define J716F_SMMU_S2CR(i)              (0x0C00u + 4u * (i))
#define J716F_SMMU_CBAR(b)              (0x1000u + 4u * (b))
#define J716F_SMMU_CBA2R(b)             (0x1800u + 4u * (b))

/* Context bank register offsets. */
#define J716F_CB_SCTLR                  0x0000u
#define J716F_CB_RESUME                 0x0008u
#define J716F_CB_TCR2                   0x0010u
#define J716F_CB_TTBR0_LO               0x0020u
#define J716F_CB_TTBR0_HI               0x0024u
#define J716F_CB_TCR                    0x0030u
#define J716F_CB_MAIR0                  0x0038u
#define J716F_CB_FSR                    0x0058u
#define J716F_CB_FAR_LO                 0x0060u
#define J716F_CB_FAR_HI                 0x0064u
#define J716F_CB_FSYNR0                 0x0068u
#define J716F_CB_TLBIALL                0x0618u
#define J716F_CB_TLBSYNC                0x07F0u
#define J716F_CB_TLBSTATUS              0x07F4u

typedef struct J716fDpuRegIo {
    uint32_t (*rd)(void *ctx, uint32_t off);
    void (*wr)(void *ctx, uint32_t off, uint32_t val);
    void *ctx;
} J716fDpuRegIo;

typedef struct J716fDpuPipeInfo {
    const char *name;
    uint32_t off;
    uint32_t src_size;
    uint32_t src_xy;
    uint32_t out_size;
    uint32_t out_xy;
    uint32_t src0_addr;
    uint32_t stride0;
    uint32_t format;
    uint32_t op_mode;
} J716fDpuPipeInfo;

typedef struct J716fDpuCtlInfo {
    uint32_t index;
    uint32_t off;
    uint32_t layer0;
    uint32_t layer1;
    uint32_t top;
    uint32_t flush;
    uint32_t intf_active;
    uint32_t fetch_pipe_active;
    uint32_t flush_active;
} J716fDpuCtlInfo;

typedef struct J716fDpuMdpReport {
    uint32_t mdss_hw_ver;
    uint32_t mdp_hw_ver;
    J716fDpuPipeInfo pipes[J716F_MDP_SSPP_COUNT];
    J716fDpuCtlInfo ctls[J716F_MDP_CTL_COUNT];
    uint32_t intf_dsi0_en;
    uint32_t intf_dsi0_fmt;
    uint32_t intf_dsi1_en;
    uint32_t intf_dsi1_fmt;
} J716fDpuMdpReport;

typedef struct J716fDpuSmmuMatch {
    uint16_t sid;
    uint16_t slot;
    uint32_t smr;
    uint32_t s2cr;
    uint32_t bank;
    uint32_t cbar;
    uint32_t cba2r;
    uint32_t sctlr;
    uint32_t tcr2;
    uint32_t ttbr0_lo;
    uint32_t ttbr0_hi;
    uint32_t tcr;
    uint32_t mair0;
    uint32_t fsr;
    uint32_t far_lo;
    uint32_t far_hi;
    uint32_t fsynr0;
    bool valid_cb;
} J716fDpuSmmuMatch;

typedef struct J716fDpuSmrSummary {
    uint16_t slot;
    uint32_t smr;
    uint32_t s2cr;
} J716fDpuSmrSummary;

typedef struct J716fDpuSmmuReport {
    uint32_t scr0;
    uint32_t idr0;
    uint32_t idr1;
    uint32_t idr2;
    uint32_t sgfsr;
    uint32_t sgfsynr0;
    uint32_t sgfsynr1;
    uint32_t groups;
    uint32_t banks;
    uint32_t pages;
    uint32_t valid_smrs;
    uint32_t match_count;
    J716fDpuSmmuMatch matches[4];
    uint32_t summary_count;
    J716fDpuSmrSummary summaries[20];
} J716fDpuSmmuReport;

static inline bool j716f_dpu_smr_matches_sid(uint32_t smr, uint16_t sid) {
    if ((smr & 0x80000000u) == 0u) return false;
    const uint32_t mask = (smr >> 16) & 0x7FFFu;
    return (((smr ^ (uint32_t)sid) & 0x7FFFu) & ~mask) == 0u;
}

static inline void j716f_dpu_observe_mdp(const J716fDpuRegIo *mdss_io, J716fDpuMdpReport *out) {
    static const char *const kPipeNames[J716F_MDP_SSPP_COUNT] = {
        "VIG0", "VIG1", "VIG2", "VIG3", "DMA0", "DMA1", "DMA2", "DMA3",
    };
    static const uint32_t kPipeOffs[J716F_MDP_SSPP_COUNT] = {
        0x4000u, 0x6000u, 0x8000u, 0xA000u, 0x24000u, 0x26000u, 0x28000u, 0x2A000u,
    };

    memset(out, 0, sizeof(*out));
    if (!mdss_io || !mdss_io->rd) return;

    out->mdss_hw_ver = mdss_io->rd(mdss_io->ctx, 0x0000u);
    out->mdp_hw_ver  = mdss_io->rd(mdss_io->ctx, J716F_MDP_SUB_OFF + J716F_MDP_HW_VERSION);

    for (uint32_t i = 0; i < J716F_MDP_SSPP_COUNT; ++i) {
        const uint32_t base = J716F_MDP_SUB_OFF + kPipeOffs[i];
        J716fDpuPipeInfo *p = &out->pipes[i];
        p->name      = kPipeNames[i];
        p->off       = kPipeOffs[i];
        p->src_size  = mdss_io->rd(mdss_io->ctx, base + J716F_SSPP_SRC_SIZE);
        p->src_xy    = mdss_io->rd(mdss_io->ctx, base + J716F_SSPP_SRC_XY);
        p->out_size  = mdss_io->rd(mdss_io->ctx, base + J716F_SSPP_OUT_SIZE);
        p->out_xy    = mdss_io->rd(mdss_io->ctx, base + J716F_SSPP_OUT_XY);
        p->src0_addr = mdss_io->rd(mdss_io->ctx, base + J716F_SSPP_SRC0_ADDR);
        p->stride0   = mdss_io->rd(mdss_io->ctx, base + J716F_SSPP_SRC_YSTRIDE0);
        p->format    = mdss_io->rd(mdss_io->ctx, base + J716F_SSPP_SRC_FORMAT);
        p->op_mode   = mdss_io->rd(mdss_io->ctx, base + J716F_SSPP_SRC_OP_MODE);
    }

    for (uint32_t c = 0; c < J716F_MDP_CTL_COUNT; ++c) {
        const uint32_t rel  = J716F_MDP_CTL_BASE + c * J716F_MDP_CTL_STRIDE;
        const uint32_t base = J716F_MDP_SUB_OFF + rel;
        J716fDpuCtlInfo *ci = &out->ctls[c];
        ci->index             = c;
        ci->off               = rel;
        ci->layer0            = mdss_io->rd(mdss_io->ctx, base + J716F_MDP_CTL_LAYER0);
        ci->layer1            = mdss_io->rd(mdss_io->ctx, base + J716F_MDP_CTL_LAYER1);
        ci->top               = mdss_io->rd(mdss_io->ctx, base + J716F_MDP_CTL_TOP);
        ci->flush             = mdss_io->rd(mdss_io->ctx, base + J716F_MDP_CTL_FLUSH);
        ci->intf_active       = mdss_io->rd(mdss_io->ctx, base + J716F_MDP_CTL_INTF_ACTIVE);
        ci->fetch_pipe_active = mdss_io->rd(mdss_io->ctx, base + J716F_MDP_CTL_FETCH_PIPE_ACTIVE);
        ci->flush_active      = mdss_io->rd(mdss_io->ctx, base + J716F_MDP_CTL_FLUSH_ACTIVE);
    }

    out->intf_dsi0_en  = mdss_io->rd(mdss_io->ctx, J716F_MDP_SUB_OFF + J716F_MDP_INTF_DSI0 + J716F_INTF_TIMING_ENGINE_EN);
    out->intf_dsi0_fmt = mdss_io->rd(mdss_io->ctx, J716F_MDP_SUB_OFF + J716F_MDP_INTF_DSI0 + J716F_INTF_PANEL_FORMAT);
    out->intf_dsi1_en  = mdss_io->rd(mdss_io->ctx, J716F_MDP_SUB_OFF + J716F_MDP_INTF_DSI1 + J716F_INTF_TIMING_ENGINE_EN);
    out->intf_dsi1_fmt = mdss_io->rd(mdss_io->ctx, J716F_MDP_SUB_OFF + J716F_MDP_INTF_DSI1 + J716F_INTF_PANEL_FORMAT);
}

static inline void j716f_dpu_smmu_inspect(const J716fDpuRegIo *smmu_io, J716fDpuSmmuReport *out) {
    static const uint16_t kDisplaySids[4] = {0x0820u, 0x0822u, 0x0C20u, 0x0C22u};

    memset(out, 0, sizeof(*out));
    if (!smmu_io || !smmu_io->rd) return;

    out->scr0     = smmu_io->rd(smmu_io->ctx, J716F_SMMU_SCR0);
    out->idr0     = smmu_io->rd(smmu_io->ctx, J716F_SMMU_IDR0);
    out->idr1     = smmu_io->rd(smmu_io->ctx, J716F_SMMU_IDR1);
    out->idr2     = smmu_io->rd(smmu_io->ctx, J716F_SMMU_IDR2);
    out->sgfsr    = smmu_io->rd(smmu_io->ctx, J716F_SMMU_SGFSR);
    out->sgfsynr0 = smmu_io->rd(smmu_io->ctx, J716F_SMMU_SGFSYNR0);
    out->sgfsynr1 = smmu_io->rd(smmu_io->ctx, J716F_SMMU_SGFSYNR1);

    uint32_t groups = out->idr0 & 0xFFu;
    if (groups > 128u) groups = 128u;
    const uint32_t banks = out->idr1 & 0xFFu;
    const uint32_t pages = 1u << (((out->idr1 >> 28) & 7u) + 1u);
    out->groups = groups;
    out->banks  = banks;
    out->pages  = pages;

    bool sid_found[4] = {false, false, false, false};

    for (uint32_t i = 0; i < groups; ++i) {
        const uint32_t smr = smmu_io->rd(smmu_io->ctx, J716F_SMMU_SMR(i));
        if ((smr & 0x80000000u) == 0u) continue;
        ++out->valid_smrs;
        const uint32_t s2cr = smmu_io->rd(smmu_io->ctx, J716F_SMMU_S2CR(i));
        if (out->summary_count < 20u) {
            out->summaries[out->summary_count].slot = (uint16_t)i;
            out->summaries[out->summary_count].smr  = smr;
            out->summaries[out->summary_count].s2cr = s2cr;
            ++out->summary_count;
        }
        for (uint32_t k = 0; k < 4u; ++k) {
            if (sid_found[k] || !j716f_dpu_smr_matches_sid(smr, kDisplaySids[k])) continue;
            sid_found[k] = true;
            if (out->match_count >= 4u) continue;
            J716fDpuSmmuMatch *m = &out->matches[out->match_count++];
            m->sid  = kDisplaySids[k];
            m->slot = (uint16_t)i;
            m->smr  = smr;
            m->s2cr = s2cr;
            m->bank = s2cr & 0xFFu;
            const uint32_t s2_type = (s2cr >> 16) & 3u;
            if (s2_type == 0u && m->bank < banks && (pages + banks) * 4096u <= J716F_APPS_SMMU_PHYS_SIZE) {
                const uint32_t cb = (pages + m->bank) * 4096u;
                m->valid_cb = true;
                m->cbar     = smmu_io->rd(smmu_io->ctx, J716F_SMMU_CBAR(m->bank));
                m->cba2r    = smmu_io->rd(smmu_io->ctx, J716F_SMMU_CBA2R(m->bank));
                m->sctlr    = smmu_io->rd(smmu_io->ctx, cb + J716F_CB_SCTLR);
                m->tcr2     = smmu_io->rd(smmu_io->ctx, cb + J716F_CB_TCR2);
                m->ttbr0_lo = smmu_io->rd(smmu_io->ctx, cb + J716F_CB_TTBR0_LO);
                m->ttbr0_hi = smmu_io->rd(smmu_io->ctx, cb + J716F_CB_TTBR0_HI);
                m->tcr      = smmu_io->rd(smmu_io->ctx, cb + J716F_CB_TCR);
                m->mair0    = smmu_io->rd(smmu_io->ctx, cb + J716F_CB_MAIR0);
                m->fsr      = smmu_io->rd(smmu_io->ctx, cb + J716F_CB_FSR);
                m->far_lo   = smmu_io->rd(smmu_io->ctx, cb + J716F_CB_FAR_LO);
                m->far_hi   = smmu_io->rd(smmu_io->ctx, cb + J716F_CB_FAR_HI);
                m->fsynr0   = smmu_io->rd(smmu_io->ctx, cb + J716F_CB_FSYNR0);
            }
        }
    }
}

/* Build the 66-page (264 KiB) Stage-1 table for Display SMMU:
 *   - Self-linked L0[0]/L1[0] so both 3-level (L1->L2->L3) and 4-level (L0->L1->L2->L3) walks work.
 *   - 1 GiB identity blocks at [2] (0x80000000..0xBFFFFFFF, covering boot GOP 0x9C000000)
 *     and [3] (0xC0000000..0xFFFFFFFF).
 *   - 64 L3 tables (4 slots * 16 * 2 MiB = 128 MiB) at [128..191] covering 0x10000000..0x17FFFFFF. */
static inline bool j716f_dpu_build_stage1_tables(uint64_t *tables, uint64_t tables_pa) {
    if (!tables || !tables_pa || (tables_pa & 0xFFFULL) != 0ULL) return false;
    memset(tables, 0, J716F_DPU_TOTAL_TABLE_PAGES * 4096u);
    tables[0] = tables_pa | 3ULL;
    tables[2] = 0x80000000ULL | J716F_DPU_PTE_ATTRS | 1ULL;
    tables[3] = 0xC0000000ULL | J716F_DPU_PTE_ATTRS | 1ULL;
    const uint32_t l2_base_idx = J716F_DPU_WINDOW >> 21; /* 128 */
    for (uint32_t t = 0; t < J716F_DPU_SLOTS * J716F_DPU_SLOT_TABLES; ++t) {
        tables[l2_base_idx + t] = (tables_pa + (uint64_t)(2u + t) * 4096ULL) | 3ULL;
    }
    return true;
}

static inline uint32_t j716f_dpu_fill_slot_l3(uint64_t *tables, unsigned slot,
                                              const uint64_t *pages, unsigned count) {
    if (!tables || !pages || slot >= J716F_DPU_SLOTS ||
        count == 0u || count > J716F_DPU_SLOT_MAX_PAGES) {
        return 0u;
    }
    for (unsigned i = 0; i < count; ++i) {
        if (!pages[i] || (pages[i] & 0xFFFULL) != 0ULL) return 0u;
    }
    uint64_t *l3 = tables + (2u + slot * J716F_DPU_SLOT_TABLES) * 512u;
    for (unsigned i = 0; i < count; ++i) {
        l3[i] = pages[i] | J716F_DPU_PTE_ATTRS | 3ULL;
    }
    for (unsigned i = count; i < J716F_DPU_SLOT_MAX_PAGES; ++i) {
        l3[i] = 0ULL;
    }
    return J716F_DPU_WINDOW + slot * J716F_DPU_SLOT_BYTES;
}

#ifdef __cplusplus
}
#endif

#endif /* J716F_DPU_SMMU_H */
