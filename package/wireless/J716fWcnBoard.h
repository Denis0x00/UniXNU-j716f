// SPDX-License-Identifier: BSD-2-Clause-Patent
// Hardware constants and Qualcomm QCA6390 ("Hastings" v2.0) Bluetooth / Wi-Fi
// protocol helpers for Lenovo TB-J716F (SM8250-AC).
//
// Verified against live TB-J716F running.dtb, /proc/iomem, Linux pinctrl-sm8250.c,
// Linux gcc-sm8250.c, and Linux pcie-qcom.c (cfg_1_9_0 / ops_1_9_0):
//   - All 10 wireless GPIOs (16..21, 79..81, 124) live in TLMM_NORTH (0x0F900000).
//   - Bluetooth UART: QUPv3 Wrapper 0 SE6 (0x00998000, wrapper 0x009C0000, SPI 607 / INTID 639).
//     GPIO 16=CTS, 17=RTS, 18=TX, 19=RX (func 1 = qup6).
//   - Control GPIOs: WLAN_EN=20, BT_EN=21, PCIE0_PERST#=79, PCIE0_CLKREQ#=80,
//     PCIE0_WAKE#=81, SW_CTRL=124.
//   - Shared QCA6390 RPMh VRM rails (cmd-db @ 0x80860000, apps_rsc drv-2 @ 0x18220000):
//     smpa6 (950 mV), smpf2 (950 mV), smpa4 (1800 mV), smpa5 (1900 mV),
//     smpc8 (1350 mV), ldoc5 (1800 mV), ldoa16 (3025 mV).
//   - PCIe0 PHY RPMh VRM rails: ldoa5 (880 mV), ldoa9 (1200 mV).
//   - PCIe0 controller: PARF 0x01C00000, QMP PHY 0x01C06000, DBI 0x60000000,
//     iATU 0x60001000, CFG 0x60100000, MEM 0x60300000..0x63FFFFFF, SID 0x1C00/0x1C01.
#ifndef J716F_WCN_BOARD_H
#define J716F_WCN_BOARD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define J716F_TLMM_NORTH_BASE             0x0F900000ULL
#define J716F_TLMM_NORTH_SIZE             0x00100000u
#define J716F_TLMM_GPIO_STRIDE            0x1000u

/* Wireless GPIOs (all in TLMM_NORTH). */
#define J716F_GPIO_BT_UART_CTS            16u  /* func 1 = qup6 */
#define J716F_GPIO_BT_UART_RTS            17u  /* func 1 = qup6 */
#define J716F_GPIO_BT_UART_TX             18u  /* func 1 = qup6 */
#define J716F_GPIO_BT_UART_RX             19u  /* func 1 = qup6 */
#define J716F_GPIO_WLAN_EN                20u  /* GPIO output */
#define J716F_GPIO_BT_EN                  21u  /* GPIO output */
#define J716F_GPIO_PCIE0_PERST            79u  /* GPIO output (active-low reset) */
#define J716F_GPIO_PCIE0_CLKREQ           80u  /* func 1 = pci_e0 */
#define J716F_GPIO_PCIE0_WAKE             81u  /* GPIO input */
#define J716F_GPIO_SW_CTRL                124u /* GPIO input (QCA6390 strap/status) */

/* SM8250 GCC QUP0 Wrapper CBCR halt registers (relative to GCC 0x00100000). */
#define J716F_GCC_QUP0_M_AHB_CBCR         0x17004u
#define J716F_GCC_QUP0_S_AHB_CBCR         0x17008u
#define J716F_GCC_QUP0_CORE_CBCR          0x23000u
#define J716F_GCC_QUP0_CORE_2X_CBCR       0x23008u

