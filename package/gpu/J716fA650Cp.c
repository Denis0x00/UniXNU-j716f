/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* TB-J716F (SM8250-AC, Adreno 650 v2.0) Phase 3: KGSL SMMU Bank 0 GPU context +
 * A650 hw_init + SQE/CP Ring 0 + CP_ME_INIT + CP_MEM_WRITE verification engine. */
#include "J716fA650Cp.h"

#include <string.h>

/* CP_PROTECT register range encoders (Linux v6.6 a650_protect) */
#define A650_PROTECT_RDONLY(reg, len) ((((uint32_t)(len) & 0x3FFFu) << 18) | ((uint32_t)(reg) & 0x3FFFFu))
#define A650_PROTECT_NORDWR(reg, len) ((UINT32_C(1) << 31) | A650_PROTECT_RDONLY((reg), (len)))

/* A6XX / A650 PM4 opcodes and register dword indices (a6xx.xml / adreno_pm4.xml) */
#define A650_PM4_MEM_WRITE          0x3Du
#define A650_PM4_EVENT_WRITE        0x46u
#define A650_PM4_ME_INIT            0x48u
#define A650_EVENT_CACHE_FLUSH_TS   0x04u
#define A650_EVENT_IRQ              (UINT32_C(1) << 31)

#define A650_REG_RBBM_GBIF_QOS      0x0011u
#define A650_REG_RBBM_GBIF_HALT     0x0016u
#define A650_REG_RBBM_HANG_INT_CNTL 0x001Fu
#define A650_REG_RBBM_INT_0_MASK    0x0038u
#define A650_REG_RBBM_CLOCK_CNTL    0x00AEu
#define A650_REG_RBBM_INT_0_STATUS  0x0201u
#define A650_REG_RBBM_STATUS        0x0210u
#define A650_REG_RBBM_PERFCTR_CNTL  0x0500u
#define A650_REG_RBBM_BUSY_MASKED   0x050Bu

#define A650_REG_CP_RB_BASE         0x0800u
#define A650_REG_CP_RB_CNTL         0x0802u
#define A650_REG_CP_RB_RPTR_ADDR    0x0804u
#define A650_REG_CP_RB_RPTR         0x0806u
#define A650_REG_CP_RB_WPTR         0x0807u
#define A650_REG_CP_SQE_CNTL        0x0808u
#define A650_REG_CP_HW_FAULT        0x0821u
#define A650_REG_CP_INT_STATUS      0x0823u
#define A650_REG_CP_SQE_INSTR_BASE  0x0830u
#define A650_REG_CP_APRIV_CNTL      0x0844u
#define A650_REG_CP_PROTECT_CNTL    0x084Fu
#define A650_REG_CP_PROTECT_BASE    0x0850u
#define A650_REG_CP_SCRATCH_2       0x0885u
#define A650_REG_CP_ROQ_THRESH_1    0x08C1u
#define A650_REG_CP_ROQ_THRESH_2    0x08C2u
#define A650_REG_CP_MEM_POOL_SIZE   0x08C3u
#define A650_REG_CP_PERFCTR_SEL_0   0x08D0u
#define A650_REG_CP_AHB_CNTL        0x098Du
#define A650_REG_GBIF_HALT          0x3C45u

/* A6XX interrupt mask (exact Linux A6XX_INT_MASK from a6xx_gpu.c, including bit 20 CP_CACHE_FLUSH_TS) */
#define A650_RBBM_INT_MASK          0x03D0E242u

/* GMU core byte offsets used during hw_init */
#define A650_GMU_SPTPRAC_CLK_CTRL   (0x0080u * 4u)
#define A650_GMU_PWR_CNT_ENABLE     (0x5040u * 4u)
#define A650_GMU_PWR_CNT_SEL_0      (0x5041u * 4u)
#define A650_GMU_AO_CX_BUSY_MASK    (0x930Eu * 4u)

