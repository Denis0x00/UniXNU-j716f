/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Host ASan/UBSan test for TB-J716F Adreno 650 Phase 3:
 *   - Dual-context KGSL SMMU (GMU Bank 1 slots 2,3 + GPU Bank 0 slots 0,1,4,5) on exact TB-J716F HW regs
 *   - A650 hw_init + SQE load + Ring 0 + CP_ME_INIT + CP_MEM_WRITE decoded via upstream kits/AdrenoPM4 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "J716fA650Cp.h"

/* ---- Fake TB-J716F QSMMUv500 (6 SMRs, 5 banks, 36-bit OAS) ---- */
typedef struct {
    uint8_t regs[J716F_KGSL_SMMU_SIZE];
    unsigned writes;
} FakeSmmu;

static int smmu_rd32(void *cookie, size_t at, uint32_t *out) {
    FakeSmmu *s = (FakeSmmu *)cookie;
    if (!s || !out || (at & 3u) || at + 4u > sizeof(s->regs)) return 1;
    memcpy(out, s->regs + at, 4u);
    return 0;
}

static int smmu_rd64(void *cookie, size_t at, uint64_t *out) {
    FakeSmmu *s = (FakeSmmu *)cookie;
    if (!s || !out || (at & 7u) || at + 8u > sizeof(s->regs)) return 1;
    memcpy(out, s->regs + at, 8u);
    return 0;
}

static int smmu_wr32(void *cookie, size_t at, uint32_t v) {
    FakeSmmu *s = (FakeSmmu *)cookie;
    if (!s || (at & 3u) || at + 4u > sizeof(s->regs)) return 1;
    ++s->writes;
    /* Emulate QSMMUv500 RO bits for both CB0 (0x8000) and CB1 (0x9000). */
    if (at == 0x8010u || at == 0x9010u) v |= 0x60u;
    if (at == 0x8000u || at == 0x9000u) v &= ~0x1000u;
    if (at == 0x8058u || at == 0x9058u) {
        uint32_t cur = 0u;
        memcpy(&cur, s->regs + at, 4u);
        cur &= ~v;
        memcpy(s->regs + at, &cur, 4u);
        return 0;
    }
    if (at == 0x87F0u) {
        uint32_t zero = 0u;
        memcpy(s->regs + 0x87F4u, &zero, 4u);
    }
    if (at == 0x97F0u) {
        uint32_t zero = 0u;
        memcpy(s->regs + 0x97F4u, &zero, 4u);
    }
    memcpy(s->regs + at, &v, 4u);
    return 0;
}

static int smmu_wr64(void *cookie, size_t at, uint64_t v) {
    FakeSmmu *s = (FakeSmmu *)cookie;
    if (!s || (at & 7u) || at + 8u > sizeof(s->regs)) return 1;
    ++s->writes;
    memcpy(s->regs + at, &v, 8u);
    return 0;
}

static int smmu_barrier(void *cookie) {
    (void)cookie;
    return 0;
}

static uint64_t walk_lpae_32(const uint64_t *pool, uint64_t pool_pa, size_t pool_pages, uint64_t iova) {
    const uint64_t mask = UINT64_C(0x0000FFFFFFFFF000);
    size_t l1 = (size_t)((iova >> 30) & 3u);
    size_t l2 = (size_t)((iova >> 21) & 511u);
    size_t l3 = (size_t)((iova >> 12) & 511u);
    uint64_t d1 = pool[l1], d2;
    size_t p2, p3;
    if ((d1 & 3u) != 3u) return 0u;
    p2 = (size_t)(((d1 & mask) - pool_pa) / 4096u);
    assert(p2 < pool_pages);
    d2 = pool[p2 * 512u + l2];
    if ((d2 & 3u) != 3u) return 0u;
    p3 = (size_t)(((d2 & mask) - pool_pa) / 4096u);
    assert(p3 < pool_pages);
    return pool[p3 * 512u + l3];
}

