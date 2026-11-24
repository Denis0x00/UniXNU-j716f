// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host sanitizer unit test for package/power/J716fBatteryBoard.h.
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include "../J716fBatteryBoard.h"

int main(void) {
    // 1. MSOC percentage mapping (0..255)
    assert(battery_percent(0u) == 0u);
    assert(battery_percent(1u) == 1u);
    assert(battery_percent(128u) == 50u);
    assert(battery_percent(254u) == 99u);
    assert(battery_percent(255u) == 100u);
    assert(battery_percent(300u) == 100u);

    // 2. Voltage guard prevents false critical shutdown when MSOC is uncalibrated
    assert(battery_percent_with_voltage_guard(0u, 3200) == 0u);
    assert(battery_percent_with_voltage_guard(0u, 3350) == 10u);
    assert(battery_percent_with_voltage_guard(0u, 4350) == 96u);
    assert(battery_percent_with_voltage_guard(0u, 4500) == 96u);
    assert(battery_percent_with_voltage_guard(128u, 3800) == 50u);

    // 3. Voltage decoding: 122.070 uV/LSB -> 0x8000 = 32768 * 122.070 uV = 3999989 uV (~4000 mV)
    assert(battery_voltage_uv(0u) == 0);
    assert(battery_voltage_uv(0x8000u) / 1000 == 3999);
    assert(battery_voltage_uv(35000u) / 1000 == 4272);

    // 4. Signed 15-bit current decoding: 488.281 uA/LSB
    assert(battery_current_ua(0u) == 0);
    assert(battery_current_ua(2048u) / 1000 == 999);    // +1000 mA
    assert(battery_current_ua(0xFFFFu) == -488);        // -1 LSB
    assert(battery_current_ua(0xF800u) / 1000 == -999); // -2048 LSB (~-1000 mA)

    // 5. 10-bit signed temperature decoding: 0.25 C/LSB (250 mC)
    assert(battery_temp_millic(0u) == 0);
    assert(battery_temp_millic(101u) == 25250);       // +25.25 C
    assert(battery_temp_millic(0x3FFu) == -250);      // -0.25 C
    assert(battery_temp_millic(0x3D8u) == -10000);    // -10.00 C

    // 6. SMB5 charger stage classification
    assert(!battery_charging(0u));
    assert(battery_charging(1u)); // trickle
    assert(battery_charging(2u)); // pre
    assert(battery_charging(3u)); // full-on
    assert(battery_charging(4u)); // taper
    assert(!battery_charging(5u)); // terminate
    assert(!battery_charging(7u)); // disabled

    // 7. Little-endian 16-bit helper
    const uint8_t b[2] = {0x34u, 0x12u};
    assert(battery_u16(b) == 0x1234u);

    printf("J716fBatteryBoardTest: PASS\n");
    return 0;
}
