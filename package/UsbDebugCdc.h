// SPDX-License-Identifier: BSD-2-Clause-Patent
// Lenovo Xiaoxin Pad Pro 2021 (TB-J716F, SM8250 "kona") USB0 DWC3 + 7nm Femto v2 HS PHY
// register definitions for the live XNU USB CDC-ACM diagnostic terminal.
// Verified against j716f.dtb (ssusb@a600000, hsphy@88e3000) and Kona ClockDxe.

#ifndef J716F_USB_DEBUG_CDC_H
#define J716F_USB_DEBUG_CDC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void native_usb_debug_probe(void);

#ifdef __cplusplus
}
#endif

// --- SM8250 USB0 / SMMU / EUD+HSPHY / GCC physical geometry ----------------
#define J716F_USB0_CORE_BASE          0x0A600000ULL
#define J716F_USB0_CORE_SIZE          0x00100000ULL
#define J716F_USB0_SMMU_SID_0         0x0000u
#define J716F_USB0_SMMU_SID_1         0x0001u
#define J716F_TOUCH_SMMU_SID          0x0043u
#define J716F_UFS_SMMU_SID            0x0060u
#define J716F_MDSS_SMMU_SID           0x0800u
#define J716F_APPS_SMMU_BASE          0x15000000ULL
#define J716F_APPS_SMMU_SIZE          0x00100000ULL

// hsphy@88e3000 ("qcom,usb-hsphy-snps-femto") and non-secure eud_mode_mgr@88e1000.
// Never map or read 0x088e2000..0x088e2fff (eud_mode_mgr2 is qcom,secure-eud-en XPU-locked)!
#define J716F_EUD_BASE                0x088E1000ULL
#define J716F_EUD_SIZE                0x00001000ULL
#define EUD_SW_ATTACH_DET             0x0018u
#define J716F_HSPHY_BASE              0x088E3000ULL
#define J716F_HSPHY_SIZE              0x00001000ULL

// RPMh AOP cmd-db & apps_rsc drv-2 (j716f.dtb)
#define J716F_CMD_DB_BASE             0x80860000ULL
#define J716F_CMD_DB_SIZE             0x00020000ULL
#define J716F_RSC_DRV_BASE            0x18220000ULL
#define J716F_RSC_DRV_SIZE            0x00010000ULL
#define J716F_RSC_TCS_OFFSET          0x0D00u

// Ramoops DBGC persistent ring (BootRamoops.c) & warm-fastboot registers
#define J716F_RAMOOPS_BASE            0xB0000000ULL
#define J716F_RAMOOPS_SIZE            0x00080000ULL
#define J716F_RAMOOPS_SIG             0x43474244u  // "DBGC"
#define J716F_RAMOOPS_HEAD            12u
#define J716F_RAMOOPS_DATA            (J716F_RAMOOPS_SIZE - J716F_RAMOOPS_HEAD)

#define J716F_PS_HOLD_PHYS            0x00C26000ULL
#define J716F_IMEM_REASON_PAGE        0x146BF000ULL
#define J716F_IMEM_REASON_OFFSET      0x0850u
#define J716F_RESTART_BOOTLOADER      0x77665500u
#define J716F_SCM_WDOG_DEBUG_OFF      0xC2000109ULL
#define J716F_SCM_HALT_PMIC_ARB       0xC2000901ULL
#define J716F_PSCI_SYSTEM_RESET       0x84000009ULL

// --- SM8250 GCC (0x00100000) USB30_PRIM & QUSB2PHY offsets -----------------
#define J716F_GCC_BASE                0x00100000ULL
#define J716F_GCC_SIZE                0x00100000ULL

#define GCC_GPLL0_MODE                0x00000u
#define GCC_APCS_GPLL_ENA_VOTE        0x52000u
#define GCC_USB30_PRIM_BCR            0x0F000u
#define GCC_USB30_PRIM_GDSCR          0x0F004u
#define GCC_USB30_PRIM_MASTER_CLK     0x0F010u
#define GCC_USB30_PRIM_SLEEP_CLK      0x0F018u
#define GCC_USB30_PRIM_MOCK_UTMI_CLK  0x0F01Cu
#define GCC_USB30_PRIM_MASTER_CMD_RCGR    0x0F020u
#define GCC_USB30_PRIM_MASTER_CFG_RCGR    0x0F024u
#define GCC_USB30_PRIM_MOCK_UTMI_CMD_RCGR 0x0F038u
#define GCC_USB30_PRIM_MOCK_UTMI_CFG_RCGR 0x0F03Cu
#define GCC_USB3_PRIM_PHY_AUX_CLK     0x0F054u
#define GCC_USB3_PRIM_PHY_COM_AUX_CLK 0x0F058u
#define GCC_USB3_PRIM_PHY_PIPE_CLK    0x0F05Cu
#define GCC_USB3_PRIM_PHY_AUX_CMD_RCGR    0x0F064u
#define GCC_USB3_PRIM_PHY_AUX_CFG_RCGR    0x0F068u
#define GCC_CFG_NOC_USB3_PRIM_AXI_CLK 0x0F07Cu
#define GCC_AGGRE_USB3_PRIM_AXI_CLK   0x0F080u
#define GCC_QUSB2PHY_PRIM_BCR         0x12000u
#define GCC_USB3_PHY_PRIM_BCR         0x50000u
#define GCC_USB3PHY_PHY_PRIM_BCR      0x50004u