/* SM8250 GCC PCIe0 power, clock, and reset registers (verified from gcc-sm8250.c). */
#define J716F_GCC_PCIE0_BCR               0x6B000u
#define J716F_GCC_PCIE0_GDSCR             0x6B004u
#define J716F_GCC_PCIE0_SLV_Q2A_CBCR      0x6B010u
#define J716F_GCC_PCIE0_SLV_AXI_CBCR      0x6B014u
#define J716F_GCC_PCIE0_MSTR_AXI_CBCR     0x6B01Cu
#define J716F_GCC_PCIE0_CFG_AHB_CBCR      0x6B024u
#define J716F_GCC_PCIE0_AUX_CBCR          0x6B028u
#define J716F_GCC_PCIE0_PIPE_CBCR         0x6B02Cu
#define J716F_GCC_PCIE0_AUX_CMD_RCGR      0x6B038u
#define J716F_GCC_PCIE0_AUX_CFG_RCGR      0x6B03Cu
#define J716F_GCC_PCIE0_PHY_BCR           0x6C01Cu
#define J716F_GCC_PCIE_PHY_AUX_CBCR       0x6F004u
#define J716F_GCC_PCIE_PHY_REFGEN_CMD     0x6F014u
#define J716F_GCC_PCIE_PHY_REFGEN_CFG     0x6F018u
#define J716F_GCC_PCIE0_REFGEN_CBCR       0x6F02Cu
#define J716F_GCC_PCIE_WIFI_CLKREF_EN     0x8C004u
#define J716F_GCC_DDRSS_PCIE_SF_TBU_CBCR  0x8D058u
#define J716F_GCC_PCIE_TBU_CBCR           0x9000Cu
/* Bits 0..5 in SM8250_GCC_APCS_CLOCK_BRANCH_ENA_VOTE (0x52008):
 * 0=slv_axi, 1=mstr_axi, 2=cfg_ahb, 3=aux, 4=pipe, 5=slv_q2a. */
#define J716F_GCC_PCIE0_VOTE8_MASK        0x0000003Fu

/* SM8250 PCIe0 MMIO windows (from live TB-J716F running.dtb /soc/qcom,pcie@1c00000). */
#define J716F_PCIE0_PARF_BASE             0x01C00000ULL
#define J716F_PCIE0_PARF_SIZE             0x00003000u
#define J716F_PCIE0_PHY_BASE              0x01C06000ULL
#define J716F_PCIE0_PHY_SIZE              0x00001000u
#define J716F_PCIE0_DBI_BASE              0x60000000ULL
#define J716F_PCIE0_DBI_SIZE              0x00001000u
#define J716F_PCIE0_ATU_BASE              0x60001000ULL
#define J716F_PCIE0_ATU_SIZE              0x00001000u
#define J716F_PCIE0_CFG_BASE              0x60100000ULL
#define J716F_PCIE0_CFG_SIZE              0x00100000u
#define J716F_PCIE0_MEM_BASE              0x60300000ULL
#define J716F_PCIE0_MEM_SIZE              0x03D00000u

/* PARF registers (Linux pcie-qcom.c cfg_1_9_0 / ops_1_9_0). */
#define J716F_PARF_SYS_CTRL               0x0000u
#define J716F_PARF_PM_CTRL                0x0020u
#define J716F_PARF_PHY_CTRL               0x0040u
#define J716F_PARF_MHI_CLOCK_RESET_CTRL   0x0174u
#define J716F_PARF_AXI_MSTR_WR_ADDR_HALT2 0x01A8u
#define J716F_PARF_LTSSM                  0x01B0u
#define J716F_PARF_DBI_BASE_V2            0x0350u
#define J716F_PARF_DBI_BASE_V2_HI         0x0354u
#define J716F_PARF_SLV_SPACE_V2           0x0358u
#define J716F_PARF_SLV_SPACE_V2_HI        0x035Cu
#define J716F_PARF_ATU_BASE               0x0634u
#define J716F_PARF_ATU_BASE_HI            0x0638u
#define J716F_PARF_DEVICE_TYPE            0x1000u
#define J716F_PARF_BDF_TO_SID_TABLE       0x2000u
#define J716F_PARF_BDF_TO_SID_CFG         0x2C00u

/* DesignWare DBI registers. */
#define J716F_DBI_COMMAND                 0x0004u
#define J716F_DBI_BUS_NUMBERS             0x0018u
#define J716F_DBI_MEM_BASE_LIMIT          0x0020u
#define J716F_DBI_LINK_CTRL_STATUS        0x0080u
#define J716F_DBI_PORT_DEBUG0             0x0728u
#define J716F_DBI_PORT_DEBUG1             0x072Cu

/* QMP Gen3x1 PCIe0 PHY status (from running.dtb: qcom,phy-status-offset=0x814, bit 6). */
#define J716F_PCIE0_PHY_STATUS            0x0814u
#define J716F_PCIE0_PHY_STATUS_BUSY_BIT   (1u << 6)
#define J716F_PCIE0_PHY_SEQ_LEN           99u

