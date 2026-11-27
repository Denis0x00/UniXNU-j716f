// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host unit tests for J716fWcnBoard.h (QCA6390 Bluetooth version parser,
// SM8250 PCIe0 BDF-to-SID CRC-8 hash, and QMP PHY table invariants).
#include "../J716fWcnBoard.h"
#include "../J716fMhiBoot.h"
#include "../J716fMhiQrtr.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_qca6390_cmd_complete_parser(void) {
    /* Live 21-byte response captured from TB-J716F QCA6390 v2.0 Hastings over GENI UART6. */
    static const uint8_t kLive[21] = {
        0x04, 0x0E, 0x12, 0x01, 0x00, 0xFC, 0x00, 0x19, 0x0C,
        0x10, 0x00, 0x00, 0x00, /* product_id = 0x00000010 (Hastings) */
        0x2B, 0x0D,             /* patch_ver  = 0x0D2B */
        0x00, 0x02,             /* rom_ver    = 0x0200 (v2.0) */
        0x00, 0x02, 0x0A, 0x40, /* soc_id     = 0x400A0200 (QCA6390 v2.0) */
    };
    J716fQcaBtVersion v = {0};
    assert(j716f_qca_parse_ver_evt(kLive, sizeof(kLive), &v));
    assert(v.valid);
    assert(v.event_code == 0x0Eu);
    assert(v.product_id == 0x00000010u);
    assert(v.patch_ver == 0x0D2Bu);
    assert(v.rom_ver == 0x0200u);
    assert(v.soc_id == 0x400A0200u);

    /* Truncated 20-byte frame must be rejected cleanly. */
    assert(!j716f_qca_parse_ver_evt(kLive, 20u, &v));
    assert(!v.valid);
}

static void test_qca_vendor_event_parser(void) {
    static const uint8_t kVendor[19] = {
        0xFF, 0x00, /* leading noise */
        0x04, 0xFF, 0x0E, 0x00, 0x02,
        0x21, 0x00, 0x00, 0x00,
        0x34, 0x12,
        0x00, 0x02,
        0x00, 0x02, 0x0C, 0x40,
    };
    J716fQcaBtVersion v = {0};
    assert(j716f_qca_parse_ver_evt(kVendor, sizeof(kVendor), &v));
    assert(v.valid);
    assert(v.event_code == 0xFFu);
    assert(v.product_id == 0x21u);
    assert(v.patch_ver == 0x1234u);
    assert(v.rom_ver == 0x0200u);
    assert(v.soc_id == 0x400C0200u);
}

static void test_pcie_sid_crc8(void) {
    /* Root port requester 0x0000 hashes to slot 0. */
    assert(j716f_pcie_crc8_bdf(0x0000u) == 0x00u);
    /* Endpoint bus 1 dev 0 (0x0100) hashes to a non-zero slot distinct from 0. */
    const uint8_t epHash = j716f_pcie_crc8_bdf(0x0100u);
    assert(epHash == 0x15u);
    assert(epHash != 0x00u);
}

static void test_pcie_phy_table_invariants(void) {
    assert(sizeof(kJ716fPcie0PhySeq) / sizeof(kJ716fPcie0PhySeq[0]) == J716F_PCIE0_PHY_SEQ_LEN);
    assert(kJ716fPcie0PhySeq[0].offset == 0x0840u && kJ716fPcie0PhySeq[0].value == 0x03u);
    assert(kJ716fPcie0PhySeq[J716F_PCIE0_PHY_SEQ_LEN - 2u].offset == 0x0800u &&
           kJ716fPcie0PhySeq[J716F_PCIE0_PHY_SEQ_LEN - 2u].value == 0x00u);
    assert(kJ716fPcie0PhySeq[J716F_PCIE0_PHY_SEQ_LEN - 1u].offset == 0x0844u &&
           kJ716fPcie0PhySeq[J716F_PCIE0_PHY_SEQ_LEN - 1u].value == 0x03u);
    for (unsigned i = 0; i < J716F_PCIE0_PHY_SEQ_LEN; ++i) {
        assert((kJ716fPcie0PhySeq[i].offset & 3u) == 0u);
        assert(kJ716fPcie0PhySeq[i].offset < J716F_PCIE0_PHY_SIZE);
    }
}

