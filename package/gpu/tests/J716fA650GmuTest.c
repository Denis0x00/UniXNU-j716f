/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Host ASan/UBSan unit test for package/gpu/J716fA650Gmu.c against upstream kits/AdrenoFirmware. */
#include <assert.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../J716fA650Gmu.h"

#if !defined(__aarch64__)
/* Portable C11 host mirror of kits/AdrenoFirmware/AdrenoHfi.c for x86_64 CI runs. */
#define HFI_WORDS 1024u

static AdrenoHfiStatus host_hfi_valid(AdrenoHfi *h, unsigned q, volatile uint32_t **hdr,
                                      volatile uint32_t **data, uint32_t *rd, uint32_t *wr) {
    if (!h || !h->shared || !h->notify || !h->iova || (h->iova & 4095u) ||
        ((uintptr_t)h->shared & 4095u)) return AdrenoHfiInvalid;
    if (h->poisoned) return AdrenoHfiPoisoned;
    volatile uint32_t *m = h->shared;
    atomic_thread_fence(memory_order_acquire);
    if (m[0] != 0u || m[1] != 168u || m[2] != 6u || m[3] != 12u || m[4] != 3u || m[5] != 3u) {
        h->poisoned = 1u;
        return AdrenoHfiCorrupt;
    }
    volatile uint32_t *v = m + 6u + 12u * q;
    if (v[0] != 1u || v[1] != h->iova + (q + 1u) * 4096u || v[2] != (0xa00u | q) ||
        v[3] != HFI_WORDS || v[4] != 0u) {
        h->poisoned = 1u;
        return AdrenoHfiCorrupt;
    }
    *rd = v[10];
    *wr = v[11];
    if (*rd >= HFI_WORDS || *wr >= HFI_WORDS || (*rd & 3u) || (*wr & 3u)) {
        h->poisoned = 1u;
        return AdrenoHfiCorrupt;
    }
    *hdr = v;
    *data = m + (q + 1u) * HFI_WORDS;
    return AdrenoHfiSuccess;
}

AdrenoHfiStatus AdrenoHfiInitializeStopped(AdrenoHfi *h, volatile uint32_t *shared,
                                           size_t bytes, uint64_t iova, void *cookie, int (*notify)(void *)) {
    if (!h || !shared || !notify || bytes != 16384u || ((uintptr_t)shared & 4095u) ||
        !iova || (iova & 4095u) || iova > UINT32_MAX - 16383u) return AdrenoHfiInvalid;
    for (unsigned i = 0; i < 4096u; ++i) shared[i] = 0u;
    shared[1] = 168u; shared[2] = 6u; shared[3] = 12u; shared[4] = 3u; shared[5] = 3u;
    for (unsigned q = 0; q < 3u; ++q) {
        volatile uint32_t *v = shared + 6u + 12u * q;
        v[0] = 1u; v[1] = (uint32_t)iova + (q + 1u) * 4096u; v[2] = 0xa00u | q; v[3] = HFI_WORDS;
        v[6] = 1u; v[7] = 1u; v[8] = 1u;
    }
    atomic_thread_fence(memory_order_release);
    *h = (AdrenoHfi){shared, (uint32_t)iova, 0u, 0u, 0u, cookie, notify, 0u, 0u, 0u, 0u};
    return AdrenoHfiSuccess;
}

AdrenoHfiStatus AdrenoHfiSend(AdrenoHfi *h, unsigned id, const uint32_t *payload,
                              size_t words, uint32_t *wire_header) {
    if (!wire_header || id > 255u || words > 254u || (words && !payload)) return AdrenoHfiInvalid;
    volatile uint32_t *v = NULL, *data = NULL;
    uint32_t rd = 0, wr = 0;
    AdrenoHfiStatus st = host_hfi_valid(h, 0u, &v, &data, &rd, &wr);
    if (st != AdrenoHfiSuccess) return st;
    if (h->pending_header) return AdrenoHfiBusy;
    unsigned count = (unsigned)words + 1u;
    unsigned padded = (count + 3u) & ~3u;
    if (((rd - wr - 1u) & (HFI_WORDS - 1u)) < padded) return AdrenoHfiNoSpace;
    uint32_t seq = (h->sequence + 1u) % 4095u;
    uint32_t hdr = (seq << 20) | (count << 8) | id;
    data[wr] = hdr;
    for (unsigned i = 0; i < words; ++i) data[(wr + 1u + i) & (HFI_WORDS - 1u)] = payload[i];
    for (unsigned i = count; i < padded; ++i) data[(wr + i) & (HFI_WORDS - 1u)] = 0xfafafafau;
    atomic_thread_fence(memory_order_release);
    v[11] = (wr + padded) & (HFI_WORDS - 1u);
    atomic_thread_fence(memory_order_seq_cst);
    h->pending_header = hdr;
    h->sequence = seq;
    *wire_header = hdr;
    if (h->notify(h->cookie)) { h->poisoned = 1u; return AdrenoHfiNotifyError; }
    return AdrenoHfiSuccess;
}

