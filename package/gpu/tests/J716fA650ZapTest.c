/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Host ASan/UBSan unit test for TB-J716F Adreno 650 Phase 3b:
 *   - J716fZapParseElf / J716fZapBuildMetadata (matching real TB-J716F a650_zap.elf 3-phdr geometry)
 *   - J716fZapAuthenticate (PAS_INIT_IMAGE, PAS_MEM_SETUP, PIL WC copy + unmap ordering, PAS_AUTH_AND_RESET)
 *   - QCOM_SCM_INTERRUPTED multi-resume with x6 token + QCOM_SCM_V2_EBUSY retry
 *   - J716fScmOpenCpAperture (IS_CALL_AVAIL + CP_SMMU_APERTURE)
 *   - Negative / bounds / overflow rejection checks. */

#include "J716fA650Zap.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void wr16le(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

static void wr32le(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static void write_phdr(uint8_t *fw, uint32_t phoff, unsigned idx,
                       uint32_t type, uint32_t off, uint32_t va, uint32_t pa,
                       uint32_t filesz, uint32_t memsz, uint32_t flags, uint32_t align) {
    uint8_t *p = fw + phoff + idx * 32u;
    wr32le(p + 0, type);
    wr32le(p + 4, off);
    wr32le(p + 8, va);
    wr32le(p + 12, pa);
    wr32le(p + 16, filesz);
    wr32le(p + 20, memsz);
    wr32le(p + 24, flags);
    wr32le(p + 28, align);
}

/* Build a synthetic unified ELF32 matching TB-J716F stock a650_zap.elf exact layout:
 *   size = 0x368c (13964 B), 3 phdrs:
 *   ph[0]: type=0 off=0 filesz=0x94 memsz=0 flags=0x07000000
 *   ph[1]: type=0 off=0x1000 pa=0x6000 filesz=0x1a38 memsz=0x2000 flags=0x02200000 (HASH)
 *   ph[2]: type=1 off=0x3000 pa=0x5000 filesz=0x68c memsz=0x68c flags=0x08000007 (LOAD + RELOC) */
static void build_synthetic_zap(uint8_t *fw, size_t size, uint32_t load_flags, uint32_t load_memsz) {
    memset(fw, 0, size);
    fw[0] = 0x7Fu;
    fw[1] = 'E';
    fw[2] = 'L';
    fw[3] = 'F';
    fw[4] = 1u; /* ELFCLASS32 */
    fw[5] = 1u; /* ELFDATA2LSB */
    fw[6] = 1u; /* EV_CURRENT */
    wr32le(fw + 28u, 52u); /* e_phoff */
    wr16le(fw + 40u, 52u); /* e_ehsize */
    wr16le(fw + 42u, 32u); /* e_phentsize */
    wr16le(fw + 44u, 3u);  /* e_phnum */

    write_phdr(fw, 52u, 0u, 0u, 0x0000u, 0x0000u, 0x0000u, 0x0094u, 0x0000u, 0x07000000u, 0u);
    write_phdr(fw, 52u, 1u, 0u, 0x1000u, 0x6000u, 0x6000u, 0x1A38u, 0x2000u, 0x02200000u, 0x1000u);
    write_phdr(fw, 52u, 2u, 1u, 0x3000u, 0x5000u, 0x5000u, 0x068Cu, load_memsz, load_flags, 0x100000u);

    for (size_t i = 0; i < 0x1A38u; ++i) fw[0x1000u + i] = (uint8_t)(0xA0u ^ (i & 0xFFu));
    for (size_t i = 0; i < 0x068Cu; ++i) fw[0x3000u + i] = (uint8_t)(0x55u ^ (i & 0xFFu));
}

typedef struct {
    uint64_t time_us;
    uint32_t sleep_calls;
    uint32_t smc_calls;
    int init_called;
    int mem_called;
    int pil_loaded;
    int pil_unmapped;
    int auth_called;
    int inject_interrupts;
    int inject_ebusy;
    int fail_auth;
    int aperture_avail;
    int aperture_fail;
    uint8_t meta_copy[J716F_ZAP_MAX_META];
    size_t meta_len;
    uint8_t pil_buf[0x2000];
} MockTz;

static uint64_t mock_now_us(void *cookie) {
    MockTz *tz = (MockTz *)cookie;
    tz->time_us += 2500u;
    return tz->time_us;
}

static void mock_sleep_ms(void *cookie, uint32_t ms) {
    MockTz *tz = (MockTz *)cookie;
    tz->sleep_calls++;
    tz->time_us += (uint64_t)ms * 1000u;
}

static void mock_smc(void *cookie,
                     uint64_t x0, uint64_t x1, uint64_t x2, uint64_t x3,
                     uint64_t x4, uint64_t x5, uint64_t x6,
                     uint64_t *out_x0, uint64_t *out_x1, uint64_t *out_x6) {
    MockTz *tz = (MockTz *)cookie;
    tz->smc_calls++;
    *out_x0 = 0u;
    *out_x1 = 0u;
    *out_x6 = 0u;

    /* Resume path for QCOM_SCM_INTERRUPTED */
    if (x0 == 1u) {
        assert(x6 == 0xCAFEBABE12340000ULL);
        if (tz->inject_interrupts > 0) {
            tz->inject_interrupts--;
            *out_x0 = 1u;
            *out_x6 = 0xCAFEBABE12340000ULL;
            return;
        }
        *out_x0 = 0u;
        *out_x1 = 0u;
        return;
    }

    if (x0 == J716fScmFn64(J716F_SCM_SVC_PIL, J716F_SCM_PAS_INIT_IMAGE)) {
        assert(x1 == (2u | (2u << 6)));
        assert(x2 == J716F_ZAP_GPU_PAS_ID);
        assert(x3 == 0x123450000ULL);
        if (tz->inject_ebusy > 0) {
            tz->inject_ebusy--;
            *out_x0 = (uint64_t)(int64_t)J716F_SCM_V2_EBUSY;
            return;
        }
        tz->init_called++;
        if (tz->inject_interrupts > 0) {
            tz->inject_interrupts--;
            *out_x0 = 1u;
            *out_x6 = 0xCAFEBABE12340000ULL;
            return;
        }
        return;
    }

    if (x0 == J716fScmFn64(J716F_SCM_SVC_PIL, J716F_SCM_PAS_MEM_SETUP)) {
        assert(tz->init_called == 1);
        assert(x1 == 3u);
        assert(x2 == J716F_ZAP_GPU_PAS_ID);
        assert(x3 == J716F_ZAP_PIL_PHYS);
        assert(x4 == 0x1000u);
        tz->mem_called++;
        return;
    }

    if (x0 == J716fScmFn64(J716F_SCM_SVC_PIL, J716F_SCM_PAS_AUTH_AND_RESET)) {
        /* Crucial security invariant: PIL region MUST be copied and CPU-unmapped BEFORE TZ auth! */
        assert(tz->pil_loaded == 1);
        assert(tz->pil_unmapped == 1);
        assert(x1 == 1u);
        assert(x2 == J716F_ZAP_GPU_PAS_ID);
        tz->auth_called++;
        if (tz->fail_auth) {
            *out_x0 = (uint64_t)(int64_t)(-2);
        }
        return;
    }

    if (x0 == J716fScmFn64(J716F_SCM_SVC_INFO, J716F_SCM_INFO_IS_CALL_AVAIL)) {
        assert(x1 == 1u);
        assert(x2 == ((2ull << 24) | (0x0Cu << 8) | 0x1Bu));
        *out_x0 = 0u;
        *out_x1 = tz->aperture_avail ? 1u : 0u;
        return;
    }

    if (x0 == J716fScmFn64(J716F_SCM_SVC_MP, J716F_SCM_MP_CP_SMMU_APERTURE)) {
        assert(x1 == 4u);
        assert(x2 == 0xFFFF0000ULL);
        assert(x3 == 0xFFFFFFFFULL);
        assert(x4 == 0xFFFFFFFFULL);
        assert(x5 == 0xFFFFFFFFULL);
        *out_x0 = tz->aperture_fail ? (uint64_t)(int64_t)(-5) : 0u;
        return;
    }

    assert(0 && "Unexpected SMC function ID");
}

static int mock_prepare_meta(void *cookie, const uint8_t *meta, size_t meta_bytes,
                             size_t aligned_bytes, uint64_t *out_pa) {
    MockTz *tz = (MockTz *)cookie;
    assert(meta_bytes == 0x94u + 0x1A38u);
    assert(aligned_bytes == 0x2000u);
    memcpy(tz->meta_copy, meta, aligned_bytes);
    tz->meta_len = meta_bytes;
    *out_pa = 0x123450000ULL;
    return 0;
}

static int mock_load_pil(void *cookie, uint64_t pil_phys, uint64_t pil_bytes,
                         const uint8_t *fw, size_t fw_size, const J716fZapPlan *plan) {
    MockTz *tz = (MockTz *)cookie;
    assert(pil_phys == J716F_ZAP_PIL_PHYS);
    assert(pil_bytes == J716F_ZAP_PIL_BYTES);
    memset(tz->pil_buf, 0xCC, sizeof(tz->pil_buf));
    const uint64_t base = plan->relocatable ? plan->min_paddr : pil_phys;
    for (uint32_t i = 0; i < plan->seg_count; ++i) {
        const J716fZapSeg *s = &plan->segs[i];
        const uint64_t at = (uint64_t)s->p_paddr - base;
        assert(at + s->p_memsz <= pil_bytes);
        assert((uint64_t)s->p_offset + s->p_filesz <= fw_size);
        memcpy(tz->pil_buf + at, fw + s->p_offset, s->p_filesz);
        if (s->p_memsz > s->p_filesz) {
            memset(tz->pil_buf + at + s->p_filesz, 0, s->p_memsz - s->p_filesz);
        }
    }
    tz->pil_loaded = 1;
    tz->pil_unmapped = 1;
    return 0;
}

static void test_happy_path_with_interrupts_and_ebusy(void) {
    uint8_t fw[0x368Cu];
    build_synthetic_zap(fw, sizeof(fw), 0x08000007u, 0x068Cu);

    MockTz tz;
    memset(&tz, 0, sizeof(tz));
    tz.inject_interrupts = 3;
    tz.inject_ebusy = 2;
    tz.aperture_avail = 1;

    const J716fScmOps scm = {&tz, mock_smc, mock_now_us, mock_sleep_ms};
    const J716fZapMemOps mem = {&tz, mock_prepare_meta, mock_load_pil};
    J716fZapReport rep;

    int rc = J716fZapAuthenticate(fw, sizeof(fw), J716F_ZAP_PIL_PHYS, J716F_ZAP_PIL_BYTES,
                                  4u, &scm, &mem, &rep);
    assert(rc == J716F_ZAP_OK);
    assert(rep.status == J716F_ZAP_OK);
    assert(rep.stage == 7u);
    assert(rep.relocatable == 1u);
    assert(rep.meta_bytes == 0x1ACCu);
    assert(rep.span_bytes == 0x1000u);
    assert(rep.init_rc == 0 && rep.init_resumes == 3u);
    assert(rep.mem_rc == 0);
    assert(rep.auth_rc == 0);
    assert(tz.init_called == 1 && tz.mem_called == 1 && tz.auth_called == 1);
    assert(tz.sleep_calls == 2u);

    /* Verify metadata concatenation: ehdr (0..0x94) || hash (0x1000..0x2a38) */
    assert(memcmp(tz.meta_copy, fw, 0x94u) == 0);
    assert(memcmp(tz.meta_copy + 0x94u, fw + 0x1000u, 0x1A38u) == 0);
    /* Verify PT_LOAD payload copied to PIL offset 0 */
    assert(memcmp(tz.pil_buf, fw + 0x3000u, 0x068Cu) == 0);

    /* Verify CP SMMU aperture open */
    uint32_t avail = 0u;
    int32_t ap_rc = -1;
    rc = J716fScmOpenCpAperture(&scm, 0u, &avail, &ap_rc);
    assert(rc == J716F_ZAP_OK && avail == 1u && ap_rc == 0);
}

static void test_negative_and_limits(void) {
    uint8_t fw[0x368Cu];
    J716fZapPlan plan;

    /* Bad ELF magic */
    build_synthetic_zap(fw, sizeof(fw), 0x08000007u, 0x068Cu);
    fw[0] = 0x00u;
    assert(J716fZapParseElf(fw, sizeof(fw), J716F_ZAP_PIL_BYTES, &plan) == J716F_ZAP_ERR_ELF_MAGIC);

    /* Overflow PIL region (> 0x2000) */
    build_synthetic_zap(fw, sizeof(fw), 0x08000007u, 0x3000u);
    assert(J716fZapParseElf(fw, sizeof(fw), J716F_ZAP_PIL_BYTES, &plan) == J716F_ZAP_ERR_RANGE);

    /* Auth failure propagation + aperture unavailable */
    build_synthetic_zap(fw, sizeof(fw), 0x08000007u, 0x068Cu);
    MockTz tz;
    memset(&tz, 0, sizeof(tz));
    tz.fail_auth = 1;
    tz.aperture_avail = 0;
    const J716fScmOps scm = {&tz, mock_smc, mock_now_us, mock_sleep_ms};
    const J716fZapMemOps mem = {&tz, mock_prepare_meta, mock_load_pil};
    J716fZapReport rep;
    int rc = J716fZapAuthenticate(fw, sizeof(fw), J716F_ZAP_PIL_PHYS, J716F_ZAP_PIL_BYTES,
                                  4u, &scm, &mem, &rep);
    assert(rc == J716F_ZAP_ERR_AUTH_SMC && rep.auth_rc == -2 && rep.stage == 6u);

    uint32_t avail = 99u;
    int32_t ap_rc = 0;
    rc = J716fScmOpenCpAperture(&scm, 0u, &avail, &ap_rc);
    assert(rc == J716F_ZAP_ERR_INIT_SMC && avail == 0u && ap_rc == -1);
}

int main(void) {
    test_happy_path_with_interrupts_and_ebusy();
    test_negative_and_limits();
    printf("PASS: J716fA650ZapTest (ELF32 MDT, SCM PAS 13, INTERRUPTED/EBUSY, PIL unmap order, CP aperture)\n");
    return 0;
}
