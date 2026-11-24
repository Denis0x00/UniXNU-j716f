/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "J716fA650Gmu.h"
#include "AdrenoFirmware.h"
#include "AdrenoHfiVotes.h"

/* a6xx_gmu.xml.h, GMU block (dword offsets). */
enum {
    GMU_CM3_ITCM_START = 0x0c00, GMU_CM3_DTCM_START = 0x1c00,
    GMU_ICACHE_CONFIG = 0x4c00, GMU_DCACHE_CONFIG = 0x4c01, GMU_SYS_BUS_CONFIG = 0x4c0f,
    GMU_CM3_SYSRESET = 0x5000, GMU_CM3_BOOT_CONFIG = 0x5001, GMU_CM3_FW_INIT_RESULT = 0x501c, GMU_CM3_CFG = 0x502d,
    GMU_PWR_COL_INTER_FRAME_CTRL = 0x50c0, GMU_SPTPRAC_PWR_CLK_STATUS = 0x50d0, GMU_RPMH_CTRL = 0x50e8,
    GMU_CX_FAL_INTF = 0x50f0, GMU_CX_FALNEXT_INTF = 0x50f1, GMU_PWR_COL_CP_MSG = 0x5100, GMU_PWR_COL_CP_RESP = 0x5101,
    GMU_HFI_CTRL_STATUS = 0x5180, GMU_HFI_SFR_ADDR = 0x5182, GMU_HFI_QTBL_INFO = 0x5184, GMU_HFI_QTBL_ADDR = 0x5185,
    GMU_HFI_CTRL_INIT = 0x5186,
    GMU_GMU2HOST_INTR_CLR = 0x5191, GMU_GMU2HOST_INTR_INFO = 0x5192, GMU_GMU2HOST_INTR_MASK = 0x5193,
    GMU_HOST2GMU_INTR_SET = 0x5194, GMU_GENERAL_7 = 0x51cc,
    GMU_AO_HOST_INTERRUPT_CLR = 0x9304, GMU_AO_HOST_INTERRUPT_STATUS = 0x9305, GMU_AO_HOST_INTERRUPT_MASK = 0x9306,
    GMU_RSCC_CONTROL_REQ = 0x9307, GMU_RSCC_CONTROL_ACK = 0x9308, GMU_AHB_FENCE_RANGE_0 = 0x9311,
};

/* RSCC & PDC block dword offsets. */
enum {
    RSCC_RSC_STATUS0_DRV0 = 0x004, RSCC_PDC_SEQ_START_ADDR = 0x008, RSCC_PDC_MATCH_VALUE_LO = 0x009,
    RSCC_PDC_MATCH_VALUE_HI = 0x00a, RSCC_PDC_SLAVE_ID_DRV0 = 0x00b, RSCC_HIDDEN_TCS_CMD0_ADDR = 0x00d,
    RSCC_HIDDEN_TCS_CMD0_DATA = 0x00e, RSCC_OVERRIDE_START_ADDR = 0x100, RSCC_SEQ_BUSY_DRV0 = 0x101,
    RSCC_SEQ_MEM_0_DRV0 = 0x180,
    PDC_GPU_ENABLE_PDC = 0x1140, PDC_GPU_SEQ_START_ADDR = 0x1148,
};

#define AO_FATAL        0x29u
#define CM3_FATAL       0x00800000u
#define RPMH_CTRL_VOTES 0x00000f11u
#define FENCE_RANGE     ((1u << 31) | (0xau << 18) | 0xa0u)
#define ITCM_BASE       0x00000000u
#define DTCM_BASE       0x10004000u
#define TCM_BYTES       0x4000u
#define ICACHE_IOVA     0x00004000u
#define DUMMY_IOVA      0x60000000u
#define GX_OFF_BITS     0xc0u
#define OOB_GPU_SET_REQ 30u
#define OOB_GPU_SET_ACK 31u

unsigned A650GmuDdrVote = 0u;

static int read32(A650GmuDevice *d, unsigned w, uint32_t dword, uint32_t *v, A650GmuResult *r) {
    ++r->read_attempts;
    return d->io.read32(d->io.cookie, w, (size_t)dword * 4u, v);
}

