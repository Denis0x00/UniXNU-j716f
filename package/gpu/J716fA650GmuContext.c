/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* TB-J716F (SM8250-AC, Adreno 650 v2.0) KGSL SMMU 4 KiB GMU context bring-up. */
#include "J716fA650GmuContext.h"

#include <string.h>

static void zero_mem(void *p, size_t n) {
    memset(p, 0, n);
}

void J716fGmuCtxInitDefaultConfig(J716fGmuCtxConfig *cfg) {
    if (!cfg) return;
    zero_mem(cfg, sizeof(*cfg));
    cfg->regions[J716F_GMU_BUF_ICACHE].iova = J716F_GMU_ICACHE_IOVA;
    cfg->regions[J716F_GMU_BUF_ICACHE].bytes = J716F_GMU_ICACHE_BYTES;
    cfg->regions[J716F_GMU_BUF_DUMMY].iova = J716F_GMU_DUMMY_IOVA;
    cfg->regions[J716F_GMU_BUF_DUMMY].bytes = J716F_GMU_DUMMY_BYTES;
    cfg->regions[J716F_GMU_BUF_LOG].iova = J716F_GMU_LOG_IOVA;
    cfg->regions[J716F_GMU_BUF_LOG].bytes = J716F_GMU_LOG_BYTES;
    cfg->regions[J716F_GMU_BUF_HFI].iova = J716F_GMU_HFI_IOVA;
    cfg->regions[J716F_GMU_BUF_HFI].bytes = J716F_GMU_HFI_BYTES;
    cfg->regions[J716F_GMU_BUF_DEBUG].iova = J716F_GMU_DEBUG_IOVA;
    cfg->regions[J716F_GMU_BUF_DEBUG].bytes = J716F_GMU_DEBUG_BYTES;

    cfg->table_capacity_pages = J716F_GMU_TABLE_PAGES;
    cfg->bank = J716F_GMU_SMMU_BANK;
    cfg->asid = J716F_GMU_SMMU_ASID;
    cfg->coherent_walk = 0u; /* TB-J716F stock DTB kgsl-smmu@3da0000 has no dma-coherent */
    cfg->poll_budget = 1000u;
    cfg->route_count = J716F_GMU_SMMU_ROUTE_COUNT;
    cfg->routes[0].slot = J716F_GMU_SMMU_SLOT_0;
    cfg->routes[0].sid = J716F_GMU_SMMU_SID_0;
    cfg->routes[1].slot = J716F_GMU_SMMU_SLOT_1;
    cfg->routes[1].sid = J716F_GMU_SMMU_SID_1;
}

ArmSmmu4KStatus J716fGmuCtxDecodeOas(uint32_t id2, unsigned *output_bits_out) {
    static const uint8_t kOasBits[6] = {32u, 36u, 40u, 42u, 44u, 48u};
    unsigned code;
    unsigned bits;
    if (!output_bits_out) return ArmSmmu4KArgument;
    *output_bits_out = 0u;
    code = (id2 >> 4) & 0xFu;
    if (code >= 6u) return ArmSmmu4KUnsupported;
    bits = kOasBits[code];
    if (bits > J716F_GMU_MAX_OUTPUT_BITS) bits = J716F_GMU_MAX_OUTPUT_BITS;
    *output_bits_out = bits;
    return ArmSmmu4KSuccess;
}

