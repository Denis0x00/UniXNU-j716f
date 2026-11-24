/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* TB-J716F (SM8250-AC, Adreno 650 v2.0) Phase 3b:
 *   - Unified ELF32 a650_zap.mbn / .elf MDT parser & bounds validator
 *   - Qualcomm SM8250 SIP SMC64 PAS authentication (PAS_ID=13, pil_gpu_region@8681a000)
 *   - Qualcomm SCM CP SMMU aperture open (qcom_scm_set_gpu_smmu_aperture bank 0)
 * Pure C, zero IOKit/MMIO dependencies; host-tested under ASan/UBSan. */

#ifndef J716F_A650_ZAP_H
#define J716F_A650_ZAP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* TB-J716F stock DTB: /reserved-memory/pil_gpu_region@8681a000 (size 0x2000) */
#define J716F_ZAP_PIL_PHYS      UINT64_C(0x8681A000)
#define J716F_ZAP_PIL_BYTES     UINT64_C(0x00002000)
#define J716F_ZAP_GPU_PAS_ID    13u
#define J716F_ZAP_MAX_PHDRS     16u
#define J716F_ZAP_MAX_META      0x4000u

/* ARM SMCCC Qualcomm SIP SMC64 service/command encoders (qcom_scm-smc.c) */
#define J716F_SCM_SVC_PIL               0x02u
#define J716F_SCM_PAS_INIT_IMAGE        0x01u
#define J716F_SCM_PAS_MEM_SETUP         0x02u
#define J716F_SCM_PAS_AUTH_AND_RESET    0x05u
#define J716F_SCM_PAS_SHUTDOWN          0x06u
#define J716F_SCM_PAS_IS_SUPPORTED      0x07u

#define J716F_SCM_SVC_INFO              0x06u
#define J716F_SCM_INFO_IS_CALL_AVAIL    0x01u

#define J716F_SCM_SVC_MP                0x0Cu
#define J716F_SCM_MP_CP_SMMU_APERTURE   0x1Bu

#define J716F_SCM_INTERRUPTED           1
#define J716F_SCM_V2_EBUSY              (-12)

/* Argument types (2 bits per arg starting at bit 4) */
#define J716F_SCM_ARG_VAL               0u
#define J716F_SCM_ARG_RO                1u
#define J716F_SCM_ARG_RW                2u

static inline uint32_t J716fScmFn64(uint32_t svc, uint32_t cmd) {
    return (1u << 30) | (2u << 24) | ((svc & 0xFFu) << 8) | (cmd & 0xFFu);
}

static inline uint32_t J716fScmArgs(unsigned count, unsigned t0, unsigned t1, unsigned t2, unsigned t3) {
    return (count & 0xFu) | ((t0 & 3u) << 4) | ((t1 & 3u) << 6) | ((t2 & 3u) << 8) | ((t3 & 3u) << 10);
}

typedef enum {
    J716F_ZAP_OK = 0,
    J716F_ZAP_ERR_ARG = -1,
    J716F_ZAP_ERR_ELF_MAGIC = -2,
    J716F_ZAP_ERR_ELF_PHDR = -3,
    J716F_ZAP_ERR_PH0_LOAD = -4,
    J716F_ZAP_ERR_NO_HASH = -5,
    J716F_ZAP_ERR_RANGE = -6,
    J716F_ZAP_ERR_META_BOUNDS = -7,
    J716F_ZAP_ERR_META_ALLOC = -8,
    J716F_ZAP_ERR_META_CLEAN = -9,
    J716F_ZAP_ERR_INIT_SMC = -10,
    J716F_ZAP_ERR_MEM_SMC = -11,
    J716F_ZAP_ERR_SEG_LOAD = -12,
    J716F_ZAP_ERR_AUTH_SMC = -13,
    J716F_ZAP_ERR_SMC_TIMEOUT = -14,
} J716fZapStatus;

typedef struct {
    uint32_t p_offset;
    uint32_t p_paddr;
    uint32_t p_filesz;
    uint32_t p_memsz;
    uint32_t p_flags;
} J716fZapSeg;

