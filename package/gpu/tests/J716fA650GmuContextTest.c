/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Host ASan/UBSan test for TB-J716F KGSL SMMU 4 KiB GMU context bring-up
 * linked directly against unmodified upstream UniXNU kits/ArmSmmu4K. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "J716fA650GmuContext.h"

typedef struct {
    uint8_t regs[J716F_KGSL_SMMU_SIZE];
    unsigned reads;
    unsigned writes;
    unsigned barriers;
    unsigned simulate_tbi_ro;
    unsigned simulate_asidpne_ro;
} FakeKgslSmmu;

static int smmu_rd32(void *cookie, size_t at, uint32_t *out) {
    FakeKgslSmmu *s = (FakeKgslSmmu *)cookie;
    if (!s || !out || (at & 3u) || at + 4u > sizeof(s->regs)) return 1;
    ++s->reads;
    memcpy(out, s->regs + at, 4u);
    return 0;
}

static int smmu_rd64(void *cookie, size_t at, uint64_t *out) {
    FakeKgslSmmu *s = (FakeKgslSmmu *)cookie;
    if (!s || !out || (at & 7u) || at + 8u > sizeof(s->regs)) return 1;
    ++s->reads;
    memcpy(out, s->regs + at, 8u);
    return 0;
}

static int smmu_wr32(void *cookie, size_t at, uint32_t v) {
    FakeKgslSmmu *s = (FakeKgslSmmu *)cookie;
    if (!s || (at & 3u) || at + 4u > sizeof(s->regs)) return 1;
    ++s->writes;
    /* Emulate QSMMUv500 read-only quirk bits when enabled in test. */
    if (s->simulate_tbi_ro && at == 0x9010u) v |= 0x60u;
    if (s->simulate_asidpne_ro && at == 0x9000u) v &= ~0x1000u;
    /* Clearing FSR (CB+0x58) writes 1-to-clear in hardware. */
    if (at == 0x9058u) {
        uint32_t cur = 0;
        memcpy(&cur, s->regs + at, 4u);
        cur &= ~v;
        memcpy(s->regs + at, &cur, 4u);
        return 0;
    }
    /* TLBSYNC (CB+0x7f0) immediately completes -> TLBSTATUS (CB+0x7f4) = 0. */
    if (at == 0x97F0u) {
        uint32_t zero = 0;
        memcpy(s->regs + 0x97F4u, &zero, 4u);
    }
    memcpy(s->regs + at, &v, 4u);
    return 0;
}

static int smmu_wr64(void *cookie, size_t at, uint64_t v) {
    FakeKgslSmmu *s = (FakeKgslSmmu *)cookie;
    if (!s || (at & 7u) || at + 8u > sizeof(s->regs)) return 1;
    ++s->writes;
    memcpy(s->regs + at, &v, 8u);
    return 0;
}

static int smmu_barrier(void *cookie) {
    FakeKgslSmmu *s = (FakeKgslSmmu *)cookie;
    if (!s) return 1;
    ++s->barriers;
    return 0;
}

static void init_fake_smmu(FakeKgslSmmu *s, unsigned exids, unsigned oas_code) {
    uint32_t id0, id1, id2, scr0, fault = 2u << 16;
    unsigned i;
    memset(s, 0, sizeof(*s));
    /* S1TS(30) | SMS(27) | NUMSIDB=11(9..12) | EXIDS(8) | NUMSMRG=8(0..7) */
    id0 = (1u << 30) | (1u << 27) | (11u << 9) | ((exids & 1u) << 8) | 8u;
    /* PAGESIZE=4K(31=0) | NUMPAGENDXB=2(30:28 -> 8 pages, CB0=0x8000) | NUMCB=4 */
    id1 = (2u << 28) | 4u;
    /* PTFS_4K(12) | UBS=5(48-bit VA) | OAS=oas_code */
    id2 = (1u << 12) | (5u << 8) | ((oas_code & 0xFu) << 4);
    /* Real QSMMUv500 sCR0: USFCFG(10)=1, GFRE(1)=1, GFIE(2)=1, CLIENTPD(0)=0, EXIDENABLE(3)=exids */
    scr0 = 0x002F0406u | ((exids & 1u) << 3);

    memcpy(s->regs + 0x000u, &scr0, 4u);
    memcpy(s->regs + 0x020u, &id0, 4u);
    memcpy(s->regs + 0x024u, &id1, 4u);
    memcpy(s->regs + 0x028u, &id2, 4u);
    for (i = 0; i < 8u; ++i) {
        memcpy(s->regs + 0xC00u + 4u * i, &fault, 4u);
    }
}

