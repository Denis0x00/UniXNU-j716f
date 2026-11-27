// SPDX-License-Identifier: BSD-2-Clause-Patent
#include "J716fMhiBoot.h"

static uint32_t rd(J716fMhiBoot *m, uint32_t reg) {
    return m->io.read(m->io.ctx, reg);
}

static void wr(J716fMhiBoot *m, uint32_t reg, uint32_t value) {
    m->io.write(m->io.ctx, reg, value);
}

int j716f_mhi_boot_open(J716fMhiBoot *m, const J716fMhiIo *io) {
    for (unsigned i = 0; i < sizeof(*m); ++i) ((uint8_t *)m)[i] = 0u;
    m->io = *io;
    m->reglen = io->space ? io->space : rd(m, J716F_MHI_REGLEN);
    m->bhi = rd(m, J716F_MHI_BHIOFF);
    m->bhie = rd(m, J716F_MHI_BHIEOFF);
    const int good = m->reglen >= 0x100u && m->reglen <= 0x200000u &&
                     m->bhi >= 0x100u && m->bhi + J716F_MHI_BHI_ERRCODE + 4u <= m->reglen &&
                     m->bhie >= 0x100u && m->bhie + J716F_MHI_BHIE_TXVECSTATUS + 4u <= m->reglen &&
                     !(m->bhi & 3u) && !(m->bhie & 3u);
    if (!good) {
        m->phase = J716fMhiFailed;
        m->error = J716fMhiErrLayout;
        return -1;
    }
    m->phase = J716fMhiIdle;
    return 0;
}

uint32_t j716f_mhi_boot_ee(J716fMhiBoot *m) {
    return m->ee = rd(m, m->bhi + J716F_MHI_BHI_EXECENV);
}

uint32_t j716f_mhi_boot_state(J716fMhiBoot *m) {
    return m->state = (rd(m, J716F_MHI_STATUS) >> J716F_MHI_STATE_SHIFT) & 0xFFu;
}

static uint32_t fail(J716fMhiBoot *m, uint32_t error) {
    m->failed_phase = m->phase;
    m->error = error;
    m->phase = J716fMhiFailed;
    return m->phase;
}

static uint32_t start_sbl(J716fMhiBoot *m) {
    wr(m, m->bhi + J716F_MHI_BHI_INTVEC, m->msi_vector);
    wr(m, m->bhi + J716F_MHI_BHI_IMGADDR_HIGH, (uint32_t)(m->sbl_addr >> 32));
    wr(m, m->bhi + J716F_MHI_BHI_IMGADDR_LOW, (uint32_t)m->sbl_addr);
    wr(m, m->bhi + J716F_MHI_BHI_IMGSIZE, m->sbl_size);
    wr(m, m->bhi + J716F_MHI_BHI_IMGTXDB, m->seq);
    m->phase = J716fMhiSblLoading;
    return m->phase;
}

uint32_t j716f_mhi_boot_begin(J716fMhiBoot *m, uint32_t msi_vector,
                              uint64_t sbl_addr, uint32_t sbl_size,
                              uint64_t table_addr, uint32_t table_size,
                              uint32_t seq) {
    if (m->phase != J716fMhiIdle) return fail(m, J716fMhiErrState);
    m->msi_vector = msi_vector;
    m->sbl_addr = sbl_addr;
    m->sbl_size = sbl_size;
    m->table_addr = table_addr;
    m->table_size = table_size;
    m->seq = seq & J716F_MHI_SEQ_MASK;
    if (!m->seq) m->seq = 1u;

    const uint32_t ee = j716f_mhi_boot_ee(m);
    if (ee != J716fMhiEePbl && ee != J716fMhiEeEdl) return fail(m, J716fMhiErrNotPbl);
    const uint32_t status = rd(m, J716F_MHI_STATUS);
    m->state = (status >> J716F_MHI_STATE_SHIFT) & 0xFFu;
    wr(m, m->bhi + J716F_MHI_BHI_INTVEC, msi_vector);
    if ((status & J716F_MHI_STATUS_SYSERR) || m->state == J716fMhiStateSysErr) {
        wr(m, J716F_MHI_CTRL, J716F_MHI_CTRL_RESET);
        m->phase = J716fMhiResetting;
        return m->phase;
    }
    return start_sbl(m);
}

static uint32_t start_image(J716fMhiBoot *m) {
    m->seq = (m->seq + 1u) & J716F_MHI_SEQ_MASK;
    if (!m->seq) m->seq = 1u;
    wr(m, m->bhie + J716F_MHI_BHIE_TXVECADDR_HI, (uint32_t)(m->table_addr >> 32));
    wr(m, m->bhie + J716F_MHI_BHIE_TXVECADDR_LO, (uint32_t)m->table_addr);
    wr(m, m->bhie + J716F_MHI_BHIE_TXVECSIZE, m->table_size);
    wr(m, m->bhie + J716F_MHI_BHIE_TXVECDB, m->seq);
    m->phase = J716fMhiImageLoading;
    return m->phase;
}