static void test_qca_nvm_patcher(void) {
    /* Synthetic QCA6390 NVM TLV containing Tag 2, Tag 17 (HCI), and Tag 27 (Deep Sleep). */
    uint8_t nvm[4 + (12 + 6) + (12 + 6) + (12 + 9)] = {
        0x02, 57, 0x00, 0x00,
        /* Tag 2 (BD_ADDR), len 6 */
        0x02, 0x00, 0x06, 0x00, 0, 0, 0, 0, 0, 0, 0, 0,
        0xAD, 0x5A, 0x00, 0x00, 0x00, 0x00,
        /* Tag 17 (HCI), len 6: data[0]=0x95 (IBS on), data[1]=0x0E (3M) */
        0x11, 0x00, 0x06, 0x00, 0, 0, 0, 0, 0, 0, 0, 0,
        0x95, 0x0E, 0x22, 0x00, 0x2E, 0x00,
        /* Tag 27 (Deep Sleep), len 9: data[0]=0x01 */
        0x1B, 0x00, 0x09, 0x00, 0, 0, 0, 0, 0, 0, 0, 0,
        0x01, 0x01, 0x0A, 0x28, 0x00, 0x00, 0x00, 0xFF, 0xFF,
    };
    const unsigned n = j716f_qca_patch_nvm(nvm, sizeof(nvm), J716F_QCA_BAUD_3200000, false, false);
    assert(n == 2u);
    assert(nvm[4 + 18 + 12] == 0x15u);
    assert(nvm[4 + 18 + 13] == J716F_QCA_BAUD_3200000);
    assert(nvm[4 + 18 + 18 + 12] == 0x00u);

    const unsigned n2 = j716f_qca_patch_nvm(nvm, sizeof(nvm), J716F_QCA_BAUD_3000000, true, true);
    assert(n2 == 2u);
    assert(nvm[4 + 18 + 12] == 0x95u);
    assert(nvm[4 + 18 + 13] == J716F_QCA_BAUD_3000000);
    assert(nvm[4 + 18 + 18 + 12] == 0x01u);
}

static void test_ble_adv_parser(void) {
    /* H4 LE Advertising Report: 1 report, addr 66:55:44:33:22:11, AD Flags + Complete Name "AirPods", RSSI -58. */
    static const uint8_t kAdv[] = {
        0x04, 0x3E, 24,
        0x02, 0x01, 0x00, 0x01,
        0x11, 0x22, 0x33, 0x44, 0x55, 0x66,
        12,
        0x02, 0x01, 0x06,
        0x08, 0x09, 'A', 'i', 'r', 'P', 'o', 'd', 's',
        (uint8_t)-58,
    };
    J716fBleAdv adv = {0};
    assert(j716f_hci_parse_le_adv(kAdv, sizeof(kAdv), &adv));
    assert(adv.valid);
    assert(adv.addr[0] == 0x11u && adv.addr[5] == 0x66u);
    assert(adv.rssi == -58);
    assert(adv.name[0] == 'A' && adv.name[6] == 's' && adv.name[7] == '\0');
}

struct MockMhiRegs {
    uint32_t regs[0x400 / 4];
};

static uint32_t mock_mhi_rd(void *ctx, uint32_t reg) {
    struct MockMhiRegs *r = (struct MockMhiRegs *)ctx;
    return reg < sizeof(r->regs) ? r->regs[reg / 4u] : 0xFFFFFFFFu;
}

static void mock_mhi_wr(void *ctx, uint32_t reg, uint32_t val) {
    struct MockMhiRegs *r = (struct MockMhiRegs *)ctx;
    if (reg < sizeof(r->regs)) r->regs[reg / 4u] = val;
}

