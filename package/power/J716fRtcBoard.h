/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* TB-J716F (SM8250-AC / PM8150) SPMI v5 RTC + persistent offset core logic.
 * Pure register-pointer and arithmetic helpers host-tested by tests/J716fRtcBoardTest.c
 * against a software model of the SM8250 SPMI PMIC arbiter (version 5 layout).
 *
 * Verified on TB-J716F hardware (COM9 rtc.scan, stock DTB qcom,pm8941-rtc):
 *   - SPMI Arbiter v5 (0x50010000, 512 APIDs):
 *       CORE   0x0C440000  APID_MAP   at +0x0900 + 4*apid (bits 19:8 = PPID)
 *       CNFG   0x0C40A000  CNFG_OWNER at +0x0700 + 4*apid (bits 2:0  = EE owner, APSS = 0)
 *       OBSRVR 0x0E600000  stride 0x80 per APID (EE 0), read-only for every peripheral
 *       CHNLS  0x0C600000  stride 0x10000 per APID, writable only when owner == EE
 *   - PM8150 RTC_RW (USID 0, base 0x6000 -> PPID 0x060, APID 2, owner 1):
 *       0x46 RTC_CTRL    bit 7 (0x80) = RTC_ENABLE (always running on battery domain)
 *       0x48..0x4B       32-bit little-endian seconds counter (unlatched; require two
 *                        consecutive reads within 1 second to guard carry rollover)
 *       0x40..0x43       RTC_WRITE is owned by EE 1 (TZ/RPM) and never written.
 *
 * Wall time is derived as: wall_seconds = (int64_t)rtc_counter + stored_offset.
 * Offset persistence supports:
 *   1. Default: 64-byte CRC32-protected "J716FRTC" record stored in the reserved tail
 *      (offset 512..575) of the UFS SHNGDATA header block (zero PMIC SDAM conflict).
 *   2. Optional: PMIC SDAM cell (4-byte or 8-byte tagged) when explicitly configured. */

#ifndef J716F_RTC_BOARD_H
#define J716F_RTC_BOARD_H

#include <stdint.h>
#include <string.h>
#ifdef KERNEL
#include <libkern/libkern.h>
#else
#include <stdio.h>
#endif

enum {
    J716F_RTC_SPMI_VER_MIN      = 0x50000000u,
    J716F_RTC_SPMI_VER_MAX      = 0x70000000u,
    J716F_RTC_SPMI_FEATURES     = 0x0004u,
    J716F_RTC_SPMI_APIDS_MASK   = 0x07FFu,
    J716F_RTC_SPMI_MAX_APIDS    = 512u,
    J716F_RTC_SPMI_APID_MAP     = 0x0900u,
    J716F_RTC_SPMI_CNFG_OWNER   = 0x0700u,
    J716F_RTC_SPMI_OBS_STRIDE   = 0x0080u,
    J716F_RTC_SPMI_CHNL_STRIDE  = 0x10000u,

    J716F_RTC_SPMI_CMD          = 0x00u,
    J716F_RTC_SPMI_STATUS       = 0x08u,
    J716F_RTC_SPMI_WDATA0       = 0x10u,
    J716F_RTC_SPMI_RDATA0       = 0x18u,
    J716F_RTC_SPMI_OP_WRITEL    = 0u,
    J716F_RTC_SPMI_OP_READL     = 1u,
    J716F_RTC_SPMI_STATUS_DONE  = 1u << 0,
    J716F_RTC_SPMI_STATUS_ERR   = 0x0Eu,
    J716F_RTC_SPMI_POLLS        = 100000u,

    J716F_RTC_PPID              = 0x060u, /* PM8150 USID 0, 0x6000 RTC_RW */
    J716F_RTC_ALARM_PPID        = 0x061u, /* PM8150 USID 0, 0x6100 RTC_ALARM */
    J716F_RTC_CTRL              = 0x46u,
    J716F_RTC_RUNNING           = 0x80u,
    J716F_RTC_COUNTER           = 0x48u,

