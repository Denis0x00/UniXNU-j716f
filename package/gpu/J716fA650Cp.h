/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* TB-J716F (SM8250-AC, Adreno 650 v2.0) Phase 3:
 *   1. KGSL SMMU Bank 0 / ASID 2 4 KiB GPU context (slots 0,1,4,5 -> SIDs 0x0000,0x0001,0x0400,0x0401)
 *      built directly on unmodified upstream UniXNU kits/ArmSmmu4K.
 *   2. Adreno 650 hw_init (HWCG, UBWC HBB=16, UCHE, 39-entry CP_PROTECT, APRIV) + SQE microcode load +
 *      Ring 0 bring-up + CP_ME_INIT + CP_MEM_WRITE (0xA6500C0D) smoke test using unmodified kits/AdrenoPM4.
 * Host-tested under ASan/UBSan by package/gpu/tests/J716fA650CpTest.c.
 */
#ifndef J716F_A650_CP_H
#define J716F_A650_CP_H

#include <stddef.h>
#include <stdint.h>

#include "AdrenoPM4.h"
#include "ArmSmmu4K.h"
#include "ArmSmmu4KContext.h"
#include "ArmSmmu4KProgram.h"
#include "ArmSmmu4KStreams.h"
#include "J716fA650GmuContext.h"

#ifdef __cplusplus
extern "C" {
#endif

/* KGSL SMMU Bank 0 / ASID 2 geometry (TB-J716F stock DTB: 6 SMRs total; GMU owns 2,3; GPU owns 0,1,4,5) */
#define J716F_GPU_SMMU_BANK         0u
#define J716F_GPU_SMMU_ASID         2u
#define J716F_GPU_SMMU_ROUTE_COUNT  4u
#define J716F_GPU_SMMU_SLOT_0       0u
#define J716F_GPU_SMMU_SLOT_1       1u
#define J716F_GPU_SMMU_SLOT_2       4u
#define J716F_GPU_SMMU_SLOT_3       5u
#define J716F_GPU_SMMU_SID_0        0x0000u
#define J716F_GPU_SMMU_SID_1        0x0001u
#define J716F_GPU_SMMU_SID_2        0x0400u
#define J716F_GPU_SMMU_SID_3        0x0401u

/* GPU kernel bring-up buffers placed at 32 MiB+ (outside A650 GMEM base 0 window) */
#define J716F_GPU_INPUT_BITS        32u
#define J716F_GPU_APERTURE_BYTES    0x80000000ULL
#define J716F_GPU_SQE_IOVA          0x02000000ULL
#define J716F_GPU_SQE_BYTES         0x00020000ULL /* 128 KiB (a650_sqe.fw minus 4B header) */
#define J716F_GPU_RING_IOVA         0x02020000ULL
#define J716F_GPU_RING_BYTES        0x00008000ULL /* 32 KiB (8192 dwords) */
#define J716F_GPU_MEMPTRS_IOVA      0x02028000ULL
#define J716F_GPU_MEMPTRS_BYTES     0x00001000ULL /* 4 KiB (+0 RPTR shadow, +4 fence timestamp) */
#define J716F_GPU_SCRATCH_IOVA      0x02029000ULL
#define J716F_GPU_SCRATCH_BYTES     0x00001000ULL /* 4 KiB (CP_MEM_WRITE verification target) */

#define J716F_GPU_TABLE_PAGES       8u
#define J716F_GPU_TABLE_BYTES       (J716F_GPU_TABLE_PAGES * 4096u)
#define J716F_GPU_LEAF_BUDGET       256u
#define J716F_GPU_SMOKE_MARKER      0xA6500C0Du

enum {
    J716F_GPU_BUF_SQE     = 0,
    J716F_GPU_BUF_RING    = 1,
    J716F_GPU_BUF_MEMPTRS = 2,
    J716F_GPU_BUF_SCRATCH = 3,
    J716F_GPU_BUF_COUNT   = 4,
};

typedef enum {
    J716F_GPU_CTX_STEP_NONE = 0,
    J716F_GPU_CTX_STEP_READ_ID = 1,
    J716F_GPU_CTX_STEP_BUILD_TABLES = 2,
    J716F_GPU_CTX_STEP_PUBLISH_TABLES = 3,
    J716F_GPU_CTX_STEP_PROGRAM = 4,
    J716F_GPU_CTX_STEP_ACTLR_SYNC = 5,
    J716F_GPU_CTX_STEP_ATTACH = 6,
    J716F_GPU_CTX_STEP_READY = 7,
} J716fGpuCtxStep;

typedef struct {
    J716fGmuRegionSpec regions[J716F_GPU_BUF_COUNT];
    uint64_t table_physical;
    uint64_t *table_cpu;
    size_t table_capacity_pages;
    void *publish_cookie;
    int (*publish_tables)(void *cookie, const void *table_cpu, size_t bytes);
    unsigned bank;
    unsigned asid;
    unsigned coherent_walk;
    unsigned poll_budget;
    unsigned route_count;
    ArmSmmu4KStream routes[8];
} J716fGpuCtxConfig;

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
    uint32_t program_readback_valid;
    uint32_t program_enabled;
    uint32_t routes_verified;
    uint32_t actlr_before, actlr_after;
    uint32_t smr, s2cr;
    uint64_t ttbr0;
    uint32_t must_retain_resources;
} J716fGpuCtxReport;

