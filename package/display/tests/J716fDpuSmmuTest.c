// SPDX-License-Identifier: BSD-2-Clause-Patent
// Offline C11 ASan/UBSan unit tests for J716fDpuSmmu.h (SM8250 DPU 6.0 observation,
// Display APPS SMMU SID 0x820/0x402 inspection, and Stage-1 4 KiB window tables).
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../J716fDpuSmmu.h"

typedef struct FakeMem {
    uint32_t *words;
    uint32_t word_count;
} FakeMem;

static uint32_t fake_rd(void *ctx, uint32_t off) {
    FakeMem *m = (FakeMem *)ctx;
    assert((off & 3u) == 0u);
    const uint32_t idx = off / 4u;
    assert(idx < m->word_count);
    return m->words[idx];
}

static void fake_wr(void *ctx, uint32_t off, uint32_t val) {
    FakeMem *m = (FakeMem *)ctx;
    assert((off & 3u) == 0u);
    const uint32_t idx = off / 4u;
    assert(idx < m->word_count);
    m->words[idx] = val;
}

static void test_smr_matching(void) {
    /* Invalid SMR (bit 31 = 0) never matches. */
    assert(!j716f_dpu_smr_matches_sid(0x04020820u, 0x0820u));
    /* Valid SMR with mask 0x402 matches 0x820, 0x822, 0xC20, 0xC22 and nothing else. */
    const uint32_t smr = 0x80000000u | (0x0402u << 16) | 0x0820u;
    assert(j716f_dpu_smr_matches_sid(smr, 0x0820u));
    assert(j716f_dpu_smr_matches_sid(smr, 0x0822u));
    assert(j716f_dpu_smr_matches_sid(smr, 0x0C20u));
    assert(j716f_dpu_smr_matches_sid(smr, 0x0C22u));
    assert(!j716f_dpu_smr_matches_sid(smr, 0x0821u));
    assert(!j716f_dpu_smr_matches_sid(smr, 0x0800u));
    assert(!j716f_dpu_smr_matches_sid(smr, 0x1C00u));
}

static void test_smmu_inspect(void) {
    uint32_t *backing = (uint32_t *)calloc(J716F_APPS_SMMU_PHYS_SIZE / 4u, sizeof(uint32_t));
    assert(backing != NULL);
    FakeMem mem = {backing, J716F_APPS_SMMU_PHYS_SIZE / 4u};
    J716fDpuRegIo io = {fake_rd, fake_wr, &mem};

    /* 128 global pages (NUMPAGENDXB = 6 -> 1 << 7 = 128), 74 banks, 128 groups. */
    fake_wr(&mem, J716F_SMMU_SCR0, 0x002F0406u);
    fake_wr(&mem, J716F_SMMU_IDR0, 0x40000080u);
    fake_wr(&mem, J716F_SMMU_IDR1, (6u << 28) | 74u);
    fake_wr(&mem, J716F_SMMU_IDR2, 0x00001555u);

    /* Slot 2: Display stream 0x820 mask 0x402 -> CB 3 (translate). */
    fake_wr(&mem, J716F_SMMU_SMR(2), 0x80000000u | (0x0402u << 16) | 0x0820u);
    fake_wr(&mem, J716F_SMMU_S2CR(2), 3u);
    fake_wr(&mem, J716F_SMMU_CBAR(3), 0x00010000u);
    fake_wr(&mem, J716F_SMMU_CBA2R(3), 0x00000001u);

    const uint32_t cb3 = (128u + 3u) * 4096u;
    fake_wr(&mem, cb3 + J716F_CB_SCTLR, 0x000000E0u);
    fake_wr(&mem, cb3 + J716F_CB_TCR2, 0x00000005u);
    fake_wr(&mem, cb3 + J716F_CB_FSR, 0x00000000u);

    /* Slot 5: Unrelated UFS bypass stream -> CB 73. */
    fake_wr(&mem, J716F_SMMU_SMR(5), 0x80000060u);
    fake_wr(&mem, J716F_SMMU_S2CR(5), 73u);

    J716fDpuSmmuReport rep;
    j716f_dpu_smmu_inspect(&io, &rep);

    assert(rep.groups == 128u);
    assert(rep.banks == 74u);
    assert(rep.pages == 128u);
    assert(rep.valid_smrs == 2u);
    assert(rep.summary_count == 2u);
    assert(rep.match_count == 4u); /* 0x820, 0x822, 0xC20, 0xC22 all match slot 2 */
    for (uint32_t i = 0; i < 4u; ++i) {
        assert(rep.matches[i].slot == 2u);
        assert(rep.matches[i].bank == 3u);
        assert(rep.matches[i].valid_cb);
        assert(rep.matches[i].cbar == 0x00010000u);
        assert(rep.matches[i].cba2r == 0x00000001u);
        assert(rep.matches[i].sctlr == 0x000000E0u);
    }
    free(backing);
}

