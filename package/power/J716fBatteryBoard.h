// SPDX-License-Identifier: BSD-2-Clause-Patent
// Lenovo TB-J716F (SM8250-AC) PM8150B gen4 fuel gauge and SMB5 charger register map
// and pure decoding arithmetic (host-tested by J716fBatteryBoardTest.c).
// Verified against TB-J716F stock DTB (qcom,pm8150b@2: qpnp,fg@4000..4300, qcom,qpnp-smb5@1000..1700)
// and Linux qpnp-fg-gen4.c / fg-reg.h / fg-util.c / smb5-lib.c.
//
// SPMI PPID = (USID << 8) | (peripheral_address >> 8); PM8150B is USID 2:
//   0x240 fg-batt-soc   0x09 MSOC, 0x0A MSOC_CP (0..255, 255 = full)
//   0x241 fg-batt-info  0xA0/A1 VBATT (CP 0xA6/A7): uV = raw * 122070 / 1000
//                       0xA2/A3 IBATT (CP 0xA8/A9): uA = sign15(raw) * 488281 / 1000
//                       0x50/51 BATT_TEMP (10-bit signed, 0.25 C/LSB)
//   0x210 SMB5 CHGR     0x06 stage bits 2:0 (1 trickle, 2 pre, 3 full-on, 4 taper, 5 terminate, 7 disabled)
//                       0x42 charge enable bit 0
//   0x212 SMB5 BATIF    0x10 bit 5:4 battery missing
//   0x213 SMB5 USBIN    0x10 bit 4 VBUS present
//   0x216 SMB5 MISC     0x53 watchdog config (read-only)

#ifndef J716F_BATTERY_BOARD_H
#define J716F_BATTERY_BOARD_H

#include <stdint.h>

#define BAT_PPID_FG_SOC   0x240u
#define BAT_PPID_FG_INFO  0x241u
#define BAT_PPID_FG_RRADC 0x242u
#define BAT_PPID_FG_MEMIF 0x243u
#define BAT_PPID_SMB_CHGR  0x210u
#define BAT_PPID_SMB_DCDC  0x211u
#define BAT_PPID_SMB_BATIF 0x212u
#define BAT_PPID_SMB_USBIN 0x213u
#define BAT_PPID_SMB_MISC  0x216u

#define BAT_FG_MSOC         0x09u
#define BAT_FG_MSOC_CP      0x0Au
#define BAT_FG_VBATT_LSB    0xA0u
#define BAT_FG_VBATT_LSB_CP 0xA6u
#define BAT_FG_IBATT_LSB    0xA2u
#define BAT_FG_IBATT_LSB_CP 0xA8u
#define BAT_FG_BATT_TEMP_LSB 0x50u

#define BAT_FG_TEMP_SRAM_WORD 328u
#define BAT_FG_SRAM_RD_DATA0  0x67u
#define BAT_FG_MEM_INTF_CFG   0x50u
#define BAT_FG_IMA_CTL        0x51u
#define BAT_FG_IMA_OPR_STS    0x54u
#define BAT_FG_IMA_EXP_STS    0x55u
#define BAT_FG_IMA_HW_STS     0x56u
#define BAT_FG_IMA_ERR_STS    0x5Fu
#define BAT_FG_ADDR_LSB       0x61u
#define BAT_FG_MEM_GNT        0x80u
#define BAT_FG_MEM_IACS_SLCT  0x20u
#define BAT_FG_IACS_RDY       0x02u

#define BAT_SMB_CHG_STAGE     0x06u
#define BAT_SMB_CHG_ENABLE    0x42u
#define BAT_SMB_USBIN_RT_STS  0x10u
#define BAT_SMB_BATIF_RT_STS  0x10u
#define BAT_SMB_WDOG_CFG      0x53u
#define BAT_SMB_STAGE_TRICKLE   1u
#define BAT_SMB_STAGE_TAPER     4u
#define BAT_SMB_STAGE_TERMINATE 5u
#define BAT_SMB_USBIN_PLUGIN    (1u << 4)
#define BAT_SMB_BATT_MISSING    ((1u << 5) | (1u << 4))

#define BAT_SOC_FULL     255u
#define BAT_DESIGN_MAH   8600u
#define BAT_TIME_UNKNOWN 0xFFFFu

static inline unsigned battery_percent(unsigned raw) {
    if (raw >= BAT_SOC_FULL) return 100u;
    if (!raw) return 0u;
    return ((raw - 1u) * 98u + 126u) / 253u + 1u;
}

// Guard against uncalibrated MSOC reading <=5% when cell voltage is clearly healthy (>=3350 mV).
static inline unsigned battery_percent_with_voltage_guard(unsigned raw_soc, int voltage_mv) {
    unsigned pct = battery_percent(raw_soc);
    if (pct <= 5u && voltage_mv >= 3350) {
        int mv = voltage_mv > 4350 ? 4350 : voltage_mv;
        unsigned v_pct = (unsigned)((mv - 3300) * 90 / 1050 + 6);
        if (v_pct > pct) pct = v_pct;
    }
    return pct;
}

static inline int battery_voltage_uv(unsigned raw) {
    return (int)((uint64_t)raw * 122070u / 1000u);
}

static inline int battery_current_ua(unsigned raw) {
    int v = (int)(raw & 0x7FFFu);
    if (raw & 0x8000u) v -= 0x8000;
    return (int)(((int64_t)v * 488281) / 1000);
}

static inline int battery_temp_millic(unsigned raw) {
    int v = (int)(raw & 0x3FFu);
    if (raw & 0x200u) v -= 0x400;
    return v * 250;
}

static inline int battery_charging(unsigned stage) {
    return stage >= BAT_SMB_STAGE_TRICKLE && stage <= BAT_SMB_STAGE_TAPER;
}

static inline uint16_t battery_u16(const uint8_t *bytes) {
    return (uint16_t)((uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8));
}

#endif // J716F_BATTERY_BOARD_H