static int write32(A650GmuDevice *d, unsigned w, uint32_t dword, uint32_t v, A650GmuResult *r) {
    ++r->write_attempts;
    return d->io.write32(d->io.cookie, w, (size_t)dword * 4u, v);
}

static int core_read(A650GmuDevice *d, uint32_t dword, uint32_t *v, A650GmuResult *r) {
    return read32(d, A650GmuCore, dword, v, r);
}

static int core_write(A650GmuDevice *d, uint32_t dword, uint32_t v, A650GmuResult *r) {
    return write32(d, A650GmuCore, dword, v, r);
}

static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int extent_ok(uintptr_t at, size_t bytes) {
    return at != 0 && bytes != 0 && bytes - 1u <= UINTPTR_MAX - at;
}

static int ranges_overlap(uintptr_t a, size_t na, uintptr_t b, size_t nb) {
    return a <= b ? (b - a < na) : (a - b < nb);
}

static A650GmuStatus check_faults(A650GmuDevice *d, A650GmuResult *r) {
    if (core_read(d, GMU_AO_HOST_INTERRUPT_STATUS, &r->ao_fault_status, r) ||
        core_read(d, GMU_GMU2HOST_INTR_INFO, &r->cm3_fault_status, r)) {
        return A650GmuIoError;
    }
    return (r->ao_fault_status & AO_FATAL) || (r->cm3_fault_status & CM3_FATAL) ? A650GmuFault : A650GmuSuccess;
}

static A650GmuStatus poll_reg(A650GmuDevice *d, unsigned w, uint32_t dword, uint32_t mask, uint32_t expected,
                              unsigned budget, int faults, uint32_t *observed, A650GmuResult *r) {
    uint64_t start = 0, last = 0, now = 0;
    if (d->services.now_us(d->services.cookie, &start)) return A650GmuClockError;
    last = start;
    for (unsigned i = 0; i < budget; ++i) {
        if (d->services.now_us(d->services.cookie, &now) || now < last) return A650GmuClockError;
        if (now - start >= 10000u) return A650GmuTimeout;
        last = now;
        ++r->poll_reads;
        if (faults) {
            A650GmuStatus f = check_faults(d, r);
            if (f != A650GmuSuccess) return f;
        }
        if (read32(d, w, dword, observed, r)) return A650GmuIoError;
        if ((*observed & mask) == expected) {
            return d->io.barrier(d->io.cookie) ? A650GmuIoError : A650GmuSuccess;
        }
        if (i + 1u < budget && d->services.wait_us(d->services.cookie, 100u)) return A650GmuClockError;
    }
    return A650GmuTimeout;
}

static void reset_after_failure(A650GmuDevice *d, A650GmuResult *r) {
    uint32_t actual = 0;
    r->reset_verified = 0;
    d->poisoned = 1;
    d->hfi.poisoned = 1;
    if (!core_write(d, GMU_CM3_SYSRESET, 1u, r) && !d->io.barrier(d->io.cookie) &&
        !core_read(d, GMU_CM3_SYSRESET, &actual, r) && actual == 1u) {
        r->reset_verified = 1;
    }
}

static int notify_gmu(void *cookie) {
    A650GmuDevice *d = (A650GmuDevice *)cookie;
    return d->io.barrier(d->io.cookie) ||
           d->io.write32(d->io.cookie, A650GmuCore, (size_t)GMU_HOST2GMU_INTR_SET * 4u, 1u) ||
           d->io.barrier(d->io.cookie);
}