static void test_mhi_boot_state_machine(void) {
    /* Verify QCA6390 live register layout (BHIOFF=0x100, BHIEOFF=0x224) + full PBL->SBL->AMSS boot. */
    struct MockMhiRegs mock;
    memset(&mock, 0, sizeof(mock));
    mock.regs[J716F_MHI_REGLEN / 4u] = 0x100u;
    mock.regs[J716F_MHI_BHIOFF / 4u] = 0x100u;
    mock.regs[J716F_MHI_BHIEOFF / 4u] = 0x224u;
    mock.regs[(0x100u + J716F_MHI_BHI_EXECENV) / 4u] = J716fMhiEePbl;

    const J716fMhiIo io = {mock_mhi_rd, mock_mhi_wr, &mock, 0x200000u};
    J716fMhiBoot m;
    assert(j716f_mhi_boot_open(&m, &io) == 0);
    assert(m.bhi == 0x100u && m.bhie == 0x224u);

    uint64_t addrs[9];
    for (unsigned i = 0; i < 9u; ++i) addrs[i] = 0x91000000ULL + (uint64_t)i * 0x80000ULL;
    J716fMhiBhieEntry tbl[16];
    const uint32_t n = j716f_mhi_bhie_table(tbl, 16u, addrs, 4372624u, 524288u);
    assert(n == 9u);
    assert(tbl[0].address == 0x91000000ULL && tbl[0].size == 524288u);
    assert(tbl[8].address == 0x91400000ULL && tbl[8].size == 4372624u - 8u * 524288u);

    assert(j716f_mhi_boot_begin(&m, 0u, addrs[0], 524288u, 0x92000000ULL, n * 16u, 0x5EEDu) == J716fMhiSblLoading);
    assert(mock.regs[(0x100u + J716F_MHI_BHI_IMGTXDB) / 4u] == 0x5EEDu);

    mock.regs[(0x100u + J716F_MHI_BHI_STATUS) / 4u] = (J716F_MHI_XFER_SUCCESS << 30) | 0x5EEDu;
    assert(j716f_mhi_boot_step(&m) == J716fMhiWaitReady);

    mock.regs[J716F_MHI_STATUS / 4u] = J716F_MHI_STATUS_READY;
    assert(j716f_mhi_boot_step(&m) == J716fMhiNeedContexts);

    assert(j716f_mhi_boot_contexts_done(&m) == J716fMhiWaitSbl);
    assert(((mock.regs[J716F_MHI_CTRL / 4u] >> J716F_MHI_STATE_SHIFT) & 0xFFu) == J716fMhiStateM0);

    mock.regs[(0x100u + J716F_MHI_BHI_EXECENV) / 4u] = J716fMhiEeSbl;
    assert(j716f_mhi_boot_step(&m) == J716fMhiImageLoading);
    assert(mock.regs[(0x224u + J716F_MHI_BHIE_TXVECDB) / 4u] == 0x5EEEu);

    mock.regs[(0x224u + J716F_MHI_BHIE_TXVECSTATUS) / 4u] = (J716F_MHI_XFER_SUCCESS << 30) | 0x5EEEu;
    assert(j716f_mhi_boot_step(&m) == J716fMhiWaitMission);

    mock.regs[(0x100u + J716F_MHI_BHI_EXECENV) / 4u] = J716fMhiEeAmss;
    assert(j716f_mhi_boot_step(&m) == J716fMhiDone);
    assert(m.error == J716fMhiErrNone);
}