static void test_mdp_observe(void) {
    uint32_t *backing = (uint32_t *)calloc(J716F_MDSS_PHYS_SIZE / 4u, sizeof(uint32_t));
    assert(backing != NULL);
    FakeMem mem = {backing, J716F_MDSS_PHYS_SIZE / 4u};
    J716fDpuRegIo io = {fake_rd, fake_wr, &mem};

    fake_wr(&mem, 0x0000u, 0x60000000u);
    fake_wr(&mem, J716F_MDP_SUB_OFF + J716F_MDP_HW_VERSION, 0x60000000u);

    /* VIG0 (left half 1280x1600) and VIG1 (right half 1280x1600 @ x=1280). */
    const uint32_t vig0 = J716F_MDP_SUB_OFF + 0x4000u;
    const uint32_t vig1 = J716F_MDP_SUB_OFF + 0x6000u;
    fake_wr(&mem, vig0 + J716F_SSPP_SRC_SIZE, (1600u << 16) | 1280u);
    fake_wr(&mem, vig0 + J716F_SSPP_SRC0_ADDR, J716F_DPU_BOOT_IOVA);
    fake_wr(&mem, vig0 + J716F_SSPP_SRC_YSTRIDE0, J716F_DPU_BOOT_STRIDE);
    fake_wr(&mem, vig1 + J716F_SSPP_SRC_SIZE, (1600u << 16) | 1280u);
    fake_wr(&mem, vig1 + J716F_SSPP_SRC_XY, 1280u);
    fake_wr(&mem, vig1 + J716F_SSPP_SRC0_ADDR, J716F_DPU_BOOT_IOVA);
    fake_wr(&mem, vig1 + J716F_SSPP_SRC_YSTRIDE0, J716F_DPU_BOOT_STRIDE);

    const uint32_t ctl0 = J716F_MDP_SUB_OFF + J716F_MDP_CTL_BASE;
    fake_wr(&mem, ctl0 + J716F_MDP_CTL_TOP, 0x00000010u);
    fake_wr(&mem, ctl0 + J716F_MDP_CTL_INTF_ACTIVE, 0x00000006u);

    J716fDpuMdpReport rep;
    j716f_dpu_observe_mdp(&io, &rep);

    assert(rep.mdss_hw_ver == 0x60000000u);
    assert(rep.mdp_hw_ver == 0x60000000u);
    assert(rep.pipes[0].src0_addr == J716F_DPU_BOOT_IOVA);
    assert(rep.pipes[0].stride0 == J716F_DPU_BOOT_STRIDE);
    assert(rep.pipes[1].src_xy == 1280u);
    assert(rep.ctls[0].top == 0x00000010u);
    assert(rep.ctls[0].intf_active == 0x00000006u);
    free(backing);
}

static void test_stage1_tables_and_slots(void) {
    uint64_t *tables = (uint64_t *)calloc(J716F_DPU_TOTAL_TABLE_PAGES * 512u, sizeof(uint64_t));
    assert(tables != NULL);
    const uint64_t pa = 0x104000000ULL;

    assert(!j716f_dpu_build_stage1_tables(NULL, pa));
    assert(!j716f_dpu_build_stage1_tables(tables, 0x104000100ULL));
    assert(j716f_dpu_build_stage1_tables(tables, pa));

    assert(tables[0] == (pa | 3ULL));
    assert(tables[2] == (0x80000000ULL | J716F_DPU_PTE_ATTRS | 1ULL));
    assert(tables[3] == (0xC0000000ULL | J716F_DPU_PTE_ATTRS | 1ULL));
    assert(tables[128] == ((pa + 2ULL * 4096ULL) | 3ULL));
    assert(tables[191] == ((pa + 65ULL * 4096ULL) | 3ULL));
    assert(tables[192] == 0ULL);

    uint64_t pages[4] = {0x120000000ULL, 0x120001000ULL, 0x180000000ULL, 0x1F0000000ULL};
    assert(j716f_dpu_fill_slot_l3(tables, 4, pages, 4) == 0u);
    assert(j716f_dpu_fill_slot_l3(tables, 0, pages, 0) == 0u);
    assert(j716f_dpu_fill_slot_l3(tables, 0, pages, J716F_DPU_SLOT_MAX_PAGES + 1u) == 0u);

    uint64_t bad_page[1] = {0x120000800ULL};
    assert(j716f_dpu_fill_slot_l3(tables, 0, bad_page, 1) == 0u);

    const uint32_t iova0 = j716f_dpu_fill_slot_l3(tables, 0, pages, 4);
    const uint32_t iova2 = j716f_dpu_fill_slot_l3(tables, 2, pages, 2);
    assert(iova0 == J716F_DPU_WINDOW);
    assert(iova2 == J716F_DPU_WINDOW + 2u * J716F_DPU_SLOT_BYTES);

    const uint64_t *slot0_l3 = tables + 2u * 512u;
    assert(slot0_l3[0] == (pages[0] | J716F_DPU_PTE_ATTRS | 3ULL));
    assert(slot0_l3[3] == (pages[3] | J716F_DPU_PTE_ATTRS | 3ULL));
    assert(slot0_l3[4] == 0ULL);

    /* Refilling slot 0 with fewer pages clears trailing stale PTEs. */
    assert(j716f_dpu_fill_slot_l3(tables, 0, pages, 1) == J716F_DPU_WINDOW);
    assert(slot0_l3[0] == (pages[0] | J716F_DPU_PTE_ATTRS | 3ULL));
    assert(slot0_l3[1] == 0ULL);
    assert(slot0_l3[3] == 0ULL);

    free(tables);
}

int main(void) {
    test_smr_matching();
    test_smmu_inspect();
    test_mdp_observe();
    test_stage1_tables_and_slots();
    printf("J716fDpuSmmuTest: PASS (4 suites)\n");
    return 0;
}
