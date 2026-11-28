/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Host sanitizer unit test for package/power/J716fRtcBoard.h against a software
 * model of the SM8250 SPMI PMIC arbiter (version 5 layout) and persistent record codec. */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static void sim_spmi_command(volatile uint32_t *ch, uint32_t word);
#define J716F_RTC_COMMAND(ch, word) sim_spmi_command((ch), (word))
#include "../J716fRtcBoard.h"

enum {
    SIM_APIDS      = 48,
    SIM_RTC_APID   = 17,
    SIM_SDAM_APID  = 23,
    SIM_SDAM_DECOY = 12,
    SIM_SDAM_PPID  = 0x0B2u
};

static uint32_t g_core[0x1000 / 4];
static uint32_t g_cnfg[0x1000 / 4];
static uint32_t g_obs[(SIM_APIDS * J716F_RTC_SPMI_OBS_STRIDE) / 4];
static uint32_t g_chnl[0x1000 / 4];

static uint8_t g_rtc[256];
static uint8_t g_sdam[256];

static uint32_t g_script[8];
static unsigned g_script_len = 0;
static unsigned g_script_idx = 0;
static uint32_t g_force_status = 0;
static int g_sdam_ro = 0;

static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static uint32_t get32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void sim_spmi_command(volatile uint32_t *ch, uint32_t word) {
    ch[J716F_RTC_SPMI_CMD / 4u] = word;
    if (g_force_status) {
        ch[J716F_RTC_SPMI_STATUS / 4u] = g_force_status;
        return;
    }
    const uint8_t op = (uint8_t)((word >> 27) & 0x1Fu);
    const uint8_t addr = (uint8_t)((word >> 4) & 0xFFu);
    const uint8_t bytes = (uint8_t)((word & 7u) + 1u);
    assert(bytes >= 1u && bytes <= 4u);

    if (ch == g_chnl) {
        assert(op == J716F_RTC_SPMI_OP_WRITEL);
        if (!g_sdam_ro) {
            const uint32_t w = ch[J716F_RTC_SPMI_WDATA0 / 4u];
            for (unsigned i = 0; i < bytes; ++i) {
                g_sdam[(addr + i) & 0xFFu] = (uint8_t)((w >> (8u * i)) & 0xFFu);
            }
        }
        ch[J716F_RTC_SPMI_STATUS / 4u] = J716F_RTC_SPMI_STATUS_DONE;
        return;
    }

    assert(op == J716F_RTC_SPMI_OP_READL);
    const uintptr_t diff = (uintptr_t)ch - (uintptr_t)g_obs;
    const int apid = (int)(diff / J716F_RTC_SPMI_OBS_STRIDE);
    assert(apid == SIM_RTC_APID || apid == SIM_SDAM_APID);
    const uint8_t *mem = (apid == SIM_RTC_APID) ? g_rtc : g_sdam;
    uint32_t data = 0;
    if (apid == SIM_RTC_APID && addr == J716F_RTC_COUNTER && bytes == 4u &&
        g_script_idx < g_script_len) {
        data = g_script[g_script_idx++];
    } else {
        for (unsigned i = 0; i < bytes; ++i) {
            data |= (uint32_t)mem[(addr + i) & 0xFFu] << (8u * i);
        }
    }
    ch[J716F_RTC_SPMI_RDATA0 / 4u] = data;
    ch[J716F_RTC_SPMI_STATUS / 4u] = J716F_RTC_SPMI_STATUS_DONE;
}