static A650GmuStatus validate_cfg(A650GmuDevice *d, const A650GmuConfig *c) {
    static const size_t windows[A650GmuWindows] = {A650_GMU_CORE_BYTES, A650_GMU_RSCC_BYTES, A650_GMU_PDC_BYTES};
    if (!d || !c || !d->io.read32 || !d->io.write32 || !d->io.barrier ||
        !d->services.now_us || !d->services.wait_us ||
        c->chip_id != A650_GMU_CHIP_ID || !c->firmware || c->firmware_bytes < 16u || c->firmware_bytes > 0x100000u ||
        !extent_ok((uintptr_t)c->firmware, c->firmware_bytes) || !c->poll_budget || c->poll_budget > 1024u) {
        return A650GmuArgument;
    }
    if (d->poisoned) return A650GmuPoisoned;
    if (d->boot_attempted) return A650GmuBusy;
    for (unsigned i = 0; i < A650GmuWindows; ++i) {
        if (d->io.bytes[i] != windows[i]) return A650GmuArgument;
    }
    const A650GmuMemory *mem[4] = {&c->icache, &c->dummy, &c->log, &c->hfi};
    for (unsigned i = 0; i < 4u; ++i) {
        const A650GmuMemory *m = mem[i];
        if (!m->bytes || (m->bytes & 4095u) || !extent_ok((uintptr_t)m->cpu, m->bytes) ||
            ((uintptr_t)m->cpu & 4095u) || !m->iova || (m->iova & 4095u) ||
            m->iova > UINT32_MAX - (m->bytes - 1u) ||
            ranges_overlap((uintptr_t)m->cpu, m->bytes, (uintptr_t)c->firmware, c->firmware_bytes)) {
            return A650GmuArgument;
        }
        for (unsigned j = 0; j < i; ++j) {
            if (ranges_overlap((uintptr_t)m->cpu, m->bytes, (uintptr_t)mem[j]->cpu, mem[j]->bytes) ||
                ranges_overlap((uintptr_t)m->iova, m->bytes, (uintptr_t)mem[j]->iova, mem[j]->bytes)) {
                return A650GmuArgument;
            }
        }
    }
    if (c->icache.iova != ICACHE_IOVA || c->icache.bytes < 0x10000u ||
        c->dummy.iova != DUMMY_IOVA || c->log.bytes != 0x4000u || c->hfi.bytes != 0x4000u) {
        return A650GmuArgument;
    }
    return A650GmuSuccess;
}

static volatile uint32_t *block_memory(const A650GmuConfig *c, const AdrenoFirmwareBlock *b) {
    if (b->region_identifier == 2u) return c->icache.cpu + (b->address - ICACHE_IOVA) / 4u;
    if (b->region_identifier == 4u) return c->dummy.cpu + (b->address - DUMMY_IOVA) / 4u;
    return 0;
}

static uint32_t block_register(const AdrenoFirmwareBlock *b) {
    return b->region_identifier == 1u
        ? GMU_CM3_ITCM_START + (b->address - ITCM_BASE) / 4u
        : GMU_CM3_DTCM_START + (b->address - DTCM_BASE) / 4u;
}