    J716F_RTC_SDAM_TYPE_REG     = 0x04u,
    J716F_RTC_SDAM_SUBTYPE_REG  = 0x05u,
    J716F_RTC_SDAM_MEM_SIZE_REG = 0x44u,
    J716F_RTC_SDAM_MEM_START    = 0x40u,

    J716F_RTC_READ_TRIES        = 4u,
    J716F_RTC_EARLIEST          = 1672531200u, /* 2023-01-01 00:00:00 UTC */
    J716F_RTC_SET_BACK          = 86400u,
    J716F_RTC_TAG_MASK          = 0x5A3C96A5u,

    J716F_RTC_RECORD_BYTES      = 64u,
    J716F_RTC_RECORD_VERSION    = 1u,
    J716F_RTC_UFS_HEADER_OFF    = 512u
};

#define J716F_RTC_LATEST_WALL 4102444800ULL /* 2100-01-01 00:00:00 UTC */

enum {
    J716F_RTC_OK          = 0,
    J716F_RTC_TIMEOUT     = 1,
    J716F_RTC_SPMI_ERR    = 2,
    J716F_RTC_UNSTABLE    = 3,
    J716F_RTC_EMPTY       = 4,
    J716F_RTC_RANGE       = 5,
    J716F_RTC_STOPPED     = 6,
    J716F_RTC_NOT_OWNER   = 7,
    J716F_RTC_MISMATCH    = 8,
    J716F_RTC_FOREIGN     = 9,
    J716F_RTC_CURRENT     = 10,
    J716F_RTC_NO_CELL     = 11,
    J716F_RTC_BAD_CRC     = 12,
    J716F_RTC_NOT_TRIED   = 0xFF
};

struct j716f_rtc_arbiter {
    volatile uint32_t *core;
    volatile uint32_t *cnfg;
    uint8_t ee;
};

struct j716f_rtc_record {
    int64_t  offset;
    uint32_t saved_counter;
    uint32_t flags;
    uint64_t saved_wall;
    uint64_t generation;
};

#ifndef J716F_RTC_COMMAND
#define J716F_RTC_COMMAND(ch, word) ((ch)[J716F_RTC_SPMI_CMD / 4u] = (word))
#endif

static inline uint32_t j716f_rtc_crc32(const uint8_t *data, size_t bytes) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < bytes; ++i) {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8u; ++bit) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

static inline void j716f_rtc_put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static inline void j716f_rtc_put64(uint8_t *p, uint64_t v) {
    j716f_rtc_put32(p, (uint32_t)(v & 0xFFFFFFFFULL));
    j716f_rtc_put32(p + 4, (uint32_t)(v >> 32));
}

static inline uint32_t j716f_rtc_get32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline uint64_t j716f_rtc_get64(const uint8_t *p) {
    return (uint64_t)j716f_rtc_get32(p) | ((uint64_t)j716f_rtc_get32(p + 4) << 32);
}

static inline void j716f_rtc_record_encode(uint8_t buf[J716F_RTC_RECORD_BYTES],
                                           const struct j716f_rtc_record *rec) {
    memset(buf, 0, J716F_RTC_RECORD_BYTES);
    memcpy(buf, "J716FRTC", 8u);
    j716f_rtc_put32(buf + 8, J716F_RTC_RECORD_VERSION);
    j716f_rtc_put32(buf + 12, rec->flags);
    j716f_rtc_put64(buf + 16, (uint64_t)rec->offset);
    j716f_rtc_put32(buf + 24, rec->saved_counter);
    j716f_rtc_put32(buf + 28, 0u);
    j716f_rtc_put64(buf + 32, rec->saved_wall);
    j716f_rtc_put64(buf + 40, rec->generation);
    j716f_rtc_put32(buf + 60, j716f_rtc_crc32(buf, 60u));
}

