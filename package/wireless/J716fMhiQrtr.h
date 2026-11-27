// SPDX-License-Identifier: BSD-2-Clause-Patent
// Pure C11 Qualcomm MHI IPCR ring helpers + QRTR v1/v2 framing + QMI WLFW
// (ath11k QCA6390) handshake encoder/decoder for Lenovo TB-J716F.
#ifndef J716F_MHI_QRTR_H
#define J716F_MHI_QRTR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- MHI register & ring constants (upstream Linux drivers/bus/mhi/common.h) ---- */
#define J716F_MHI_CHDBOFF                   0x18u
#define J716F_MHI_ERDBOFF                   0x20u
#define J716F_MHI_CRDB_LO                   0x70u
#define J716F_MHI_CRDB_HI                   0x74u

#define J716F_MHI_CH_IPCR_UL                20u
#define J716F_MHI_CH_IPCR_DL                21u
#define J716F_MHI_CH_STATE_ENABLED          1u
#define J716F_MHI_DB_BRST_DISABLE           2u
#define J716F_MHI_CH_TYPE_OUTBOUND          1u
#define J716F_MHI_CH_TYPE_INBOUND           2u

#define J716F_MHI_CMD_RESET_CHAN            16u
#define J716F_MHI_CMD_STOP_CHAN             17u
#define J716F_MHI_CMD_START_CHAN            18u

#define J716F_MHI_TRE_TYPE_TRANSFER         2u
#define J716F_MHI_PKT_STATE_CHANGE_EVENT    0x20u
#define J716F_MHI_PKT_CMD_COMPLETION_EVENT  0x21u
#define J716F_MHI_PKT_TX_EVENT              0x22u
#define J716F_MHI_PKT_EE_EVENT              0x40u

#define J716F_MHI_EV_CC_SUCCESS             1u
#define J716F_MHI_EV_CC_EOT                 2u

typedef struct J716fMhiRingElement {
    uint64_t ptr;
    uint32_t dword0;
    uint32_t dword1;
} J716fMhiRingElement;

static inline void j716f_mhi_make_cmd_tre(J716fMhiRingElement *el, uint32_t chid, uint32_t cmd) {
    el->ptr = 0ULL;
    el->dword0 = 0u;
    el->dword1 = ((chid & 0xFFu) << 24) | ((cmd & 0xFFu) << 16);
}

static inline void j716f_mhi_make_xfer_tre(J716fMhiRingElement *el, uint64_t buf_pa, uint32_t len, bool ieot) {
    el->ptr = buf_pa;
    el->dword0 = len & 0xFFFFu;
    el->dword1 = (J716F_MHI_TRE_TYPE_TRANSFER << 16) | (ieot ? (1u << 9) : 0u);
}

static inline uint32_t j716f_mhi_ev_type(const J716fMhiRingElement *el) {
    return (el->dword1 >> 16) & 0xFFu;
}

static inline uint32_t j716f_mhi_ev_chid(const J716fMhiRingElement *el) {
    return (el->dword1 >> 24) & 0xFFu;
}

static inline uint32_t j716f_mhi_ev_code(const J716fMhiRingElement *el) {
    return (el->dword0 >> 24) & 0xFFu;
}

static inline uint32_t j716f_mhi_ev_len(const J716fMhiRingElement *el) {
    return el->dword0 & 0xFFFFu;
}

/* ---- QRTR v1 / v2 wire protocol ---- */
#define J716F_QRTR_VER_1                    1u
#define J716F_QRTR_VER_2                    3u
#define J716F_QRTR_LOCAL_NODE               1u
#define J716F_QRTR_NODE_BCAST               0xFFFFFFFFu
#define J716F_QRTR_PORT_CTRL                0xFFFFFFFEu

#define J716F_QRTR_TYPE_DATA                1u
#define J716F_QRTR_TYPE_HELLO               2u
#define J716F_QRTR_TYPE_BYE                 3u
#define J716F_QRTR_TYPE_NEW_SERVER          4u
#define J716F_QRTR_TYPE_DEL_SERVER          5u
#define J716F_QRTR_TYPE_DEL_CLIENT          6u
#define J716F_QRTR_TYPE_RESUME_TX           7u
#define J716F_QRTR_TYPE_EXIT                8u
#define J716F_QRTR_TYPE_PING                9u
#define J716F_QRTR_TYPE_NEW_LOOKUP          10u
#define J716F_QRTR_TYPE_DEL_LOOKUP          11u

#define J716F_QRTR_WLFW_SERVICE_ID          0x45u

typedef struct J716fQrtrHeader {
    uint32_t version;
    uint32_t type;
    uint32_t src_node;
    uint32_t src_port;
    uint32_t dst_node;
    uint32_t dst_port;
    uint32_t size;
    uint32_t confirm_rx;
} J716fQrtrHeader;

typedef struct J716fQrtrServer {
    uint32_t service;
    uint32_t instance;
    uint32_t node;
    uint32_t port;
} J716fQrtrServer;

size_t j716f_qrtr_encode_v1(uint8_t *out, size_t cap,
                            uint32_t type, uint32_t src_node, uint32_t src_port,
                            uint32_t dst_node, uint32_t dst_port, uint32_t confirm_rx,
                            const void *payload, size_t payload_len);