A650GmuStatus A650GmuLoadAndStartHfi(A650GmuDevice *d, const A650GmuConfig *c, A650GmuResult *r) {
    if (!r) return A650GmuArgument;
    *r = (A650GmuResult){0};
    r->resources_must_retain = 1u;
    A650GmuStatus status = validate_cfg(d, c);
    if (status != A650GmuSuccess) return status;

    const AdrenoFirmwareRegion regions[] = {
        {ITCM_BASE, TCM_BYTES, 1u},
        {ICACHE_IOVA, c->icache.bytes, 2u},
        {DTCM_BASE, TCM_BYTES, 3u},
        {DUMMY_IOVA, c->dummy.bytes, 4u},
    };
    AdrenoFirmwareBlock blocks[48];
    size_t count = 0;
    if (AdrenoFirmwarePlanGmu(c->firmware, c->firmware_bytes, regions, 4u, blocks, 48u, &count) != AdrenoFirmwareSuccess) {
        return A650GmuFirmware;
    }
    r->blocks = (uint32_t)count;

    /* Phase 1 (read-only): GMU held in reset, RSC sequencer idle, HFI not ready. */
    r->phase = 1u;
    uint32_t value = 0;
    if (core_read(d, GMU_CM3_SYSRESET, &value, r)) return A650GmuIoError;
    if (value != 1u) return A650GmuBusy;
    r->reset_verified = 1u;
    if (read32(d, A650GmuRscc, RSCC_SEQ_BUSY_DRV0, &value, r)) return A650GmuIoError;
    if (value) return A650GmuBusy;
    if (core_read(d, GMU_HFI_CTRL_STATUS, &value, r)) return A650GmuIoError;
    if (value & 1u) return A650GmuStateMismatch;

    /* Phase 2: IRQ clear/mask, RSC microcode + PDC enable, RPMh stop. */
    r->phase = 2u;
    d->boot_attempted = 1u;
    if (core_write(d, GMU_AO_HOST_INTERRUPT_CLR, ~0u, r) ||
        core_write(d, GMU_AO_HOST_INTERRUPT_MASK, ~AO_FATAL, r) ||
        core_write(d, GMU_GMU2HOST_INTR_CLR, ~0u, r) ||
        d->io.barrier(d->io.cookie)) {
        status = A650GmuIoError;
        goto failed;
    }
    status = check_faults(d, r);
    if (status != A650GmuSuccess) goto failed;

    static const struct { unsigned window; uint32_t dword, value; } kRsc[] = {
        {A650GmuRscc, RSCC_RSC_STATUS0_DRV0, 1u << 24},
        {A650GmuRscc, RSCC_PDC_SLAVE_ID_DRV0, 1u},
        {A650GmuRscc, RSCC_HIDDEN_TCS_CMD0_DATA, 0u}, {A650GmuRscc, RSCC_HIDDEN_TCS_CMD0_ADDR, 0u},
        {A650GmuRscc, RSCC_HIDDEN_TCS_CMD0_DATA + 2u, 0u}, {A650GmuRscc, RSCC_HIDDEN_TCS_CMD0_ADDR + 2u, 0u},
        {A650GmuRscc, RSCC_HIDDEN_TCS_CMD0_DATA + 4u, 0x80000000u}, {A650GmuRscc, RSCC_HIDDEN_TCS_CMD0_ADDR + 4u, 0u},
        {A650GmuRscc, RSCC_OVERRIDE_START_ADDR, 0u},
        {A650GmuRscc, RSCC_PDC_SEQ_START_ADDR, 0x4520u},
        {A650GmuRscc, RSCC_PDC_MATCH_VALUE_LO, 0x4510u}, {A650GmuRscc, RSCC_PDC_MATCH_VALUE_HI, 0x4514u},
        {A650GmuRscc, RSCC_SEQ_MEM_0_DRV0, 0xeaaae5a0u},
        {A650GmuRscc, RSCC_SEQ_MEM_0_DRV0 + 1u, 0xe1a1ebabu},
        {A650GmuRscc, RSCC_SEQ_MEM_0_DRV0 + 2u, 0xa2e0a581u},
        {A650GmuRscc, RSCC_SEQ_MEM_0_DRV0 + 3u, 0xecac82e2u},
        {A650GmuRscc, RSCC_SEQ_MEM_0_DRV0 + 4u, 0x0020edadu},
        {A650GmuPdc, PDC_GPU_SEQ_START_ADDR, 0u},
        {A650GmuPdc, PDC_GPU_ENABLE_PDC, 0x80000001u},
    };
    for (unsigned i = 0; i < sizeof(kRsc) / sizeof(kRsc[0]); ++i) {
        if (write32(d, kRsc[i].window, kRsc[i].dword, kRsc[i].value, r)) {
            status = A650GmuIoError;
            goto failed;
        }
    }
    if (d->io.barrier(d->io.cookie)) { status = A650GmuIoError; goto failed; }
    if (core_write(d, GMU_RSCC_CONTROL_REQ, 1u, r)) { status = A650GmuIoError; goto failed; }
    {
        A650GmuStatus stop = poll_reg(d, A650GmuRscc, RSCC_RSC_STATUS0_DRV0, 1u << 16, 1u << 16,
                                      c->poll_budget, 0, &value, r);
        if (stop == A650GmuIoError || stop == A650GmuClockError) { status = stop; goto failed; }
        r->rsc_stop_status = (uint32_t)stop;
    }
    if (core_write(d, GMU_RSCC_CONTROL_REQ, 0u, r)) { status = A650GmuIoError; goto failed; }

    /* Phase 3: FAL interfaces, retention, RSC wake-up. */
    r->phase = 3u;
    if (core_write(d, GMU_CX_FALNEXT_INTF, 1u, r) ||
        core_write(d, GMU_CX_FAL_INTF, 1u, r) ||
        core_write(d, GMU_GENERAL_7, 1u, r) ||
        core_write(d, GMU_RSCC_CONTROL_REQ, 1u << 1, r) ||
        d->io.barrier(d->io.cookie)) {
        status = A650GmuIoError;
        goto failed;
    }
    status = poll_reg(d, A650GmuCore, GMU_RSCC_CONTROL_ACK, 1u << 1, 1u << 1, c->poll_budget, 1, &r->rscc_ack, r);
    if (status != A650GmuSuccess) goto failed;
    status = poll_reg(d, A650GmuRscc, RSCC_SEQ_BUSY_DRV0, ~0u, 0u, c->poll_budget, 1, &value, r);
    if (status != A650GmuSuccess) goto failed;
    if (core_write(d, GMU_RSCC_CONTROL_REQ, 0u, r)) { status = A650GmuIoError; goto failed; }

    /* Phase 4: zero GMU memory, init HFI queues, load & readback-verify every firmware block. */
    r->phase = 4u;
    {
        const A650GmuMemory *zero[3] = {&c->icache, &c->dummy, &c->log};
        r->memory_changed = 1u;
        for (unsigned m = 0; m < 3u; ++m) {
            for (size_t i = 0; i < zero[m]->bytes / 4u; ++i) zero[m]->cpu[i] = 0u;
        }
    }
    if (AdrenoHfiInitializeStopped(&d->hfi, c->hfi.cpu, c->hfi.bytes, c->hfi.iova, d, notify_gmu) != AdrenoHfiSuccess) {
        status = A650GmuArgument;
        goto failed;
    }
    for (size_t i = 0; i < count; ++i) {
        const AdrenoFirmwareBlock *b = blocks + i;
        if (!b->bytes) continue;
        volatile uint32_t *memory = block_memory(c, b);
        for (size_t at = 0; at < b->bytes; at += 4u) {
            const uint32_t word = le32(c->firmware + b->payload_offset + at);
            if (memory) memory[at / 4u] = word;
            else if (core_write(d, block_register(b) + (uint32_t)(at / 4u), word, r)) {
                status = A650GmuIoError;
                goto failed;
            }
        }
    }
    if (d->io.barrier(d->io.cookie)) { status = A650GmuIoError; goto failed; }
    for (size_t i = 0; i < count; ++i) {
        const AdrenoFirmwareBlock *b = blocks + i;
        if (!b->bytes) continue;
        volatile uint32_t *memory = block_memory(c, b);
        for (size_t at = 0; at < b->bytes; at += 4u) {
            if (memory) value = memory[at / 4u];
            else if (core_read(d, block_register(b) + (uint32_t)(at / 4u), &value, r)) {
                status = A650GmuIoError;
                goto failed;
            }
            if (value != le32(c->firmware + b->payload_offset + at)) {
                status = A650GmuStateMismatch;
                goto failed;
            }
        }
    }

    /* Phase 5: boot registers & power config (read back). */
    r->phase = 5u;
    {
        const uint32_t chipid = (c->chip_id & 0xffff0000u) | ((c->chip_id << 4) & 0xf000u) | ((c->chip_id << 8) & 0x0f00u);
        const uint32_t regs[] = {
            GMU_CM3_FW_INIT_RESULT, GMU_CM3_BOOT_CONFIG, GMU_HFI_QTBL_ADDR, GMU_HFI_QTBL_INFO,
            GMU_AHB_FENCE_RANGE_0, GMU_CM3_CFG, GMU_HFI_SFR_ADDR, GMU_PWR_COL_CP_MSG,
            GMU_SYS_BUS_CONFIG, GMU_ICACHE_CONFIG, GMU_DCACHE_CONFIG, GMU_PWR_COL_INTER_FRAME_CTRL,
        };
        const uint32_t values[] = {
            0u, 2u, (uint32_t)c->hfi.iova, 1u,
            FENCE_RANGE, 0x4052u, chipid, (uint32_t)c->log.iova | (uint32_t)(c->log.bytes / 4096u - 1u),
            1u, 1u, 1u, 0x9c40400u,
        };
        for (unsigned i = 0; i < sizeof(regs) / sizeof(regs[0]); ++i) {
            if (core_write(d, regs[i], values[i], r)) { status = A650GmuIoError; goto failed; }
        }
        if (core_read(d, GMU_RPMH_CTRL, &value, r) ||
            core_write(d, GMU_RPMH_CTRL, value | RPMH_CTRL_VOTES, r) ||
            d->io.barrier(d->io.cookie)) {
            status = A650GmuIoError;
            goto failed;
        }
        for (unsigned i = 0; i < sizeof(regs) / sizeof(regs[0]); ++i) {
            if (core_read(d, regs[i], &value, r)) { status = A650GmuIoError; goto failed; }
            if (value != values[i]) { status = A650GmuStateMismatch; goto failed; }
        }
        if (core_read(d, GMU_RPMH_CTRL, &value, r)) { status = A650GmuIoError; goto failed; }
        if ((value & RPMH_CTRL_VOTES) != RPMH_CTRL_VOTES) { status = A650GmuStateMismatch; goto failed; }
    }

    /* Phase 6: release CM3 from reset and await firmware ready. */
    r->phase = 6u;
    if (core_read(d, GMU_CM3_DTCM_START + 0xff8u, &r->firmware_version, r)) { status = A650GmuIoError; goto failed; }
    {
        const uint32_t mask = r->firmware_version <= 0x20010004u ? 0xffffffffu : 0x1ffu;
        const uint32_t ready = mask == 0xffffffffu ? 0xbabefaceu : 0x100u;
        if (core_write(d, GMU_CM3_SYSRESET, 1u, r) ||
            core_write(d, GMU_PWR_COL_CP_RESP, 0u, r) ||
            d->io.barrier(d->io.cookie)) {
            status = A650GmuIoError;
            goto failed;
        }
        r->reset_verified = 0u;
        if (core_write(d, GMU_CM3_SYSRESET, 0u, r) || d->io.barrier(d->io.cookie)) {
            status = A650GmuIoError;
            goto failed;
        }
        status = poll_reg(d, A650GmuCore, GMU_CM3_FW_INIT_RESULT, mask, ready, c->poll_budget, 1, &r->firmware_init_result, r);
        if (status != A650GmuSuccess) goto failed;
        r->firmware_ready_observed = 1u;
    }

    /* Phase 7: start HFI and verify ready. */
    r->phase = 7u;
    if (core_write(d, GMU_HFI_CTRL_INIT, 1u, r) || d->io.barrier(d->io.cookie)) { status = A650GmuIoError; goto failed; }
    status = poll_reg(d, A650GmuCore, GMU_HFI_CTRL_STATUS, 1u, 1u, c->poll_budget, 1, &r->hfi_status, r);
    if (status != A650GmuSuccess) goto failed;
    status = check_faults(d, r);
    if (status != A650GmuSuccess) goto failed;
    r->hfi_ready_observed = 1u;
    r->phase = 8u;
    return A650GmuSuccess;

failed:
    reset_after_failure(d, r);
    return status;
}