#define GCC_GDSCR_SW_COLLAPSE         (1u << 0)
#define GCC_GDSCR_PWR_ON              (1u << 31)
#define GCC_BRANCH_ENABLE_BIT         (1u << 0)
#define GCC_BRANCH_CLK_OFF            (1u << 31)

// --- 7nm Synopsys Femto v2 HS PHY registers (offset from 0x088E3000) -------
#define FEMTO_UTMI_CTRL0              0x3Cu
#define FEMTO_UTMI_CTRL1              0x40u
#define FEMTO_UTMI_CTRL5              0x50u
#define FEMTO_COMMON0                 0x54u
#define FEMTO_COMMON1                 0x58u
#define FEMTO_COMMON2                 0x5Cu
#define FEMTO_CTRL1                   0x60u
#define FEMTO_CTRL2                   0x64u
#define FEMTO_PARAM_OVRD_X1           0x70u  // j716f.dtb: qcom,param-override-seq = <0x83 0x70>
#define FEMTO_CHARGING_DET_CTRL       0x80u
#define FEMTO_CHARGING_DET_OUTPUT     0x84u
#define FEMTO_CFG0                    0x94u
#define FEMTO_REFCLK                  0xA0u
#define FEMTO_RTUNE_SEL               0xB4u

// --- QSCRATCH wrapper registers inside USB0 core_base ----------------------
#define QSCRATCH_REG_OFFSET           0x000F8800u
#define QSCRATCH_GENERAL_CFG          (QSCRATCH_REG_OFFSET + 0x08u)
#define QSCRATCH_HS_PHY_CTRL          (QSCRATCH_REG_OFFSET + 0x10u)
#define QSCRATCH_CGCTL                (QSCRATCH_REG_OFFSET + 0x28u)
#define QSCRATCH_SS_PHY_CTRL          (QSCRATCH_REG_OFFSET + 0x30u)
#define QSCRATCH_PIPE_UTMI_CLK_SEL    (1u << 0)
#define QSCRATCH_PIPE3_PHYSTATUS_SW   (1u << 3)
#define QSCRATCH_PIPE_UTMI_CLK_DIS    (1u << 8)
#define QSCRATCH_UTMI_OTG_VBUS_VALID  (1u << 20)
#define QSCRATCH_SW_SESSVLD_SEL       (1u << 28)
#define QSCRATCH_LANE0_PWR_PRESENT    (1u << 24)

// --- Synopsys DWC3 registers (offset from 0x0A600000) ----------------------
#define DWC3_GSBUSCFG0                0xC100u
#define DWC3_GSBUSCFG1                0xC104u
#define DWC3_GUCTL1                   0xC10Cu
#define DWC3_GCTL                     0xC110u
#define DWC3_GEVTEN                   0xC114u
#define DWC3_GSTS                     0xC118u
#define DWC3_GUCTL                    0xC11Cu
#define DWC3_GSNPSID                  0xC120u
#define DWC3_OSTS_WRAP                0xD010u
#define DWC3_GUSB2PHYCFG(n)           (0xC200u + ((n) * 0x04u))
#define DWC3_GUSB3PIPECTL(n)          (0xC2C0u + ((n) * 0x04u))
#define DWC3_GTXFIFOSIZ(n)            (0xC300u + ((n) * 0x04u))
#define DWC3_GEVNTADRLO(n)            (0xC400u + ((n) * 0x10u))
#define DWC3_GEVNTADRHI(n)            (0xC404u + ((n) * 0x10u))
#define DWC3_GEVNTSIZ(n)              (0xC408u + ((n) * 0x10u))
#define DWC3_GEVNTCOUNT(n)            (0xC40Cu + ((n) * 0x10u))

#define DWC3_DCFG                     0xC700u
#define DWC3_DCTL                     0xC704u
#define DWC3_DEVTEN                   0xC708u
#define DWC3_DSTS                     0xC70Cu
#define DWC3_DALEPENA                 0xC720u
#define DWC3_DEPCMDPAR2(n)            (0xC800u + ((n) * 0x10u))
#define DWC3_DEPCMDPAR1(n)            (0xC804u + ((n) * 0x10u))
#define DWC3_DEPCMDPAR0(n)            (0xC808u + ((n) * 0x10u))
#define DWC3_DEPCMD(n)                (0xC80Cu + ((n) * 0x10u))

#define DWC3_GCTL_DSBLCLKGTNG         (1u << 0)
#define DWC3_GCTL_CORESOFTRESET       (1u << 11)
#define DWC3_GCTL_PRTCAPDIR(n)        ((n) << 12)
#define DWC3_GCTL_PRTCAP_DEVICE       2u

#define DWC3_GUSB2PHYCFG_SUSPHY       (1u << 6)
#define DWC3_GUSB2PHYCFG_ENBLSLPM     (1u << 8)
#define DWC3_GUSB2PHYCFG_PHYSOFTRST   (1u << 31)
#define DWC3_GUSB3PIPECTL_SUSPHY      (1u << 17)

