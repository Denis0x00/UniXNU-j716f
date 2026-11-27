// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host unit tests for J716fWcnBoard.h (QCA6390 Bluetooth version parser,
// SM8250 PCIe0 BDF-to-SID CRC-8 hash, and QMP PHY table invariants).
#include "../J716fWcnBoard.h"

#include <assert.h>
#include <stdio.h>

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

int main(void) {
    test_qca6390_cmd_complete_parser();
    test_qca_vendor_event_parser();
    test_pcie_sid_crc8();
    test_pcie_phy_table_invariants();
    puts("J716fWcnBoardTest: PASS (4 suites)");
    return 0;
}