/* GENI UART registers (relative to SE6 0x00998000). */
#define J716F_GENI_FORCE_DEFAULT          0x0020u
#define J716F_GENI_OUTPUT_CTRL            0x0024u
#define J716F_GENI_CGC_CTRL               0x0028u
#define J716F_GENI_STATUS                 0x0040u
#define J716F_GENI_M_CLK_CFG              0x0048u
#define J716F_GENI_S_CLK_CFG              0x004Cu
#define J716F_GENI_IF_DISABLE             0x0064u
#define J716F_GENI_FW_REV                 0x0068u
#define J716F_GENI_CLK_SEL                0x007Cu
#define J716F_GENI_UART_LOOPBACK          0x022Cu
#define J716F_GENI_BYTE_GRAN              0x0254u
#define J716F_GENI_DMA_MODE_EN            0x0258u
#define J716F_GENI_UART_TX_CFG            0x025Cu
#define J716F_GENI_TX_PACK0               0x0260u
#define J716F_GENI_TX_PACK1               0x0264u
#define J716F_GENI_UART_TX_WORD           0x0268u
#define J716F_GENI_UART_TX_STOP           0x026Cu
#define J716F_GENI_UART_TX_LEN            0x0270u
#define J716F_GENI_UART_RX_CFG            0x0280u
#define J716F_GENI_RX_PACK0               0x0284u
#define J716F_GENI_RX_PACK1               0x0288u
#define J716F_GENI_UART_RX_WORD           0x028Cu
#define J716F_GENI_UART_RX_STALE          0x0294u
#define J716F_GENI_UART_TX_PARITY         0x02A4u
#define J716F_GENI_UART_RX_PARITY         0x02A8u
#define J716F_GENI_UART_MANUAL_RFR        0x02ACu
#define J716F_GENI_M_CMD0                 0x0600u
#define J716F_GENI_M_CMD_CTRL             0x0604u
#define J716F_GENI_M_IRQ_STATUS           0x0610u
#define J716F_GENI_M_IRQ_EN               0x0614u
#define J716F_GENI_M_IRQ_CLEAR            0x0618u
#define J716F_GENI_S_CMD0                 0x0630u
#define J716F_GENI_S_CMD_CTRL             0x0634u
#define J716F_GENI_S_IRQ_STATUS           0x0640u
#define J716F_GENI_S_IRQ_EN               0x0644u
#define J716F_GENI_S_IRQ_CLEAR            0x0648u
#define J716F_GENI_TX_FIFO                0x0700u
#define J716F_GENI_RX_FIFO                0x0780u
#define J716F_GENI_TX_FIFO_STATUS         0x0800u
#define J716F_GENI_RX_FIFO_STATUS         0x0804u
#define J716F_GENI_RX_WM                  0x0810u
#define J716F_GENI_RX_RFR_WM              0x0814u
#define J716F_GENI_DMA_TX_PTR_L           0x0C30u
#define J716F_GENI_DMA_TX_PTR_H           0x0C34u
#define J716F_GENI_DMA_TX_ATTR            0x0C38u
#define J716F_GENI_DMA_TX_LEN             0x0C3Cu
#define J716F_GENI_DMA_TX_IRQ_STAT        0x0C40u
#define J716F_GENI_DMA_TX_IRQ_CLR         0x0C44u
#define J716F_GENI_DMA_TX_IRQ_EN_SET      0x0C4Cu
#define J716F_GENI_DMA_RX_PTR_L           0x0D30u
#define J716F_GENI_DMA_RX_PTR_H           0x0D34u
#define J716F_GENI_DMA_RX_ATTR            0x0D38u
#define J716F_GENI_DMA_RX_LEN             0x0D3Cu
#define J716F_GENI_DMA_RX_IRQ_STAT        0x0D40u
#define J716F_GENI_DMA_RX_IRQ_CLR         0x0D44u
#define J716F_GENI_DMA_RX_IRQ_EN_SET      0x0D4Cu
#define J716F_GENI_DMA_RX_LEN_IN          0x0D54u
#define J716F_GENI_DMA_RX_RESET           0x0D58u
#define J716F_GENI_GSI_EVENT_EN           0x0E18u
#define J716F_GENI_IRQ_EN                 0x0E1Cu
#define J716F_GENI_HW_PARAM1              0x0E28u
#define J716F_GENI_DMA_CFG                0x0E30u
#define J716F_GENI_DMA_IF_EN              0x2004u
#define J716F_GENI_RFR_OFF                0x80000002u