static inline int j716f_rtc_record_decode(const uint8_t buf[J716F_RTC_RECORD_BYTES],
                                          struct j716f_rtc_record *out) {
    int all_zero = 1;
    for (unsigned i = 0; i < J716F_RTC_RECORD_BYTES; ++i) {
        if (buf[i] != 0u) {
            all_zero = 0;
            break;
        }
    }
    if (all_zero) return J716F_RTC_EMPTY;
    if (memcmp(buf, "J716FRTC", 8u) != 0) return J716F_RTC_FOREIGN;
    if (j716f_rtc_get32(buf + 8) != J716F_RTC_RECORD_VERSION) return J716F_RTC_FOREIGN;
    if (j716f_rtc_get32(buf + 60) != j716f_rtc_crc32(buf, 60u)) return J716F_RTC_BAD_CRC;

    struct j716f_rtc_record r;
    r.flags = j716f_rtc_get32(buf + 12);
    r.offset = (int64_t)j716f_rtc_get64(buf + 16);
    r.saved_counter = j716f_rtc_get32(buf + 24);
    r.saved_wall = j716f_rtc_get64(buf + 32);
    r.generation = j716f_rtc_get64(buf + 40);
    if (r.saved_wall < (uint64_t)J716F_RTC_EARLIEST || r.saved_wall > J716F_RTC_LATEST_WALL) {
        return J716F_RTC_RANGE;
    }
    if (out) *out = r;
    return J716F_RTC_OK;
}

/* Compute restored wall time from a validated persistent record and live PM8150 counter.
 * If live_counter >= saved_counter (normal warm/cold reboot with battery intact), returns
 * exact (live_counter + offset). If PMIC counter restarted (battery drain), safely falls
 * back to max(saved_wall, floor_secs) instead of jumping into the future or 1970. */
static inline int j716f_rtc_record_restore(const struct j716f_rtc_record *rec,
                                           uint32_t live_counter, uint64_t floor_secs,
                                           uint64_t *out_wall, int *out_counter_used) {
    if (!rec || !out_wall) return J716F_RTC_RANGE;
    if (live_counter >= rec->saved_counter) {
        const int64_t wall = (int64_t)live_counter + rec->offset;
        if (wall < (int64_t)J716F_RTC_EARLIEST || (uint64_t)wall > J716F_RTC_LATEST_WALL) {
            return J716F_RTC_RANGE;
        }
        *out_wall = (uint64_t)wall;
        if (out_counter_used) *out_counter_used = 1;
        return J716F_RTC_OK;
    }
    uint64_t fallback = rec->saved_wall;
    if (floor_secs > fallback && floor_secs <= J716F_RTC_LATEST_WALL) {
        fallback = floor_secs;
    }
    if (fallback < (uint64_t)J716F_RTC_EARLIEST || fallback > J716F_RTC_LATEST_WALL) {
        return J716F_RTC_RANGE;
    }
    *out_wall = fallback;
    if (out_counter_used) *out_counter_used = 0;
    return J716F_RTC_OK;
}

static inline uint32_t j716f_rtc_spmi_cmd(uint8_t op, uint8_t addr, uint8_t bytes) {
    return ((uint32_t)op << 27) | ((uint32_t)addr << 4) | ((uint32_t)(bytes - 1u) & 7u);
}

static inline int j716f_rtc_spmi_wait(volatile uint32_t *ch) {
    for (uint32_t n = 0; n < J716F_RTC_SPMI_POLLS; ++n) {
        const uint32_t s = ch[J716F_RTC_SPMI_STATUS / 4u];
        if (s & J716F_RTC_SPMI_STATUS_DONE) {
            return (s & J716F_RTC_SPMI_STATUS_ERR) ? J716F_RTC_SPMI_ERR : J716F_RTC_OK;
        }
    }
    return J716F_RTC_TIMEOUT;
}

static inline int j716f_rtc_arbiter_check(const struct j716f_rtc_arbiter *a,
                                          uint32_t *out_ver, uint32_t *out_apids) {
    const uint32_t ver = a->core[0];
    const uint32_t apids = a->core[J716F_RTC_SPMI_FEATURES / 4u] & J716F_RTC_SPMI_APIDS_MASK;
    if (out_ver) *out_ver = ver;
    if (out_apids) *out_apids = apids;
    if (ver < J716F_RTC_SPMI_VER_MIN || ver >= J716F_RTC_SPMI_VER_MAX) return 1;
    if (!apids || apids > J716F_RTC_SPMI_MAX_APIDS) return 2;
    return 0;
}

