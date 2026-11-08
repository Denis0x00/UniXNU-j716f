/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

enum {
    SHENG_WCN_WLAN = 0,
    SHENG_WCN_BLUETOOTH = 1,
};

/* Reference-counted QCA6390 PMU power rails. WLAN also votes the PCIe NoC. */
int sheng_wcn_power_get(unsigned radio);
int sheng_wcn_power_put(unsigned radio);
unsigned sheng_wcn_power_owners(void);

#ifdef __cplusplus
}
#endif