typedef struct {
    unsigned calls;
    size_t last_bytes;
    int fail;
} PublishSpy;

static int fake_publish(void *cookie, const void *table_cpu, size_t bytes) {
    PublishSpy *spy = (PublishSpy *)cookie;
    if (!spy || !table_cpu || !bytes) return 1;
    ++spy->calls;
    spy->last_bytes = bytes;
    return spy->fail ? 1 : 0;
}

/* Walk a 32-bit 3-level 4 KiB LPAE page table built by ArmSmmu4KBuild. */
static uint64_t walk_lpae_32(const uint64_t *pool, uint64_t pool_pa, size_t pool_pages, uint64_t iova) {
    const uint64_t addr_mask = UINT64_C(0x0000FFFFFFFFF000);
    size_t l1_idx = (size_t)((iova >> 30) & 3u);
    size_t l2_idx = (size_t)((iova >> 21) & 511u);
    size_t l3_idx = (size_t)((iova >> 12) & 511u);
    uint64_t d1 = pool[l1_idx];
    uint64_t d2, pa2, pa3;
    size_t p2, p3;
    if ((d1 & 3u) != 3u) return 0u;
    pa2 = d1 & addr_mask;
    if (pa2 < pool_pa || (pa2 - pool_pa) / 4096u >= pool_pages) return 0u;
    p2 = (size_t)((pa2 - pool_pa) / 4096u);
    d2 = pool[p2 * 512u + l2_idx];
    if ((d2 & 3u) != 3u) return 0u;
    pa3 = d2 & addr_mask;
    if (pa3 < pool_pa || (pa3 - pool_pa) / 4096u >= pool_pages) return 0u;
    p3 = (size_t)((pa3 - pool_pa) / 4096u);
    return pool[p3 * 512u + l3_idx];
}

static void populate_valid_pa(J716fGmuCtxConfig *cfg, uint64_t *table_pool) {
    J716fGmuCtxInitDefaultConfig(cfg);
    cfg->regions[J716F_GMU_BUF_ICACHE].physical = 0x100000000ULL;
    cfg->regions[J716F_GMU_BUF_DUMMY].physical  = 0x102000000ULL;
    cfg->regions[J716F_GMU_BUF_LOG].physical    = 0x102004000ULL;
    cfg->regions[J716F_GMU_BUF_HFI].physical    = 0x102008000ULL;
    cfg->regions[J716F_GMU_BUF_DEBUG].physical  = 0x10200C000ULL;
    cfg->table_physical = 0x103000000ULL;
    cfg->table_cpu = table_pool;
    cfg->table_capacity_pages = J716F_GMU_TABLE_PAGES;
}

static void test_decode_oas(void) {
    unsigned bits = 0;
    assert(J716fGmuCtxDecodeOas(0u << 4, &bits) == ArmSmmu4KSuccess && bits == 32u);
    assert(J716fGmuCtxDecodeOas(1u << 4, &bits) == ArmSmmu4KSuccess && bits == 36u);
    assert(J716fGmuCtxDecodeOas(2u << 4, &bits) == ArmSmmu4KSuccess && bits == 40u);
    /* OAS > 40 is capped at 40 bits (J716F_GMU_MAX_OUTPUT_BITS). */
    assert(J716fGmuCtxDecodeOas(3u << 4, &bits) == ArmSmmu4KSuccess && bits == 40u);
    assert(J716fGmuCtxDecodeOas(5u << 4, &bits) == ArmSmmu4KSuccess && bits == 40u);
    assert(J716fGmuCtxDecodeOas(6u << 4, &bits) == ArmSmmu4KUnsupported && bits == 0u);
    assert(J716fGmuCtxDecodeOas(15u << 4, &bits) == ArmSmmu4KUnsupported && bits == 0u);
    assert(J716fGmuCtxDecodeOas(2u << 4, NULL) == ArmSmmu4KArgument);
}