static void test_dual_context_kgsl_smmu(void) {
    FakeSmmu smmu;
    ArmSmmu4KIo io;
    J716fGmuCtxConfig gmu_cfg;
    J716fGmuCtxState gmu_state;
    J716fGmuCtxReport gmu_rep;
    J716fGpuCtxConfig gpu_cfg;
    J716fGpuCtxState gpu_state;
    J716fGpuCtxReport gpu_rep;
    uint32_t scr0 = 0x00230406u, id0 = 0x4C017E06u, id1 = 0x20000005u, id2 = 0x00005511u;
    uint32_t qsmmu_fault = 0x00F2C2FFu, actlr_init = 0x0000032Bu;
    uint64_t *gmu_tbl = (uint64_t *)aligned_alloc(4096u, J716F_GMU_TABLE_BYTES);
    uint64_t *gpu_tbl = (uint64_t *)aligned_alloc(4096u, J716F_GPU_TABLE_BYTES);
    unsigned i;
    assert(gmu_tbl && gpu_tbl);

    memset(&smmu, 0, sizeof(smmu));
    memcpy(smmu.regs + 0x000u, &scr0, 4u);
    memcpy(smmu.regs + 0x020u, &id0, 4u);
    memcpy(smmu.regs + 0x024u, &id1, 4u);
    memcpy(smmu.regs + 0x028u, &id2, 4u);
    memcpy(smmu.regs + 0x8004u, &actlr_init, 4u); /* CB0 ACTLR with CPRE bit 1 set */
    for (i = 0u; i < 6u; ++i) {
        memcpy(smmu.regs + 0xC00u + 4u * i, &qsmmu_fault, 4u);
    }

    io.cookie = &smmu;
    io.register_bytes = sizeof(smmu.regs);
    io.read32 = smmu_rd32;
    io.read64 = smmu_rd64;
    io.write32 = smmu_wr32;
    io.write64 = smmu_wr64;
    io.barrier = smmu_barrier;

    /* 1. Bring up GMU Bank 1 (slots 2, 3 -> SIDs 0x0005, 0x0405) */
    J716fGmuCtxInitDefaultConfig(&gmu_cfg);
    gmu_cfg.regions[0].physical = 0x100000000ULL;
    gmu_cfg.regions[1].physical = 0x102000000ULL;
    gmu_cfg.regions[2].physical = 0x102004000ULL;
    gmu_cfg.regions[3].physical = 0x102008000ULL;
    gmu_cfg.regions[4].physical = 0x10200C000ULL;
    gmu_cfg.table_physical = 0x103000000ULL;
    gmu_cfg.table_cpu = gmu_tbl;
    memset(&gmu_state, 0, sizeof(gmu_state));
    assert(J716fGmuCtxBringup(&io, &gmu_cfg, &gmu_state, &gmu_rep) == ArmSmmu4KSuccess);
    assert(gmu_rep.step == J716F_GMU_CTX_STEP_READY && gmu_rep.routes_verified == 2u);

    /* 2. Bring up GPU Bank 0 (slots 0, 1, 4, 5 -> SIDs 0x0000, 0x0001, 0x0400, 0x0401) */
    J716fGpuCtxInitDefaultConfig(&gpu_cfg);
    gpu_cfg.regions[J716F_GPU_BUF_SQE].physical     = 0x104000000ULL;
    gpu_cfg.regions[J716F_GPU_BUF_RING].physical    = 0x104020000ULL;
    gpu_cfg.regions[J716F_GPU_BUF_MEMPTRS].physical = 0x104028000ULL;
    gpu_cfg.regions[J716F_GPU_BUF_SCRATCH].physical = 0x104029000ULL;
    gpu_cfg.table_physical = 0x104100000ULL;
    gpu_cfg.table_cpu = gpu_tbl;
    memset(&gpu_state, 0, sizeof(gpu_state));
    assert(J716fGpuCtxBringup(&io, &gpu_cfg, &gpu_state, &gpu_rep) == ArmSmmu4KSuccess);
    assert(gpu_rep.step == J716F_GPU_CTX_STEP_READY);
    assert(gpu_rep.output_bits == 36u && gpu_rep.table_pages == 3u && gpu_rep.leaf_pages == 42u);
    assert(gpu_rep.routes_verified == 4u);
    assert(gpu_rep.actlr_before == 0x32Bu && gpu_rep.actlr_after == 0x329u);
    assert((gpu_rep.ttbr0 >> 48) == J716F_GPU_SMMU_ASID);

    /* 3. Verify all 6 SMR slots match exact GMU + GPU assignments and GMU observe still passes! */
    static const uint32_t kExpSmr[6] = {
        0x80000000u, 0x80000001u, 0x80000005u, 0x80000405u, 0x80000400u, 0x80000401u
    };
    static const uint32_t kExpBank[6] = {0u, 0u, 1u, 1u, 0u, 0u};
    for (i = 0u; i < 6u; ++i) {
        uint32_t smr = 0u, s2cr = 0u;
        assert(smmu_rd32(&smmu, 0x800u + 4u * i, &smr) == 0 && smr == kExpSmr[i]);
        assert(smmu_rd32(&smmu, 0xC00u + 4u * i, &s2cr) == 0 && s2cr == kExpBank[i]);
    }
    {
        ArmSmmu4KStreamResult obs;
        assert(J716fGmuCtxObserve(&io, &gmu_state, &obs) == ArmSmmu4KSuccess);
    }

    /* 4. Walk GPU 3-level 4K LPAE page tables for all 4 buffers & check APRIV permissions. */
    for (i = 0u; i < J716F_GPU_BUF_COUNT; ++i) {
        uint64_t d = walk_lpae_32(gpu_tbl, gpu_cfg.table_physical, gpu_rep.table_pages, gpu_cfg.regions[i].iova);
        assert((d & 3u) == 3u && (d & UINT64_C(0x0000FFFFFFFFF000)) == gpu_cfg.regions[i].physical);
        if (i == J716F_GPU_BUF_SCRATCH) {
            assert(((d >> 6) & 1u) == 1u); /* unprivileged RW */
        } else {
            assert(((d >> 6) & 1u) == 0u); /* privileged (APRIV) */
        }
    }

    free(gmu_tbl);
    free(gpu_tbl);
}

