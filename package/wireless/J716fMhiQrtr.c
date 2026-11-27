// SPDX-License-Identifier: BSD-2-Clause-Patent
// Pure C11 QRTR v1/v2 framing and QMI WLFW (ath11k QCA6390) wire encoder/decoder.
#include "J716fMhiQrtr.h"

#include <string.h>

static inline uint16_t rd_le16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static inline uint32_t rd_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline void wr_le16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

static inline void wr_le32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFFu);
    p[1] = (uint8_t)((v >> 8) & 0xFFu);
    p[2] = (uint8_t)((v >> 16) & 0xFFu);
    p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

static inline void wr_le64(uint8_t *p, uint64_t v) {
    wr_le32(p, (uint32_t)(v & 0xFFFFFFFFULL));
    wr_le32(p + 4u, (uint32_t)(v >> 32));
}

/* ---- QRTR v1 / v2 ---- */

size_t j716f_qrtr_encode_v1(uint8_t *out, size_t cap,
                            uint32_t type, uint32_t src_node, uint32_t src_port,
                            uint32_t dst_node, uint32_t dst_port, uint32_t confirm_rx,
                            const void *payload, size_t payload_len) {
    const size_t padded = (payload_len + 3u) & ~(size_t)3u;
    const size_t total = 32u + padded;
    if (!out || total > cap || (payload_len > 0u && !payload)) return 0u;

    wr_le32(out + 0u,  J716F_QRTR_VER_1);
    wr_le32(out + 4u,  type);
    wr_le32(out + 8u,  src_node);
    wr_le32(out + 12u, src_port);
    wr_le32(out + 16u, confirm_rx);
    wr_le32(out + 20u, (uint32_t)payload_len);
    wr_le32(out + 24u, dst_node);
    wr_le32(out + 28u, dst_port);
    if (payload_len > 0u) memcpy(out + 32u, payload, payload_len);
    if (padded > payload_len) memset(out + 32u + payload_len, 0, padded - payload_len);
    return total;
}

size_t j716f_qrtr_encode_hello(uint8_t *out, size_t cap, uint32_t src_node, uint32_t dst_node) {
    uint8_t ctrl[20];
    memset(ctrl, 0, sizeof(ctrl));
    wr_le32(ctrl, J716F_QRTR_TYPE_HELLO);
    return j716f_qrtr_encode_v1(out, cap, J716F_QRTR_TYPE_HELLO,
                                src_node, J716F_QRTR_PORT_CTRL,
                                dst_node, J716F_QRTR_PORT_CTRL,
                                0u, ctrl, sizeof(ctrl));
}

size_t j716f_qrtr_encode_resume_tx(uint8_t *out, size_t cap,
                                   uint32_t src_node, uint32_t src_port,
                                   uint32_t dst_node, uint32_t dst_port) {
    uint8_t ctrl[20];
    memset(ctrl, 0, sizeof(ctrl));
    wr_le32(ctrl + 0u, J716F_QRTR_TYPE_RESUME_TX);
    wr_le32(ctrl + 4u, src_node);
    wr_le32(ctrl + 8u, src_port);
    return j716f_qrtr_encode_v1(out, cap, J716F_QRTR_TYPE_RESUME_TX,
                                src_node, src_port, dst_node, dst_port,
                                0u, ctrl, sizeof(ctrl));
}

bool j716f_qrtr_decode(const uint8_t *pkt, size_t len,
                       J716fQrtrHeader *out_hdr,
                       const uint8_t **out_payload, size_t *out_payload_len) {
    if (!pkt || len < 4u || !out_hdr) return false;
    const uint32_t ver = pkt[0];
    size_t hl = 0u;
    J716fQrtrHeader h;
    memset(&h, 0, sizeof(h));

    if (ver == J716F_QRTR_VER_1 && len >= 32u) {
        hl = 32u;
        h.version    = rd_le32(pkt + 0u);
        h.type       = rd_le32(pkt + 4u);
        h.src_node   = rd_le32(pkt + 8u);
        h.src_port   = rd_le32(pkt + 12u);
        h.confirm_rx = rd_le32(pkt + 16u);
        h.size       = rd_le32(pkt + 20u);
        h.dst_node   = rd_le32(pkt + 24u);
        h.dst_port   = rd_le32(pkt + 28u);
    } else if (ver == J716F_QRTR_VER_2 && len >= 16u) {
        hl = 16u + (size_t)pkt[3];
        h.version    = ver;
        h.type       = pkt[1];
        h.confirm_rx = pkt[2] & 1u;
        h.size       = rd_le32(pkt + 4u);
        h.src_node   = rd_le16(pkt + 8u);
        h.src_port   = rd_le16(pkt + 10u);
        h.dst_node   = rd_le16(pkt + 12u);
        h.dst_port   = rd_le16(pkt + 14u);
        if (h.src_node == 0xFFFFu) h.src_node = J716F_QRTR_NODE_BCAST;
        if (h.src_port == 0xFFFFu) h.src_port = J716F_QRTR_PORT_CTRL;
        if (h.dst_node == 0xFFFFu) h.dst_node = J716F_QRTR_NODE_BCAST;
        if (h.dst_port == 0xFFFFu) h.dst_port = J716F_QRTR_PORT_CTRL;
    } else {
        return false;
    }

    if (hl > len || (size_t)h.size > len - hl) return false;
    *out_hdr = h;
    if (out_payload) *out_payload = pkt + hl;
    if (out_payload_len) *out_payload_len = (size_t)h.size;
    return true;
}