size_t j716f_qrtr_encode_hello(uint8_t *out, size_t cap, uint32_t src_node, uint32_t dst_node);

size_t j716f_qrtr_encode_resume_tx(uint8_t *out, size_t cap,
                                   uint32_t src_node, uint32_t src_port,
                                   uint32_t dst_node, uint32_t dst_port);

bool j716f_qrtr_decode(const uint8_t *pkt, size_t len,
                       J716fQrtrHeader *out_hdr,
                       const uint8_t **out_payload, size_t *out_payload_len);

bool j716f_qrtr_parse_new_server(const uint8_t *payload, size_t len, J716fQrtrServer *out);

/* ---- QMI WLFW (ath11k QCA6390) message & TLV definitions ---- */
#define J716F_QMI_TYPE_REQ                  0x00u
#define J716F_QMI_TYPE_RESP                 0x02u
#define J716F_QMI_TYPE_IND                  0x04u

#define J716F_QMI_WLFW_IND_REGISTER_REQ     0x0020u
#define J716F_QMI_WLFW_FW_READY_IND         0x0021u
#define J716F_QMI_WLFW_CAP_REQ              0x0024u
#define J716F_QMI_WLFW_BDF_DOWNLOAD_REQ     0x0025u
#define J716F_QMI_WLFW_HOST_CAP_REQ         0x0034u
#define J716F_QMI_WLFW_REQUEST_MEM_IND      0x0035u
#define J716F_QMI_WLFW_RESPOND_MEM_REQ      0x0036u
#define J716F_QMI_WLFW_FW_MEM_READY_IND     0x0037u
#define J716F_QMI_WLFW_FW_INIT_DONE_IND     0x0038u
#define J716F_QMI_WLFW_M3_INFO_REQ          0x003Cu
#define J716F_QMI_WLFW_COLD_CAL_DONE_IND    0x003Eu

#define J716F_QMI_WLFW_CLIENT_ID            0x4B4E454Cu
#define J716F_QMI_BDF_MAX_CHUNK             6144u
#define J716F_QMI_BDF_TYPE_BIN              0u
#define J716F_QMI_BDF_TYPE_ELF              1u
#define J716F_QMI_MAX_MEM_SEGS              16u

typedef struct J716fQmiHeader {
    uint8_t type;
    uint16_t txn_id;
    uint16_t msg_id;
    uint16_t msg_len;
} J716fQmiHeader;

typedef struct J716fQmiMemReqSeg {
    uint32_t size;
    uint32_t type;
    uint8_t cfg_count;
} J716fQmiMemReqSeg;

typedef struct J716fQmiMemRespSeg {
    uint64_t addr;
    uint32_t size;
    uint32_t type;
    uint8_t restore;
} J716fQmiMemRespSeg;

typedef struct J716fQmiCapInfo {
    bool valid;
    bool has_chip;
    bool has_board;
    bool has_soc;
    bool has_fw_ver;
    bool has_build_id;
    bool has_num_macs;
    uint32_t chip_id;
    uint32_t chip_family;
    uint32_t board_id;
    uint32_t soc_id;
    uint32_t fw_version;
    uint8_t num_macs;
    char fw_timestamp[33];
    char fw_build_id[129];
} J716fQmiCapInfo;

bool j716f_qmi_decode_header(const uint8_t *buf, size_t len, J716fQmiHeader *hdr,
                             const uint8_t **tlvs, size_t *tlvs_len);

bool j716f_qmi_parse_resp_result(const uint8_t *tlvs, size_t tlvs_len,
                                 uint16_t *out_result, uint16_t *out_error);

size_t j716f_qmi_build_ind_register_req(uint8_t *out, size_t cap, uint16_t txn_id);
size_t j716f_qmi_build_host_cap_req(uint8_t *out, size_t cap, uint16_t txn_id, bool m3_support);

bool j716f_qmi_parse_request_mem_ind(const uint8_t *tlvs, size_t tlvs_len,
                                     J716fQmiMemReqSeg *segs, uint32_t max_segs,
                                     uint32_t *out_count);

size_t j716f_qmi_build_respond_mem_req(uint8_t *out, size_t cap, uint16_t txn_id,
                                       const J716fQmiMemRespSeg *segs, uint32_t count);

size_t j716f_qmi_build_cap_req(uint8_t *out, size_t cap, uint16_t txn_id);

bool j716f_qmi_parse_cap_resp(const uint8_t *tlvs, size_t tlvs_len, J716fQmiCapInfo *out);

size_t j716f_qmi_build_bdf_download_req(uint8_t *out, size_t cap, uint16_t txn_id,
                                        uint32_t seg_id, uint32_t total_size,
                                        const uint8_t *chunk, uint16_t chunk_len,
                                        bool is_end, uint8_t bdf_type);

size_t j716f_qmi_build_m3_info_req(uint8_t *out, size_t cap, uint16_t txn_id,
                                   uint64_t m3_pa, uint32_t m3_size);

#ifdef __cplusplus
}
#endif

#endif /* J716F_MHI_QRTR_H */