static inline int j716f_rtc_find_ppid(const struct j716f_rtc_arbiter *a, uint32_t apids,
                                      uint32_t ppid, int *out_owner) {
    int found = -1;
    int owner = -1;
    for (uint32_t i = 0; i < apids; ++i) {
        const uint32_t map = a->core[(J716F_RTC_SPMI_APID_MAP + 4u * i) / 4u];
        if (!map || ((map >> 8) & 0xFFFu) != ppid) continue;
        const int o = (int)(a->cnfg[(J716F_RTC_SPMI_CNFG_OWNER + 4u * i) / 4u] & 7u);
        if (found < 0 || o == (int)a->ee) {
            found = (int)i;
            owner = o;
        }
    }
    if (out_owner) *out_owner = owner;
    return found;
}

static inline int j716f_rtc_obs_read(volatile uint32_t *obs_base, int apid,
                                     uint8_t byte_addr, uint8_t bytes, uint32_t *value) {
    if (!obs_base || apid < 0 || bytes < 1u || bytes > 4u || !value) return J716F_RTC_SPMI_ERR;
    volatile uint32_t *ch = obs_base + ((uint32_t)apid * J716F_RTC_SPMI_OBS_STRIDE) / 4u;
    J716F_RTC_COMMAND(ch, j716f_rtc_spmi_cmd(J716F_RTC_SPMI_OP_READL, byte_addr, bytes));
    const int rc = j716f_rtc_spmi_wait(ch);
    if (rc) return rc;
    const uint32_t data = ch[J716F_RTC_SPMI_RDATA0 / 4u];
    *value = (bytes >= 4u) ? data : (data & ((1u << (8u * bytes)) - 1u));
    return J716F_RTC_OK;
}

static inline int j716f_rtc_obs_read_stable(volatile uint32_t *obs_base, int apid,
                                            uint8_t byte_addr, uint8_t bytes, uint32_t *value) {
    uint32_t prev = 0, next = 0;
    int rc = j716f_rtc_obs_read(obs_base, apid, byte_addr, bytes, &prev);
    for (unsigned i = 1u; !rc && i < J716F_RTC_READ_TRIES; ++i) {
        rc = j716f_rtc_obs_read(obs_base, apid, byte_addr, bytes, &next);
        if (rc) break;
        if (next == prev) {
            *value = next;
            return J716F_RTC_OK;
        }
        prev = next;
    }
    return rc ? rc : J716F_RTC_UNSTABLE;
}

static inline int j716f_rtc_read_counter(volatile uint32_t *obs_base, int rtc_apid,
                                         uint8_t *out_ctrl, uint32_t *out_seconds) {
    uint32_t ctrl = 0, prev = 0, next = 0;
    int rc = j716f_rtc_obs_read_stable(obs_base, rtc_apid, J716F_RTC_CTRL, 1u, &ctrl);
    if (rc) return rc;
    if (out_ctrl) *out_ctrl = (uint8_t)ctrl;
    if (!(ctrl & J716F_RTC_RUNNING)) return J716F_RTC_STOPPED;
    rc = j716f_rtc_obs_read(obs_base, rtc_apid, J716F_RTC_COUNTER, 4u, &prev);
    for (unsigned i = 1u; !rc && i < J716F_RTC_READ_TRIES; ++i) {
        rc = j716f_rtc_obs_read(obs_base, rtc_apid, J716F_RTC_COUNTER, 4u, &next);
        if (rc) break;
        if ((uint32_t)(next - prev) <= 1u) {
            *out_seconds = next;
            return J716F_RTC_OK;
        }
        prev = next;
    }
    return rc ? rc : J716F_RTC_UNSTABLE;
}

static inline int j716f_rtc_wall_time(uint32_t counter, uint32_t offset, uint64_t *seconds) {
    if (!offset) return J716F_RTC_EMPTY;
    const uint64_t sum = (uint64_t)counter + (uint64_t)offset;
    if (sum > UINT32_MAX) return J716F_RTC_RANGE;
    *seconds = sum;
    return J716F_RTC_OK;
}