typedef struct {
    A650GmuDevice *device;
    A650GmuStartupResult *result;
} StartupContext;

static int startup_now(void *cookie, uint64_t *value) {
    A650GmuDevice *d = ((StartupContext *)cookie)->device;
    return d->services.now_us(d->services.cookie, value);
}

static int startup_wait(void *cookie, unsigned us) {
    A650GmuDevice *d = ((StartupContext *)cookie)->device;
    return d->services.wait_us(d->services.cookie, us);
}

static int startup_health(void *cookie) {
    StartupContext *c = (StartupContext *)cookie;
    A650GmuDevice *d = c->device;
    A650GmuStartupResult *r = c->result;
    uint32_t reset = 0, hfi = 0;
    r->health_status = check_faults(d, &r->load);
    if (r->health_status != A650GmuSuccess) return 1;
    if (core_read(d, GMU_CM3_SYSRESET, &reset, &r->load) ||
        core_read(d, GMU_HFI_CTRL_STATUS, &hfi, &r->load)) {
        r->health_status = A650GmuIoError;
    } else if (reset != 0u || !(hfi & 1u)) {
        r->health_status = A650GmuStateMismatch;
    }
    return r->health_status != A650GmuSuccess;
}

A650GmuStatus A650GmuColdStart(A650GmuDevice *d, const A650GmuConfig *c, const AdrenoHfiStartupConfig *config,
                               const A650GmuStartupLimits *limits, A650GmuStartupResult *r) {
    if (!r) return A650GmuArgument;
    *r = (A650GmuStartupResult){0};
    r->load.resources_must_retain = 1u;
    r->protocol.resources_must_retain = 1u;
    r->protocol_status = AdrenoHfiStartupInvalid;
    if (!d || !c || !limits || AdrenoHfiStartupValidate(config) != AdrenoHfiStartupSuccess) {
        return A650GmuArgument;
    }
    StartupContext context = {d, r};
    AdrenoHfiStartupServices services = {
        &context, startup_now, startup_wait, startup_health,
        limits->polls_per_command, limits->wait_interval_us,
        limits->command_timeout_us, limits->total_timeout_us,
    };
    if (AdrenoHfiStartupValidateServices(&services) != AdrenoHfiStartupSuccess) return A650GmuArgument;
    if (d->poisoned || d->startup.poisoned) return A650GmuPoisoned;
    if (d->startup.attempted || d->startup.completed) return A650GmuBusy;

    A650GmuStatus status = A650GmuLoadAndStartHfi(d, c, &r->load);
    if (status != A650GmuSuccess) return status;

    r->protocol_status = AdrenoHfiStartupRun(&d->startup, &d->hfi, config, &services, &r->protocol);
    if (r->protocol_status == AdrenoHfiStartupSuccess) {
        if (core_write(d, GMU_GMU2HOST_INTR_CLR, ~0u, &r->load) ||
            core_write(d, GMU_GMU2HOST_INTR_MASK, ~CM3_FATAL, &r->load) ||
            d->io.barrier(d->io.cookie)) {
            status = A650GmuIoError;
            goto failed;
        }
        return A650GmuSuccess;
    }
    switch (r->protocol_status) {
    case AdrenoHfiStartupClockError:
    case AdrenoHfiStartupWaitError:
        status = A650GmuClockError;
        break;
    case AdrenoHfiStartupTimeout:
        status = A650GmuTimeout;
        break;
    case AdrenoHfiStartupPlatformFault:
        status = r->health_status;
        break;
    default:
        status = A650GmuProtocolError;
        break;
    }
failed:
    reset_after_failure(d, &r->load);
    d->startup.poisoned = 1u;
    return status;
}