bool j716f_qrtr_parse_new_server(const uint8_t *payload, size_t len, J716fQrtrServer *out) {
    if (!payload || len < 20u || !out) return false;
    if (rd_le32(payload + 0u) != J716F_QRTR_TYPE_NEW_SERVER) return false;
    out->service  = rd_le32(payload + 4u);
    out->instance = rd_le32(payload + 8u);
    out->node     = rd_le32(payload + 12u);
    out->port     = rd_le32(payload + 16u);
    return true;
}

/* ---- QMI TLV builder/parser ---- */

typedef struct QmiWriter {
    uint8_t *buf;
    size_t cap;
    size_t pos;
    bool overflow;
} QmiWriter;

static void qw_init(QmiWriter *w, uint8_t *buf, size_t cap, uint8_t type, uint16_t txn_id, uint16_t msg_id) {
    w->buf = buf;
    w->cap = cap;
    w->pos = 7u;
    w->overflow = (!buf || cap < 7u);
    if (!w->overflow) {
        buf[0] = type;
        wr_le16(buf + 1u, txn_id);
        wr_le16(buf + 3u, msg_id);
        wr_le16(buf + 5u, 0u);
    }
}

static void qw_tlv(QmiWriter *w, uint8_t tag, const void *data, uint16_t len) {
    if (w->overflow || (size_t)len + 3u > w->cap - w->pos) {
        w->overflow = true;
        return;
    }
    w->buf[w->pos++] = tag;
    wr_le16(w->buf + w->pos, len);
    w->pos += 2u;
    if (len > 0u && data) {
        memcpy(w->buf + w->pos, data, len);
        w->pos += len;
    }
}

static void qw_u8(QmiWriter *w, uint8_t tag, uint8_t v) {
    qw_tlv(w, tag, &v, 1u);
}

static void qw_u32(QmiWriter *w, uint8_t tag, uint32_t v) {
    uint8_t b[4];
    wr_le32(b, v);
    qw_tlv(w, tag, b, 4u);
}

static void qw_u64(QmiWriter *w, uint8_t tag, uint64_t v) {
    uint8_t b[8];
    wr_le64(b, v);
    qw_tlv(w, tag, b, 8u);
}

static size_t qw_finish(QmiWriter *w) {
    if (w->overflow || w->pos < 7u || (w->pos - 7u) > 0xFFFFu) return 0u;
    wr_le16(w->buf + 5u, (uint16_t)(w->pos - 7u));
    return w->pos;
}

bool j716f_qmi_decode_header(const uint8_t *buf, size_t len, J716fQmiHeader *hdr,
                             const uint8_t **tlvs, size_t *tlvs_len) {
    if (!buf || len < 7u || !hdr) return false;
    const uint16_t mlen = rd_le16(buf + 5u);
    if ((size_t)mlen > len - 7u) return false;
    hdr->type = buf[0];
    hdr->txn_id = rd_le16(buf + 1u);
    hdr->msg_id = rd_le16(buf + 3u);
    hdr->msg_len = mlen;
    if (tlvs) *tlvs = buf + 7u;
    if (tlvs_len) *tlvs_len = mlen;
    return true;
}

bool j716f_qmi_parse_resp_result(const uint8_t *tlvs, size_t tlvs_len,
                                 uint16_t *out_result, uint16_t *out_error) {
    if (!tlvs) return false;
    size_t off = 0u;
    while (off + 3u <= tlvs_len) {
        const uint8_t tag = tlvs[off];
        const uint16_t tlen = rd_le16(tlvs + off + 1u);
        off += 3u;
        if ((size_t)tlen > tlvs_len - off) return false;
        if (tag == 0x02u && tlen >= 4u) {
            if (out_result) *out_result = rd_le16(tlvs + off);
            if (out_error)  *out_error  = rd_le16(tlvs + off + 2u);
            return true;
        }
        off += tlen;
    }
    return false;
}