AdrenoHfiStatus AdrenoHfiPoll(AdrenoHfi *h, uint32_t *payload, size_t cap,
                              size_t *words, uint32_t *error) {
    if (!payload || cap < 16u || !words || !error) return AdrenoHfiInvalid;
    *words = 0;
    *error = 0;
    volatile uint32_t *v = NULL, *data = NULL;
    uint32_t rd = 0, wr = 0;
    AdrenoHfiStatus st = host_hfi_valid(h, 1u, &v, &data, &rd, &wr);
    if (st != AdrenoHfiSuccess) return st;
    if (!h->pending_header) return AdrenoHfiInvalid;
    if (rd == wr) { v[8] = 1u; return AdrenoHfiEmpty; }
    uint32_t hdr = data[rd];
    unsigned count = (hdr >> 8) & 255u;
    unsigned padded = (count + 3u) & ~3u;
    uint32_t ret_hdr = data[(rd + 1u) & (HFI_WORDS - 1u)];
    uint32_t err_code = data[(rd + 2u) & (HFI_WORDS - 1u)];
    v[10] = (rd + padded) & (HFI_WORDS - 1u);
    if ((hdr & 255u) != 126u || ret_hdr != h->pending_header) {
        h->poisoned = 1u;
        return AdrenoHfiUnexpected;
    }
    h->pending_header = 0u;
    *error = err_code;
    *words = count > 3u ? count - 3u : 0u;
    return err_code ? AdrenoHfiFirmwareError : AdrenoHfiSuccess;
}

AdrenoHfiStatus AdrenoHfiExchange(AdrenoHfi *h, unsigned id, const uint32_t *in, size_t in_words,
                                  unsigned budget, int (*wait_once)(void *), uint32_t *out,
                                  size_t out_cap, size_t *out_words, uint32_t *error) {
    uint32_t hdr = 0;
    AdrenoHfiStatus st = AdrenoHfiSend(h, id, in, in_words, &hdr);
    if (st != AdrenoHfiSuccess) return st;
    for (unsigned i = 0; i < budget; ++i) {
        st = AdrenoHfiPoll(h, out, out_cap, out_words, error);
        if (st != AdrenoHfiEmpty) return st;
        if (wait_once && wait_once(h->cookie)) { h->poisoned = 1u; return AdrenoHfiWaitError; }
    }
    h->poisoned = 1u;
    return AdrenoHfiTimeout;
}
#endif

struct mock_gmu {
    uint32_t core[A650_GMU_CORE_BYTES / 4u];
    uint32_t rscc[A650_GMU_RSCC_BYTES / 4u];
    uint32_t pdc[A650_GMU_PDC_BYTES / 4u];
    uint64_t now_us;
    volatile uint32_t *hfi_shared;
    unsigned hfi_cmds_seen;
    unsigned hfi_ids[16];
    int corrupt_itcm_readback;
    int inject_ao_fault_on_boot;
};

static void mock_hfi_reply(struct mock_gmu *m) {
    volatile uint32_t *q0 = m->hfi_shared + 6u;
    volatile uint32_t *q1 = m->hfi_shared + 18u;
    uint32_t rd = q0[10], wr = q0[11];
    if (rd == wr) return;
    uint32_t hdr = m->hfi_shared[1024u + rd];
    unsigned count = (hdr >> 8) & 255u;
    unsigned padded = (count + 3u) & ~3u;
    q0[10] = (rd + padded) & 1023u;
    if (m->hfi_cmds_seen < 16u) m->hfi_ids[m->hfi_cmds_seen] = hdr & 255u;
    ++m->hfi_cmds_seen;

    uint32_t at = q1[11];
    m->hfi_shared[2048u + at] = (3u << 8) | 126u;
    m->hfi_shared[2048u + ((at + 1u) & 1023u)] = hdr;
    m->hfi_shared[2048u + ((at + 2u) & 1023u)] = 0u;
    m->hfi_shared[2048u + ((at + 3u) & 1023u)] = 0xfafafafau;
    q1[11] = (at + 4u) & 1023u;
}