static int hfi_wait(void *cookie) {
    A650GmuDevice *d = (A650GmuDevice *)cookie;
    return d->services.wait_us(d->services.cookie, 100u);
}

A650GmuStatus A650GmuPowerGx(A650GmuDevice *d, unsigned perf_index, A650GmuGxResult *r) {
    if (!r) return A650GmuArgument;
    *r = (A650GmuGxResult){0};
    if (!d || perf_index < 1u || perf_index > 7u) return A650GmuArgument;
    if (d->poisoned || d->hfi.poisoned) return A650GmuPoisoned;
    if (!d->startup.completed) return A650GmuStateMismatch;

    A650GmuResult io = {0};
    if (core_read(d, GMU_SPTPRAC_PWR_CLK_STATUS, &r->gx_before, &io)) return A650GmuIoError;

    const uint32_t vote[3] = {1u, perf_index, A650GmuDdrVote ? 1u : 0u};
    uint32_t out[16];
    size_t count = 0;
    r->vote_status = (uint32_t)AdrenoHfiExchange(&d->hfi, 30u, vote, 3u, 1000u, hfi_wait,
                                                 out, 16u, &count, &r->vote_error);
    if (r->vote_status != (uint32_t)AdrenoHfiSuccess) {
        return r->vote_status == (uint32_t)AdrenoHfiFirmwareError ? A650GmuProtocolError : A650GmuTimeout;
    }
    if (core_write(d, GMU_HOST2GMU_INTR_SET, 1u << OOB_GPU_SET_REQ, &io) ||
        d->io.barrier(d->io.cookie)) {
        return A650GmuIoError;
    }
    A650GmuStatus status = A650GmuTimeout;
    for (r->oob_polls = 1u; r->oob_polls <= 100u; ++r->oob_polls) {
        if (core_read(d, GMU_GMU2HOST_INTR_INFO, &r->oob_info, &io)) return A650GmuIoError;
        if (r->oob_info & (1u << OOB_GPU_SET_ACK)) { status = A650GmuSuccess; break; }
        if (d->services.wait_us(d->services.cookie, 100u)) return A650GmuClockError;
    }
    if (core_write(d, GMU_GMU2HOST_INTR_CLR, 1u << OOB_GPU_SET_ACK, &io) ||
        d->io.barrier(d->io.cookie)) {
        return A650GmuIoError;
    }
    if (core_read(d, GMU_SPTPRAC_PWR_CLK_STATUS, &r->gx_after, &io)) return A650GmuIoError;
    r->gx_on = (status == A650GmuSuccess && !(r->gx_after & GX_OFF_BITS)) ? 1u : 0u;
    if (status == A650GmuSuccess && !r->gx_on) status = A650GmuStateMismatch;
    return status;
}