static const uint32_t kA650Protect[39] = {
    A650_PROTECT_RDONLY(0x00000, 0x04FF), A650_PROTECT_RDONLY(0x00501, 0x0005), A650_PROTECT_RDONLY(0x0050B, 0x02F4),
    A650_PROTECT_NORDWR(0x0050E, 0x0000), A650_PROTECT_NORDWR(0x00510, 0x0000), A650_PROTECT_NORDWR(0x00534, 0x0000),
    A650_PROTECT_NORDWR(0x00800, 0x0082), A650_PROTECT_NORDWR(0x008A0, 0x0008), A650_PROTECT_NORDWR(0x008AB, 0x0024),
    A650_PROTECT_RDONLY(0x008DE, 0x00AE), A650_PROTECT_NORDWR(0x00900, 0x004D), A650_PROTECT_NORDWR(0x0098D, 0x0272),
    A650_PROTECT_NORDWR(0x00E00, 0x0001), A650_PROTECT_NORDWR(0x00E03, 0x000C), A650_PROTECT_NORDWR(0x03C00, 0x00C3),
    A650_PROTECT_RDONLY(0x03CC4, 0x1FFF), A650_PROTECT_NORDWR(0x08630, 0x01CF), A650_PROTECT_NORDWR(0x08E00, 0x0000),
    A650_PROTECT_NORDWR(0x08E08, 0x0000), A650_PROTECT_NORDWR(0x08E50, 0x001F), A650_PROTECT_NORDWR(0x08E80, 0x027F),
    A650_PROTECT_NORDWR(0x09624, 0x01DB), A650_PROTECT_NORDWR(0x09E60, 0x0011), A650_PROTECT_NORDWR(0x09E78, 0x0187),
    A650_PROTECT_NORDWR(0x0A630, 0x01CF), A650_PROTECT_NORDWR(0x0AE02, 0x0000), A650_PROTECT_NORDWR(0x0AE50, 0x032F),
    A650_PROTECT_NORDWR(0x0B604, 0x0000), A650_PROTECT_NORDWR(0x0B608, 0x0007), A650_PROTECT_NORDWR(0x0BE02, 0x0001),
    A650_PROTECT_NORDWR(0x0BE20, 0x17DF), A650_PROTECT_NORDWR(0x0F000, 0x0BFF), A650_PROTECT_RDONLY(0x0FC00, 0x1FFF),
    A650_PROTECT_NORDWR(0x18400, 0x1FFF), A650_PROTECT_NORDWR(0x1A800, 0x1FFF), A650_PROTECT_NORDWR(0x1F400, 0x0443),
    A650_PROTECT_RDONLY(0x1F844, 0x007B), A650_PROTECT_NORDWR(0x1F887, 0x001B), A650_PROTECT_NORDWR(0x1F8C0, 0x0000),
};

static const uint32_t kA650Hwcg[49][2] = {
    {0x00B0, 0x02222222}, {0x00B4, 0x02222220}, {0x00B8, 0x00000080}, {0x00BC, 0x0000F3CF},
    {0x00C0, 0x02222222}, {0x00C4, 0x22222222}, {0x00C8, 0x22222222}, {0x00CC, 0x00022222},
    {0x00D0, 0x11111111}, {0x00D4, 0x11111111}, {0x00D8, 0x11111111}, {0x00DC, 0x00011111},
    {0x00E0, 0x77777777}, {0x00E4, 0x77777777}, {0x00E8, 0x77777777}, {0x00EC, 0x00077777},
    {0x00F0, 0x22222222}, {0x00F4, 0x01002222}, {0x00F8, 0x00002220}, {0x0100, 0x00040F00},
    {0x0104, 0x25222022}, {0x0105, 0x00005555}, {0x0106, 0x00000011}, {0x0107, 0x00445044},
    {0x0108, 0x04222222}, {0x0111, 0x00002222}, {0x0114, 0x00222222}, {0x0117, 0x00000002},
    {0x011B, 0x00002222}, {0x0109, 0x00004000}, {0x0112, 0x00002222}, {0x0115, 0x00000200},
    {0x011C, 0x00000000}, {0x010A, 0x00000000}, {0x0113, 0x00000000}, {0x0116, 0x04104004},
    {0x011D, 0x00000000}, {0x0120, 0x00000222}, {0x0121, 0x00000111}, {0x0122, 0x00000777},
    {0x010B, 0x22222222}, {0x0110, 0x00000004}, {0x010F, 0x00000002}, {0x0533, 0x00000182},
    {0x0044, 0x00000000}, {0x0042, 0x00000000}, {0x0118, 0x00000222}, {0x0119, 0x00000111},
    {0x011A, 0x00000555},
};

/* ============================================================================
 * Part A: KGSL SMMU Bank 0 / ASID 2 GPU Context
 * ============================================================================ */

void J716fGpuCtxInitDefaultConfig(J716fGpuCtxConfig *cfg) {
    if (!cfg) return;
    memset(cfg, 0, sizeof(*cfg));
    cfg->regions[J716F_GPU_BUF_SQE].iova = J716F_GPU_SQE_IOVA;
    cfg->regions[J716F_GPU_BUF_SQE].bytes = J716F_GPU_SQE_BYTES;
    cfg->regions[J716F_GPU_BUF_RING].iova = J716F_GPU_RING_IOVA;
    cfg->regions[J716F_GPU_BUF_RING].bytes = J716F_GPU_RING_BYTES;
    cfg->regions[J716F_GPU_BUF_MEMPTRS].iova = J716F_GPU_MEMPTRS_IOVA;
    cfg->regions[J716F_GPU_BUF_MEMPTRS].bytes = J716F_GPU_MEMPTRS_BYTES;
    cfg->regions[J716F_GPU_BUF_SCRATCH].iova = J716F_GPU_SCRATCH_IOVA;
    cfg->regions[J716F_GPU_BUF_SCRATCH].bytes = J716F_GPU_SCRATCH_BYTES;

    cfg->table_capacity_pages = J716F_GPU_TABLE_PAGES;
    cfg->bank = J716F_GPU_SMMU_BANK;
    cfg->asid = J716F_GPU_SMMU_ASID;
    cfg->coherent_walk = 0u; /* Non-coherent walks with dc cvac table cleaning */
    cfg->poll_budget = 1000u;
    cfg->route_count = J716F_GPU_SMMU_ROUTE_COUNT;
    cfg->routes[0].slot = J716F_GPU_SMMU_SLOT_0;
    cfg->routes[0].sid = J716F_GPU_SMMU_SID_0;
    cfg->routes[1].slot = J716F_GPU_SMMU_SLOT_1;
    cfg->routes[1].sid = J716F_GPU_SMMU_SID_1;
    cfg->routes[2].slot = J716F_GPU_SMMU_SLOT_2;
    cfg->routes[2].sid = J716F_GPU_SMMU_SID_2;
    cfg->routes[3].slot = J716F_GPU_SMMU_SLOT_3;
    cfg->routes[3].sid = J716F_GPU_SMMU_SID_3;
}