static void test_build_ranges_validation(void) {
    J716fGmuCtxConfig cfg;
    ArmSmmu4KRange ranges[J716F_GMU_BUF_COUNT];
    uint64_t dummy_pool[512];

    populate_valid_pa(&cfg, dummy_pool);
    assert(J716fGmuCtxBuildRanges(cfg.regions, 36u, ranges) == ArmSmmu4KSuccess);
    assert(ranges[0].permissions == (ArmSmmu4KRead | ArmSmmu4KWrite | ArmSmmu4KExecute | ArmSmmu4KPrivileged));
    for (unsigned i = 1; i < J716F_GMU_BUF_COUNT; ++i) {
        assert(ranges[i].permissions == (ArmSmmu4KRead | ArmSmmu4KWrite));
        assert(ranges[i].writeback == 0u && ranges[i].shareability == 2u);
    }

    /* PA >= 2^32 rejected for 32-bit OAS */
    assert(J716fGmuCtxBuildRanges(cfg.regions, 32u, ranges) == ArmSmmu4KBadRange);

    /* Unaligned physical address rejected */
    cfg.regions[J716F_GMU_BUF_LOG].physical += 0x100u;
    assert(J716fGmuCtxBuildRanges(cfg.regions, 36u, ranges) == ArmSmmu4KBadRange);

    /* Overlapping physical backing rejected */
    populate_valid_pa(&cfg, dummy_pool);
    cfg.regions[J716F_GMU_BUF_HFI].physical = cfg.regions[J716F_GMU_BUF_LOG].physical + 0x2000u;
    assert(J716fGmuCtxBuildRanges(cfg.regions, 36u, ranges) == ArmSmmu4KOverlap);

    /* Overlapping IOVAs rejected */
    populate_valid_pa(&cfg, dummy_pool);
    cfg.regions[J716F_GMU_BUF_LOG].iova = cfg.regions[J716F_GMU_BUF_DUMMY].iova + 0x2000u;
    assert(J716fGmuCtxBuildRanges(cfg.regions, 36u, ranges) == ArmSmmu4KOverlap);
}