ArmSmmu4KStatus J716fGmuCtxBuildRanges(const J716fGmuRegionSpec regions[J716F_GMU_BUF_COUNT],
                                       unsigned output_bits,
                                       ArmSmmu4KRange out[J716F_GMU_BUF_COUNT]) {
    uint64_t pa_limit;
    uint64_t prev_end = 0u;
    unsigned i;

    if (!regions || !out) return ArmSmmu4KArgument;
    if (!(output_bits == 32u || output_bits == 36u || output_bits == 40u ||
          output_bits == 42u || output_bits == 44u || output_bits == 48u)) {
        return ArmSmmu4KUnsupported;
    }
    pa_limit = UINT64_C(1) << output_bits;

    for (i = 0u; i < J716F_GMU_BUF_COUNT; ++i) {
        const J716fGmuRegionSpec *r = &regions[i];
        if (!r->bytes || ((r->iova | r->physical | r->bytes) & 4095ULL)) {
            return ArmSmmu4KBadRange;
        }
        if (r->iova >= J716F_GMU_APERTURE_BYTES || r->bytes > J716F_GMU_APERTURE_BYTES - r->iova) {
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
        /* Only icache is executable and privileged; data buffers stay RW non-executable. */
        out[i].permissions = (i == J716F_GMU_BUF_ICACHE)
                                 ? (ArmSmmu4KRead | ArmSmmu4KWrite | ArmSmmu4KExecute | ArmSmmu4KPrivileged)
                                 : (ArmSmmu4KRead | ArmSmmu4KWrite);
        /* GMU maps all five buffers Normal non-cacheable (MAIR index 0), outer shareable. */
        out[i].writeback = 0u;
        out[i].shareability = 2u;
    }

    /* Require mutually disjoint physical backing spans across the 5 buffers. */
    for (i = 0u; i < J716F_GMU_BUF_COUNT; ++i) {
        unsigned j;
        for (j = i + 1u; j < J716F_GMU_BUF_COUNT; ++j) {
            if (regions[i].physical < regions[j].physical + regions[j].bytes &&
                regions[j].physical < regions[i].physical + regions[i].bytes) {
                return ArmSmmu4KOverlap;
            }
        }
    }
    return ArmSmmu4KSuccess;
}

static uint32_t compute_mismatch_mask(const ArmSmmu4KContextConfig *cfg,
                                      const ArmSmmu4KProgramResult *prog) {
    ArmSmmu4KContextConfig actual;
    ArmSmmu4KContextImage want;
    uint32_t sctlr_want;
    if (!cfg || !prog || prog->phase < 3u) return 0u;
    actual = *cfg;
    actual.id0 = prog->id0;
    actual.id2 = prog->id2;
    if (ArmSmmu4KEncodeContext(&actual, &want) != ArmSmmu4KSuccess) return 0u;
    sctlr_want = prog->enabled ? want.sctlr : 0u;
    return ((prog->readback.cbar != want.cbar) ? (1u << 0) : 0u) |
           ((prog->readback.cba2r != want.cba2r) ? (1u << 1) : 0u) |
           ((prog->readback.tcr != want.tcr) ? (1u << 2) : 0u) |
           ((prog->readback.tcr2 != want.tcr2) ? (1u << 3) : 0u) |
           ((prog->readback.mair0 != want.mair0) ? (1u << 4) : 0u) |
           ((prog->readback.mair1 != want.mair1) ? (1u << 5) : 0u) |
           ((prog->readback.ttbr0 != want.ttbr0) ? (1u << 6) : 0u) |
           ((prog->readback.ttbr1 != want.ttbr1) ? (1u << 7) : 0u) |
           ((prog->readback.sctlr != sctlr_want) ? (1u << 8) : 0u);
}

static void readback_summary(const ArmSmmu4KIo *io,
                             const ArmSmmu4KProgramResult *prog,
                             unsigned bank,
                             unsigned first_slot,
                             J716fGmuCtxReport *report) {
    size_t page;
    if (!io || !prog || !report || !prog->context_offset || !prog->id1) return;
    page = (prog->id1 & (1u << 31)) ? 0x10000u : 0x1000u;
    (void)io->read32(io->cookie, 0x800u + 4u * first_slot, &report->smr);
    (void)io->read32(io->cookie, 0xC00u + 4u * first_slot, &report->s2cr);
    (void)io->read32(io->cookie, page + 4u * bank, &report->cbar);
    (void)io->read32(io->cookie, (size_t)prog->context_offset, &report->sctlr);
    (void)io->read64(io->cookie, (size_t)prog->context_offset + 0x20u, &report->ttbr0);
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
        /* TB-J716F QSMMUv500 leaves unused faulting S2CRs at 0x00f2c2ff (TYPE=2 FAULT with
         * non-zero reset attribute/CBNDX bits that hardware ignores while TYPE==FAULT).
         * Only when a leased slot is ALREADY invalid and ALREADY TYPE==FAULT, normalize it
         * to canonical FAULT (0x00020000) so unmodified upstream ArmSmmu4KAttachStreams accepts it. */
        if (!live && ((s2cr >> 16) & 3u) == 2u && (s2cr & ~valid_s2cr_bit) != canonical_fault) {
            if (io->write32(io->cookie, s2cr_off, canonical_fault) || io->barrier(io->cookie)) {
                return ArmSmmu4KIoError;
            }
        }
    }
    return ArmSmmu4KSuccess;
}