ArmSmmu4KStatus J716fGpuCtxBuildRanges(const J716fGmuRegionSpec regions[J716F_GPU_BUF_COUNT],
                                       unsigned output_bits,
                                       ArmSmmu4KRange out[J716F_GPU_BUF_COUNT]) {
    uint64_t pa_limit;
    uint64_t prev_end = 0u;
    unsigned i;

    if (!regions || !out) return ArmSmmu4KArgument;
    if (!(output_bits == 32u || output_bits == 36u || output_bits == 40u ||
          output_bits == 42u || output_bits == 44u || output_bits == 48u)) {
        return ArmSmmu4KUnsupported;
    }
    pa_limit = UINT64_C(1) << output_bits;

    for (i = 0u; i < J716F_GPU_BUF_COUNT; ++i) {
        const J716fGmuRegionSpec *r = &regions[i];
        if (!r->bytes || ((r->iova | r->physical | r->bytes) & 4095ULL)) {
            return ArmSmmu4KBadRange;
        }
        if (r->iova >= J716F_GPU_APERTURE_BYTES || r->bytes > J716F_GPU_APERTURE_BYTES - r->iova) {
            return ArmSmmu4KBadRange;
        }
        if (r->physical >= pa_limit || r->bytes > pa_limit - r->physical) {
            return ArmSmmu4KBadRange;
        }
        if (i > 0u && r->iova < prev_end) {
            return ArmSmmu4KOverlap;
        }
        prev_end = r->iova + r->bytes;

        out[i].iova = r->iova;
        out[i].physical = r->physical;
        out[i].bytes = r->bytes;
        /* SQE, ring, and memptrs are accessed by CP under APRIV (privileged); scratch is unprivileged RW. */
        if (i == J716F_GPU_BUF_SQE) {
            out[i].permissions = ArmSmmu4KRead | ArmSmmu4KWrite | ArmSmmu4KExecute | ArmSmmu4KPrivileged;
        } else if (i == J716F_GPU_BUF_RING || i == J716F_GPU_BUF_MEMPTRS) {
            out[i].permissions = ArmSmmu4KRead | ArmSmmu4KWrite | ArmSmmu4KPrivileged;
        } else {
            out[i].permissions = ArmSmmu4KRead | ArmSmmu4KWrite;
        }
        out[i].writeback = 0u;    /* Normal NC matching CPU WC view */
        out[i].shareability = 2u; /* Outer shareable */
    }

    for (i = 0u; i < J716F_GPU_BUF_COUNT; ++i) {
        unsigned j;
        for (j = i + 1u; j < J716F_GPU_BUF_COUNT; ++j) {
            if (regions[i].physical < regions[j].physical + regions[j].bytes &&
                regions[j].physical < regions[i].physical + regions[i].bytes) {
                return ArmSmmu4KOverlap;
            }
        }
    }
    return ArmSmmu4KSuccess;
}

static ArmSmmu4KStatus normalize_unused_fault_s2cr(const ArmSmmu4KIo *io,
                                                   uint32_t id0,
                                                   const ArmSmmu4KStream *routes,
                                                   unsigned route_count) {
    const unsigned exids = (id0 >> 8) & 1u;
    const uint32_t valid_s2cr_bit = exids ? (1u << 10) : 0u;
    const uint32_t canonical_fault = (UINT32_C(2) << 16);
    unsigned i;

    for (i = 0u; i < route_count; ++i) {
        const size_t smr_off = 0x800u + 4u * routes[i].slot;
        const size_t s2cr_off = 0xC00u + 4u * routes[i].slot;
        uint32_t smr = 0u, s2cr = 0u;
        unsigned live;
        if (smr_off + 4u > io->register_bytes || s2cr_off + 4u > io->register_bytes) {
            return ArmSmmu4KBadRange;
        }
        if (io->read32(io->cookie, smr_off, &smr) || io->read32(io->cookie, s2cr_off, &s2cr)) {
            return ArmSmmu4KIoError;
        }
        live = exids ? ((s2cr & valid_s2cr_bit) != 0u) : ((smr & (UINT32_C(1) << 31)) != 0u);
        if (!live && ((s2cr >> 16) & 3u) == 2u && (s2cr & ~valid_s2cr_bit) != canonical_fault) {
            if (io->write32(io->cookie, s2cr_off, canonical_fault) || io->barrier(io->cookie)) {
                return ArmSmmu4KIoError;
            }
        }
    }
    return ArmSmmu4KSuccess;
}