#define DWC3_DCFG_HIGHSPEED           0u
#define DWC3_DCFG_SPEED_MASK          0x7u
#define DWC3_DCFG_DEVADDR(n)          (((n) & 0x7Fu) << 3)
#define DWC3_DCFG_DEVADDR_MASK        (0x7Fu << 3)

#define DWC3_DCTL_HIRD_THRES(n)       (((n) & 0x1Fu) << 24)
#define DWC3_DCTL_CSFTRST             (1u << 30)
#define DWC3_DCTL_RUN_STOP            (1u << 31)

#define DWC3_DSTS_CONNECTSPD_MASK     0x7u
#define DWC3_DSTS_DEVCTRLHLT          (1u << 22)

#define DWC3_GSNPSID_MASK             0xFFFF0000u
#define DWC3_GSNPSID_USB3             0x55330000u
#define DWC3_GSNPSID_USB31            0x55340000u
#define DWC3_GSNPSID_USB31_3331       0x33310000u

#define DWC3_DEPCMD_SETEPCONFIG       0x01u
#define DWC3_DEPCMD_SETTRANSFRESOURCE 0x02u
#define DWC3_DEPCMD_SETSTALL          0x04u
#define DWC3_DEPCMD_CLEARSTALL        0x05u
#define DWC3_DEPCMD_STARTTRANSFER     0x06u
#define DWC3_DEPCMD_ENDTRANSFER       0x08u
#define DWC3_DEPCMD_DEPSTARTCFG       0x09u
#define DWC3_DEPCMD_CMDIOC            (1u << 8)
#define DWC3_DEPCMD_CMDACT            (1u << 10)
#define DWC3_DEPCMD_HIPRI_FORCERM     (1u << 11)
#define DWC3_DEPCMD_STATUS(x)         (((x) >> 12) & 0x0Fu)
#define DWC3_DEPCMD_PARAM(x)          (((x) & 0x7Fu) << 16)
#define DWC3_DEPCMD_RSC_IDX(x)        (((x) >> 16) & 0x7Fu)

#define DWC3_DALEPENA_EP(n)           (1u << (n))

#define DWC3_DEPCFG_EP_TYPE(n)          (((n) & 0x3u) << 1)
#define DWC3_DEPCFG_MAX_PACKET_SIZE(n)  (((n) & 0x7FFu) << 3)
#define DWC3_DEPCFG_FIFO_NUMBER(n)      (((n) & 0x1Fu) << 17)
#define DWC3_DEPCFG_XFER_COMPLETE_EN    (1u << 8)
#define DWC3_DEPCFG_XFER_IN_PROGRESS_EN (1u << 9)
#define DWC3_DEPCFG_XFER_NOT_READY_EN   (1u << 10)
#define DWC3_DEPCFG_EP_NUMBER(n)        (((n) & 0x1Fu) << 25)
#define DWC3_DEPXFERCFG_NUM_XFER_RES(n) ((n) & 0xFFFFu)

#define DWC3_TRB_CTRL_HWO             (1u << 0)
#define DWC3_TRB_CTRL_LST             (1u << 1)
#define DWC3_TRB_CTRL_CHN             (1u << 2)
#define DWC3_TRB_CTRL_CSP             (1u << 3)
#define DWC3_TRB_CTRL_TRBCTL(n)       (((n) & 0x3Fu) << 4)
#define DWC3_TRB_CTRL_ISP_IMI         (1u << 10)
#define DWC3_TRB_CTRL_IOC             (1u << 11)
#define DWC3_TRB_SIZE_MASK            0x00FFFFFFu
#define DWC3_TRB_SIZE_TRBSTS(x)       (((x) >> 28) & 0x0Fu)
#define DWC3_TRBSTS_SETUP_PENDING     2u

#define DWC3_TRBCTL_NORMAL            DWC3_TRB_CTRL_TRBCTL(1)
#define DWC3_TRBCTL_CONTROL_SETUP     DWC3_TRB_CTRL_TRBCTL(2)
#define DWC3_TRBCTL_CONTROL_STATUS2   DWC3_TRB_CTRL_TRBCTL(3)
#define DWC3_TRBCTL_CONTROL_STATUS3   DWC3_TRB_CTRL_TRBCTL(4)
#define DWC3_TRBCTL_CONTROL_DATA      DWC3_TRB_CTRL_TRBCTL(5)

#define DWC3_DEPEVT_XFERCOMPLETE      1u
#define DWC3_DEPEVT_XFERNOTREADY      3u
#define DEPEVT_STATUS_CONTROL_DATA    1u
#define DEPEVT_STATUS_CONTROL_STATUS  2u

#define DWC3_DEV_EVENT_DISCONNECT     0u
#define DWC3_DEV_EVENT_RESET          1u
#define DWC3_DEV_EVENT_CONNECT_DONE   2u
#define DWC3_DEV_EVENT_LINK_STATUS    3u

#endif // J716F_USB_DEBUG_CDC_H
