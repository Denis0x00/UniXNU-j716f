/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* TB-J716F (SM8250-AC, Adreno 650 v2.0) Phase 3b:
 * Unified ELF32 zap shader MDT parser + SM8250 SIP SMC64 PAS authentication + CP SMMU aperture. */
#include "J716fA650Zap.h"

#include <string.h>

static uint16_t rd16le(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd32le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint32_t ph_word(const uint8_t *fw, uint32_t phoff, unsigned i, unsigned field) {
    return rd32le(fw + phoff + i * 32u + field * 4u);
}

int J716fZapParseElf(const uint8_t *fw, size_t fw_size, uint64_t pil_bytes, J716fZapPlan *plan) {
    if (!fw || !plan || pil_bytes == 0) return J716F_ZAP_ERR_ARG;
    memset(plan, 0, sizeof(*plan));

    /* ELF32 header check (qcom_mdt_header_valid) */
    if (fw_size < 52u || fw[0] != 0x7Fu || fw[1] != 'E' || fw[2] != 'L' || fw[3] != 'F' || fw[4] != 1u) {
        return J716F_ZAP_ERR_ELF_MAGIC;
    }

    const uint32_t phoff = rd32le(fw + 28u);
    const uint16_t phent = rd16le(fw + 42u);
    const uint16_t phnum = rd16le(fw + 44u);
    if (phent != 32u || phnum < 2u || phnum > J716F_ZAP_MAX_PHDRS ||
        (uint64_t)phoff + (uint64_t)phnum * 32u > (uint64_t)fw_size) {
        return J716F_ZAP_ERR_ELF_PHDR;
    }

    /* Program header 0 must not be PT_LOAD (it describes ELF header + phdr table). */
    if (ph_word(fw, phoff, 0u, 0u) == 1u) {
        return J716F_ZAP_ERR_PH0_LOAD;
    }

    /* Locate hash segment: QCOM_MDT_TYPE_HASH = 2 in p_flags[26:24]. */
    unsigned hash_idx = 0u;
    for (unsigned i = 1u; i < phnum; ++i) {
        const uint32_t flags = ph_word(fw, phoff, i, 6u);
        if (((flags >> 24) & 7u) == 2u) {
            hash_idx = i;
            break;
        }
    }
    if (hash_idx == 0u) {
        return J716F_ZAP_ERR_NO_HASH;
    }

    uint64_t lo = UINT64_MAX;
    uint64_t hi = 0u;
    uint32_t relocate = 0u;
    uint32_t seg_count = 0u;

    for (unsigned i = 0u; i < phnum; ++i) {
        const uint32_t p_type   = ph_word(fw, phoff, i, 0u);
        const uint32_t p_offset = ph_word(fw, phoff, i, 1u);
        const uint32_t p_paddr  = ph_word(fw, phoff, i, 3u);
        const uint32_t p_filesz = ph_word(fw, phoff, i, 4u);
        const uint32_t p_memsz  = ph_word(fw, phoff, i, 5u);
        const uint32_t p_flags  = ph_word(fw, phoff, i, 6u);

        /* mdt_phdr_loadable: PT_LOAD (1), not HASH (type 2), non-zero memsz. */
        if (p_type != 1u || ((p_flags >> 24) & 7u) == 2u || p_memsz == 0u) {
            continue;
        }
        if (p_filesz > p_memsz || (uint64_t)p_offset + (uint64_t)p_filesz > (uint64_t)fw_size) {
            return J716F_ZAP_ERR_RANGE;
        }
        if (p_flags & (1u << 27)) {
            relocate = 1u;
        }
        if ((uint64_t)p_paddr < lo) {
            lo = (uint64_t)p_paddr;
        }
        const uint64_t end = ((uint64_t)p_paddr + (uint64_t)p_memsz + 0xFFFull) & ~UINT64_C(0xFFF);
        if (end > hi) {
            hi = end;
        }
        plan->segs[seg_count].p_offset = p_offset;
        plan->segs[seg_count].p_paddr  = p_paddr;
        plan->segs[seg_count].p_filesz = p_filesz;
        plan->segs[seg_count].p_memsz  = p_memsz;
        plan->segs[seg_count].p_flags  = p_flags;
        seg_count++;
    }

    if (seg_count == 0u || lo >= hi || (hi - lo) > pil_bytes) {
        return J716F_ZAP_ERR_RANGE;
    }

    const uint32_t ehdr_bytes  = ph_word(fw, phoff, 0u, 4u);
    const uint32_t hash_offset = ph_word(fw, phoff, hash_idx, 1u);
    const uint32_t hash_bytes  = ph_word(fw, phoff, hash_idx, 4u);
    if (ehdr_bytes == 0u || hash_bytes == 0u ||
        (uint64_t)ehdr_bytes > (uint64_t)fw_size ||
        (uint64_t)hash_offset + (uint64_t)hash_bytes > (uint64_t)fw_size ||
        (uint64_t)ehdr_bytes + (uint64_t)hash_bytes > J716F_ZAP_MAX_META) {
        return J716F_ZAP_ERR_META_BOUNDS;
    }

    plan->ehdr_bytes       = ehdr_bytes;
    plan->hash_offset      = hash_offset;
    plan->hash_bytes       = hash_bytes;
    plan->metadata_bytes   = ehdr_bytes + hash_bytes;
    plan->metadata_aligned = (plan->metadata_bytes + 0xFFFu) & ~0xFFFu;
    plan->min_paddr        = lo;
    plan->max_paddr        = hi;
    plan->span_bytes       = hi - lo;
    plan->relocatable      = relocate;
    plan->seg_count        = seg_count;
    return J716F_ZAP_OK;
}

int J716fZapBuildMetadata(const uint8_t *fw, size_t fw_size, const J716fZapPlan *plan,
                          uint8_t *dst, size_t dst_cap) {
    if (!fw || !plan || !dst) return J716F_ZAP_ERR_ARG;
    if (plan->metadata_aligned == 0u || dst_cap < plan->metadata_aligned) return J716F_ZAP_ERR_META_BOUNDS;
    if ((uint64_t)plan->ehdr_bytes > (uint64_t)fw_size ||
        (uint64_t)plan->hash_offset + (uint64_t)plan->hash_bytes > (uint64_t)fw_size) {
        return J716F_ZAP_ERR_META_BOUNDS;
    }
    memset(dst, 0, plan->metadata_aligned);
    memcpy(dst, fw, plan->ehdr_bytes);
    memcpy(dst + plan->ehdr_bytes, fw + plan->hash_offset, plan->hash_bytes);
    return J716F_ZAP_OK;
}

J716fScmResult J716fScmCall(const J716fScmOps *ops, uint64_t fn, uint64_t arginfo,
                            uint64_t p0, uint64_t p1, uint64_t p2, uint64_t p3) {
    J716fScmResult out;
    memset(&out, 0, sizeof(out));
    if (!ops || !ops->smc) {
        out.a0 = -1;
        return out;
    }

    const uint64_t start_us = ops->now_us ? ops->now_us(ops->cookie) : 0u;
    uint64_t first = fn;
    uint64_t a6 = 0u;
    unsigned resumes = 0u;
    unsigned busy = 0u;

    while (resumes < 10000u) {
        uint64_t rx0 = 0u, rx1 = 0u, rx6 = 0u;
        ops->smc(ops->cookie, first, arginfo, p0, p1, p2, p3, a6, &rx0, &rx1, &rx6);
        if ((int64_t)rx0 == J716F_SCM_INTERRUPTED) {
            first = 1u;
            a6 = rx6;
            resumes++;
            continue;
        }
        if ((int64_t)rx0 == J716F_SCM_V2_EBUSY && busy < 20u) {
            busy++;
            first = fn;
            a6 = 0u;
            if (ops->sleep_ms) ops->sleep_ms(ops->cookie, 30u);
            continue;
        }
        out.a0 = (int64_t)rx0;
        out.a1 = rx1;
        out.resumes = resumes;
        if (ops->now_us) {
            const uint64_t end_us = ops->now_us(ops->cookie);
            out.ms = (end_us >= start_us) ? (uint32_t)((end_us - start_us) / 1000u) : 0u;
        }
        return out;
    }

    out.a0 = J716F_ZAP_ERR_SMC_TIMEOUT;
    out.resumes = resumes;
    if (ops->now_us) {
        const uint64_t end_us = ops->now_us(ops->cookie);
        out.ms = (end_us >= start_us) ? (uint32_t)((end_us - start_us) / 1000u) : 0u;
    }
    return out;
}

int J716fZapAuthenticate(const uint8_t *fw, size_t fw_size,
                         uint64_t pil_phys, uint64_t pil_bytes,
                         uint32_t limit,
                         const J716fScmOps *scm, const J716fZapMemOps *mem,
                         J716fZapReport *report) {
    J716fZapPlan plan;
    static uint8_t meta_scratch[J716F_ZAP_MAX_META];
    uint64_t meta_pa = 0u;
    J716fScmResult r;
    int rc;

    if (!report) return J716F_ZAP_ERR_ARG;
    memset(report, 0, sizeof(*report));

    if (!scm || !scm->smc || !mem || !mem->prepare_metadata || !mem->load_pil_region || !pil_phys || !pil_bytes) {
        report->status = J716F_ZAP_ERR_ARG;
        return J716F_ZAP_ERR_ARG;
    }

    /* Stage 1: Parse ELF32 MDT/MBN image */
    report->stage = 1u;
    rc = J716fZapParseElf(fw, fw_size, pil_bytes, &plan);
    if (rc != J716F_ZAP_OK) {
        report->status = rc;
        return rc;
    }
    report->relocatable = plan.relocatable;
    report->meta_bytes  = plan.metadata_bytes;
    report->span_bytes  = (uint32_t)plan.span_bytes;

    /* Stage 2: Assemble and cache-clean physically contiguous metadata buffer */
    report->stage = 2u;
    rc = J716fZapBuildMetadata(fw, fw_size, &plan, meta_scratch, sizeof(meta_scratch));
    if (rc != J716F_ZAP_OK) {
        report->status = rc;
        return rc;
    }
    if (mem->prepare_metadata(mem->cookie, meta_scratch, plan.metadata_bytes,
                              plan.metadata_aligned, &meta_pa) != 0 || meta_pa == 0u) {
        report->status = J716F_ZAP_ERR_META_ALLOC;
        return J716F_ZAP_ERR_META_ALLOC;
    }

    /* Stage 3: qcom_scm_pas_init_image(GPU_PAS_ID=13, meta_pa) — ARGS(2, VAL, RW) */
    report->stage = 3u;
    r = J716fScmCall(scm,
                     J716fScmFn64(J716F_SCM_SVC_PIL, J716F_SCM_PAS_INIT_IMAGE),
                     J716fScmArgs(2u, J716F_SCM_ARG_VAL, J716F_SCM_ARG_RW, 0u, 0u),
                     J716F_ZAP_GPU_PAS_ID, meta_pa, 0u, 0u);
    report->init_rc      = (int32_t)(r.a0 ? r.a0 : (int64_t)r.a1);
    report->init_ms      = r.ms;
    report->init_resumes = r.resumes;
    if (r.a0 != 0 || r.a1 != 0u) {
        report->status = J716F_ZAP_ERR_INIT_SMC;
        return J716F_ZAP_ERR_INIT_SMC;
    }
    if (limit < 2u) {
        report->status = J716F_ZAP_OK;
        return J716F_ZAP_OK;
    }

    /* Stage 4: qcom_scm_pas_mem_setup(GPU_PAS_ID=13, pil_phys, span_bytes) if relocatable */
    if (plan.relocatable) {
        report->stage = 4u;
        r = J716fScmCall(scm,
                         J716fScmFn64(J716F_SCM_SVC_PIL, J716F_SCM_PAS_MEM_SETUP),
                         J716fScmArgs(3u, J716F_SCM_ARG_VAL, J716F_SCM_ARG_VAL, J716F_SCM_ARG_VAL, 0u),
                         J716F_ZAP_GPU_PAS_ID, pil_phys, plan.span_bytes, 0u);
        report->mem_rc      = (int32_t)(r.a0 ? r.a0 : (int64_t)r.a1);
        report->mem_ms      = r.ms;
        report->mem_resumes = r.resumes;
        if (r.a0 != 0 || r.a1 != 0u) {
            report->status = J716F_ZAP_ERR_MEM_SMC;
            return J716F_ZAP_ERR_MEM_SMC;
        }
    } else {
        report->mem_rc = -99;
    }

    /* Stage 5: Copy PT_LOAD segments into pil_gpu_region WC mapping and UNMAP before TZ takes ownership */
    report->stage = 5u;
    if (mem->load_pil_region(mem->cookie, pil_phys, pil_bytes, fw, fw_size, &plan) != 0) {
        report->status = J716F_ZAP_ERR_SEG_LOAD;
        return J716F_ZAP_ERR_SEG_LOAD;
    }
    if (limit < 3u) {
        report->status = J716F_ZAP_OK;
        return J716F_ZAP_OK;
    }

    /* Stage 6: qcom_scm_pas_auth_and_reset(GPU_PAS_ID=13) — ARGS(1, VAL) */
    report->stage = 6u;
    r = J716fScmCall(scm,
                     J716fScmFn64(J716F_SCM_SVC_PIL, J716F_SCM_PAS_AUTH_AND_RESET),
                     J716fScmArgs(1u, J716F_SCM_ARG_VAL, 0u, 0u, 0u),
                     J716F_ZAP_GPU_PAS_ID, 0u, 0u, 0u);
    report->auth_rc      = (int32_t)(r.a0 ? r.a0 : (int64_t)r.a1);
    report->auth_ms      = r.ms;
    report->auth_resumes = r.resumes;
    if (r.a0 != 0 || r.a1 != 0u) {
        report->status = J716F_ZAP_ERR_AUTH_SMC;
        return J716F_ZAP_ERR_AUTH_SMC;
    }

    report->stage = 7u;
    report->status = J716F_ZAP_OK;
    return J716F_ZAP_OK;
}

int J716fScmOpenCpAperture(const J716fScmOps *scm, uint32_t context_bank,
                           uint32_t *out_avail, int32_t *out_rc) {
    J716fScmResult avail, r;
    if (out_avail) *out_avail = 0u;
    if (out_rc) *out_rc = -1;
    if (!scm || !scm->smc || context_bank > 63u) return J716F_ZAP_ERR_ARG;

    /* QCOM_SCM_INFO_IS_CALL_AVAIL for SVC_MP (0x0C) / CP_SMMU_APERTURE (0x1B). */
    const uint64_t query_id = (2ull << 24) | ((uint64_t)J716F_SCM_SVC_MP << 8) | J716F_SCM_MP_CP_SMMU_APERTURE;
    avail = J716fScmCall(scm,
                         J716fScmFn64(J716F_SCM_SVC_INFO, J716F_SCM_INFO_IS_CALL_AVAIL),
                         J716fScmArgs(1u, J716F_SCM_ARG_VAL, 0u, 0u, 0u),
                         query_id, 0u, 0u, 0u);
    if (out_avail) {
        *out_avail = avail.a0 ? (0x80000000u | (uint32_t)avail.a0) : (uint32_t)avail.a1;
    }
    if (avail.a0 != 0 || avail.a1 == 0u) {
        if (out_rc) *out_rc = -1;
        return J716F_ZAP_ERR_INIT_SMC;
    }

    /* args[0] = 0xffff0000 | (QCOM_SCM_CP_APERTURE_REG (0) << 8) | context_bank; args[1..3] = 0xffffffff */
    const uint64_t reg_val = 0xFFFF0000ull | ((uint64_t)context_bank & 0xFFull);
    r = J716fScmCall(scm,
                     J716fScmFn64(J716F_SCM_SVC_MP, J716F_SCM_MP_CP_SMMU_APERTURE),
                     J716fScmArgs(4u, J716F_SCM_ARG_VAL, J716F_SCM_ARG_VAL, J716F_SCM_ARG_VAL, J716F_SCM_ARG_VAL),
                     reg_val, 0xFFFFFFFFull, 0xFFFFFFFFull, 0xFFFFFFFFull);
    if (out_rc) *out_rc = (int32_t)r.a0;
    return r.a0 == 0 ? J716F_ZAP_OK : J716F_ZAP_ERR_AUTH_SMC;
}