ArmSmmu4KStatus J716fGpuCtxBringup(const ArmSmmu4KIo *io,
                                   const J716fGpuCtxConfig *cfg,
                                   J716fGpuCtxState *state,
                                   J716fGpuCtxReport *report) {
    ArmSmmu4KRange ranges[J716F_GPU_BUF_COUNT];
    ArmSmmu4KProgramResult prog;
    ArmSmmu4KStreamResult sres;
    ArmSmmu4KStatus st;
    unsigned output_bits = 0u;
    unsigned i;

    if (!report) return ArmSmmu4KArgument;
    memset(report, 0, sizeof(*report));
    report->must_retain_resources = 1u;

    if (!io || !cfg || !state || !io->read32 || !io->read64 || !io->write32 || !io->write64 || !io->barrier) {
        return ArmSmmu4KArgument;
    }
    if (state->used) return ArmSmmu4KBusy;
    if (cfg->bank > 63u || !cfg->asid || cfg->asid > 0xFFFFu || cfg->coherent_walk > 1u ||
        !cfg->poll_budget || cfg->poll_budget > 4096u || !cfg->route_count || cfg->route_count > 8u ||
        !cfg->table_cpu || cfg->table_capacity_pages < J716F_GPU_TABLE_PAGES) {
        return ArmSmmu4KArgument;
    }
    for (i = 0u; i < cfg->route_count; ++i) {
        if (cfg->routes[i].slot > 127u) return ArmSmmu4KArgument;
    }

    memset(state, 0, sizeof(*state));
    state->used = 1u;
    state->bank = cfg->bank;
    memset(&prog, 0, sizeof(prog));
    memset(&sres, 0, sizeof(sres));

    /* Step 1: Read real ID2 and decode output width. */
    report->step = J716F_GPU_CTX_STEP_READ_ID;
    if (io->read32(io->cookie, 0x28u, &report->id2)) return ArmSmmu4KIoError;
    st = J716fGmuCtxDecodeOas(report->id2, &output_bits);
    if (st != ArmSmmu4KSuccess) return st;
    report->output_bits = output_bits;

    /* Step 2: Build static 4 KiB page tables via unmodified ArmSmmu4KBuild. */
    report->step = J716F_GPU_CTX_STEP_BUILD_TABLES;
    st = J716fGpuCtxBuildRanges(cfg->regions, output_bits, ranges);
    report->build_status = (uint32_t)st;
    if (st != ArmSmmu4KSuccess) return st;

    state->table_cfg.input_bits = J716F_GPU_INPUT_BITS;
    state->table_cfg.output_bits = output_bits;
    state->table_cfg.table_physical = cfg->table_physical;
    state->table_cfg.leaf_budget = J716F_GPU_LEAF_BUDGET;

    st = ArmSmmu4KBuild(&state->table_cfg, ranges, J716F_GPU_BUF_COUNT,
                        cfg->table_cpu, cfg->table_capacity_pages, &state->table_res);
    report->build_status = (uint32_t)st;
    if (st != ArmSmmu4KSuccess) return st;
    report->table_pages = (uint32_t)state->table_res.table_pages;
    report->leaf_pages = (uint32_t)state->table_res.leaf_pages;

    /* Step 3: Clean table pages to PoC before touching SMMU context bank. */
    report->step = J716F_GPU_CTX_STEP_PUBLISH_TABLES;
    if (cfg->publish_tables) {
        if (cfg->publish_tables(cfg->publish_cookie, cfg->table_cpu, state->table_res.table_pages * 4096u)) {
            return ArmSmmu4KIoError;
        }
    }

    /* Step 4: Program disabled context bank 0 via unmodified ArmSmmu4KProgramDisabledContext. */
    report->step = J716F_GPU_CTX_STEP_PROGRAM;
    memset(&state->ctx_cfg, 0, sizeof(state->ctx_cfg));
    state->ctx_cfg.lower = &state->table_cfg;
    state->ctx_cfg.lower_tables = &state->table_res;
    state->ctx_cfg.asid = cfg->asid;
    state->ctx_cfg.coherent_walk = cfg->coherent_walk;
    state->ctx_cfg.stall_on_fault = 0u;
    state->ctx_cfg.continue_after_fault = 1u;

    st = ArmSmmu4KProgramDisabledContext(io, cfg->bank, &state->ctx_cfg, cfg->poll_budget, &prog);
    report->program_status = (uint32_t)st;
    report->id0 = prog.id0;
    report->id1 = prog.id1;
    report->id2 = prog.id2 ? prog.id2 : report->id2;
    report->program_phase = prog.phase;
    report->program_readback_valid = prog.readback_valid;
    report->program_enabled = prog.enabled;
    state->bank0_offset = prog.context_offset;
    if (prog.context_offset) {
        (void)io->read64(io->cookie, (size_t)prog.context_offset + 0x20u, &report->ttbr0);
    }
    if (st != ArmSmmu4KSuccess) return st;

    /* Step 5: Clear CB0.ACTLR CPRE (bit 1, MMU-500 errata 841119/826419) + TLBIALL/TLBSYNC. */
    report->step = J716F_GPU_CTX_STEP_ACTLR_SYNC;
    {
        const size_t cb = (size_t)prog.context_offset;
        ArmSmmu4KProgramResult tlb_res;
        if (io->read32(io->cookie, cb + 0x4u, &report->actlr_before) ||
            io->write32(io->cookie, cb + 0x4u, report->actlr_before & ~2u) ||
            io->barrier(io->cookie) ||
            io->read32(io->cookie, cb + 0x4u, &report->actlr_after)) {
            return ArmSmmu4KIoError;
        }
        memset(&tlb_res, 0, sizeof(tlb_res));
        st = ArmSmmu4KSyncContextTlb(io, prog.context_offset, cfg->asid, 1u, cfg->poll_budget, &tlb_res);
        if (st != ArmSmmu4KSuccess) return st;
    }

    /* Step 6: Normalize unused faulting S2CRs on GPU slots, then attach 4 GPU streams via ArmSmmu4KAttachStreams. */
    report->step = J716F_GPU_CTX_STEP_ATTACH;
    st = normalize_unused_fault_s2cr(io, report->id0, cfg->routes, cfg->route_count);
    if (st != ArmSmmu4KSuccess) {
        report->route_status = (uint32_t)st;
        return st;
    }
    st = ArmSmmu4KAttachStreams(io, &state->stream_state, cfg->routes, cfg->route_count,
                                cfg->bank, &state->ctx_cfg, &sres);
    report->route_status = (uint32_t)st;
    report->routes_verified = sres.streams_verified;
    (void)io->read32(io->cookie, 0x800u + 4u * cfg->routes[0].slot, &report->smr);
    (void)io->read32(io->cookie, 0xC00u + 4u * cfg->routes[0].slot, &report->s2cr);
    if (st != ArmSmmu4KSuccess) return st;

    report->step = J716F_GPU_CTX_STEP_READY;
    state->ready = 1u;
    return ArmSmmu4KSuccess;
}