static void test_full_bringup(unsigned exids, unsigned oas_code, unsigned qsmmu_quirks) {
    FakeKgslSmmu smmu;
    ArmSmmu4KIo io;
    J716fGmuCtxConfig cfg;
    J716fGmuCtxState state;
    J716fGmuCtxReport rep;
    ArmSmmu4KStreamResult obs;
    PublishSpy spy = {0, 0, 0};
    uint64_t *table_pool = (uint64_t *)aligned_alloc(4096u, J716F_GMU_TABLE_BYTES);
    assert(table_pool != NULL);
    memset(table_pool, 0xA5, J716F_GMU_TABLE_BYTES);

    init_fake_smmu(&smmu, exids, oas_code);
    smmu.simulate_tbi_ro = qsmmu_quirks;
    smmu.simulate_asidpne_ro = qsmmu_quirks;

    io.cookie = &smmu;
    io.register_bytes = sizeof(smmu.regs);
    io.read32 = smmu_rd32;
    io.read64 = smmu_rd64;
    io.write32 = smmu_wr32;
    io.write64 = smmu_wr64;
    io.barrier = smmu_barrier;

    populate_valid_pa(&cfg, table_pool);
    cfg.publish_cookie = &spy;
    cfg.publish_tables = fake_publish;

    memset(&state, 0, sizeof(state));
    assert(J716fGmuCtxBringup(&io, &cfg, &state, &rep) == ArmSmmu4KSuccess);
    assert(state.ready == 1u);
    assert(rep.step == J716F_GMU_CTX_STEP_READY);
    assert(rep.output_bits == (oas_code == 1u ? 36u : 40u));
    assert(rep.table_pages == 13u);
    assert(rep.leaf_pages == 4111u);
    assert(spy.calls == 1u && spy.last_bytes == 13u * 4096u);
    assert(rep.program_enabled == 1u && rep.program_readback_valid == 1u);
    assert(rep.routes_verified == 2u);
    assert((rep.ttbr0 >> 48) == J716F_GMU_SMMU_ASID);
    assert((rep.ttbr0 & UINT64_C(0x0000FFFFFFFFFFFF)) == cfg.table_physical);

    /* Verify exact SMR/S2CR programming for GMU slots 2 (0x0005) and 3 (0x0405). */
    uint32_t smr2 = 0, s2cr2 = 0, smr3 = 0, s2cr3 = 0, s2cr0 = 0;
    assert(smmu_rd32(&smmu, 0x800u + 4u * 2u, &smr2) == 0);
    assert(smmu_rd32(&smmu, 0xC00u + 4u * 2u, &s2cr2) == 0);
    assert(smmu_rd32(&smmu, 0x800u + 4u * 3u, &smr3) == 0);
    assert(smmu_rd32(&smmu, 0xC00u + 4u * 3u, &s2cr3) == 0);
    assert(smmu_rd32(&smmu, 0xC00u + 4u * 0u, &s2cr0) == 0);
    assert((smr2 & 0xFFFFu) == 0x0005u);
    assert((smr3 & 0xFFFFu) == 0x0405u);
    assert((s2cr2 & 0xFFu) == J716F_GMU_SMMU_BANK);
    assert((s2cr3 & 0xFFu) == J716F_GMU_SMMU_BANK);
    /* Slot 0 (reserved for GPU) must remain untouched in FAULT! */
    assert(s2cr0 == (2u << 16));

    /* Walk actual 3-level LPAE tables from TTBR0 for every GMU buffer start & end page. */
    for (unsigned i = 0; i < J716F_GMU_BUF_COUNT; ++i) {
        uint64_t start_d = walk_lpae_32(table_pool, cfg.table_physical, rep.table_pages, cfg.regions[i].iova);
        uint64_t end_iova = cfg.regions[i].iova + cfg.regions[i].bytes - 4096ULL;
        uint64_t end_pa = cfg.regions[i].physical + cfg.regions[i].bytes - 4096ULL;
        uint64_t end_d = walk_lpae_32(table_pool, cfg.table_physical, rep.table_pages, end_iova);
        assert((start_d & 3u) == 3u && (start_d & UINT64_C(0x0000FFFFFFFFF000)) == cfg.regions[i].physical);
        assert((end_d & 3u) == 3u && (end_d & UINT64_C(0x0000FFFFFFFFF000)) == end_pa);
        if (i == J716F_GMU_BUF_ICACHE) {
            /* Executable + privileged: PXN=0, UXN=0, AP[1]=0 */
            assert(((start_d >> 53) & 3u) == 0u);
            assert(((start_d >> 6) & 1u) == 0u);
        } else {
            /* Data buffers: PXN=1, UXN=1, AP[1]=1 (unpriv RW, XN) */
            assert(((start_d >> 53) & 3u) == 3u);
            assert(((start_d >> 6) & 1u) == 1u);
        }
    }
    /* Unmapped addresses must walk to 0 (invalid). */
    assert(walk_lpae_32(table_pool, cfg.table_physical, rep.table_pages, 0x00000000ULL) == 0u);
    assert(walk_lpae_32(table_pool, cfg.table_physical, rep.table_pages, 0x6000C000ULL) == 0u);

    /* Read-only observation must succeed and verify 2 routes. */
    assert(J716fGmuCtxObserve(&io, &state, &obs) == ArmSmmu4KSuccess);
    assert(obs.streams_verified == 2u);

    /* Second bringup attempt on same state must refuse with ArmSmmu4KBusy. */
    assert(J716fGmuCtxBringup(&io, &cfg, &state, &rep) == ArmSmmu4KBusy);

    free(table_pool);
}