typedef struct {
    ArmSmmu4KConfig table_cfg;
    ArmSmmu4KResult table_res;
    ArmSmmu4KContextConfig ctx_cfg;
    ArmSmmu4KStreamState stream_state;
    uint64_t bank0_offset;
    unsigned bank;
    unsigned ready;
    unsigned used;
} J716fGpuCtxState;

typedef enum {
    J716fCpSuccess = 0,
    J716fCpArgument,
    J716fCpFirmware,
    J716fCpReadbackMismatch,
    J716fCpIoError,
    J716fCpPm4Error,
    J716fCpInitTimeout,
    J716fCpWriteTimeout,
    J716fCpMarkerMismatch,
} J716fCpStatus;

typedef struct {
    void *cookie;
    /* GPU register read/write by dword register index (0..0xFFFF). */
    int (*gpu_read32)(void *cookie, uint32_t dword_reg, uint32_t *out);
    int (*gpu_write32)(void *cookie, uint32_t dword_reg, uint32_t value);
    /* GMU core register read/write by byte offset (for SPTPRAC_CLOCK_CONTROL & busy counters). */
    int (*gmu_read32)(void *cookie, size_t byte_off, uint32_t *out);
    int (*gmu_write32)(void *cookie, size_t byte_off, uint32_t value);
    int (*barrier)(void *cookie);
    void (*delay_us)(void *cookie, unsigned us);
} J716fA650CpIo;

typedef struct {
    const uint8_t *sqe_firmware;
    size_t sqe_firmware_bytes;
    volatile uint8_t *sqe_cpu;
    size_t sqe_bytes;
    uint64_t sqe_iova;
    volatile uint32_t *ring_cpu;
    size_t ring_bytes;
    uint64_t ring_iova;
    volatile uint32_t *memptrs_cpu;
    size_t memptrs_bytes;
    uint64_t memptrs_iova;
    volatile uint32_t *scratch_cpu;
    size_t scratch_bytes;
    uint64_t scratch_iova;
    unsigned hwcg_enable;       /* default 1 (a650_hwcg 49-entry table) */
    unsigned apriv_enable;      /* default 1 (CP_APRIV_CNTL for SQE/ring/rptr) */
    unsigned fence_poll_budget; /* default 10000 (x 20 us = 200 ms) */
    unsigned poll_delay_us;     /* default 20 us */
} J716fA650CpConfig;

typedef struct {
    uint32_t step; /* 1=sqe, 2=hw_init, 3=ring_attach, 4=me_init, 5=mem_write, 6=done */
    uint32_t sqe_bytes;
    uint32_t sqe_mismatch_index;
    uint32_t hwcg_before, hwcg_after;
    uint32_t sptp_before, sptp_after;
    uint32_t init_fence_seen, init_polls;
    uint32_t write_fence_seen, write_polls;
    uint32_t marker_value;
    uint32_t rbbm_status, rptr, wptr, hw_fault, cp_int, rbbm_int0;
} J716fA650CpReport;

void J716fGpuCtxInitDefaultConfig(J716fGpuCtxConfig *cfg);
ArmSmmu4KStatus J716fGpuCtxBuildRanges(const J716fGmuRegionSpec regions[J716F_GPU_BUF_COUNT],
                                       unsigned output_bits,
                                       ArmSmmu4KRange out[J716F_GPU_BUF_COUNT]);
ArmSmmu4KStatus J716fGpuCtxBringup(const ArmSmmu4KIo *io,
                                   const J716fGpuCtxConfig *cfg,
                                   J716fGpuCtxState *state,
                                   J716fGpuCtxReport *report);

J716fCpStatus J716fA650HwInitAndLoadSqe(const J716fA650CpIo *io,
                                        const J716fA650CpConfig *cfg,
                                        J716fA650CpReport *report);
J716fCpStatus J716fA650CpStart(const J716fA650CpIo *io,
                               const J716fA650CpConfig *cfg,
                               J716fA650CpReport *report);

#ifdef __cplusplus
}
#endif

#endif /* J716F_A650_CP_H */