/* ============================================================================
 * Part B: Adreno 650 hw_init + SQE/CP Ring 0 + CP_ME_INIT + CP_MEM_WRITE
 * ============================================================================ */

static int gw32(const J716fA650CpIo *io, uint32_t reg, uint32_t v) {
    return io->gpu_write32(io->cookie, reg, v);
}

static int gw64(const J716fA650CpIo *io, uint32_t reg, uint64_t v) {
    return gw32(io, reg, (uint32_t)v) || gw32(io, reg + 1u, (uint32_t)(v >> 32));
}

static int gr32(const J716fA650CpIo *io, uint32_t reg, uint32_t *v) {
    return io->gpu_read32(io->cookie, reg, v);
}

static uint32_t ilog2_u32(size_t v) {
    uint32_t r = 0u;
    while (v > 1u) {
        v >>= 1;
        ++r;
    }
    return r;
}

static void capture_cp_state(const J716fA650CpIo *io, J716fA650CpReport *rep) {
    (void)gr32(io, A650_REG_RBBM_STATUS, &rep->rbbm_status);
    (void)gr32(io, A650_REG_CP_RB_RPTR, &rep->rptr);
    (void)gr32(io, A650_REG_CP_RB_WPTR, &rep->wptr);
    (void)gr32(io, A650_REG_CP_HW_FAULT, &rep->hw_fault);
    (void)gr32(io, A650_REG_CP_INT_STATUS, &rep->cp_int);
    (void)gr32(io, A650_REG_RBBM_INT_0_STATUS, &rep->rbbm_int0);
}