static void reset_sim(void) {
    memset(g_core, 0, sizeof(g_core));
    memset(g_cnfg, 0, sizeof(g_cnfg));
    memset(g_obs, 0, sizeof(g_obs));
    memset(g_chnl, 0, sizeof(g_chnl));
    memset(g_rtc, 0, sizeof(g_rtc));
    memset(g_sdam, 0, sizeof(g_sdam));
    g_script_len = 0;
    g_script_idx = 0;
    g_force_status = 0;
    g_sdam_ro = 0;

    g_core[0] = 0x50010000u; /* Measured TB-J716F SM8250 SPMI Arbiter v5 */
    g_core[J716F_RTC_SPMI_FEATURES / 4u] = SIM_APIDS;

    /* PM8150 RTC_RW (PPID 0x060), owned by EE 1 */
    g_core[(J716F_RTC_SPMI_APID_MAP + 4u * SIM_RTC_APID) / 4u] = (J716F_RTC_PPID << 8) | 1u;
    g_cnfg[(J716F_RTC_SPMI_CNFG_OWNER + 4u * SIM_RTC_APID) / 4u] = 1u;

    /* Decoy SDAM owned by EE 2, followed by real SDAM owned by APSS (EE 0) */
    g_core[(J716F_RTC_SPMI_APID_MAP + 4u * SIM_SDAM_DECOY) / 4u] = (SIM_SDAM_PPID << 8) | 1u;
    g_cnfg[(J716F_RTC_SPMI_CNFG_OWNER + 4u * SIM_SDAM_DECOY) / 4u] = 2u;
    g_core[(J716F_RTC_SPMI_APID_MAP + 4u * SIM_SDAM_APID) / 4u] = (SIM_SDAM_PPID << 8) | 1u;
    g_cnfg[(J716F_RTC_SPMI_CNFG_OWNER + 4u * SIM_SDAM_APID) / 4u] = 0u;

    g_rtc[J716F_RTC_CTRL] = J716F_RTC_RUNNING;
    put32(g_rtc + J716F_RTC_COUNTER, 5050000u);
}