static int mock_read32(void *cookie, unsigned w, size_t off, uint32_t *out) {
    struct mock_gmu *m = (struct mock_gmu *)cookie;
    uint32_t dw = (uint32_t)(off / 4u);
    if (w == A650GmuCore) {
        *out = m->core[dw];
        if (m->corrupt_itcm_readback && dw == 0x0c00u) *out ^= 0x1u;
        return 0;
    }
    if (w == A650GmuRscc) { *out = m->rscc[dw]; return 0; }
    if (w == A650GmuPdc) { *out = m->pdc[dw]; return 0; }
    return -1;
}

static int mock_write32(void *cookie, unsigned w, size_t off, uint32_t val) {
    struct mock_gmu *m = (struct mock_gmu *)cookie;
    uint32_t dw = (uint32_t)(off / 4u);
    if (w == A650GmuCore) {
        m->core[dw] = val;
        if (dw == 0x9307u) { /* GMU_RSCC_CONTROL_REQ */
            if (val == 1u) m->rscc[0x004u] |= (1u << 16);
            if (val == 2u) { m->core[0x9308u] = 2u; m->rscc[0x101u] = 0u; }
        } else if (dw == 0x5000u && val == 0u) { /* CM3_SYSRESET release */
            if (m->inject_ao_fault_on_boot) {
                m->core[0x9305u] = 0x01u; /* WDOG_BITE */
            } else {
                m->core[0x501cu] = 0xbabefaceu;
            }
        } else if (dw == 0x5186u && val == 1u) { /* HFI_CTRL_INIT */
            m->core[0x5180u] = 1u;
        } else if (dw == 0x5194u) { /* HOST2GMU_INTR_SET */
            if (val & 1u) mock_hfi_reply(m);
            if (val & (1u << 30)) {
                m->core[0x5192u] |= (1u << 31); /* OOB_GPU_SET_ACK */
                m->core[0x50d0u] &= ~0xc0u;     /* GX power/clock on */
            }
        } else if (dw == 0x5191u) { /* GMU2HOST_INTR_CLR */
            m->core[0x5192u] &= ~val;
        } else if (dw == 0x9304u) { /* AO_HOST_INTERRUPT_CLR */
            m->core[0x9305u] &= ~val;
        }
        return 0;
    }
    if (w == A650GmuRscc) { m->rscc[dw] = val; return 0; }
    if (w == A650GmuPdc) { m->pdc[dw] = val; return 0; }
    return -1;
}

static int mock_barrier(void *cookie) { (void)cookie; return 0; }
static int mock_now(void *cookie, uint64_t *us) { *us = ((struct mock_gmu *)cookie)->now_us; return 0; }
static int mock_wait(void *cookie, unsigned us) { ((struct mock_gmu *)cookie)->now_us += us; return 0; }

static void put_le16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put_le32(uint8_t *p, uint32_t v) { put_le16(p, (uint16_t)v); put_le16(p + 2, (uint16_t)(v >> 16)); }

static size_t build_synthetic_gmu_fw(uint8_t *out) {
    size_t pos = 0;
    /* Block 1: ITCM 0x00000000, 8 bytes */
    put_le32(out + pos + 0, 0x00000000u);
    put_le32(out + pos + 4, 8u);
    put_le32(out + pos + 8, 0u);
    put_le32(out + pos + 12, 0u);
    put_le32(out + pos + 16, 0x11223344u);
    put_le32(out + pos + 20, 0x55667788u);
    pos += 24u;
    /* Block 2: ICACHE 0x00004000, 8 bytes */
    put_le32(out + pos + 0, 0x00004000u);
    put_le32(out + pos + 4, 8u);
    put_le32(out + pos + 8, 0u);
    put_le32(out + pos + 12, 0u);
    put_le32(out + pos + 16, 0xCAFEBABEu);
    put_le32(out + pos + 20, 0xDEADBEEFu);
    pos += 24u;
    /* Block 3: DTCM version word at DTCM_BASE + 0x3FE0 (dword 0xFF8), 4 bytes -> 0x20010004 */
    put_le32(out + pos + 0, 0x10004000u + 0x3FE0u);
    put_le32(out + pos + 4, 4u);
    put_le32(out + pos + 8, 0u);
    put_le32(out + pos + 12, 0u);
    put_le32(out + pos + 16, 0x20010004u);
    pos += 20u;
    return pos;
}

