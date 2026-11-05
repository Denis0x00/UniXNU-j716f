// SPDX-License-Identifier: BSD-2-Clause-Patent
#include <assert.h>
#include <stdio.h>
#include "TouchBoard.h"

int main(void) {
    assert(TB_QUP_WRAPPER_BASE == 0x00ac0000ULL);
    assert(TB_SE_BASE == 0x00a94000ULL);
    assert(TB_SE_SIZE == 0x4000ULL);
    assert(TB_QUP_SMMU_SID == 0x0043u);
    assert(TB_APPS_SMMU_BASE == 0x15000000ULL);
    assert(TB_CMD_DB_BASE == 0x80860000ULL);
    assert(TB_RSC_DRV_BASE == 0x18220000ULL);
    assert(TB_GCC_BASE == 0x00100000ULL);
    assert(TB_TLMM_SOUTH_BASE == 0x0f500000ULL);
    assert(TB_GPIO_I2C_SDA == 36u);
    assert(TB_GPIO_I2C_SCL == 37u);
    assert(TB_GPIO_RESET == 38u);
    assert(TB_GPIO_IRQ == 39u);

    assert(GTX8_ADDR_PRIMARY == 0x5du);
    assert(GTX8_ADDR_FALLBACK == 0x14u);
    assert(GTX8_REG_CMD == 0x8040u);
    assert(GTX8_REG_HANDSHAKE == 0x8043u);
    assert(GTX8_REG_VERSION == 0x8240u);
    assert(GTX8_REG_TOUCH_DATA == 0x824du);
    assert(TB_PANEL_MAX_X == 2560u);
    assert(TB_PANEL_MAX_Y == 1600u);

    assert(tb_src_hz(0) == 19200000ULL);
    assert(tb_src_hz(1) == 600000000ULL);
    assert(tb_src_hz(6) == 300000000ULL);
    assert(tb_src_hz(7) == 0ULL);

    // 19.2 MHz undivided (hid = 1 -> *2 / 2 = 19.2 MHz)
    assert(tb_calc_rate(19200000ULL, 0, 0, 0, 1) == 19200000ULL);
    // 600 MHz / 6 = 100 MHz (hid = 11 -> *2 / 12 = /6)
    assert(tb_calc_rate(600000000ULL, 0, 0, 0, 11) == 100000000ULL);
    // DFS level with src=0 (19.2 MHz), mode=0, hid=1 -> 19.2 MHz
    assert(tb_dfs_level_hz((0u << 8) | 1u, 0, 0) == 19200000ULL);

    // Goodix GT7386 8-bit rolling tracking ID (0..255) -> stable 0..9 contact slot mapping across >300 strokes
    int slot_raw[10];
    for (unsigned i = 0; i < 10u; ++i) slot_raw[i] = -1;
    for (unsigned stroke = 0; stroke < 300u; ++stroke) {
        uint8_t raw_id = (uint8_t)(stroke & 0xffu);
        int out_slot = -1;
        tb_update_slots(slot_raw, 1u, &raw_id, &out_slot);
        assert(out_slot == 0);
        // Hold for a second frame: same slot
        int held_slot = -1;
        tb_update_slots(slot_raw, 1u, &raw_id, &held_slot);
        assert(held_slot == 0);
        // Lift (count = 0): slot freed
        tb_update_slots(slot_raw, 0u, &raw_id, &out_slot);
        assert(slot_raw[0] == -1);
    }

    // Simultaneous lift of finger A (raw 0x1a in slot 0) + press of finger B (raw 0x1b) in the same frame must not reuse slot 0
    {
        uint8_t id_a = 0x1au, id_b = 0x1bu;
        int slot_a = -1, slot_b = -1;
        tb_update_slots(slot_raw, 1u, &id_a, &slot_a);
        assert(slot_a == 0);
        tb_update_slots(slot_raw, 1u, &id_b, &slot_b);
        assert(slot_b == 1);
        assert(slot_raw[0] == -1 && slot_raw[1] == 0x1b);
        tb_update_slots(slot_raw, 0u, &id_b, &slot_b);
    }

    puts("j716f TouchBoard tests passed");
    return 0;
}