int main(void) {
    struct j716f_rtc_arbiter arb = {g_core, g_cnfg, 0u};
    uint32_t ver = 0, apids = 0;
    int owner = -1;

    /* 1. Arbiter v5 version & APID discovery */
    reset_sim();
    assert(j716f_rtc_arbiter_check(&arb, &ver, &apids) == 0);
    assert(ver == 0x50010000u && apids == SIM_APIDS);
    g_core[0] = 0x70000000u; /* v7 must be rejected */
    assert(j716f_rtc_arbiter_check(&arb, &ver, &apids) == 1);
    g_core[0] = 0x50010000u;

    assert(j716f_rtc_find_ppid(&arb, apids, J716F_RTC_PPID, &owner) == SIM_RTC_APID && owner == 1);
    assert(j716f_rtc_find_ppid(&arb, apids, SIM_SDAM_PPID, &owner) == SIM_SDAM_APID && owner == 0);
    assert(j716f_rtc_find_ppid(&arb, apids, 0x099u, &owner) == -1 && owner == -1);

    /* 2. RTC counter read, stopped guard, and carry-rollover glitch recovery */
    uint8_t ctrl = 0;
    uint32_t secs = 0;
    assert(j716f_rtc_read_counter(g_obs, SIM_RTC_APID, &ctrl, &secs) == J716F_RTC_OK);
    assert(ctrl == J716F_RTC_RUNNING && secs == 5050000u);

    g_rtc[J716F_RTC_CTRL] = 0x00u;
    assert(j716f_rtc_read_counter(g_obs, SIM_RTC_APID, &ctrl, &secs) == J716F_RTC_STOPPED);
    g_rtc[J716F_RTC_CTRL] = J716F_RTC_RUNNING;

    g_script[0] = 0x00000000u;
    g_script[1] = 0x00010000u;
    g_script[2] = 0x00010000u;
    g_script_len = 3;
    g_script_idx = 0;
    assert(j716f_rtc_read_counter(g_obs, SIM_RTC_APID, &ctrl, &secs) == J716F_RTC_OK);
    assert(secs == 0x00010000u && g_script_idx == 3);

    g_script[0] = 5050000u;
    g_script[1] = 5050001u;
    g_script_len = 2;
    g_script_idx = 0;
    assert(j716f_rtc_read_counter(g_obs, SIM_RTC_APID, &ctrl, &secs) == J716F_RTC_OK);
    assert(secs == 5050001u && g_script_idx == 2);

    g_script[0] = 10u;
    g_script[1] = 500u;
    g_script[2] = 900u;
    g_script[3] = 1500u;
    g_script_len = 4;
    g_script_idx = 0;
    assert(j716f_rtc_read_counter(g_obs, SIM_RTC_APID, &ctrl, &secs) == J716F_RTC_UNSTABLE);
    g_script_len = 0;

    g_force_status = J716F_RTC_SPMI_STATUS_DONE | 0x02u;
    assert(j716f_rtc_read_counter(g_obs, SIM_RTC_APID, &ctrl, &secs) == J716F_RTC_SPMI_ERR);
    g_force_status = 0;

    /* 3. 64-byte UFS persistent RTC record codec, CRC32 & reboot restoration */
    uint8_t buf[J716F_RTC_RECORD_BYTES];
    memset(buf, 0, sizeof(buf));
    struct j716f_rtc_record dec;
    assert(j716f_rtc_record_decode(buf, &dec) == J716F_RTC_EMPTY);

    const uint64_t wall1 = 1795814400ULL; /* 2026-11-28 21:20:00 UTC */
    struct j716f_rtc_record enc = {
        .offset = (int64_t)wall1 - 5050000LL,
        .saved_counter = 5050000u,
        .flags = 0u,
        .saved_wall = wall1,
        .generation = 7ULL
    };
    j716f_rtc_record_encode(buf, &enc);
    assert(j716f_rtc_record_decode(buf, &dec) == J716F_RTC_OK);
    assert(dec.offset == enc.offset && dec.saved_counter == 5050000u &&
           dec.saved_wall == wall1 && dec.generation == 7ULL);

    /* Bit flip must fail CRC32 */
    buf[20] ^= 0x04u;
    assert(j716f_rtc_record_decode(buf, &dec) == J716F_RTC_BAD_CRC);
    buf[20] ^= 0x04u;

    /* Foreign bytes must return J716F_RTC_FOREIGN */
    buf[0] = 'X';
    assert(j716f_rtc_record_decode(buf, &dec) == J716F_RTC_FOREIGN);

    /* Normal reboot 45 minutes later: PM8150 counter advanced by 2700s */
    uint64_t restored = 0;
    int used_cnt = -1;
    assert(j716f_rtc_record_restore(&dec, 5050000u + 2700u, 1795341438ULL, &restored, &used_cnt) == J716F_RTC_OK);
    assert(used_cnt == 1 && restored == wall1 + 2700ULL);

    /* Also works when real wall time is earlier than build floor (e.g. April 2026 NTP vs Nov 2026 build) */
    const uint64_t april_wall = 1776000000ULL;
    struct j716f_rtc_record enc_apr = {
        .offset = (int64_t)april_wall - 5050000LL,
        .saved_counter = 5050000u,
        .flags = 0u,
        .saved_wall = april_wall,
        .generation = 8ULL
    };
    assert(j716f_rtc_record_restore(&enc_apr, 5050060u, 1795341438ULL, &restored, &used_cnt) == J716F_RTC_OK);
    assert(used_cnt == 1 && restored == april_wall + 60ULL);

    /* Battery disconnect / PMIC counter reset: live_counter < saved_counter -> safe fallback */
    assert(j716f_rtc_record_restore(&dec, 120u, 1795341438ULL, &restored, &used_cnt) == J716F_RTC_OK);
    assert(used_cnt == 0 && restored == wall1);

    /* 4. Optional tagged 8-byte SDAM persistence round-trip */
    uint32_t off1 = 0;
    assert(j716f_rtc_calc_offset(wall1, 5050000u, &off1) == J716F_RTC_OK);
    uint32_t cell = 0, tag = 0, rb = 0;
    assert(j716f_rtc_read_cell(g_obs, SIM_SDAM_APID, 0x40u, 1, &cell, &tag) == J716F_RTC_OK);
    assert(j716f_rtc_check_cell(5050000u, cell, tag, 1, UINT32_MAX) == J716F_RTC_EMPTY);
    assert(j716f_rtc_store_cell(g_obs, g_chnl, SIM_SDAM_APID, 4, 0u, 0x40u, 1, off1, &rb) == J716F_RTC_NOT_OWNER);
    assert(j716f_rtc_store_cell(g_obs, g_chnl, SIM_SDAM_APID, 0, 0u, 0x40u, 1, off1, &rb) == J716F_RTC_OK);
    assert(rb == off1 && get32(g_sdam + 0x40u) == off1 && get32(g_sdam + 0x44u) == j716f_rtc_tag_for(off1));

    g_sdam_ro = 1;
    assert(j716f_rtc_store_cell(g_obs, g_chnl, SIM_SDAM_APID, 0, 0u, 0x60u, 1, off1, &rb) == J716F_RTC_MISMATCH);
    g_sdam_ro = 0;

    printf("J716fRtcBoardTest: PASS\n");
    return 0;
}