static void build_arc_levels(uint8_t *buf, const uint16_t *vals, size_t n) {
    for (size_t i = 0; i < n; ++i) put_le16(buf + 2u * i, vals[i]);
}

static void *alloc_4k(size_t bytes) {
    void *p = aligned_alloc(4096u, bytes);
    assert(p != NULL);
    memset(p, 0, bytes);
    return p;
}

int main(void) {
    /* 1. Verify TB-J716F HFI startup table builder (A650GmuBuildStartup) */
    const uint16_t gfx_lvls[] = {0, 48, 64, 128, 144, 192, 224, 256, 320, 384};
    const uint16_t mx_lvls[]  = {0, 48, 64, 128, 144, 192, 224, 256, 320, 384};
    const uint16_t cx_lvls[]  = {0, 48, 64, 128, 192, 256, 320};
    uint8_t gfx_raw[sizeof(gfx_lvls)], mx_raw[sizeof(mx_lvls)], cx_raw[sizeof(cx_lvls)];
    build_arc_levels(gfx_raw, gfx_lvls, sizeof(gfx_lvls) / 2u);
    build_arc_levels(mx_raw, mx_lvls, sizeof(mx_lvls) / 2u);
    build_arc_levels(cx_raw, cx_lvls, sizeof(cx_lvls) / 2u);

    AdrenoHfiStartupConfig startup_cfg;
    A650GmuDdrVote = 0u;
    assert(A650GmuBuildStartup(gfx_raw, sizeof(gfx_raw), mx_raw, sizeof(mx_raw),
                               cx_raw, sizeof(cx_raw), 0x30000u, 0x30004u, 0x30008u, 0x50074u,
                               &startup_cfg) == AdrenoHfiStartupSuccess);
    assert(startup_cfg.gpu_count == 8u && startup_cfg.gpu[7].hz == 670000000ULL);
    assert(startup_cfg.gmu_count == 2u && startup_cfg.gmu[1].hz == 200000000ULL);
    assert(startup_cfg.bandwidth_count == 1u);

    A650GmuDdrVote = 1u;
    assert(A650GmuBuildStartup(gfx_raw, sizeof(gfx_raw), mx_raw, sizeof(mx_raw),
                               cx_raw, sizeof(cx_raw), 0x30000u, 0x30004u, 0x30008u, 0x50074u,
                               &startup_cfg) == AdrenoHfiStartupSuccess);
    assert(startup_cfg.bandwidth_count == 2u);
    assert(A650GmuBuildStartup(NULL, 0u, mx_raw, sizeof(mx_raw), cx_raw, sizeof(cx_raw),
                               1u, 1u, 1u, 1u, &startup_cfg) == AdrenoHfiStartupInvalid);

    /* 2. Full happy-path A650GmuColdStart + A650GmuPowerGx */
    struct mock_gmu *hw = (struct mock_gmu *)calloc(1u, sizeof(*hw));
    assert(hw != NULL);
    volatile uint32_t *icache = (volatile uint32_t *)alloc_4k(0x10000u);
    volatile uint32_t *dummy  = (volatile uint32_t *)alloc_4k(0x4000u);
    volatile uint32_t *logbuf = (volatile uint32_t *)alloc_4k(0x4000u);
    volatile uint32_t *hfimem = (volatile uint32_t *)alloc_4k(0x4000u);
    hw->hfi_shared = hfimem;
    hw->core[0x5000u] = 1u;   /* GMU held in reset */
    hw->core[0x50d0u] = 0xc0u; /* GX initially off */

    uint8_t fw[128] = {0};
    const size_t fw_len = build_synthetic_gmu_fw(fw);

    A650GmuDevice dev;
    memset(&dev, 0, sizeof(dev));
    dev.io = (A650GmuIo){hw, {A650_GMU_CORE_BYTES, A650_GMU_RSCC_BYTES, A650_GMU_PDC_BYTES},
                         mock_read32, mock_write32, mock_barrier};
    dev.services = (A650GmuServices){hw, mock_now, mock_wait};

    const A650GmuConfig cfg = {
        A650_GMU_CHIP_ID, fw, fw_len,
        {icache, 0x10000u, 0x00004000u},
        {dummy,  0x4000u,  0x60000000u},
        {logbuf, 0x4000u,  0x60010000u},
        {hfimem, 0x4000u,  0x60020000u},
        64u,
    };
    const A650GmuStartupLimits limits = {64u, 100u, 50000u, 200000u};
    A650GmuStartupResult cold;
    assert(A650GmuColdStart(&dev, &cfg, &startup_cfg, &limits, &cold) == A650GmuSuccess);
    assert(cold.load.phase == 8u && cold.load.firmware_ready_observed == 1u && cold.load.hfi_ready_observed == 1u);
    assert(cold.protocol_status == AdrenoHfiStartupSuccess);
    assert(icache[0] == 0xCAFEBABEu && icache[1] == 0xDEADBEEFu);
    assert(hw->hfi_cmds_seen >= 4u);

    /* Power on GX at top 670 MHz level (perf_index = 7) */
    A650GmuGxResult gx;
    assert(A650GmuPowerGx(&dev, 0u, &gx) == A650GmuArgument);
    assert(A650GmuPowerGx(&dev, 8u, &gx) == A650GmuArgument);
    assert(A650GmuPowerGx(&dev, 7u, &gx) == A650GmuSuccess);
    assert(gx.gx_on == 1u && (gx.gx_before & 0xc0u) == 0xc0u && (gx.gx_after & 0xc0u) == 0u);

    /* 3. Safety check: busy GMU (SYSRESET != 1) rejected before any write */
    memset(hw, 0, sizeof(*hw));
    hw->hfi_shared = hfimem;
    hw->core[0x5000u] = 0u; /* already running */
    memset(&dev, 0, sizeof(dev));
    dev.io = (A650GmuIo){hw, {A650_GMU_CORE_BYTES, A650_GMU_RSCC_BYTES, A650_GMU_PDC_BYTES},
                         mock_read32, mock_write32, mock_barrier};
    dev.services = (A650GmuServices){hw, mock_now, mock_wait};
    assert(A650GmuColdStart(&dev, &cfg, &startup_cfg, &limits, &cold) == A650GmuBusy);
    assert(cold.load.write_attempts == 0u);

    /* 4. Safety check: ITCM readback mismatch resets CM3 and poisons device */
    memset(hw, 0, sizeof(*hw));
    hw->hfi_shared = hfimem;
    hw->core[0x5000u] = 1u;
    hw->corrupt_itcm_readback = 1;
    memset(&dev, 0, sizeof(dev));
    dev.io = (A650GmuIo){hw, {A650_GMU_CORE_BYTES, A650_GMU_RSCC_BYTES, A650_GMU_PDC_BYTES},
                         mock_read32, mock_write32, mock_barrier};
    dev.services = (A650GmuServices){hw, mock_now, mock_wait};
    assert(A650GmuColdStart(&dev, &cfg, &startup_cfg, &limits, &cold) == A650GmuStateMismatch);
    assert(dev.poisoned == 1u && cold.load.reset_verified == 1u);
    assert(A650GmuColdStart(&dev, &cfg, &startup_cfg, &limits, &cold) == A650GmuPoisoned);

    /* 5. Safety check: AO fatal watchdog fault during boot aborts and poisons device */
    memset(hw, 0, sizeof(*hw));
    hw->hfi_shared = hfimem;
    hw->core[0x5000u] = 1u;
    hw->inject_ao_fault_on_boot = 1;
    memset(&dev, 0, sizeof(dev));
    dev.io = (A650GmuIo){hw, {A650_GMU_CORE_BYTES, A650_GMU_RSCC_BYTES, A650_GMU_PDC_BYTES},
                         mock_read32, mock_write32, mock_barrier};
    dev.services = (A650GmuServices){hw, mock_now, mock_wait};
    assert(A650GmuColdStart(&dev, &cfg, &startup_cfg, &limits, &cold) == A650GmuFault);
    assert(dev.poisoned == 1u && (cold.load.ao_fault_status & 0x01u) == 0x01u);

    free((void *)icache);
    free((void *)dummy);
    free((void *)logbuf);
    free((void *)hfimem);
    free(hw);
    printf("J716fA650GmuTest: PASS\n");
    return 0;
}