AdrenoHfiStartupStatus A650GmuBuildStartup(const uint8_t *gfx, size_t gfx_bytes,
                                           const uint8_t *mx, size_t mx_bytes,
                                           const uint8_t *cx, size_t cx_bytes,
                                           uint32_t mc0, uint32_t sh0, uint32_t acv, uint32_t cn0,
                                           AdrenoHfiStartupConfig *out) {
    /* Verified against TB-J716F stock DTB gpu-opp-table_v2 (305..670 MHz, RPMh levels 64..320). */
    static const uint64_t gpu_hz[8] = {
        0ULL, 305000000ULL, 400000000ULL, 441600000ULL,
        490000000ULL, 525000000ULL, 587000000ULL, 670000000ULL,
    };
    static const uint32_t gpu_level[8] = {0u, 64u, 128u, 144u, 192u, 224u, 256u, 320u};
    static const uint64_t gmu_hz[2] = {0ULL, 200000000ULL};
    static const uint32_t gmu_level[2] = {0u, 48u};

    if (!out || !gfx || !mx || !cx || !mc0 || !sh0 || !acv || !cn0) return AdrenoHfiStartupInvalid;
    AdrenoHfiStartupConfig c = {0};
    uint32_t votes[16];
    if (AdrenoHfiArcVotes(gpu_level, 8u, gfx, gfx_bytes, mx, mx_bytes, votes, 16u)) {
        return AdrenoHfiStartupInvalid;
    }
    c.gpu_count = 8u;
    for (unsigned i = 0; i < 8u; ++i) {
        c.gpu[i] = (AdrenoHfiPerformanceLevel){gpu_hz[i], votes[i]};
    }
    if (AdrenoHfiArcVotes(gmu_level, 2u, cx, cx_bytes, mx, mx_bytes, votes, 16u)) {
        return AdrenoHfiStartupInvalid;
    }
    c.gmu_count = 2u;
    for (unsigned i = 0; i < 2u; ++i) {
        c.gmu[i] = (AdrenoHfiPerformanceLevel){gmu_hz[i], votes[i]};
    }
    c.bandwidth_count = A650GmuDdrVote ? 2u : 1u;
    c.ddr_count = 3u;
    c.ddr_wait_mask = 1u;
    c.ddr_addresses[0] = mc0;
    c.ddr_addresses[1] = sh0;
    c.ddr_addresses[2] = acv;
    c.ddr_data[0][0] = c.ddr_data[0][1] = c.ddr_data[0][2] = 0x40000000u;
    if (A650GmuDdrVote) {
        c.ddr_data[1][0] = c.ddr_data[1][1] = 0x60000000u | (0x3fffu << 14) | 0x3fffu;
        c.ddr_data[1][2] = 0x60000008u;
    }
    c.cnoc_count = 1u;
    c.cnoc_wait_mask = 1u;
    c.cnoc_addresses[0] = cn0;
    c.cnoc_data[0][0] = 0x40000000u;
    c.cnoc_data[1][0] = 0x60000001u;
    if (AdrenoHfiStartupValidate(&c) != AdrenoHfiStartupSuccess) return AdrenoHfiStartupInvalid;
    *out = c;
    return AdrenoHfiStartupSuccess;
}
