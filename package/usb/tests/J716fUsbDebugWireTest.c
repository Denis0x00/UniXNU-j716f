// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host sanitizer unit test for package/usb/J716fUsbDebugWire.h.
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../J716fUsbDebugWire.h"

static uint16_t rd_le16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t rd_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

int main(void) {
    // 1. Device descriptor: 18B, USB 2.0, CDC/ACM 02/02, VID 0x1209, PID 0x0001
    assert(sizeof(kUsbCdcDeviceDesc) == 18u);
    assert(kUsbCdcDeviceDesc[0] == 18u && kUsbCdcDeviceDesc[1] == 0x01u);
    assert(kUsbCdcDeviceDesc[4] == 0x02u && kUsbCdcDeviceDesc[5] == 0x02u);
    assert(rd_le16(kUsbCdcDeviceDesc + 8) == 0x1209u);
    assert(rd_le16(kUsbCdcDeviceDesc + 10) == 0x0001u);

    // 2. Configuration descriptor: HS (512B) and FS (64B)
    uint8_t cfg_hs[67] = {0};
    uint8_t cfg_fs[67] = {0};
    assert(usb_cdc_build_config_desc(cfg_hs, 512u) == 67u);
    assert(usb_cdc_build_config_desc(cfg_fs, 64u) == 67u);
    assert(rd_le16(cfg_hs + 2) == 67u);
    assert(cfg_hs[55] == 0x01u && rd_le16(cfg_hs + 57) == 512u); // EP1 OUT
    assert(cfg_hs[62] == 0x81u && rd_le16(cfg_hs + 64) == 512u); // EP1 IN
    assert(cfg_fs[55] == 0x01u && rd_le16(cfg_fs + 57) == 64u);
    assert(cfg_fs[62] == 0x81u && rd_le16(cfg_fs + 64) == 64u);

    // 3. IEEE 802.3 CRC-32 test vector ("123456789" -> 0xCBF43926)
    const uint8_t vec[9] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
    assert(usb_crc32(vec, 9u) == 0xCBF43926u);
    uint32_t inc = usb_crc32_update(0u, vec, 4u);
    inc = usb_crc32_update(inc, vec + 4u, 5u);
    assert(inc == 0xCBF43926u);

    // 4. GSC1 44-byte header layout and CRC-32 self-check
    uint8_t hdr[44] = {0};
    usb_gsc1_header(hdr, 42u, 1u, 960u, 600u, 960u * 600u * 2u, 2048u, 1024u, 0x11223344u, 0x55667788u);
    const uint8_t kMagic[8] = {0x00, 'G', 'S', 'C', '1', '\r', '\n', 0x1A};
    assert(memcmp(hdr, kMagic, 8u) == 0);
    assert(rd_le32(hdr + 8) == 42u);
    assert(rd_le32(hdr + 12) == 1u);
    assert(rd_le16(hdr + 16) == 960u);
    assert(rd_le16(hdr + 18) == 600u);
    assert(rd_le32(hdr + 20) == 960u * 600u * 2u);
    assert(rd_le32(hdr + 24) == 2048u);
    assert(rd_le32(hdr + 28) == 1024u);
    assert(rd_le32(hdr + 32) == 0x11223344u);
    assert(rd_le32(hdr + 36) == 0x55667788u);
    assert(rd_le32(hdr + 40) == usb_crc32(hdr + 8, 32u));

    printf("J716fUsbDebugWireTest: PASS\n");
    return 0;
}