/* QCA6390 Bluetooth UART baudrate & TLV download protocol constants (Linux btqca.h). */
#define J716F_QCA_BAUD_115200             0x00u
#define J716F_QCA_BAUD_3000000            0x0Eu
#define J716F_QCA_BAUD_4000000            0x0Fu
#define J716F_QCA_BAUD_3200000            0x11u
#define J716F_QCA_TLV_MAX_SEG             243u
#define J716F_QCA_TAG_BD_ADDR             2u
#define J716F_QCA_TAG_HCI                 17u
#define J716F_QCA_TAG_DEEP_SLEEP          27u

#define J716F_WCN_RAIL_COUNT              7u
#define J716F_PCIE_RAIL_COUNT             2u

typedef struct J716fBleAdv {
    bool valid;
    uint8_t evt_type;
    uint8_t addr_type;
    uint8_t addr[6];
    int8_t rssi;
    char name[25];
} J716fBleAdv;

typedef struct J716fWcnRailSpec {
    const char *name;
    uint32_t mv;
} J716fWcnRailSpec;

typedef struct J716fPciePhyStep {
    uint16_t offset;
    uint16_t value;
} J716fPciePhyStep;

typedef struct J716fQcaBtVersion {
    bool valid;
    uint8_t event_code;
    uint32_t product_id;
    uint16_t patch_ver;
    uint16_t rom_ver;
    uint32_t soc_id;
} J716fQcaBtVersion;

/* Exact 99-step SM8250 QMP Gen3x1 PCIe0 PHY sequence from live TB-J716F running.dtb
 * (/soc/qcom,pcie@1c00000 qcom,phy-sequence, all delays 0). Starts with
 * POWER_DOWN_CONTROL=0x03 (0x840) and ends with SW_RESET=0x00 (0x800) & START_CONTROL=0x03 (0x844). */
static const J716fPciePhyStep kJ716fPcie0PhySeq[J716F_PCIE0_PHY_SEQ_LEN] = {
    {0x0840u, 0x03u}, {0x0094u, 0x08u}, {0x0154u, 0x34u}, {0x016Cu, 0x08u},
    {0x0058u, 0x0Fu}, {0x00A4u, 0x42u}, {0x0110u, 0x24u}, {0x011Cu, 0x03u},
    {0x0118u, 0xB4u}, {0x010Cu, 0x02u}, {0x01BCu, 0x11u}, {0x00BCu, 0x82u},
    {0x00D4u, 0x03u}, {0x00D0u, 0x55u}, {0x00CCu, 0x55u}, {0x00B0u, 0x1Au},
    {0x00ACu, 0x0Au}, {0x00C4u, 0x68u}, {0x00E0u, 0x02u}, {0x00DCu, 0xAAu},
    {0x00D8u, 0xABu}, {0x00B8u, 0x34u}, {0x00B4u, 0x14u}, {0x0158u, 0x01u},
    {0x0074u, 0x06u}, {0x007Cu, 0x16u}, {0x0084u, 0x36u}, {0x0078u, 0x06u},
    {0x0080u, 0x16u}, {0x0088u, 0x36u}, {0x01B0u, 0x1Eu}, {0x01ACu, 0xCAu},
    {0x01B8u, 0x18u}, {0x01B4u, 0xA2u}, {0x0050u, 0x07u}, {0x0010u, 0x01u},
    {0x001Cu, 0x31u}, {0x0020u, 0x01u}, {0x0024u, 0xDEu}, {0x0028u, 0x07u},
    {0x0030u, 0x4Cu}, {0x0034u, 0x06u}, {0x029Cu, 0x12u}, {0x0284u, 0x35u},
    {0x023Cu, 0x11u}, {0x051Cu, 0x03u}, {0x0518u, 0x1Cu}, {0x0524u, 0x1Eu},
    {0x04E8u, 0x00u}, {0x04ECu, 0x0Eu}, {0x04F0u, 0x4Au}, {0x04F4u, 0x0Fu},
    {0x05B4u, 0x04u}, {0x0434u, 0x7Fu}, {0x0444u, 0x70u}, {0x0510u, 0x17u},
    {0x04D4u, 0x04u}, {0x04D8u, 0x07u}, {0x0598u, 0xD4u}, {0x059Cu, 0x54u},
    {0x05A0u, 0xDBu}, {0x05A4u, 0x3Bu}, {0x05A8u, 0x31u}, {0x0584u, 0x24u},
    {0x0588u, 0xE4u}, {0x058Cu, 0xECu}, {0x0590u, 0x3Bu}, {0x0594u, 0x36u},
    {0x0570u, 0x3Fu}, {0x0574u, 0x3Fu}, {0x0578u, 0xFFu}, {0x057Cu, 0x7Fu},
    {0x0580u, 0x14u}, {0x04FCu, 0x00u}, {0x04F8u, 0xC0u}, {0x0460u, 0x30u},
    {0x0464u, 0x00u}, {0x05BCu, 0x0Cu}, {0x04DCu, 0x1Bu}, {0x0408u, 0x0Cu},
    {0x0414u, 0x03u}, {0x05B8u, 0x30u}, {0x09A4u, 0x01u}, {0x0C90u, 0x00u},
    {0x0C40u, 0x01u}, {0x0C48u, 0x01u}, {0x0C50u, 0x00u}, {0x0CB4u, 0x33u},
    {0x0CBCu, 0x00u}, {0x0CE0u, 0x58u}, {0x0CA4u, 0x0Fu}, {0x0048u, 0x90u},
    {0x0C1Cu, 0xC1u}, {0x0988u, 0x77u}, {0x0998u, 0x0Bu}, {0x08DCu, 0x0Du},
    {0x09ECu, 0x12u}, {0x0800u, 0x00u}, {0x0844u, 0x03u},
};

