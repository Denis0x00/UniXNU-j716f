/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* TB-J716F (SM8250-AC, Adreno 650 v2.0) KGSL SMMU 4 KiB GMU context bring-up.
 *
 * Strictly targets KGSL SMMU (0x03DA0000, size 0x10000) ONLY — never touches
 * APPS SMMU (0x15000000). Built directly on unmodified upstream UniXNU kits/ArmSmmu4K:
 *   - Bank 1 / ASID 1 (Bank 0 / ASID 2 reserved for GPU Phase 3)
 *   - SMR slots 2 & 3 -> exact SIDs 0x0005 & 0x0405 (TB-J716F stock DTB
 *     qcom,gmu@3d6a000/gmu_kernel iommus = <0x181 0x05 0x400> across gfx_0_tbu & gfx_1_tbu)
 *   - Static 32-bit lower-region (TTBR0) 4 KiB page table mapping the 5 fixed GMU
 *     buffers (icache, dummy, log, hfi, debug) before enabling SCTLR.
 * Host-tested under ASan/UBSan by package/gpu/tests/J716fA650GmuContextTest.c.
 */
#ifndef J716F_A650_GMU_CONTEXT_H
#define J716F_A650_GMU_CONTEXT_H

#include <stddef.h>
#include <stdint.h>

#include "ArmSmmu4K.h"
#include "ArmSmmu4KContext.h"
#include "ArmSmmu4KProgram.h"
#include "ArmSmmu4KStreams.h"