uint32_t j716f_mhi_boot_step(J716fMhiBoot *m) {
    if (m->phase == J716fMhiIdle || m->phase == J716fMhiDone ||
        m->phase == J716fMhiFailed || m->phase == J716fMhiNeedContexts) {
        return m->phase;
    }
    const uint32_t status = rd(m, J716F_MHI_STATUS);
    if (status == 0xFFFFFFFFu) return m->phase;
    m->state = (status >> J716F_MHI_STATE_SHIFT) & 0xFFu;
    if (m->phase == J716fMhiResetting) {
        if (rd(m, J716F_MHI_CTRL) & J716F_MHI_CTRL_RESET) return m->phase;
        return start_sbl(m);
    }
    const uint32_t ee = j716f_mhi_boot_ee(m);
    if (ee == J716fMhiEeRddm) return fail(m, J716fMhiErrRddm);
    if ((status & J716F_MHI_STATUS_SYSERR) || m->state == J716fMhiStateSysErr) {
        return fail(m, J716fMhiErrSysErr);
    }
    switch (m->phase) {
    case J716fMhiSblLoading: {
        const uint32_t bhi = rd(m, m->bhi + J716F_MHI_BHI_STATUS);
        m->last_status = bhi;
        if (bhi == 0xFFFFFFFFu) return m->phase;
        if ((bhi >> 30) == J716F_MHI_XFER_ERROR) {
            m->errcode = rd(m, m->bhi + J716F_MHI_BHI_ERRCODE);
            return fail(m, J716fMhiErrBhi);
        }
        if ((bhi >> 30) != J716F_MHI_XFER_SUCCESS) return m->phase;
        m->phase = J716fMhiWaitReady;
        return j716f_mhi_boot_step(m);
    }
    case J716fMhiWaitReady:
        if ((rd(m, J716F_MHI_CTRL) & J716F_MHI_CTRL_RESET) || !(status & J716F_MHI_STATUS_READY)) {
            return m->phase;
        }
        m->phase = J716fMhiNeedContexts;
        return m->phase;
    case J716fMhiWaitSbl:
        if (ee == J716fMhiEeAmss || ee == J716fMhiEeWfw) {
            m->phase = J716fMhiDone;
            return m->phase;
        }
        if (ee != J716fMhiEeSbl) return m->phase;
        return start_image(m);
    case J716fMhiImageLoading: {
        const uint32_t tx = rd(m, m->bhie + J716F_MHI_BHIE_TXVECSTATUS);
        m->last_status = tx;
        if ((tx & J716F_MHI_SEQ_MASK) != m->seq) return m->phase;
        if ((tx >> 30) == J716F_MHI_XFER_ERROR) return fail(m, J716fMhiErrBhie);
        if ((tx >> 30) != J716F_MHI_XFER_SUCCESS) return m->phase;
        m->phase = J716fMhiWaitMission;
        return j716f_mhi_boot_step(m);
    }
    case J716fMhiWaitMission:
        if (ee == J716fMhiEeAmss || ee == J716fMhiEeWfw) m->phase = J716fMhiDone;
        return m->phase;
    default:
        return m->phase;
    }
}

uint32_t j716f_mhi_boot_contexts_done(J716fMhiBoot *m) {
    if (m->phase != J716fMhiNeedContexts) return fail(m, J716fMhiErrState);
    wr(m, J716F_MHI_CTRL,
       (rd(m, J716F_MHI_CTRL) & ~(0xFFu << J716F_MHI_STATE_SHIFT) & ~J716F_MHI_CTRL_RESET) |
       ((uint32_t)J716fMhiStateM0 << J716F_MHI_STATE_SHIFT));
    m->phase = J716fMhiWaitSbl;
    return j716f_mhi_boot_step(m);
}

uint32_t j716f_mhi_boot_timeout(J716fMhiBoot *m) {
    if (m->phase == J716fMhiIdle || m->phase == J716fMhiDone || m->phase == J716fMhiFailed) {
        return m->phase;
    }
    if (m->phase == J716fMhiSblLoading) m->errcode = rd(m, m->bhi + J716F_MHI_BHI_ERRCODE);
    return fail(m, J716fMhiErrTimeout);
}

uint32_t j716f_mhi_bhie_table(J716fMhiBhieEntry *table, uint32_t count,
                              const uint64_t *addresses, uint32_t image_size,
                              uint32_t segment_size) {
    if (!table || !addresses || !segment_size || !image_size) return 0u;
    const uint32_t needed = (image_size + segment_size - 1u) / segment_size;
    if (needed > count) return 0u;
    for (uint32_t i = 0; i < needed; ++i) {
        const uint32_t left = image_size - i * segment_size;
        table[i].address = addresses[i];
        table[i].size = left < segment_size ? left : segment_size;
    }
    return needed;
}