/* ---- Fake Adreno 650 GPU/GMU/CP Simulator using upstream AdrenoPM4Plan ---- */
typedef struct {
    uint32_t gpu_regs[0x20000];
    uint32_t gmu_regs[0x10000];
    uint32_t *ring;
    uint32_t ring_words;
    uint32_t *memptrs;
    uint64_t memptrs_iova;
    uint32_t *scratch;
    uint64_t scratch_iova;
    uint32_t rptr;
    unsigned me_init_seen;
    unsigned mem_write_seen;
    unsigned drop_init_fence;
    unsigned drop_write_fence;
    unsigned corrupt_marker;
} FakeA650;

static void simulate_cp_execute(FakeA650 *g, uint32_t new_wptr) {
    uint32_t linear[64];
    uint32_t mask = g->ring_words - 1u;
    size_t n = 0u;
    AdrenoPM4Packet pkts[16];
    size_t pkt_count = 0u;
    size_t i;

    if (!g->gpu_regs[0x0808u]) return; /* CP_SQE_CNTL not started */
    while (g->rptr != new_wptr && n < 64u) {
        linear[n++] = g->ring[g->rptr];
        g->rptr = (g->rptr + 1u) & mask;
    }
    g->gpu_regs[0x0806u] = g->rptr;
    if (!n) return;

    assert(AdrenoPM4Plan(linear, n, pkts, 16u, &pkt_count) == AdrenoPM4Success);
    for (i = 0u; i < pkt_count; ++i) {
        const AdrenoPM4Packet *p = &pkts[i];
        const uint32_t *pl = &linear[p->header_word + 1u];
        if (p->type == 4u && p->identifier == 0x0885u && p->payload_words == 1u) {
            g->gpu_regs[0x0885u] = pl[0];
        } else if (p->type == 7u && p->identifier == 0x48u) {
            /* CP_ME_INIT (8 words, mask=0x2f, 3 contexts, 0x20000000) */
            assert(p->payload_words == 8u && pl[0] == 0x2Fu && pl[1] == 3u && pl[2] == 0x20000000u);
            ++g->me_init_seen;
        } else if (p->type == 7u && p->identifier == 0x3Du) {
            /* CP_MEM_WRITE */
            uint64_t addr = (uint64_t)pl[0] | ((uint64_t)pl[1] << 32);
            assert(p->payload_words == 3u && addr == g->scratch_iova);
            g->scratch[0] = g->corrupt_marker ? 0xBAD0BAD0u : pl[2];
            ++g->mem_write_seen;
        } else if (p->type == 7u && p->identifier == 0x46u) {
            /* CP_EVENT_WRITE (CACHE_FLUSH_TS | IRQ) */
            uint64_t addr = (uint64_t)pl[1] | ((uint64_t)pl[2] << 32);
            uint32_t seq = pl[3];
            assert(p->payload_words == 4u && pl[0] == (0x04u | (UINT32_C(1) << 31)));
            assert(addr == g->memptrs_iova + 4ULL);
            if ((seq == 1u && !g->drop_init_fence) || (seq == 2u && !g->drop_write_fence)) {
                g->memptrs[1] = seq;
            }
        }
    }
}