typedef struct {
    uint32_t ehdr_bytes;
    uint32_t hash_offset;
    uint32_t hash_bytes;
    uint32_t metadata_bytes;
    uint32_t metadata_aligned;
    uint64_t min_paddr;
    uint64_t max_paddr;
    uint64_t span_bytes;
    uint32_t relocatable;
    uint32_t seg_count;
    J716fZapSeg segs[J716F_ZAP_MAX_PHDRS];
} J716fZapPlan;

typedef struct {
    int64_t a0;
    uint64_t a1;
    uint32_t ms;
    uint32_t resumes;
} J716fScmResult;

typedef struct {
    void *cookie;
    /* Single raw SMC64 invocation; returns x0 in *out_x0, x1 in *out_x1, x6 in *out_x6. */
    void (*smc)(void *cookie,
                uint64_t x0, uint64_t x1, uint64_t x2, uint64_t x3,
                uint64_t x4, uint64_t x5, uint64_t x6,
                uint64_t *out_x0, uint64_t *out_x1, uint64_t *out_x6);
    uint64_t (*now_us)(void *cookie);
    void (*sleep_ms)(void *cookie, uint32_t ms);
} J716fScmOps;

typedef struct {
    void *cookie;
    /* Allocate/prepare a 4K-aligned physically contiguous metadata buffer of size bytes.
     * Writes metadata bytes, cleans them to PoC, and returns physical address in *out_pa. */
    int (*prepare_metadata)(void *cookie, const uint8_t *meta, size_t meta_bytes,
                            size_t aligned_bytes, uint64_t *out_pa);
    /* Map pil_phys..pil_phys+pil_bytes write-combined, write segment payloads + zero-fill,
     * execute dsb sy, and UNMAP the CPU view BEFORE returning 0! */
    int (*load_pil_region)(void *cookie, uint64_t pil_phys, uint64_t pil_bytes,
                           const uint8_t *fw, size_t fw_size, const J716fZapPlan *plan);
} J716fZapMemOps;

typedef struct {
    int32_t status;
    uint32_t stage;
    uint32_t relocatable;
    uint32_t meta_bytes;
    uint32_t span_bytes;
    int32_t init_rc;
    uint32_t init_ms;
    uint32_t init_resumes;
    int32_t mem_rc;
    uint32_t mem_ms;
    uint32_t mem_resumes;
    int32_t auth_rc;
    uint32_t auth_ms;
    uint32_t auth_resumes;
    uint32_t trust_before;
    uint32_t trust_after;
    int32_t sec_fence_rc;
    uint32_t aperture_avail;
    int32_t aperture_rc;
} J716fZapReport;

/* Parse and validate a unified ELF32 Qualcomm MDT/MBN zap shader image. */
int J716fZapParseElf(const uint8_t *fw, size_t fw_size, uint64_t pil_bytes, J716fZapPlan *plan);

/* Copy ELF header + hash segment into caller-provided metadata buffer (must be >= plan->metadata_aligned). */
int J716fZapBuildMetadata(const uint8_t *fw, size_t fw_size, const J716fZapPlan *plan,
                          uint8_t *dst, size_t dst_cap);

/* Execute a single SM8250 SIP SMC64 call with QCOM_SCM_INTERRUPTED resume and EBUSY retry. */
J716fScmResult J716fScmCall(const J716fScmOps *ops, uint64_t fn, uint64_t arginfo,
                            uint64_t p0, uint64_t p1, uint64_t p2, uint64_t p3);

/* Run PAS_INIT_IMAGE -> PAS_MEM_SETUP -> PIL copy/unmap -> PAS_AUTH_AND_RESET for GPU PAS 13.
 * limit: 1=stop after init_image, 2=stop after mem_setup+copy, 3+=through auth_and_reset. */
int J716fZapAuthenticate(const uint8_t *fw, size_t fw_size,
                         uint64_t pil_phys, uint64_t pil_bytes,
                         uint32_t limit,
                         const J716fScmOps *scm, const J716fZapMemOps *mem,
                         J716fZapReport *report);

/* Ask TZ whether qcom_scm_set_gpu_smmu_aperture exists, and open Bank 0 CP aperture if available. */
int J716fScmOpenCpAperture(const J716fScmOps *scm, uint32_t context_bank,
                           uint32_t *out_avail, int32_t *out_rc);

#ifdef __cplusplus
}
#endif

#endif
