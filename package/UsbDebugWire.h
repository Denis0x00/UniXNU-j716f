// SPDX-License-Identifier: BSD-2-Clause-Patent
// Pure protocol helpers for TB-J716F USB CDC-ACM diagnostic terminal:
//   1. Driverless Windows 10/11 usbser.sys CDC-ACM USB descriptors (HS 512B / FS 64B)
//   2. IEEE 802.3 CRC-32 matching Python binascii.crc32
//   3. GSC1 RGB565 frame packet header builder matching UniXNU scripts/debug_screen.py

#ifndef J716F_USB_DEBUG_WIRE_H
#define J716F_USB_DEBUG_WIRE_H

#include <stddef.h>
#include <stdint.h>

// USB VID/PID: 0x1209:0x0001 (pid.codes test PID), Class 0x02/0x02 binds usbser.sys automatically.
static const uint8_t kUsbCdcDeviceDesc[18] = {
    18, 0x01, 0x00, 0x02,       // bLength, DEVICE, USB 2.00
    0x02, 0x02, 0x00, 64,       // CDC class/ACM subclass, bMaxPacketSize0 = 64
    0x09, 0x12, 0x01, 0x00,     // idVendor = 0x1209, idProduct = 0x0001
    0x00, 0x01,                 // bcdDevice = 1.00
    1, 2, 3, 1                  // iManufacturer, iProduct, iSerialNumber, bNumConfigurations
};

// Build 67-byte CDC-ACM Configuration Descriptor for either High-Speed (bulkMps=512) or Full-Speed (bulkMps=64).
// Endpoint mapping matches DWC3 physical endpoints:
//   0x82 -> dep5 (EP2 IN, Interrupt, 16 bytes, FIFO 2)
//   0x01 -> dep2 (EP1 OUT, Bulk, 512/64 bytes, FIFO 0)
//   0x81 -> dep3 (EP1 IN, Bulk, 512/64 bytes, FIFO 1)
static inline unsigned usb_cdc_build_config_desc(uint8_t *out, uint16_t bulkMps) {
    const uint8_t mpsLo = (uint8_t)(bulkMps & 0xFFu);
    const uint8_t mpsHi = (uint8_t)((bulkMps >> 8) & 0xFFu);
    const uint8_t tpl[67] = {
        9, 0x02, 67, 0, 2, 1, 0, 0x80, 0xFA,          // Config 1, 2 interfaces, bus-powered 500mA
        9, 0x04, 0, 0, 1, 0x02, 0x02, 0x01, 0,        // Interface 0: CDC Control, ACM, ATv250
        5, 0x24, 0x00, 0x10, 0x01,                    // CDC Header 1.10
        5, 0x24, 0x01, 0x00, 0x01,                    // Call Management -> Data Interface 1
        4, 0x24, 0x02, 0x02,                          // ACM Capabilities (Line Coding + Control Line State)
        5, 0x24, 0x06, 0x00, 0x01,                    // Union: Control 0, Data 1
        7, 0x05, 0x82, 0x03, 16, 0, 9,                // EP2 IN (0x82), Interrupt, 16B, bInterval=9 (32ms HS)
        9, 0x04, 1, 0, 2, 0x0A, 0x00, 0x00, 0,        // Interface 1: CDC Data
        7, 0x05, 0x01, 0x02, mpsLo, mpsHi, 0,         // EP1 OUT (0x01 -> dep2), Bulk
        7, 0x05, 0x81, 0x02, mpsLo, mpsHi, 0          // EP1 IN  (0x81 -> dep3), Bulk
    };
    for (unsigned i = 0; i < 67; ++i) out[i] = tpl[i];
    return 67;
}

static inline uint32_t usb_crc32_update(uint32_t crc, const uint8_t *data, size_t len) {
    uint32_t c = ~crc;
    for (size_t i = 0; i < len; ++i) {
        c ^= data[i];
        for (unsigned b = 0; b < 8; ++b) {
            c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
        }
    }
    return ~c;
}

static inline uint32_t usb_crc32(const uint8_t *data, size_t len) {
    return usb_crc32_update(0u, data, len);
}

static inline void usb_put_le16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

static inline void usb_put_le32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

// GSC1 packet header (44 bytes): struct.Struct('<8sIIHHIIIIII') in scripts/debug_screen.py:
//   [0..7]   MAGIC = b'\0GSC1\r\n\x1a'
//   [8..11]  frame_id
//   [12..15] flags (bit 0: snapshot=1 / live=0, bit 1: stop control packet=2)
//   [16..17] width  (1..1264)
//   [18..19] height (1..2780)
//   [20..23] total  (width * height * 2, or 0 for control)
//   [24..27] offset
//   [28..31] size   (1..1024, or 0 for control)
//   [32..35] crc       (CRC32 of chunk payload, or 0 for control)
//   [36..39] frame_crc (CRC32 of entire RGB565 frame, or 0 for control)
//   [40..43] header_crc (CRC32 of bytes [8..39])
static inline void usb_gsc1_header(uint8_t out[44], uint32_t frameId, uint32_t flags,
                                   uint16_t width, uint16_t height, uint32_t total,
                                   uint32_t offset, uint32_t size,
                                   uint32_t chunkCrc, uint32_t frameCrc) {
    static const uint8_t kMagic[8] = {0x00, 'G', 'S', 'C', '1', '\r', '\n', 0x1A};
    for (unsigned i = 0; i < 8; ++i) out[i] = kMagic[i];
    usb_put_le32(out + 8,  frameId);
    usb_put_le32(out + 12, flags);
    usb_put_le16(out + 16, width);
    usb_put_le16(out + 18, height);
    usb_put_le32(out + 20, total);
    usb_put_le32(out + 24, offset);
    usb_put_le32(out + 28, size);
    usb_put_le32(out + 32, chunkCrc);
    usb_put_le32(out + 36, frameCrc);
    usb_put_le32(out + 40, usb_crc32(out + 8, 32));
}

#endif // J716F_USB_DEBUG_WIRE_H