static int fake_gpu_rd(void *cookie, uint32_t reg, uint32_t *out) {
    FakeA650 *g = (FakeA650 *)cookie;
    if (!g || !out || reg >= 0x20000u) return 1;
    *out = g->gpu_regs[reg];
    return 0;
}

static int fake_gpu_wr(void *cookie, uint32_t reg, uint32_t v) {
    FakeA650 *g = (FakeA650 *)cookie;
    if (!g || reg >= 0x20000u) return 1;
    g->gpu_regs[reg] = v;
    if (reg == 0x0807u && v != 0u) {
        simulate_cp_execute(g, v);
    }
    return 0;
}

static int fake_gmu_rd(void *cookie, size_t off, uint32_t *out) {
    FakeA650 *g = (FakeA650 *)cookie;
    if (!g || !out || (off & 3u) || off / 4u >= 0x10000u) return 1;
    *out = g->gmu_regs[off / 4u];
    return 0;
}

static int fake_gmu_wr(void *cookie, size_t off, uint32_t v) {
    FakeA650 *g = (FakeA650 *)cookie;
    if (!g || (off & 3u) || off / 4u >= 0x10000u) return 1;
    g->gmu_regs[off / 4u] = v;
    return 0;
}

static int fake_barrier(void *cookie) {
    (void)cookie;
    return 0;
}

static void fake_delay(void *cookie, unsigned us) {
    (void)cookie;
    (void)us;
}