static inline int j716f_rtc_calc_offset(uint64_t seconds, uint32_t counter, uint32_t *offset) {
    if (seconds > UINT32_MAX || seconds <= counter) return J716F_RTC_RANGE;
    *offset = (uint32_t)(seconds - counter);
    return J716F_RTC_OK;
}

static inline uint32_t j716f_rtc_tag_for(uint32_t offset) {
    return offset ^ J716F_RTC_TAG_MASK;
}

static inline int j716f_rtc_check_cell(uint32_t counter, uint32_t cell, uint32_t tag,
                                       int tagged, uint64_t latest) {
    if (tagged) {
        if (cell == 0u && tag == 0u) return J716F_RTC_EMPTY;
        if (cell == 0u || tag != j716f_rtc_tag_for(cell)) return J716F_RTC_FOREIGN;
    } else {
        if (cell == 0u) return J716F_RTC_EMPTY;
    }
    uint64_t secs = 0;
    const int rc = j716f_rtc_wall_time(counter, cell, &secs);
    if (rc || secs < J716F_RTC_EARLIEST || secs > latest) return J716F_RTC_FOREIGN;
    return J716F_RTC_OK;
}

static inline int j716f_rtc_chnl_write4(volatile uint32_t *channel, uint8_t byte_addr, uint32_t word) {
    if (!channel) return J716F_RTC_SPMI_ERR;
    channel[J716F_RTC_SPMI_WDATA0 / 4u] = word;
    J716F_RTC_COMMAND(channel, j716f_rtc_spmi_cmd(J716F_RTC_SPMI_OP_WRITEL, byte_addr, 4u));
    return j716f_rtc_spmi_wait(channel);
}

static inline int j716f_rtc_read_cell(volatile uint32_t *obs_base, int sdam_apid,
                                      uint8_t cell_addr, int tagged,
                                      uint32_t *out_cell, uint32_t *out_tag) {
    uint32_t c = 0, t = 0;
    int rc = j716f_rtc_obs_read_stable(obs_base, sdam_apid, cell_addr, 4u, &c);
    if (rc) return rc;
    if (tagged) {
        if ((unsigned)cell_addr + 8u > 256u) return J716F_RTC_NO_CELL;
        rc = j716f_rtc_obs_read_stable(obs_base, sdam_apid, (uint8_t)(cell_addr + 4u), 4u, &t);
        if (rc) return rc;
    }
    if (out_cell) *out_cell = c;
    if (out_tag) *out_tag = t;
    return J716F_RTC_OK;
}

static inline int j716f_rtc_store_cell(volatile uint32_t *obs_base, volatile uint32_t *channel,
                                       int sdam_apid, int sdam_owner, uint8_t ee,
                                       uint8_t cell_addr, int tagged, uint32_t offset,
                                       uint32_t *out_readback) {
    if (sdam_owner != (int)ee) return J716F_RTC_NOT_OWNER;
    if (!offset) return J716F_RTC_EMPTY;
    if (tagged && (unsigned)cell_addr + 8u > 256u) return J716F_RTC_NO_CELL;

    int rc = j716f_rtc_chnl_write4(channel, cell_addr, offset);
    if (rc) return rc;
    if (tagged) {
        rc = j716f_rtc_chnl_write4(channel, (uint8_t)(cell_addr + 4u), j716f_rtc_tag_for(offset));
        if (rc) return rc;
    }
    uint32_t rb_cell = 0, rb_tag = 0;
    rc = j716f_rtc_read_cell(obs_base, sdam_apid, cell_addr, tagged, &rb_cell, &rb_tag);
    if (rc) return rc;
    if (out_readback) *out_readback = rb_cell;
    if (rb_cell != offset) return J716F_RTC_MISMATCH;
    if (tagged && rb_tag != j716f_rtc_tag_for(offset)) return J716F_RTC_MISMATCH;
    return J716F_RTC_OK;
}

#endif /* J716F_RTC_BOARD_H */
