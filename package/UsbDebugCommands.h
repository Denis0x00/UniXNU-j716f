// SPDX-License-Identifier: BSD-2-Clause-Patent
// Bounded command & GSC1 framebuffer capture engine for TB-J716F USB CDC terminal.
// Included by UsbDebugCdc.cpp inside its anonymous namespace.

#ifndef J716F_USB_DEBUG_COMMANDS_H
#define J716F_USB_DEBUG_COMMANDS_H

struct UsbReplyRing {
    uint8_t *buf;
    uint32_t cap;
    uint32_t head;
    uint32_t tail;
    uint32_t used;

    void reset() { head = tail = used = 0; }
    uint32_t freeBytes() const { return cap - used; }

    bool write(const void *src, uint32_t len) {
        if (len > freeBytes()) return false;
        const uint8_t *p = static_cast<const uint8_t *>(src);
        for (uint32_t i = 0; i < len; ++i) {
            buf[head] = p[i];
            head = (head + 1u == cap) ? 0u : (head + 1u);
        }
        used += len;
        return true;
    }

    void writeStr(const char *s) {
        if (!s) return;
        uint32_t n = 0;
        while (s[n]) ++n;
        (void)write(s, n);
    }

    uint32_t pop(uint8_t *dst, uint32_t maxLen) {
        uint32_t n = used < maxLen ? used : maxLen;
        for (uint32_t i = 0; i < n; ++i) {
            dst[i] = buf[tail];
            tail = (tail + 1u == cap) ? 0u : (tail + 1u);
        }
        used -= n;
        return n;
    }
};

static bool strEq(const char *a, const char *b) {
    while (*a && *b) {
        if (*a != *b) return false;
        ++a; ++b;
    }
    return *a == *b;
}

static bool strStarts(const char *s, const char *prefix) {
    while (*prefix) {
        if (*s != *prefix) return false;
        ++s; ++prefix;
    }
    return true;
}

static uint32_t parseUint(const char *&p, bool *ok) {
    while (*p == ' ' || *p == '\t') ++p;
    if (*p < '0' || *p > '9') { *ok = false; return 0; }
    uint32_t v = 0;
    while (*p >= '0' && *p <= '9') {
        v = v * 10u + static_cast<uint32_t>(*p - '0');
        ++p;
    }
    *ok = true;
    return v;
}

struct UsbScreenState {
    uint32_t frameId;
    uint32_t maxEdge;
    uint32_t maxFps;
    bool streaming;
    uint64_t nextFrameTick;
    uint8_t *rgbBuf;
    uint32_t rgbCap;

    void init(uint8_t *buf, uint32_t cap) {
        frameId = 0;
        maxEdge = 960;
        maxFps = 2;
        streaming = false;
        nextFrameTick = 0;
        rgbBuf = buf;
        rgbCap = cap;
    }
};

static bool usbCaptureAndQueueGsc1(UsbScreenState &sc, UsbReplyRing &reply, bool isSnapshot) {
    if (!sc.rgbBuf || !&native_legacy_gop_virtual || !&native_legacy_gop_width ||
        !&native_legacy_gop_height || !&native_legacy_gop_bytes) {
        if (isSnapshot) reply.writeStr("GIULIA SCREEN: unavailable (no GOP mapping)\r\n");
        return false;
    }
    const uint32_t srcW = native_legacy_gop_width;
    const uint32_t srcH = native_legacy_gop_height;
    const uint64_t srcBytes = native_legacy_gop_bytes;
    volatile const uint32_t *fb = reinterpret_cast<volatile const uint32_t *>(native_legacy_gop_virtual);
    if (!fb || srcW < 64 || srcH < 64 || srcBytes < static_cast<uint64_t>(srcW) * srcH * 4ULL) {
        if (isSnapshot) reply.writeStr("GIULIA SCREEN: framebuffer not ready\r\n");
        return false;
    }

    uint32_t edge = sc.maxEdge;
    if (edge < 160u) edge = 160u;
    if (edge > 1264u) edge = 1264u;  // ScreenWire MAX_WIDTH is 1264

    uint32_t outW = srcW >= srcH ? edge : (edge * srcW) / srcH;
    uint32_t outH = srcW >= srcH ? (edge * srcH) / srcW : edge;
    if (outW < 1u) outW = 1u;
    if (outW > 1264u) {
        outW = 1264u;
        outH = (1264u * srcH) / srcW;
    }
    if (outH < 1u) outH = 1u;
    if (outH > 2780u) outH = 2780u;

    uint32_t total = outW * outH * 2u;
    while (total > sc.rgbCap && outW > 160u && outH > 100u) {
        outW = (outW * 3u) / 4u;
        outH = (outH * 3u) / 4u;
        total = outW * outH * 2u;
    }
    const uint32_t chunks = (total + 1023u) / 1024u;
    const uint32_t wireNeeded = total + chunks * 44u + 128u;
    if (total > sc.rgbCap || reply.freeBytes() < wireNeeded) {
        return false;
    }

    // Point-sample XNU BGRA8888 GOP framebuffer -> little-endian RGB565
    for (uint32_t y = 0; y < outH; ++y) {
        const uint32_t sy = (y * srcH) / outH;
        volatile const uint32_t *row = fb + static_cast<size_t>(sy) * srcW;
        uint8_t *dst = sc.rgbBuf + static_cast<size_t>(y) * outW * 2u;
        for (uint32_t x = 0; x < outW; ++x) {
            const uint32_t sx = (x * srcW) / outW;
            const uint32_t px = row[sx];
            const uint32_t b = (px >> 0) & 0xFFu;
            const uint32_t g = (px >> 8) & 0xFFu;
            const uint32_t r = (px >> 16) & 0xFFu;
            const uint16_t rgb565 = static_cast<uint16_t>(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
            dst[2u * x]     = static_cast<uint8_t>(rgb565 & 0xFFu);
            dst[2u * x + 1] = static_cast<uint8_t>((rgb565 >> 8) & 0xFFu);
        }
    }

    ++sc.frameId;
    if (sc.frameId == 0u) sc.frameId = 1u;  // ScreenWire requires frame_id != 0
    const uint32_t frameCrc = usb_crc32(sc.rgbBuf, total);
    const uint32_t flags = isSnapshot ? 1u : 0u;

    for (uint32_t off = 0; off < total; off += 1024u) {
        const uint32_t sz = (total - off > 1024u) ? 1024u : (total - off);
        const uint32_t cCrc = usb_crc32(sc.rgbBuf + off, sz);
        uint8_t hdr[44];
        usb_gsc1_header(hdr, sc.frameId, flags, static_cast<uint16_t>(outW),
                        static_cast<uint16_t>(outH), total, off, sz, cCrc, frameCrc);
        (void)reply.write(hdr, 44u);
        (void)reply.write(sc.rgbBuf + off, sz);
    }
    if (isSnapshot) {
        char msg[128];
        snprintf(msg, sizeof(msg), "GIULIA SCREEN: snapshot id=%u %ux%u bytes=%u\r\n",
                 sc.frameId, outW, outH, total);
        reply.writeStr(msg);
    }
    return true;
}

static void usbQueueGsc1Stop(UsbScreenState &sc, UsbReplyRing &reply) {
    ++sc.frameId;
    if (sc.frameId == 0u) sc.frameId = 1u;
    uint8_t hdr[44];
    usb_gsc1_header(hdr, sc.frameId, 2u, 0, 0, 0, 0, 0, 0, 0);
    (void)reply.write(hdr, 44u);
}

#endif // J716F_USB_DEBUG_COMMANDS_H