static int a650_hw_init(const J716fA650CpIo *io, const J716fA650CpConfig *cfg, J716fA650CpReport *rep) {
    static const uint32_t kAddrModeRegs[12] = {
        0x0842u, 0x0C01u, 0x8601u, 0x8E05u, 0x9E01u, 0xBE05u,
        0xA601u, 0x9601u, 0x0E00u, 0xAE01u, 0xB601u, 0xF810u,
    };
    uint32_t tmp = 0u;
    unsigned i;

    if (gw32(io, A650_REG_GBIF_HALT, 0u) || gr32(io, A650_REG_GBIF_HALT, &tmp)) return 1;
    if (gw32(io, A650_REG_RBBM_GBIF_HALT, 0u) || gr32(io, A650_REG_RBBM_GBIF_HALT, &tmp)) return 1;
    if (gw32(io, 0xF803u, 0u) || gw64(io, 0xF800u, 0ULL) || gw32(io, 0xF802u, 0u)) return 1;

    for (i = 0u; i < 12u; ++i) {
        if (gw32(io, kAddrModeRegs[i], 1u)) return 1;
    }

    /* Hardware clock gating (a650_hwcg 49 entries) with SPTPRAC SP clock toggled off/on. */
    if (gr32(io, A650_REG_RBBM_CLOCK_CNTL, &rep->hwcg_before)) return 1;
    if (io->gmu_read32(io->cookie, A650_GMU_SPTPRAC_CLK_CTRL, &rep->sptp_before)) return 1;
    if (io->gmu_write32(io->cookie, A650_GMU_SPTPRAC_CLK_CTRL, rep->sptp_before & ~1u)) return 1;
    for (i = 0u; i < 49u; ++i) {
        if (gw32(io, kA650Hwcg[i][0], cfg->hwcg_enable ? kA650Hwcg[i][1] : 0u)) return 1;
    }
    if (io->gmu_read32(io->cookie, A650_GMU_SPTPRAC_CLK_CTRL, &tmp)) return 1;
    if (io->gmu_write32(io->cookie, A650_GMU_SPTPRAC_CLK_CTRL, tmp | 1u)) return 1;
    if (gw32(io, A650_REG_RBBM_CLOCK_CNTL, cfg->hwcg_enable ? 0x8AA8AA82u : 0u)) return 1;
    if (io->gmu_read32(io->cookie, A650_GMU_SPTPRAC_CLK_CTRL, &rep->sptp_after)) return 1;
    if (gr32(io, A650_REG_RBBM_CLOCK_CNTL, &rep->hwcg_after)) return 1;

    /* GBIF QSB, QoS, UCHE, CP ROQ/pool, UBWC (HBB=16), TPL1 bicubic weights */
    for (i = 0x3C03u; i <= 0x3C06u; ++i) {
        if (gw32(io, i, 0x00071620u)) return 1;
    }
    if (gw32(io, A650_REG_RBBM_GBIF_QOS, 0x3u) ||
        gw32(io, A650_REG_RBBM_BUSY_MASKED, 0xFFFFFFFFu) ||
        gw64(io, 0x0E05u, 0x0001FFFFFFFFFFC0ULL) ||
        gw64(io, 0x0E09u, 0x0001FFFFFFFFF000ULL) ||
        gw64(io, 0x0E07u, 0x0001FFFFFFFFF000ULL) ||
        /* Linux a6xx_hw_init explicitly skips UCHE_GMEM_RANGE_MIN/MAX (0x0E0B/0x0E0D) on a650_family
         * (ADRENO_6XX_GEN3 uses 0-based GMEM addressing, MSM_PARAM_GMEM_BASE = 0). */
        gw32(io, 0x0E18u, 0x0804u) || gw32(io, 0x0E17u, 0x4u) ||
        gw32(io, A650_REG_CP_ROQ_THRESH_2, 0x02000140u) ||
        gw32(io, A650_REG_CP_ROQ_THRESH_1, 0x8040362Cu) ||
        gw32(io, A650_REG_CP_MEM_POOL_SIZE, 128u) ||
        gw32(io, 0x9E00u, 0x00300200u) ||
        gw32(io, A650_REG_CP_AHB_CNTL, 1u) ||
        gw32(io, A650_REG_RBBM_PERFCTR_CNTL, 1u) ||
        gw32(io, A650_REG_CP_PERFCTR_SEL_0, 0u) ||
        gw32(io, 0x8E08u, 0x816u) || gw32(io, 0xB604u, 0x6u) || gw32(io, 0xAE02u, 0x26u) ||
        gw32(io, 0x0E01u, 3u << 21) ||
        gw32(io, A650_REG_RBBM_HANG_INT_CNTL, (UINT32_C(1) << 30) | 0x1FFFFFu) ||
        gw32(io, 0x0E19u, 1u) ||
        gw32(io, 0xB608u, 0u) || gw32(io, 0xB609u, 0x3FE05FF4u) || gw32(io, 0xB60Au, 0x3FA0EBEEu) ||
        gw32(io, 0xB60Bu, 0x3F5193EDu) || gw32(io, 0xB60Cu, 0x3F0243F0u)) {
        return 1;
    }

    /* GMU CX busy / power counter setup */
    if (io->gmu_write32(io->cookie, A650_GMU_AO_CX_BUSY_MASK, 0xFF000000u) ||
        io->gmu_read32(io->cookie, A650_GMU_PWR_CNT_SEL_0, &tmp) ||
        io->gmu_write32(io->cookie, A650_GMU_PWR_CNT_SEL_0, (tmp & ~0xFFu) | (1u << 5)) ||
        io->gmu_write32(io->cookie, A650_GMU_PWR_CNT_ENABLE, 1u)) {
        return 1;
    }

    /* 39-entry CP_PROTECT table (last entry in CP_PROTECT(47)) */
    if (gw32(io, A650_REG_CP_PROTECT_CNTL, (1u << 0) | (1u << 1) | (1u << 3))) return 1;
    for (i = 0u; i < 38u; ++i) {
        if (gw32(io, A650_REG_CP_PROTECT_BASE + i, kA650Protect[i])) return 1;
    }
    if (gw32(io, A650_REG_CP_PROTECT_BASE + 47u, kA650Protect[38])) return 1;

    /* CP_APRIV_CNTL: privileged SQE instruction, ring, and rptr shadow accesses */
    if (cfg->apriv_enable) {
        if (gw32(io, A650_REG_CP_APRIV_CNTL, (1u << 6) | (1u << 5) | (1u << 3) | (1u << 2) | (1u << 1))) return 1;
    }
    if (gw32(io, A650_REG_RBBM_INT_0_MASK, A650_RBBM_INT_MASK)) return 1;
    return 0;
}