#ifdef __cplusplus
extern "C" {
#endif

/* TB-J716F KGSL SMMU & GMU hardware constants (verified from stock DTB) */
#define J716F_KGSL_SMMU_BASE        0x03DA0000ULL
#define J716F_KGSL_SMMU_SIZE        0x00010000ULL
#define J716F_GMU_PDC_BASE          0x0B290000ULL
#define J716F_GMU_PDC_SIZE          0x00010000ULL

#define J716F_GMU_SMMU_BANK         1u
#define J716F_GMU_SMMU_ASID         1u
#define J716F_GMU_SMMU_ROUTE_COUNT  2u
#define J716F_GMU_SMMU_SLOT_0       2u
#define J716F_GMU_SMMU_SLOT_1       3u
#define J716F_GMU_SMMU_SID_0        0x0005u
#define J716F_GMU_SMMU_SID_1        0x0405u

/* GMU 32-bit IOVA layout (0..2 GiB lower/TTBR0 region, matching a6xx_gmu.c) */
#define J716F_GMU_INPUT_BITS        32u
#define J716F_GMU_MAX_OUTPUT_BITS   40u
#define J716F_GMU_APERTURE_BYTES    0x80000000ULL

#define J716F_GMU_ICACHE_IOVA       0x00004000ULL
#define J716F_GMU_ICACHE_BYTES      0x00FFC000ULL /* 16 MiB - 16 KiB (4092 pages) */
#define J716F_GMU_DUMMY_IOVA        0x60000000ULL
#define J716F_GMU_DUMMY_BYTES       0x00004000ULL /* 16 KiB (4 pages) */
#define J716F_GMU_LOG_IOVA          0x60004000ULL
#define J716F_GMU_LOG_BYTES         0x00004000ULL /* 16 KiB (4 pages) */
#define J716F_GMU_HFI_IOVA          0x60008000ULL
#define J716F_GMU_HFI_BYTES         0x00004000ULL /* 16 KiB (4 pages) */
#define J716F_GMU_DEBUG_IOVA        0x60400000ULL
#define J716F_GMU_DEBUG_BYTES       0x00007000ULL /* 28 KiB (7 pages) */

/* 1 L1 + 2 L2 + 10 L3 = 13 pages required for the 5 fixed ranges; reserve 16 pages (64 KiB). */
#define J716F_GMU_TABLE_PAGES       16u
#define J716F_GMU_TABLE_BYTES       (J716F_GMU_TABLE_PAGES * 4096u)
#define J716F_GMU_LEAF_BUDGET       8192u

enum {
    J716F_GMU_BUF_ICACHE = 0,
    J716F_GMU_BUF_DUMMY  = 1,
    J716F_GMU_BUF_LOG    = 2,
    J716F_GMU_BUF_HFI    = 3,
    J716F_GMU_BUF_DEBUG  = 4,
    J716F_GMU_BUF_COUNT  = 5,
};

typedef enum {
    J716F_GMU_CTX_STEP_NONE = 0,
    J716F_GMU_CTX_STEP_READ_ID = 1,
    J716F_GMU_CTX_STEP_BUILD_TABLES = 2,
    J716F_GMU_CTX_STEP_PUBLISH_TABLES = 3,
    J716F_GMU_CTX_STEP_PROGRAM = 4,
    J716F_GMU_CTX_STEP_ATTACH = 5,
    J716F_GMU_CTX_STEP_READY = 6,
} J716fGmuCtxStep;

typedef struct {
    uint64_t iova;
    uint64_t physical;
    uint64_t bytes;
} J716fGmuRegionSpec;

typedef struct {
    /* Caller-allocated physically contiguous payloads (icache, dummy, log, hfi, debug). */
    J716fGmuRegionSpec regions[J716F_GMU_BUF_COUNT];
    /* Caller-allocated physically contiguous 4 KiB-aligned page-table pool (>= J716F_GMU_TABLE_PAGES). */
    uint64_t table_physical;
    uint64_t *table_cpu;
    size_t table_capacity_pages;
    /* Optional cache clean callback to publish written table pages to PoC before context enable. */
    void *publish_cookie;
    int (*publish_tables)(void *cookie, const void *table_cpu, size_t bytes);
    /* Context options: bank/asid/routes/walk coherency/poll budget. */
    unsigned bank;
    unsigned asid;
    unsigned coherent_walk; /* 0 = outer NC (safe default on SM8250 without dma-coherent), 1 = inner WBWA */
    unsigned poll_budget;
    unsigned route_count;
    ArmSmmu4KStream routes[8];
} J716fGmuCtxConfig;

typedef struct {
    uint32_t step;
    uint32_t id0, id1, id2;
    uint32_t output_bits;
    uint32_t build_status;
    uint32_t program_status;
    uint32_t route_status;
    uint32_t table_pages;
    uint32_t leaf_pages;
    uint32_t program_phase;
    uint32_t program_writes;
    uint32_t program_readback_valid;
    uint32_t program_enabled;
    uint32_t route_phase;
    uint32_t route_writes;
    uint32_t routes_verified;
    uint32_t mismatch_mask; /* bits 0..8: CBAR,CBA2R,TCR,TCR2,MAIR0,MAIR1,TTBR0,TTBR1,SCTLR */
    uint32_t cbar, sctlr, smr, s2cr;
    uint64_t ttbr0;
    uint32_t must_retain_resources;
} J716fGmuCtxReport;

typedef struct {
    ArmSmmu4KConfig table_cfg;
    ArmSmmu4KResult table_res;
    ArmSmmu4KContextConfig ctx_cfg;
    ArmSmmu4KStreamState stream_state;
    unsigned bank;
    unsigned ready;
    unsigned used;
} J716fGmuCtxState;

/* Populate default TB-J716F GMU SMMU config (bank 1, ASID 1, slots 2/3 -> SIDs 0x0005/0x0405,
 * coherent_walk=0, poll_budget=1000, and canonical fixed IOVAs/sizes). Caller fills physical
 * addresses, table_physical, table_cpu, and publish_tables. */
void J716fGmuCtxInitDefaultConfig(J716fGmuCtxConfig *cfg);

/* Decode SMMUv2 ID2.OAS field into output address bits, capped at J716F_GMU_MAX_OUTPUT_BITS (40). */
ArmSmmu4KStatus J716fGmuCtxDecodeOas(uint32_t id2, unsigned *output_bits_out);

/* Validate regions and produce sorted ArmSmmu4KRange[5] for ArmSmmu4KBuild. */
ArmSmmu4KStatus J716fGmuCtxBuildRanges(const J716fGmuRegionSpec regions[J716F_GMU_BUF_COUNT],
                                       unsigned output_bits,
                                       ArmSmmu4KRange out[J716F_GMU_BUF_COUNT]);

/* Bring up the GMU's KGSL SMMU context bank and attach its stream routes.
 * On any failure, must_retain_resources stays 1 and report captures the exact failing step/status. */
ArmSmmu4KStatus J716fGmuCtxBringup(const ArmSmmu4KIo *io,
                                   const J716fGmuCtxConfig *cfg,
                                   J716fGmuCtxState *state,
                                   J716fGmuCtxReport *report);

/* Read-only verification of the active GMU context bank and attached routes. */
ArmSmmu4KStatus J716fGmuCtxObserve(const ArmSmmu4KIo *io,
                                   const J716fGmuCtxState *state,
                                   ArmSmmu4KStreamResult *result);

#ifdef __cplusplus
}
#endif

#endif /* J716F_A650_GMU_CONTEXT_H */