size_t j716f_qmi_build_ind_register_req(uint8_t *out, size_t cap, uint16_t txn_id) {
    QmiWriter w;
    qw_init(&w, out, cap, J716F_QMI_TYPE_REQ, txn_id, J716F_QMI_WLFW_IND_REGISTER_REQ);
    qw_u8(&w,  0x10u, 1u);                         /* fw_ready_enable */
    qw_u32(&w, 0x15u, J716F_QMI_WLFW_CLIENT_ID);   /* client_id = 0x4b4e454c */
    qw_u8(&w,  0x16u, 1u);                         /* request_mem_enable */
    qw_u8(&w,  0x17u, 1u);                         /* fw_mem_ready_enable */
    qw_u8(&w,  0x18u, 1u);                         /* fw_init_done_enable */
    qw_u8(&w,  0x1Bu, 1u);                         /* cal_done_enable */
    return qw_finish(&w);
}

size_t j716f_qmi_build_host_cap_req(uint8_t *out, size_t cap, uint16_t txn_id, bool m3_support) {
    QmiWriter w;
    qw_init(&w, out, cap, J716F_QMI_TYPE_REQ, txn_id, J716F_QMI_WLFW_HOST_CAP_REQ);
    qw_u32(&w, 0x10u, 1u); /* num_clients = 1 */
    qw_u8(&w,  0x14u, 1u); /* bdf_support = 1 */
    if (m3_support) {
        qw_u8(&w, 0x16u, 1u); /* m3_support = 1 */
        qw_u8(&w, 0x17u, 1u); /* m3_cache_support = 1 */
    }
    qw_u8(&w,  0x1Au, 0u); /* cal_done = 0 */
    qw_u8(&w,  0x1Cu, 0u); /* mem_cfg_mode = 0 */
    return qw_finish(&w);
}

bool j716f_qmi_parse_request_mem_ind(const uint8_t *tlvs, size_t tlvs_len,
                                     J716fQmiMemReqSeg *segs, uint32_t max_segs,
                                     uint32_t *out_count) {
    if (!tlvs || !segs || !out_count) return false;
    *out_count = 0u;
    size_t off = 0u;
    while (off + 3u <= tlvs_len) {
        const uint8_t tag = tlvs[off];
        const uint16_t tlen = rd_le16(tlvs + off + 1u);
        off += 3u;
        if ((size_t)tlen > tlvs_len - off) return false;
        if (tag == 0x01u && tlen >= 1u) {
            const uint8_t *p = tlvs + off;
            const size_t end = (size_t)tlen;
            const uint8_t count = p[0];
            size_t pos = 1u;
            uint32_t parsed = 0u;
            for (uint8_t i = 0u; i < count; ++i) {
                if (pos + 9u > end) return false;
                const uint32_t sz = rd_le32(p + pos);
                const uint32_t ty = rd_le32(p + pos + 4u);
                const uint8_t ncfg = p[pos + 8u];
                pos += 9u;
                const size_t cfg_bytes = (size_t)ncfg * 13u; /* offset:8 + size:4 + secure:1 */
                if (pos + cfg_bytes > end) return false;
                pos += cfg_bytes;
                if (parsed < max_segs) {
                    segs[parsed].size = sz;
                    segs[parsed].type = ty;
                    segs[parsed].cfg_count = ncfg;
                    ++parsed;
                }
            }
            *out_count = parsed;
            return true;
        }
        off += tlen;
    }
    return false;
}

size_t j716f_qmi_build_respond_mem_req(uint8_t *out, size_t cap, uint16_t txn_id,
                                       const J716fQmiMemRespSeg *segs, uint32_t count) {
    if (!segs || count == 0u || count > 52u) return 0u;
    QmiWriter w;
    qw_init(&w, out, cap, J716F_QMI_TYPE_REQ, txn_id, J716F_QMI_WLFW_RESPOND_MEM_REQ);
    const uint16_t tlv_len = (uint16_t)(1u + count * 17u);
    if (w.overflow || (size_t)tlv_len + 3u > w.cap - w.pos) return 0u;
    w.buf[w.pos++] = 0x01u;
    wr_le16(w.buf + w.pos, tlv_len);
    w.pos += 2u;
    w.buf[w.pos++] = (uint8_t)count;
    for (uint32_t i = 0u; i < count; ++i) {
        wr_le64(w.buf + w.pos, segs[i].addr);      w.pos += 8u;
        wr_le32(w.buf + w.pos, segs[i].size);      w.pos += 4u;
        wr_le32(w.buf + w.pos, segs[i].type);      w.pos += 4u;
        w.buf[w.pos++] = segs[i].restore;
    }
    return qw_finish(&w);
}