static J716fCpStatus submit_ring_with_fence(const J716fA650CpIo *io,
                                            volatile uint32_t *ring,
                                            uint32_t ring_words,
                                            uint64_t memptrs_iova,
                                            uint32_t *wptr_inout,
                                            const uint32_t *payload,
                                            size_t payload_words,
                                            uint32_t seqno) {
    uint32_t fence_words[7];
    uint32_t mask = ring_words - 1u;
    uint32_t at = *wptr_inout;
    uint64_t fence_iova = memptrs_iova + 4ULL;
    size_t i;

    if (AdrenoPM4Type4(A650_REG_CP_SCRATCH_2, 1u, &fence_words[0]) != AdrenoPM4Success ||
        AdrenoPM4Type7(A650_PM4_EVENT_WRITE, 4u, &fence_words[2]) != AdrenoPM4Success) {
        return J716fCpPm4Error;
    }
    fence_words[1] = seqno;
    fence_words[3] = A650_EVENT_CACHE_FLUSH_TS | A650_EVENT_IRQ;
    fence_words[4] = (uint32_t)fence_iova;
    fence_words[5] = (uint32_t)(fence_iova >> 32);
    fence_words[6] = seqno;

    for (i = 0u; i < payload_words; ++i) {
        ring[(at + (uint32_t)i) & mask] = payload[i];
    }
    at = (at + (uint32_t)payload_words) & mask;
    for (i = 0u; i < 7u; ++i) {
        ring[(at + (uint32_t)i) & mask] = fence_words[i];
    }
    at = (at + 7u) & mask;

    if (io->barrier(io->cookie)) return J716fCpIoError;
    if (gw32(io, A650_REG_CP_RB_WPTR, at)) return J716fCpIoError;
    if (io->barrier(io->cookie)) return J716fCpIoError;
    *wptr_inout = at;
    return J716fCpSuccess;
}

static int wait_fence(const J716fA650CpIo *io,
                      volatile uint32_t *memptrs,
                      uint32_t target,
                      unsigned budget,
                      unsigned delay_us,
                      uint32_t *seen_out,
                      uint32_t *polls_out) {
    unsigned p;
    for (p = 0u; p < budget; ++p) {
        uint32_t v;
        if (io->barrier(io->cookie)) return -1;
        v = memptrs[1];
        *seen_out = v;
        *polls_out = p + 1u;
        if ((int32_t)(v - target) >= 0) return 0;
        if (io->delay_us && delay_us) io->delay_us(io->cookie, delay_us);
    }
    return 1;
}

J716fCpStatus J716fA650HwInitAndLoadSqe(const J716fA650CpIo *io,
                                        const J716fA650CpConfig *cfg,
                                        J716fA650CpReport *report) {
    uint32_t sqe_len;
    uint32_t i;

    if (!report) return J716fCpArgument;
    memset(report, 0, sizeof(*report));
    if (!io || !cfg || !io->gpu_read32 || !io->gpu_write32 || !io->gmu_read32 || !io->gmu_write32 || !io->barrier) {
        return J716fCpArgument;
    }
    if (!cfg->sqe_cpu || (cfg->sqe_iova & 4095ULL)) return J716fCpArgument;
    if (!cfg->sqe_firmware || cfg->sqe_firmware_bytes < 8u || (cfg->sqe_firmware_bytes & 3u) ||
        cfg->sqe_firmware_bytes - 4u > cfg->sqe_bytes) {
        return J716fCpFirmware;
    }

    report->step = 1u;
    sqe_len = (uint32_t)(cfg->sqe_firmware_bytes - 4u);
    report->sqe_bytes = sqe_len;
    for (i = 0u; i < sqe_len; ++i) {
        cfg->sqe_cpu[i] = cfg->sqe_firmware[4u + i];
    }
    if (io->barrier(io->cookie)) return J716fCpIoError;
    for (i = 0u; i < sqe_len; ++i) {
        if (cfg->sqe_cpu[i] != cfg->sqe_firmware[4u + i]) {
            report->sqe_mismatch_index = i + 1u;
            return J716fCpReadbackMismatch;
        }
    }

    report->step = 2u;
    if (a650_hw_init(io, cfg, report)) {
        capture_cp_state(io, report);
        return J716fCpIoError;
    }

    report->step = 3u;
    if (gw64(io, A650_REG_CP_SQE_INSTR_BASE, cfg->sqe_iova) || io->barrier(io->cookie)) {
        capture_cp_state(io, report);
        return J716fCpIoError;
    }
    return J716fCpSuccess;
}