static void test_cp_start_and_guards(void) {
    FakeA650 *gpu = (FakeA650 *)calloc(1u, sizeof(FakeA650));
    uint8_t *sqe_buf = (uint8_t *)calloc(1u, J716F_GPU_SQE_BYTES);
    uint32_t *ring_buf = (uint32_t *)calloc(1u, J716F_GPU_RING_BYTES);
    uint32_t *memptrs_buf = (uint32_t *)calloc(1u, J716F_GPU_MEMPTRS_BYTES);
    uint32_t *scratch_buf = (uint32_t *)calloc(1u, J716F_GPU_SCRATCH_BYTES);
    uint8_t fake_sqe_fw[260];
    J716fA650CpIo io;
    J716fA650CpConfig cfg;
    J716fA650CpReport rep;
    unsigned i;

    assert(gpu && sqe_buf && ring_buf && memptrs_buf && scratch_buf);
    for (i = 0u; i < sizeof(fake_sqe_fw); ++i) fake_sqe_fw[i] = (uint8_t)(i ^ 0x5Au);

    gpu->ring = ring_buf;
    gpu->ring_words = (uint32_t)(J716F_GPU_RING_BYTES / 4u);
    gpu->memptrs = memptrs_buf;
    gpu->memptrs_iova = J716F_GPU_MEMPTRS_IOVA;
    gpu->scratch = scratch_buf;
    gpu->scratch_iova = J716F_GPU_SCRATCH_IOVA;
    gpu->gpu_regs[0x0210u] = 1u; /* RBBM_STATUS idle */
    gpu->gmu_regs[0x0080u] = 1u; /* SPTPRAC clock on */

    io.cookie = gpu;
    io.gpu_read32 = fake_gpu_rd;
    io.gpu_write32 = fake_gpu_wr;
    io.gmu_read32 = fake_gmu_rd;
    io.gmu_write32 = fake_gmu_wr;
    io.barrier = fake_barrier;
    io.delay_us = fake_delay;

    memset(&cfg, 0, sizeof(cfg));
    cfg.sqe_firmware = fake_sqe_fw;
    cfg.sqe_firmware_bytes = sizeof(fake_sqe_fw);
    cfg.sqe_cpu = sqe_buf;
    cfg.sqe_bytes = J716F_GPU_SQE_BYTES;
    cfg.sqe_iova = J716F_GPU_SQE_IOVA;
    cfg.ring_cpu = ring_buf;
    cfg.ring_bytes = J716F_GPU_RING_BYTES;
    cfg.ring_iova = J716F_GPU_RING_IOVA;
    cfg.memptrs_cpu = memptrs_buf;
    cfg.memptrs_bytes = J716F_GPU_MEMPTRS_BYTES;
    cfg.memptrs_iova = J716F_GPU_MEMPTRS_IOVA;
    cfg.scratch_cpu = scratch_buf;
    cfg.scratch_bytes = J716F_GPU_SCRATCH_BYTES;
    cfg.scratch_iova = J716F_GPU_SCRATCH_IOVA;
    cfg.hwcg_enable = 1u;
    cfg.apriv_enable = 1u;
    cfg.fence_poll_budget = 8u;
    cfg.poll_delay_us = 0u;

    /* 1. Happy path: SQE copy + hw_init + Ring 0 + CP_ME_INIT + CP_MEM_WRITE */
    assert(J716fA650CpStart(&io, &cfg, &rep) == J716fCpSuccess);
    assert(rep.step == 6u && rep.sqe_bytes == 256u);
    assert(gpu->me_init_seen == 1u && gpu->mem_write_seen == 1u);
    assert(rep.init_fence_seen == 1u && rep.write_fence_seen == 2u);
    assert(rep.marker_value == J716F_GPU_SMOKE_MARKER);
    assert(gpu->gpu_regs[0x00AEu] == 0x8AA8AA82u); /* RBBM_CLOCK_CNTL */
    assert(gpu->gpu_regs[0x0844u] == 0x6Eu);        /* CP_APRIV_CNTL */
    assert(gpu->gpu_regs[0x0850u + 47u] == ((UINT32_C(1) << 31) | 0x1F8C0u));

    /* 2. Bad SQE firmware (< 8 B) refused cleanly */
    cfg.sqe_firmware_bytes = 4u;
    assert(J716fA650CpStart(&io, &cfg, &rep) == J716fCpFirmware);
    cfg.sqe_firmware_bytes = sizeof(fake_sqe_fw);

    /* 3. CP_ME_INIT fence timeout detected */
    memset(gpu->gpu_regs, 0, sizeof(gpu->gpu_regs));
    gpu->rptr = 0u;
    gpu->drop_init_fence = 1u;
    assert(J716fA650CpStart(&io, &cfg, &rep) == J716fCpInitTimeout);
    assert(rep.step == 4u && rep.init_polls == 8u);
    gpu->drop_init_fence = 0u;

    /* 4. CP_MEM_WRITE marker mismatch detected */
    gpu->rptr = 0u;
    gpu->corrupt_marker = 1u;
    assert(J716fA650CpStart(&io, &cfg, &rep) == J716fCpMarkerMismatch);
    assert(rep.step == 5u && rep.marker_value == 0xBAD0BAD0u);

    free(scratch_buf);
    free(memptrs_buf);
    free(ring_buf);
    free(sqe_buf);
    free(gpu);
}

int main(void) {
    test_dual_context_kgsl_smmu();
    test_cp_start_and_guards();
    printf("J716fA650CpTest: PASS\n");
    return 0;
}