/* Qualcomm PARF BDF-to-SID CRC-8 (polynomial 0x07, MSB-first, big-endian BDF). */
static inline uint8_t j716f_pcie_crc8_bdf(uint16_t bdf) {
    uint8_t crc = 0u;
    const uint8_t bytes[2] = {(uint8_t)(bdf >> 8), (uint8_t)(bdf & 0xFFu)};
    for (unsigned j = 0; j < 2u; ++j) {
        crc ^= bytes[j];
        for (unsigned bit = 0; bit < 8u; ++bit) {
            crc = (crc & 0x80u) ? (uint8_t)((crc << 1) ^ 0x07u) : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

static inline void j716f_qca_decode_ver_payload(const uint8_t *p, uint8_t evCode, J716fQcaBtVersion *out) {
    out->event_code = evCode;
    out->product_id = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                      ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    out->patch_ver  = (uint16_t)p[4] | ((uint16_t)p[5] << 8);
    out->rom_ver    = (uint16_t)p[6] | ((uint16_t)p[7] << 8);
    out->soc_id     = (uint32_t)p[8] | ((uint32_t)p[9] << 8) |
                      ((uint32_t)p[10] << 16) | ((uint32_t)p[11] << 24);
    out->valid = true;
}

/* Parse Qualcomm EDL_GET_APP_VER_REQ / QCA_CMD_GET_TARGET_VERSION (0xFC00 subcmd 0x19)
 * response inside a raw H4 RX buffer.
 * Supports both:
 *   1. Standard QCA6390 HCI_EV_CMD_COMPLETE (21 bytes):
 *      04 0E 12 01 00 FC 00 19 <rtype> <prod_id:u32> <patch_ver:u16> <rom_ver:u16> <soc_id:u32>
 *   2. Legacy HCI_EV_VENDOR (17+ bytes):
 *      04 FF <len>=0x0E 00 02 <prod_id:u32> <patch_ver:u16> <rom_ver:u16> <soc_id:u32> */
static inline bool j716f_qca_parse_ver_evt(const uint8_t *buf, size_t len, J716fQcaBtVersion *out) {
    if (!buf || !out || len < 17u) return false;
    out->valid = false;
    out->event_code = 0u;
    out->product_id = 0u;
    out->patch_ver = 0u;
    out->rom_ver = 0u;
    out->soc_id = 0u;

    for (size_t i = 0; i + 21u <= len; ++i) {
        if (buf[i] == 0x04u && buf[i + 1u] == 0x0Eu && buf[i + 2u] >= 0x12u &&
            buf[i + 4u] == 0x00u && buf[i + 5u] == 0xFCu &&
            buf[i + 6u] == 0x00u && buf[i + 7u] == 0x19u) {
            j716f_qca_decode_ver_payload(buf + i + 9u, 0x0Eu, out);
            return true;
        }
    }
    for (size_t i = 0; i + 17u <= len; ++i) {
        if (buf[i] == 0x04u && buf[i + 1u] == 0xFFu && buf[i + 2u] >= 0x0Eu &&
            buf[i + 3u] == 0x00u && buf[i + 4u] == 0x02u) {
            j716f_qca_decode_ver_payload(buf + i + 5u, 0xFFu, out);
            return true;
        }
    }
    return false;
}

/* Patch QCA6390 NVM TLV buffer in-place (Linux btqca.c qca_tlv_check_data layout):
 *   header[0..3]: type(0x02) + 24-bit LE payload length
 *   tags[]:       tag_id(u16), tag_len(u16), rsv1(u32), rsv2(u32), data[tag_len]
 * Patches Tag 17 (HCI IBS bit 0x80 + baud code) and Tag 27 (Deep Sleep bit 0x01).
 * Returns number of tags patched (2 on stock TB-J716F htnv20.bin). */
static inline unsigned j716f_qca_patch_nvm(uint8_t *nvm, size_t len, uint8_t baudCode,
                                           bool enableIbs, bool enableDeepSleep) {
    if (!nvm || len < 16u || nvm[0] != 0x02u) return 0u;
    const size_t tlvLen = (size_t)nvm[1] | ((size_t)nvm[2] << 8) | ((size_t)nvm[3] << 16);
    const size_t limit = (tlvLen + 4u <= len) ? (tlvLen + 4u) : len;
    unsigned patched = 0u;
    size_t off = 4u;
    while (off + 12u <= limit) {
        const uint16_t tid = (uint16_t)nvm[off] | ((uint16_t)nvm[off + 1u] << 8);
        const uint16_t tlen = (uint16_t)nvm[off + 2u] | ((uint16_t)nvm[off + 3u] << 8);
        const size_t dataOff = off + 12u;
        if (dataOff + tlen > limit) break;
        if (tid == J716F_QCA_TAG_HCI && tlen >= 2u) {
            if (enableIbs) nvm[dataOff] |= 0x80u;
            else nvm[dataOff] &= (uint8_t)~0x80u;
            nvm[dataOff + 1u] = baudCode;
            ++patched;
        } else if (tid == J716F_QCA_TAG_DEEP_SLEEP && tlen >= 1u) {
            if (enableDeepSleep) nvm[dataOff] |= 0x01u;
            else nvm[dataOff] &= (uint8_t)~0x01u;
            ++patched;
        }
        off = dataOff + tlen;
    }
    return patched;
}

/* Parse an H4 HCI LE Advertising Report (04 3E <plen> 02 <num_reports> ...) extracting
 * the first advertising report's MAC address, signed RSSI, and ASCII Complete/Short Name. */
static inline bool j716f_hci_parse_le_adv(const uint8_t *pkt, size_t len, J716fBleAdv *out) {
    if (!pkt || !out || len < 15u) return false;
    out->valid = false;
    out->name[0] = '\0';
    if (pkt[0] != 0x04u || pkt[1] != 0x3Eu) return false;
    const size_t plen = pkt[2];
    if (plen + 3u > len || plen < 12u) return false;
    if (pkt[3] != 0x02u || pkt[4] < 1u) return false; /* subevent 0x02 = LE Advertising Report */

    const uint8_t dataLen = pkt[13];
    if (14u + (size_t)dataLen + 1u > plen + 3u) return false;

    out->evt_type  = pkt[5];
    out->addr_type = pkt[6];
    for (unsigned i = 0; i < 6u; ++i) out->addr[i] = pkt[7u + i];
    out->rssi = (int8_t)pkt[14u + dataLen];

    /* Walk AD structures [len, type, payload...] looking for Complete (0x09) or Short (0x08) Name. */
    size_t pos = 14u;
    const size_t end = 14u + dataLen;
    while (pos + 2u <= end) {
        const uint8_t flen = pkt[pos];
        if (flen == 0u || pos + 1u + flen > end) break;
        const uint8_t ftype = pkt[pos + 1u];
        if ((ftype == 0x09u || ftype == 0x08u) && flen > 1u) {
            size_t n = flen - 1u;
            if (n > sizeof(out->name) - 1u) n = sizeof(out->name) - 1u;
            for (size_t k = 0; k < n; ++k) {
                const uint8_t c = pkt[pos + 2u + k];
                out->name[k] = (c >= 0x20u && c < 0x7Fu) ? (char)c : '?';
            }
            out->name[n] = '\0';
            if (ftype == 0x09u) break;
        }
        pos += 1u + flen;
    }
    out->valid = true;
    return true;
}

#ifdef __cplusplus
}
#endif

#endif /* J716F_WCN_BOARD_H */