size_t j716f_qmi_build_cap_req(uint8_t *out, size_t cap, uint16_t txn_id) {
    QmiWriter w;
    qw_init(&w, out, cap, J716F_QMI_TYPE_REQ, txn_id, J716F_QMI_WLFW_CAP_REQ);
    return qw_finish(&w);
}

bool j716f_qmi_parse_cap_resp(const uint8_t *tlvs, size_t tlvs_len, J716fQmiCapInfo *out) {
    if (!tlvs || !out) return false;
    memset(out, 0, sizeof(*out));
    uint16_t res = 0xFFFFu, err = 0xFFFFu;
    if (!j716f_qmi_parse_resp_result(tlvs, tlvs_len, &res, &err) || res != 0u) return false;

    size_t off = 0u;
    while (off + 3u <= tlvs_len) {
        const uint8_t tag = tlvs[off];
        const uint16_t tlen = rd_le16(tlvs + off + 1u);
        off += 3u;
        if ((size_t)tlen > tlvs_len - off) return false;
        const uint8_t *p = tlvs + off;

        if (tag == 0x10u && tlen >= 8u) {
            out->has_chip = true;
            out->chip_id = rd_le32(p);
            out->chip_family = rd_le32(p + 4u);
        } else if (tag == 0x11u && tlen >= 4u) {
            out->has_board = true;
            out->board_id = rd_le32(p);
        } else if (tag == 0x12u && tlen >= 4u) {
            out->has_soc = true;
            out->soc_id = rd_le32(p);
        } else if (tag == 0x13u && tlen >= 5u) {
            out->has_fw_ver = true;
            out->fw_version = rd_le32(p);
            /* QMI_STRING inside struct is prefixed with 1-byte length (Linux qmi_encdec). */
            uint8_t slen = p[4];
            if ((size_t)slen > (size_t)tlen - 5u) slen = (uint8_t)(tlen - 5u);
            if (slen > 32u) slen = 32u;
            memcpy(out->fw_timestamp, p + 5u, slen);
            out->fw_timestamp[slen] = '\0';
        } else if (tag == 0x14u && tlen >= 1u) {
            out->has_build_id = true;
            size_t slen = tlen;
            if (slen > 128u) slen = 128u;
            memcpy(out->fw_build_id, p, slen);
            out->fw_build_id[slen] = '\0';
        } else if (tag == 0x15u && tlen >= 1u) {
            out->has_num_macs = true;
            out->num_macs = p[0];
        }
        off += tlen;
    }
    out->valid = true;
    return true;
}

size_t j716f_qmi_build_bdf_download_req(uint8_t *out, size_t cap, uint16_t txn_id,
                                        uint32_t seg_id, uint32_t total_size,
                                        const uint8_t *chunk, uint16_t chunk_len,
                                        bool is_end, uint8_t bdf_type) {
    if (chunk_len > J716F_QMI_BDF_MAX_CHUNK || (chunk_len > 0u && !chunk)) return 0u;
    QmiWriter w;
    qw_init(&w, out, cap, J716F_QMI_TYPE_REQ, txn_id, J716F_QMI_WLFW_BDF_DOWNLOAD_REQ);
    qw_u8(&w,  0x01u, 1u);          /* valid = 1 */
    qw_u32(&w, 0x10u, 0u);          /* file_id = 0 */
    qw_u32(&w, 0x11u, total_size);  /* total_size */
    qw_u32(&w, 0x12u, seg_id);      /* seg_id */
    /* TLV 0x13: data_len (u16 LE) + data[chunk_len] */
    const uint16_t data_tlv_len = (uint16_t)(2u + chunk_len);
    if (w.overflow || (size_t)data_tlv_len + 3u > w.cap - w.pos) return 0u;
    w.buf[w.pos++] = 0x13u;
    wr_le16(w.buf + w.pos, data_tlv_len);
    w.pos += 2u;
    wr_le16(w.buf + w.pos, chunk_len);
    w.pos += 2u;
    if (chunk_len > 0u) {
        memcpy(w.buf + w.pos, chunk, chunk_len);
        w.pos += chunk_len;
    }
    qw_u8(&w,  0x14u, is_end ? 1u : 0u); /* end */
    qw_u8(&w,  0x15u, bdf_type);         /* bdf_type (0=BIN, 1=ELF) */
    return qw_finish(&w);
}

size_t j716f_qmi_build_m3_info_req(uint8_t *out, size_t cap, uint16_t txn_id,
                                   uint64_t m3_pa, uint32_t m3_size) {
    QmiWriter w;
    qw_init(&w, out, cap, J716F_QMI_TYPE_REQ, txn_id, J716F_QMI_WLFW_M3_INFO_REQ);
    qw_u64(&w, 0x01u, m3_pa);
    qw_u32(&w, 0x02u, m3_size);
    return qw_finish(&w);
}