ArmSmmu4KStatus J716fGmuCtxBringup(const ArmSmmu4KIo *io,
                                   const J716fGmuCtxConfig *cfg,
                                   J716fGmuCtxState *state,
                                   J716fGmuCtxReport *report) {
    ArmSmmu4KRange ranges[J716F_GMU_BUF_COUNT];
    ArmSmmu4KProgramResult prog;
    ArmSmmu4KStreamResult sres;
    ArmSmmu4KStatus st;
    unsigned output_bits = 0u;
    unsigned i;

    if (!report) return ArmSmmu4KArgument;
    zero_mem(report, sizeof(*report));
    report->must_retain_resources = 1u;

    if (!io || !cfg || !state || !io->read32 || !io->read64 || !io->write32 || !io->write64 || !io->barrier) {
        return ArmSmmu4KArgument;
    }
    if (state->used) return ArmSmmu4KBusy;
    if (cfg->bank > 63u || !cfg->asid || cfg->asid > 0xFFFFu || cfg->coherent_walk > 1u ||
        !cfg->poll_budget || cfg->poll_budget > 4096u || !cfg->route_count || cfg->route_count > 8u ||
        !cfg->table_cpu || cfg->table_capacity_pages < J716F_GMU_TABLE_PAGES) {
        return ArmSmmu4KArgument;
    }
    for (i = 0u; i < cfg->route_count; ++i) {
        if (cfg->routes[i].slot > 127u) return ArmSmmu4KArgument;
    }

    zero_mem(state, sizeof(*state));
    state->used = 1u;
    state->bank = cfg->bank;
    zero_mem(&prog, sizeof(prog));
    zero_mem(&sres, sizeof(sres));

    /* Step 1: Read real ID2 and decode output address width. */
    report->step = J716F_GMU_CTX_STEP_READ_ID;
    if (io->read32(io->cookie, 0x28u, &report->id2)) {
        return ArmSmmu4KIoError;
    }
    st = J716fGmuCtxDecodeOas(report->id2, &output_bits);
    if (st != ArmSmmu4KSuccess) return st;
    report->output_bits = output_bits;

    /* Step 2: Validate sorted ranges and build static 4 KiB page tables with unmodified ArmSmmu4KBuild. */
    report->step = J716F_GMU_CTX_STEP_BUILD_TABLES;
    st = J716fGmuCtxBuildRanges(cfg->regions, output_bits, ranges);
    report->build_status = (uint32_t)st;
    if (st != ArmSmmu4KSuccess) return st;

    state->table_cfg.input_bits = J716F_GMU_INPUT_BITS;
    state->table_cfg.output_bits = output_bits;
    state->table_cfg.table_physical = cfg->table_physical;
    state->table_cfg.leaf_budget = J716F_GMU_LEAF_BUDGET;

    st = ArmSmmu4KBuild(&state->table_cfg, ranges, J716F_GMU_BUF_COUNT,
                        cfg->table_cpu, cfg->table_capacity_pages, &state->table_res);
    report->build_status = (uint32_t)st;
    if (st != ArmSmmu4KSuccess) return st;
    report->table_pages = (uint32_t)state->table_res.table_pages;
    report->leaf_pages = (uint32_t)state->table_res.leaf_pages;

    /* Step 3: Publish (clean to PoC) all written table pages before touching SMMU context registers. */
    report->step = J716F_GMU_CTX_STEP_PUBLISH_TABLES;
    if (cfg->publish_tables) {
        if (cfg->publish_tables(cfg->publish_cookie, cfg->table_cpu, state->table_res.table_pages * 4096u)) {
            return ArmSmmu4KIoError;
        }
    }

    /* Step 4: Program disabled context bank with unmodified ArmSmmu4KProgramDisabledContext. */
    report->step = J716F_GMU_CTX_STEP_PROGRAM;
    zero_mem(&state->ctx_cfg, sizeof(state->ctx_cfg));
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
    report->program_writes = prog.writes;
    report->program_readback_valid = prog.readback_valid;
    report->program_enabled = prog.enabled;
    report->mismatch_mask = compute_mismatch_mask(&state->ctx_cfg, &prog);
    readback_summary(io, &prog, cfg->bank, cfg->routes[0].slot, report);
    if (st != ArmSmmu4KSuccess) return st;

    /* Step 5: Normalize unused faulting S2CR reset bits on leased slots, then attach with unmodified ArmSmmu4KAttachStreams. */
    report->step = J716F_GMU_CTX_STEP_ATTACH;
    st = normalize_unused_fault_s2cr(io, report->id0, cfg->routes, cfg->route_count);
    if (st != ArmSmmu4KSuccess) {
        report->route_status = (uint32_t)st;
        readback_summary(io, &prog, cfg->bank, cfg->routes[0].slot, report);
        return st;
    }
    st = ArmSmmu4KAttachStreams(io, &state->stream_state, cfg->routes, cfg->route_count,
                                cfg->bank, &state->ctx_cfg, &sres);
    report->route_status = (uint32_t)st;
    report->route_phase = sres.phase;
    report->route_writes = sres.writes;
    report->routes_verified = sres.streams_verified;
    readback_summary(io, &prog, cfg->bank, cfg->routes[0].slot, report);
    if (st != ArmSmmu4KSuccess) return st;

    /* Step 6: Context enabled and GMU streams verified. */
    report->step = J716F_GMU_CTX_STEP_READY;
    state->ready = 1u;
    return ArmSmmu4KSuccess;
}

ArmSmmu4KStatus J716fGmuCtxObserve(const ArmSmmu4KIo *io,
                                   const J716fGmuCtxState *state,
                                   ArmSmmu4KStreamResult *result) {
    if (!io || !state || !result) return ArmSmmu4KArgument;
    if (!state->ready) return ArmSmmu4KStateMismatch;
    return ArmSmmu4KObserveStreams(io, &state->stream_state, &state->ctx_cfg, result);
}