static void test_error_guards(void) {
    FakeKgslSmmu smmu;
    ArmSmmu4KIo io;
    J716fGmuCtxConfig cfg;
    J716fGmuCtxState state;
    J716fGmuCtxReport rep;
    uint64_t *table_pool = (uint64_t *)aligned_alloc(4096u, J716F_GMU_TABLE_BYTES);
    assert(table_pool != NULL);

    io.cookie = &smmu;
    io.register_bytes = sizeof(smmu.regs);
    io.read32 = smmu_rd32;
    io.read64 = smmu_rd64;
    io.write32 = smmu_wr32;
    io.write64 = smmu_wr64;
    io.barrier = smmu_barrier;

    /* 1. Pre-enabled CB1 (SCTLR.M = 1) must be refused before any SMMU write. */
    init_fake_smmu(&smmu, 0u, 2u);
    {
        uint32_t busy = 1u;
        memcpy(smmu.regs + 0x9000u, &busy, 4u);
    }
    populate_valid_pa(&cfg, table_pool);
    memset(&state, 0, sizeof(state));
    assert(J716fGmuCtxBringup(&io, &cfg, &state, &rep) == ArmSmmu4KBusy);
    assert(rep.step == J716F_GMU_CTX_STEP_PROGRAM);
    assert(rep.program_status == (uint32_t)ArmSmmu4KBusy);
    assert(smmu.writes == 0u);
    assert(rep.must_retain_resources == 1u);

    /* 2. Conflicting active SMR on another slot covering SID 0x0005 must refuse attach. */
    init_fake_smmu(&smmu, 0u, 2u);
    {
        uint32_t conflict_smr = (1u << 31) | 0x0005u;
        uint32_t conflict_s2cr = 0u; /* bank 0 */
        memcpy(smmu.regs + 0x800u + 4u * 6u, &conflict_smr, 4u);
        memcpy(smmu.regs + 0xC00u + 4u * 6u, &conflict_s2cr, 4u);
    }
    populate_valid_pa(&cfg, table_pool);
    memset(&state, 0, sizeof(state));
    assert(J716fGmuCtxBringup(&io, &cfg, &state, &rep) == ArmSmmu4KOverlap);
    assert(rep.step == J716F_GMU_CTX_STEP_ATTACH);
    assert(rep.route_status == (uint32_t)ArmSmmu4KOverlap);
    assert(state.ready == 0u);

    /* 3. Table pool overlapping payload physical backing must fail before any MMIO write. */
    init_fake_smmu(&smmu, 0u, 2u);
    populate_valid_pa(&cfg, table_pool);
    cfg.table_physical = cfg.regions[J716F_GMU_BUF_HFI].physical;
    memset(&state, 0, sizeof(state));
    assert(J716fGmuCtxBringup(&io, &cfg, &state, &rep) == ArmSmmu4KOverlap);
    assert(rep.step == J716F_GMU_CTX_STEP_BUILD_TABLES);
    assert(smmu.writes == 0u);

    /* 4. Bad global sCR0 (missing USFCFG) must refuse stream attach. */
    init_fake_smmu(&smmu, 0u, 2u);
    {
        uint32_t bad_scr0 = 0x002F0006u;
        memcpy(smmu.regs + 0x000u, &bad_scr0, 4u);
    }
    populate_valid_pa(&cfg, table_pool);
    memset(&state, 0, sizeof(state));
    assert(J716fGmuCtxBringup(&io, &cfg, &state, &rep) == ArmSmmu4KStateMismatch);
    assert(rep.step == J716F_GMU_CTX_STEP_ATTACH);
    assert(rep.route_status == (uint32_t)ArmSmmu4KStateMismatch);

    /* 5. Publish callback failure must abort before programming SMMU registers. */
    init_fake_smmu(&smmu, 0u, 2u);
    populate_valid_pa(&cfg, table_pool);
    {
        PublishSpy fail_spy = {0, 0, 1};
        cfg.publish_cookie = &fail_spy;
        cfg.publish_tables = fake_publish;
        memset(&state, 0, sizeof(state));
        assert(J716fGmuCtxBringup(&io, &cfg, &state, &rep) == ArmSmmu4KIoError);
        assert(rep.step == J716F_GMU_CTX_STEP_PUBLISH_TABLES);
        assert(smmu.writes == 0u);
    }

    free(table_pool);
}

int main(void) {
    test_decode_oas();
    test_build_ranges_validation();
    test_full_bringup(0u, 1u, 0u); /* Legacy SMR, 36-bit OAS */
    test_full_bringup(0u, 2u, 1u); /* Legacy SMR, 40-bit OAS + QSMMUv500 TBI/ASIDPNE quirks */
    test_full_bringup(1u, 2u, 1u); /* EXIDS SMR, 40-bit OAS + QSMMUv500 quirks */
    test_error_guards();
    printf("J716fA650GmuContextTest: PASS\n");
    return 0;
}