static void test_mhi_qrtr_and_qmi_wire(void) {
    /* 1. MHI ring element encoders/decoders */
    J716fMhiRingElement cmd;
    j716f_mhi_make_cmd_tre(&cmd, J716F_MHI_CH_IPCR_UL, J716F_MHI_CMD_START_CHAN);
    assert(cmd.ptr == 0ULL && cmd.dword0 == 0u);
    assert(j716f_mhi_ev_chid(&cmd) == 20u && j716f_mhi_ev_type(&cmd) == 18u);

    J716fMhiRingElement tre;
    j716f_mhi_make_xfer_tre(&tre, 0x97E00000ULL, 512u, true);
    assert(tre.ptr == 0x97E00000ULL);
    assert(j716f_mhi_ev_len(&tre) == 512u);
    assert(j716f_mhi_ev_type(&tre) == J716F_MHI_TRE_TYPE_TRANSFER);
    assert((tre.dword1 & (1u << 9)) != 0u);

    /* 2. QRTR HELLO round-trip & NEW_SERVER parsing */
    uint8_t pkt[256];
    const size_t hn = j716f_qrtr_encode_hello(pkt, sizeof(pkt), 1u, J716F_QRTR_NODE_BCAST);
    assert(hn == 52u);
    J716fQrtrHeader qh;
    const uint8_t *pl = NULL;
    size_t pl_len = 0u;
    assert(j716f_qrtr_decode(pkt, hn, &qh, &pl, &pl_len));
    assert(qh.version == 1u && qh.type == J716F_QRTR_TYPE_HELLO && qh.src_node == 1u && pl_len == 20u);

    uint8_t ns_payload[20] = {
        0x04, 0x00, 0x00, 0x00, /* NEW_SERVER */
        0x45, 0x00, 0x00, 0x00, /* service 0x45 (WLFW) */
        0x01, 0x00, 0x00, 0x00, /* instance 1 */
        0x07, 0x00, 0x00, 0x00, /* node 7 */
        0x02, 0x00, 0x00, 0x00, /* port 2 */
    };
    J716fQrtrServer srv;
    assert(j716f_qrtr_parse_new_server(ns_payload, sizeof(ns_payload), &srv));
    assert(srv.service == 0x45u && srv.instance == 1u && srv.node == 7u && srv.port == 2u);

    /* 3. QMI IND_REGISTER, HOST_CAP, REQUEST_MEM, RESPOND_MEM, CAP_RESP, BDF, M3 */
    uint8_t qbuf[512];
    const size_t irn = j716f_qmi_build_ind_register_req(qbuf, sizeof(qbuf), 1u);
    assert(irn > 7u);
    J716fQmiHeader mh;
    const uint8_t *tlvs = NULL;
    size_t tlvs_len = 0u;
    assert(j716f_qmi_decode_header(qbuf, irn, &mh, &tlvs, &tlvs_len));
    assert(mh.type == J716F_QMI_TYPE_REQ && mh.txn_id == 1u && mh.msg_id == J716F_QMI_WLFW_IND_REGISTER_REQ);

    const size_t hcn = j716f_qmi_build_host_cap_req(qbuf, sizeof(qbuf), 2u, true);
    assert(hcn > 7u);
    assert(j716f_qmi_decode_header(qbuf, hcn, &mh, &tlvs, &tlvs_len));
    assert(mh.msg_id == J716F_QMI_WLFW_HOST_CAP_REQ);

    /* Synthetic REQUEST_MEM_IND with 2 segments (type 1 16MiB + type 4 4MiB with 1 mem_cfg). */
    const uint8_t req_mem_tlvs[] = {
        0x01, 32, 0,
        2, /* count = 2 */
        0x00, 0x00, 0x00, 0x01, 0x01, 0x00, 0x00, 0x00, 0, /* seg0: 16MiB, type 1, 0 cfg */
        0x00, 0x00, 0x40, 0x00, 0x04, 0x00, 0x00, 0x00, 1, /* seg1: 4MiB, type 4, 1 cfg (13B) */
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x40, 0, 0,
    };
    J716fQmiMemReqSeg msegs[4];
    uint32_t mcnt = 0u;
    assert(j716f_qmi_parse_request_mem_ind(req_mem_tlvs, sizeof(req_mem_tlvs), msegs, 4u, &mcnt));
    assert(mcnt == 2u && msegs[0].size == 0x1000000u && msegs[0].type == 1u);
    assert(msegs[1].size == 0x400000u && msegs[1].type == 4u && msegs[1].cfg_count == 1u);

    const J716fQmiMemRespSeg rsegs[2] = {
        {0x98000000ULL, 0x1000000u, 1u, 0u},
        {0x99000000ULL, 0x400000u,  4u, 0u},
    };
    const size_t rmn = j716f_qmi_build_respond_mem_req(qbuf, sizeof(qbuf), 3u, rsegs, 2u);
    assert(rmn == 7u + 3u + 1u + 2u * 17u);

    /* Synthetic CAP_RESP with chip, board_id=0x0206, soc, fw_ver, build_id, num_macs. */
    const uint8_t cap_tlvs[] = {
        0x02, 4, 0,  0, 0, 0, 0,                          /* resp = SUCCESS */
        0x10, 8, 0,  0x90, 0x63, 0, 0,  0x02, 0, 0, 0,    /* chip_id, chip_family */
        0x11, 4, 0,  0x06, 0x02, 0, 0,                    /* board_id = 0x206 */
        0x12, 4, 0,  0x00, 0x02, 0x0A, 0x40,              /* soc_id */
        0x14, 7, 0,  'Q', 'C', 'A', '6', '3', '9', '0',   /* fw_build_id */
        0x15, 1, 0,  2,                                   /* num_macs */
    };
    J716fQmiCapInfo cap;
    assert(j716f_qmi_parse_cap_resp(cap_tlvs, sizeof(cap_tlvs), &cap));
    assert(cap.valid && cap.has_board && cap.board_id == 0x206u && cap.num_macs == 2u);
    assert(strcmp(cap.fw_build_id, "QCA6390") == 0);

    const uint8_t bdf_chunk[4] = {0x7Fu, 'E', 'L', 'F'};
    const size_t bn = j716f_qmi_build_bdf_download_req(
        qbuf, sizeof(qbuf), 4u, 0u, 4u, bdf_chunk, 4u, true, J716F_QMI_BDF_TYPE_ELF);
    assert(bn > 30u);
    const size_t m3n = j716f_qmi_build_m3_info_req(qbuf, sizeof(qbuf), 5u, 0x97F00000ULL, 266684u);
    assert(m3n == 7u + (3u + 8u) + (3u + 4u));
}

int main(void) {
    test_qca6390_cmd_complete_parser();
    test_qca_vendor_event_parser();
    test_pcie_sid_crc8();
    test_pcie_phy_table_invariants();
    test_qca_nvm_patcher();
    test_ble_adv_parser();
    test_mhi_boot_state_machine();
    test_mhi_qrtr_and_qmi_wire();
    puts("J716fWcnBoardTest: PASS (8 suites)");
    return 0;
}