J716fCpStatus J716fA650CpStart(const J716fA650CpIo *io,
                               const J716fA650CpConfig *cfg,
                               J716fA650CpReport *report) {
    uint32_t sqe_len;
    uint32_t ring_words;
    uint32_t rb_cntl;
    uint32_t wptr = 0u;
    uint32_t me_init[9];
    uint32_t mem_write[4];
    unsigned budget;
    unsigned delay_us;
    J716fCpStatus st;
    uint32_t i;

    if (!report) return J716fCpArgument;
    memset(report, 0, sizeof(*report));
    if (!io || !cfg || !io->gpu_read32 || !io->gpu_write32 || !io->gmu_read32 || !io->gmu_write32 || !io->barrier) {
        return J716fCpArgument;
    }
    if (!cfg->sqe_cpu || !cfg->ring_cpu || !cfg->memptrs_cpu || !cfg->scratch_cpu ||
        cfg->ring_bytes < 8192u || (cfg->ring_bytes & (cfg->ring_bytes - 1u)) ||
        cfg->memptrs_bytes < 64u || cfg->scratch_bytes < 64u ||
        (cfg->sqe_iova & 4095ULL) || (cfg->ring_iova & (cfg->ring_bytes - 1u)) ||
        (cfg->memptrs_iova & 7ULL) || (cfg->scratch_iova & 3ULL)) {
        return J716fCpArgument;
    }
    if (!cfg->sqe_firmware || cfg->sqe_firmware_bytes < 8u || (cfg->sqe_firmware_bytes & 3u) ||
        cfg->sqe_firmware_bytes - 4u > cfg->sqe_bytes) {
        return J716fCpFirmware;
    }

    budget = cfg->fence_poll_budget ? cfg->fence_poll_budget : 10000u;
    delay_us = cfg->poll_delay_us;

    /* Step 1: Copy a650_sqe.fw without its 4-byte header and verify 100% readback. */
    report->step = 1u;
    sqe_len = (uint32_t)(cfg->sqe_firmware_bytes - 4u);
    report->sqe_bytes = sqe_len;
    for (i = 0u; i < sqe_len; ++i) {
        cfg->sqe_cpu[i] = cfg->sqe_firmware[4u + i];
    }
    if (io->barrier(io->cookie)) return J716fCpIoError;
    for (i = 0u; i < sqe_len; ++i) {
        if (cfg->sqe_cpu[i] != cfg->sqe_firmware[4u + i]) {
            report->sqe_mismatch_index = i + 1u;
            return J716fCpReadbackMismatch;
        }
    }

    /* Step 2: Execute A650 hw_init (HWCG, UBWC, UCHE, CP_PROTECT, APRIV). */
    report->step = 2u;
    if (a650_hw_init(io, cfg, report)) {
        capture_cp_state(io, report);
        return J716fCpIoError;
    }

    /* Step 3: Program SQE instruction base, attach Ring 0, and release SQE microcode. */
    report->step = 3u;
    for (i = 0u; i < cfg->memptrs_bytes / 4u; ++i) cfg->memptrs_cpu[i] = 0u;
    cfg->scratch_cpu[0] = 0u;
    if (io->barrier(io->cookie)) return J716fCpIoError;

    /* BUFSZ = log2(ring_bytes / 8), BLKSZ = log2(32 / 8) = 2, NO_UPDATE = bit 27 */
    rb_cntl = (ilog2_u32(cfg->ring_bytes / 8u) & 0x3Fu) | (2u << 8) | (UINT32_C(1) << 27);
    if (gw64(io, A650_REG_CP_SQE_INSTR_BASE, cfg->sqe_iova) ||
        gw64(io, A650_REG_CP_RB_BASE, cfg->ring_iova) ||
        gw32(io, A650_REG_CP_RB_CNTL, rb_cntl) ||
        gw64(io, A650_REG_CP_RB_RPTR_ADDR, cfg->memptrs_iova) ||
        gw32(io, A650_REG_CP_RB_WPTR, 0u) ||
        gw32(io, A650_REG_CP_SCRATCH_2, 0u) ||
        io->barrier(io->cookie) ||
        gw32(io, A650_REG_CP_SQE_CNTL, 1u) ||
        io->barrier(io->cookie)) {
        capture_cp_state(io, report);
        return J716fCpIoError;
    }

    ring_words = (uint32_t)(cfg->ring_bytes / 4u);

    /* Step 4: Submit CP_ME_INIT (a6xx_cp_init: 8 payload words) + fence 1. */
    report->step = 4u;
    memset(me_init, 0, sizeof(me_init));
    if (AdrenoPM4Type7(A650_PM4_ME_INIT, 8u, &me_init[0]) != AdrenoPM4Success) {
        return J716fCpPm4Error;
    }
    me_init[1] = 0x2Fu;
    me_init[2] = 0x00000003u;
    me_init[3] = 0x20000000u;

    st = submit_ring_with_fence(io, cfg->ring_cpu, ring_words, cfg->memptrs_iova, &wptr, me_init, 9u, 1u);
    if (st != J716fCpSuccess) {
        capture_cp_state(io, report);
        return st;
    }
    if (wait_fence(io, cfg->memptrs_cpu, 1u, budget, delay_us, &report->init_fence_seen, &report->init_polls) != 0) {
        capture_cp_state(io, report);
        return J716fCpInitTimeout;
    }

    /* Step 5: Submit first GPU command CP_MEM_WRITE (0xA6500C0D -> scratch[0]) + fence 2. */
    report->step = 5u;
    if (AdrenoPM4Type7(A650_PM4_MEM_WRITE, 3u, &mem_write[0]) != AdrenoPM4Success) {
        return J716fCpPm4Error;
    }
    mem_write[1] = (uint32_t)cfg->scratch_iova;
    mem_write[2] = (uint32_t)(cfg->scratch_iova >> 32);
    mem_write[3] = J716F_GPU_SMOKE_MARKER;

    st = submit_ring_with_fence(io, cfg->ring_cpu, ring_words, cfg->memptrs_iova, &wptr, mem_write, 4u, 2u);
    if (st != J716fCpSuccess) {
        capture_cp_state(io, report);
        return st;
    }
    if (wait_fence(io, cfg->memptrs_cpu, 2u, budget, delay_us, &report->write_fence_seen, &report->write_polls) != 0) {
        capture_cp_state(io, report);
        return J716fCpWriteTimeout;
    }

    if (io->barrier(io->cookie)) return J716fCpIoError;
    report->marker_value = cfg->scratch_cpu[0];
    capture_cp_state(io, report);
    if (report->marker_value != J716F_GPU_SMOKE_MARKER) {
        return J716fCpMarkerMismatch;
    }

    report->step = 6u;
    return J716fCpSuccess;
}
