// SPDX-License-Identifier: BSD-2-Clause-Patent
// Hardware bring-up and DWC3 gadget state machine for TB-J716F (SM8250).
// Verified against OEM UsbConfigDxe.efi, UsbfnDwc3Dxe.efi, ClockDxe.efi, and j716f.dtb.
// Included by UsbDebugCdc.cpp inside its anonymous namespace.

#ifndef J716F_USB_DEBUG_HW_H
#define J716F_USB_DEBUG_HW_H

// 64 KiB physically contiguous Normal-Cacheable DMA layout (explicit ARM64 cache maintenance):
//   0x0000..0x0FFF  Event ring (4 KiB, DWC3 write-only after init)
//   0x1000..0x11FF  EP0 TRBs (up to 32 TRBs, 64B-aligned)
//   0x1200..0x1FFF  EP0 control buffer (64B-aligned)
//   0x2000..0x203F  EP1 OUT (dep2) TRBs (64B-aligned)
//   0x2100..0x28FF  EP1 OUT RX buffer (2 KiB, 64B-aligned)
//   0x3000..0x31FF  EP1 IN  (dep3) TRBs (64B-aligned)
//   0x4000..0xFFFF  EP1 IN  TX buffer (48 KiB, 4096B-aligned)
#define DMA_TOTAL_BYTES   0x10000u
#define DMA_EVT_OFF       0x0000u
#define DMA_EP0_TRB_OFF   0x1000u
#define DMA_EP0_BUF_OFF   0x1200u
#define DMA_EP2_TRB_OFF   0x2000u
#define DMA_EP2_BUF_OFF   0x2100u
#define DMA_EP3_TRB_OFF   0x3000u
#define DMA_EP3_BUF_OFF   0x4000u
#define DMA_EP3_MAX_CHUNK 0x4000u  // 16 KiB per Bulk IN burst

// op: 0 = clean (dc cvac), 1 = clean+inval (dc civac), 2 = inval (dc ivac)
static inline void usbCacheRange(const void *p, size_t len, int op) {
    if (!len) return;
    uint64_t ctr = 0;
    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
    const size_t line = 4u << ((ctr >> 16) & 15u);
    uintptr_t a = reinterpret_cast<uintptr_t>(p) & ~(line - 1u);
    const uintptr_t end = reinterpret_cast<uintptr_t>(p) + len;
    __asm__ volatile("dsb sy" ::: "memory");
    for (; a < end; a += line) {
        if (op == 0) __asm__ volatile("dc cvac, %0" :: "r"(a) : "memory");
        else if (op == 1) __asm__ volatile("dc civac, %0" :: "r"(a) : "memory");
        else __asm__ volatile("dc ivac, %0" :: "r"(a) : "memory");
    }
    __asm__ volatile("dsb sy" ::: "memory");
}

struct UsbHw {
    OSSharedPtr<IOMemoryMap> coreMap, gccMap, phyMap, eudMap, smmuMap;
    IOBufferMemoryDescriptor *dmaDesc = nullptr;
    volatile uint32_t *core    = nullptr;
    volatile uint32_t *gcc     = nullptr;
    volatile uint32_t *phy     = nullptr;
    volatile uint32_t *eud     = nullptr;
    volatile uint32_t *smmu    = nullptr;
    uint8_t *dmaVirt           = nullptr;
    uint64_t dmaPhys           = 0;

    uint32_t evtOff         = 0;
    uint16_t bulkMps        = 512;
    int ep0Stage            = 0;
    bool ep0Three           = false;
    bool ep0In              = false;
    bool ep0Zlp             = false;
    unsigned ep0StatusDep   = 1;
    unsigned ep0Armed       = 0;
    unsigned ep0Trbs        = 0;

    uint8_t configured      = 0;
    uint16_t lineState      = 0;
    bool hostReady          = false;
    bool needReplay         = true;
    bool ep2Armed           = false;
    bool ep3Busy            = false;
    uint32_t ep2RscIdx      = 0;
    uint32_t ep3RscIdx      = 0;
    uint64_t ep3ArmTick     = 0;
    uint32_t usbEvents      = 0;
    uint32_t usbResets      = 0;
    uint32_t usbCfgCount    = 0;
    int lastCmdSt           = 0;
    uint32_t smmuBank       = 0;
    uint32_t smmuOrigSmr    = 0;
    uint32_t smmuOrigS2cr   = 0;
    uint32_t phyInitMode    = 0;
    uint8_t lineCoding[7]   = {0x00, 0xC2, 0x01, 0x00, 0x00, 0x00, 0x08};

    void initDefaults() {
        bulkMps        = 512u;
        ep0StatusDep   = 1u;
        needReplay     = true;
        lineCoding[0]  = 0x00;
        lineCoding[1]  = 0xC2;
        lineCoding[2]  = 0x01;
        lineCoding[3]  = 0x00;
        lineCoding[4]  = 0x00;
        lineCoding[5]  = 0x00;
        lineCoding[6]  = 0x08;
    }

    uint32_t rd(uint32_t off) const { return core[off / 4u]; }
    void wr(uint32_t off, uint32_t v) const {
        core[off / 4u] = v;
        __asm__ volatile("dsb sy" ::: "memory");
    }
    uint32_t grd(uint32_t off) const { return gcc[off / 4u]; }
    void gwr(uint32_t off, uint32_t v) const {
        gcc[off / 4u] = v;
        __asm__ volatile("dsb sy" ::: "memory");
    }
    uint32_t prd(uint32_t off) const { return phy[off / 4u]; }
    void pwr(uint32_t off, uint32_t v) const {
        phy[off / 4u] = v;
        __asm__ volatile("dsb sy" ::: "memory");
    }
    void pmask(uint32_t off, uint32_t mask, uint32_t val) const {
        pwr(off, (prd(off) & ~mask) | (val & mask));
    }
};

static uintptr_t mapRangeUncached(uint64_t phys, uint64_t len, OSSharedPtr<IOMemoryMap> &keep) {
    IODeviceMemory *mem = IODeviceMemory::withRange(phys, len);
    if (!mem) return 0;
    keep = OSSharedPtr<IOMemoryMap>(mem->map(kIOMapAnywhere | kIOMapInhibitCache), OSNoRetain);
    mem->release();
    return keep ? static_cast<uintptr_t>(keep->getVirtualAddress()) : 0;
}

static uint32_t smmuIoRd(void *ctx, unsigned off) {
    return static_cast<volatile uint32_t *>(ctx)[off / 4u];
}
static void smmuIoWr(void *ctx, unsigned off, uint32_t val) {
    static_cast<volatile uint32_t *>(ctx)[off / 4u] = val;
    __asm__ volatile("dsb sy" ::: "memory");
}

static int rpmhSendActive(volatile uint32_t *rsc, const uint32_t *addrs, const uint32_t *vals, unsigned count) {
    const QcomRscLayout layout = qcom_rsc_layout(rsc[0]);
    volatile uint8_t *tcsVa = reinterpret_cast<volatile uint8_t *>(rsc) + J716F_RSC_TCS_OFFSET;
    QcomRpmhWrite writes[16], fin[8];

    // Ensure DRV_IRQ_ENABLE (offset 0x00) enables TCS 0 & 1 completion status and clear stale state
    *reinterpret_cast<volatile uint32_t *>(tcsVa + 0x00u) |= 3u;
    __asm__ volatile("dsb sy" ::: "memory");
    const unsigned preFn = qcom_rpmh_finish(&layout, 0, fin, 8);
    for (unsigned i = 0; i < preFn; ++i) {
        *reinterpret_cast<volatile uint32_t *>(tcsVa + fin[i].offset) = fin[i].value;
    }
    __asm__ volatile("dsb sy" ::: "memory");

    const unsigned wn = qcom_rpmh_active_request(&layout, 0, addrs, vals, count, writes, 16);
    if (!wn) return -22;
    for (unsigned i = 0; i < wn; ++i) {
        *reinterpret_cast<volatile uint32_t *>(tcsVa + writes[i].offset) = writes[i].value;
    }
    __asm__ volatile("dsb sy" ::: "memory");
    bool done = false;
    for (unsigned i = 0; i < 10000; ++i) {
        if (*reinterpret_cast<volatile uint32_t *>(tcsVa + layout.irq_status) & 1u) {
            done = true;
            break;
        }
        IODelay(5);
    }
    const unsigned fn = qcom_rpmh_finish(&layout, 0, fin, 8);
    for (unsigned i = 0; i < fn; ++i) {
        *reinterpret_cast<volatile uint32_t *>(tcsVa + fin[i].offset) = fin[i].value;
    }
    __asm__ volatile("dsb sy" ::: "memory");
    return done ? 0 : -110;
}

// Called synchronously in native_usb_debug_probe() before TouchProbe starts so TCS 0 never races.
static void usbVoteRpmhRailsOnce() {
    OSSharedPtr<IOMemoryMap> cmdDbMap, rscMap;
    const uint8_t *db = reinterpret_cast<const uint8_t *>(
        mapRangeUncached(J716F_CMD_DB_BASE, J716F_CMD_DB_SIZE, cmdDbMap));
    volatile uint32_t *rsc = reinterpret_cast<volatile uint32_t *>(
        mapRangeUncached(J716F_RSC_DRV_BASE, J716F_RSC_DRV_SIZE, rscMap));
    if (!db || !rsc) return;

    QcomRpmhResource res{};
    if (qcom_cmd_db_find(db, J716F_CMD_DB_SIZE, "xo.lvl", &res) == QcomRpmhOk && res.address) {
        uint32_t a = res.address, v = 0x3u;
        (void)rpmhSendActive(rsc, &a, &v, 1);
    }
    if (qcom_cmd_db_find(db, J716F_CMD_DB_SIZE, "cx.lvl", &res) == QcomRpmhOk && res.address) {
        uint32_t idx = 0;
        if (qcom_rpmh_level_index(&res, 192u, &idx) == QcomRpmhOk) {
            uint32_t a = res.address;
            (void)rpmhSendActive(rsc, &a, &idx, 1);
        }
    }
    struct LdoSpec { const char *name; uint32_t mv; };
    // Verified against ZUI 15.0.485 / j716f.dtb hsphy@88e3000 (l5/l12/l2) + ssphy@88e8000 (l9/l18)
    static const LdoSpec kLdos[5] = {
        {"ldoa5",  880u},
        {"ldoa12", 1800u},
        {"ldoa2",  3072u},
        {"ldoa9",  1200u},
        {"ldoa18", 920u},
    };
    for (unsigned i = 0; i < 5; ++i) {
        if (qcom_cmd_db_find(db, J716F_CMD_DB_SIZE, kLdos[i].name, &res) == QcomRpmhOk && res.address) {
            // Set MODE=NPM(7), VOLTAGE=mv, ENABLE=1
            uint32_t addrs[3] = {res.address + 0x8u, res.address + 0x0u, res.address + 0x4u};
            uint32_t vals[3]  = {7u, kLdos[i].mv, 1u};
            (void)rpmhSendActive(rsc, addrs, vals, 3);
        }
    }
    IODelay(2000);
}

static bool usbConfigureSmmuBypass(UsbHw &hw, const char **outErr = nullptr) {
    if (outErr) *outErr = nullptr;
    if (!hw.smmu) {
        hw.smmu = reinterpret_cast<volatile uint32_t *>(
            mapRangeUncached(J716F_APPS_SMMU_BASE, J716F_APPS_SMMU_SIZE, hw.smmuMap));
    }
    if (!hw.smmu) {
        hw.smmuOrigSmr  = 0xDEAD0001u;
        hw.smmuOrigS2cr = 0u;
        return true;
    }
    smmu_io io{const_cast<uint32_t *>(hw.smmu), smmuIoRd, smmuIoWr};

    // Inspect existing USB0 SMR entry left by UEFI HALIOMMUDxe (DALSys.efi IORT Node @0x7c5 "\_SB_.USB": in=0 -> out_sid=0x0000, count=1).
    // Never touch SID 0x0001 (not in IORT, trapped/rejected by QHEE hyp.mbn) and never evict existing SMR entries!
    const uint32_t idr0 = smmuIoRd(io.ctx, 0x20u);
    const uint32_t idr1 = smmuIoRd(io.ctx, 0x24u);
    unsigned count = idr0 & 0xFFu;
    if (count > 128u) count = 128u;
    const unsigned pages = 1u << (((idr1 >> 28) & 7u) + 1u);
    const unsigned banks = idr1 & 0xFFu;
    const unsigned bypassBank = (banks >= 2u) ? (banks - 1u) : 73u;
    hw.smmuBank = bypassBank;

    // Ensure shared bypass context bank (73) is configured for stage-1 disabled / stage-2 bypass
    const unsigned cbOff = (pages + bypassBank) * 4096u;
    if (smmuIoRd(io.ctx, cbOff) != 0u ||
        smmuIoRd(io.ctx, 0x1800u + 4u * bypassBank) != 1u ||
        smmuIoRd(io.ctx, 0x1000u + 4u * bypassBank) != 0x10000u) {
        smmuIoWr(io.ctx, cbOff, 0u);
        smmuIoWr(io.ctx, 0x1800u + 4u * bypassBank, 1u);
        smmuIoWr(io.ctx, 0x1000u + 4u * bypassBank, 0x10000u);
    }

    bool foundUsb0Smr = false;
    static const uint16_t kProtectedSids[] = {
        0x0060u, 0x00E0u, 0x04E0u, // UFS0 (DT + UEFI IORT)
        0x00C0u, 0x04A0u,          // SDC1 (UEFI IORT)
        0x0043u, 0x0056u, 0x0076u, 0x05A3u, 0x05B6u, // QGP0 / Touch QUP
        0x0800u, 0x0820u, 0x0821u, 0x0822u, 0x0C20u, 0x0C21u, 0x0C22u // MDP / Display
    };

    for (unsigned i = 0; i < count; ++i) {
        const uint32_t smr = smmuIoRd(io.ctx, 0x800u + 4u * i);
        if (!(smr & 0x80000000u)) continue;
        const uint32_t s2cr = smmuIoRd(io.ctx, 0xC00u + 4u * i);
        const uint32_t mask = (smr >> 16) & 0x7FFFu;

        bool hitsProtected = false;
        for (unsigned p = 0; p < sizeof(kProtectedSids) / sizeof(kProtectedSids[0]); ++p) {
            if (((smr ^ kProtectedSids[p]) & 0x7FFFu & ~mask) == 0u) {
                hitsProtected = true;
                break;
            }
        }
        if (hitsProtected) continue;

        if (((smr ^ J716F_USB0_SMMU_SID_0) & 0x7FFFu & ~mask) == 0u) {
            foundUsb0Smr    = true;
            hw.smmuOrigSmr  = smr;
            hw.smmuOrigS2cr = s2cr;
            // Keep UEFI's SMR[i] untouched! Check whether its context bank has Stage-1 M=1 enabled.
            if ((s2cr & 0x00030000u) == 0u) {
                const unsigned cb = s2cr & 0xFFu;
                if (cb < banks) {
                    const uint32_t sctlr = smmuIoRd(io.ctx, (pages + cb) * 4096u);
                    if ((sctlr & 1u) != 0u) {
                        smmuIoWr(io.ctx, 0xC00u + 4u * i, bypassBank);
                        hw.smmuOrigS2cr = smmuIoRd(io.ctx, 0xC00u + 4u * i);
                    }
                }
            }
        }
    }

    if (!foundUsb0Smr) {
        const smmu_plan plan = smmu_plan_bypass(&io, J716F_USB0_SMMU_SID_0, hw.dmaPhys, DMA_TOTAL_BYTES);
        if (!plan.error) {
            const char *cfgErr = smmu_configure_bypass(&io, &plan, J716F_USB0_SMMU_SID_0);
            hw.smmuOrigSmr  = smmuIoRd(io.ctx, 0x800u + 4u * plan.slot);
            hw.smmuOrigS2cr = smmuIoRd(io.ctx, 0xC00u + 4u * plan.slot);
            if (cfgErr && (hw.smmuOrigSmr & 0x80000000u) == 0u && !plan.reuse) {
                // QHEE hypervisor trapped/ignored new SMR write; revert temporary S2CR write.
                // With SMMU_sCR0.USFCFG == 0, unmatched streams bypass SMMUv2 automatically.
                smmuIoWr(io.ctx, 0xC00u + 4u * plan.slot, 0u);
                hw.smmuOrigSmr  = smmuIoRd(io.ctx, 0x00u);
                hw.smmuOrigS2cr = 0xCEu;
            }
        } else {
            hw.smmuOrigSmr  = smmuIoRd(io.ctx, 0x00u);
            hw.smmuOrigS2cr = 0xCFu;
        }
    }
    return true;
}

static bool usbEnableGccBranches(const UsbHw &hw) {
    static const uint32_t kBranches[] = {
        GCC_USB30_PRIM_MASTER_CLK,
        GCC_USB30_PRIM_SLEEP_CLK,
        GCC_USB30_PRIM_MOCK_UTMI_CLK,
        GCC_USB3_PRIM_PHY_AUX_CLK,
        GCC_USB3_PRIM_PHY_COM_AUX_CLK,
        GCC_USB3_PRIM_PHY_PIPE_CLK,
        GCC_CFG_NOC_USB3_PRIM_AXI_CLK,
        GCC_AGGRE_USB3_PRIM_AXI_CLK,
    };
    for (unsigned i = 0; i < sizeof(kBranches) / sizeof(kBranches[0]); ++i) {
        hw.gwr(kBranches[i], hw.grd(kBranches[i]) | GCC_BRANCH_ENABLE_BIT);
    }
    bool allOn = true;
    for (unsigned i = 0; i < sizeof(kBranches) / sizeof(kBranches[0]); ++i) {
        if (kBranches[i] == GCC_USB3_PRIM_PHY_PIPE_CLK) continue;  // Externally clocked until UTMI mux
        bool branchOn = false;
        for (unsigned w = 0; w < 2000; ++w) {
            if ((hw.grd(kBranches[i]) & GCC_BRANCH_CLK_OFF) == 0u) {
                branchOn = true;
                break;
            }
            IODelay(2);
        }
        if (!branchOn) allOn = false;
    }
    return allOn;
}

static void usbUpdateRcg(const UsbHw &hw, uint32_t cmdOff, uint32_t cfgOff, uint32_t cfgVal) {
    hw.gwr(cfgOff, cfgVal);
    hw.gwr(cmdOff, hw.grd(cmdOff) | 1u);
    for (unsigned i = 0; i < 1000; ++i) {
        if ((hw.grd(cmdOff) & 1u) == 0u) break;
        IODelay(2);
    }
}

static void usbEnsureGccRcg(const UsbHw &hw) {
    // Vote GPLL0 on in GCC_APCS_GPLL_ENA_VOTE (0x52000 bit 0) and wait for GPLL0_MODE bit 31
    hw.gwr(GCC_APCS_GPLL_ENA_VOTE, hw.grd(GCC_APCS_GPLL_ENA_VOTE) | 1u);
    for (unsigned i = 0; i < 1000; ++i) {
        if (hw.grd(GCC_GPLL0_MODE) & (1u << 31)) break;
        IODelay(2);
    }
    // ClockDxe.efi (0x2ebc8..0x300d0):
    // - master_clk: GPLL0 (src=1, div=3 -> 0x0601 = 200 MHz)
    // - mock_utmi_clk: CXO (src=0, div=1 -> 0x0200 = 19.2 MHz)
    // - phy_aux_clk: CXO (src=0, div=1 -> 0x0200 = 19.2 MHz)
    usbUpdateRcg(hw, GCC_USB30_PRIM_MASTER_CMD_RCGR, GCC_USB30_PRIM_MASTER_CFG_RCGR, 0x0601u);
    usbUpdateRcg(hw, GCC_USB30_PRIM_MOCK_UTMI_CMD_RCGR, GCC_USB30_PRIM_MOCK_UTMI_CFG_RCGR, 0x0200u);
    usbUpdateRcg(hw, GCC_USB3_PRIM_PHY_AUX_CMD_RCGR, GCC_USB3_PRIM_PHY_AUX_CFG_RCGR, 0x0200u);
}

static void usbInitFemtoHsPhy(UsbHw &hw) {
    hw.phyInitMode = 1u;
    // ZUI 15.0.485 msm_snps_hsphy_init (0x9acd14..0x9acd44) & UsbConfigDxe SSUsb1InitHSPhy (0x8388..0x83c8):
    // Pulse ONLY GCC_QUSB2PHY_PRIM_BCR (0x12000) before Femto v2 HS PHY register programming (never touch SS PHY BCRs!)
    hw.gwr(GCC_QUSB2PHY_PRIM_BCR, 1u);
    IODelay(150);
    hw.gwr(GCC_QUSB2PHY_PRIM_BCR, 0u);
    IODelay(150);

    // Exact ZUI 15.0.485 msm_snps_hsphy_init (0x9acd68..0x9ad04c) + UsbConfigDxe (0x83dc..0x8460):
    hw.pmask(FEMTO_CFG0, 0x03u, 0x02u);                  // Clear RETENTION_EN (bit 0), set OVERRIDE_EN (bit 1)
    hw.pmask(FEMTO_UTMI_CTRL5, 0x02u, 0x02u);            // POR = 1
    hw.pmask(FEMTO_COMMON0, 0x70u, 0x00u);               // FSEL = 0 (19.2 MHz refclk)
    hw.pmask(FEMTO_COMMON1, 0x20u, 0x20u);               // PLLBTUNE = 1
    hw.pmask(FEMTO_REFCLK, 0x03u, 0x02u);                // SELCK_SEP = 2
    hw.pmask(FEMTO_COMMON1, 0x10u, 0x10u);               // VBUSVLDEXTSEL0 = 1
    hw.pmask(FEMTO_CTRL1, 0x01u, 0x01u);                 // VBUSVLDEXT0 = 1
    hw.pmask(FEMTO_RTUNE_SEL, 0x01u, 0x00u);             // RTUNE_SEL = 0 (internal RCAL)
    hw.pwr(FEMTO_PARAM_OVRD_X0, 0x61u);                  // Exact DALSys.efi hs_phy_cfg (0x1b150..0x1b168)
    hw.pwr(FEMTO_PARAM_OVRD_X1, 0x43u);
    hw.pwr(FEMTO_PARAM_OVRD_X2, 0x2Eu);
    hw.pwr(FEMTO_PARAM_OVRD_X3, 0x03u);
    hw.pmask(FEMTO_COMMON2, 0x01u, 0x01u);               // VREGBYPASS = 1
    hw.pmask(FEMTO_UTMI_CTRL5, 0x01u, 0x00u);            // Clear ATERESET
    hw.pmask(FEMTO_CHARGING_DET_OUTPUT, 0x10u, 0x00u);
    hw.pmask(FEMTO_CHARGING_DET_OUTPUT, 0x40u, 0x00u);
    hw.pmask(FEMTO_COMMON0, 0x03u, 0x00u);               // Clear SIDDQ / retention powerdown
    hw.pmask(FEMTO_CHARGING_DET_CTRL, 0xFFu, 0x00u);     // Disable BC1.2 charger detect mux
    hw.pmask(FEMTO_CTRL2, 0x08u, 0x08u);                 // SUSPENDM override = 1
    hw.pmask(FEMTO_CTRL2, 0x04u, 0x04u);                 // UTMI_CLK_EN = 1
    hw.pmask(FEMTO_UTMI_CTRL0, 0x01u, 0x01u);            // SLEEPM = 1
    hw.pmask(FEMTO_UTMI_CTRL5, 0x02u, 0x00u);            // Release POR = 0
    hw.pmask(FEMTO_CTRL2, 0x08u, 0x00u);                 // Release SUSPENDM override
    IODelay(150);
    hw.pmask(FEMTO_CFG0, 0x03u, 0x00u);                  // Release CMN_CTRL_OVERRIDE_EN = 0 & RETENTION_EN = 0
    IODelay(200);
}

static bool usbGccAndPhyInit(UsbHw &hw) {
    if (!hw.gcc) {
        hw.gcc = reinterpret_cast<volatile uint32_t *>(
            mapRangeUncached(J716F_GCC_BASE, J716F_GCC_SIZE, hw.gccMap));
    }
    if (!hw.phy) {
        hw.phy = reinterpret_cast<volatile uint32_t *>(
            mapRangeUncached(J716F_HSPHY_BASE, J716F_HSPHY_SIZE, hw.phyMap));
    }
    if (!hw.eud) {
        hw.eud = reinterpret_cast<volatile uint32_t *>(
            mapRangeUncached(J716F_EUD_BASE, J716F_EUD_SIZE, hw.eudMap));
    }
    if (!hw.gcc || !hw.phy) return false;

    // Preserve live UEFI usb30_prim_gdsc & QMP SS PHY (0x088E9000) state!
    // Never collapse GCC_USB30_PRIM_GDSCR or assert GCC_USB3_PHY_PRIM_BCR / GCC_USB30_PRIM_BCR,
    // because UEFI ExitBootServices (UsbConfigDxe 0x7408 -> StopController 0x5b60 -> UsbfnDwc3Dxe 0x642c)
    // leaves usb30_prim_gdsc, QMP SS PHY (pipe_clk), Femto v2 HS PHY, and usb_ss1_bus powered and clocked.
    if (hw.grd(GCC_USB30_PRIM_GDSCR) & GCC_GDSCR_SW_COLLAPSE) {
        hw.gwr(GCC_USB30_PRIM_GDSCR, hw.grd(GCC_USB30_PRIM_GDSCR) & ~GCC_GDSCR_SW_COLLAPSE);
        for (unsigned i = 0; i < 5000; ++i) {
            if (hw.grd(GCC_USB30_PRIM_GDSCR) & GCC_GDSCR_PWR_ON) break;
            IODelay(5);
        }
    }
    if (hw.grd(GCC_USB3PHY_PHY_PRIM_BCR) & 1u) hw.gwr(GCC_USB3PHY_PHY_PRIM_BCR, 0u);
    if (hw.grd(GCC_USB3_PHY_PRIM_BCR) & 1u)    hw.gwr(GCC_USB3_PHY_PRIM_BCR, 0u);
    if (hw.grd(GCC_QUSB2PHY_PRIM_BCR) & 1u)    hw.gwr(GCC_QUSB2PHY_PRIM_BCR, 0u);
    if (hw.grd(GCC_USB30_PRIM_BCR) & 1u)       hw.gwr(GCC_USB30_PRIM_BCR, 0u);

    if (hw.grd(GCC_USB30_PRIM_MASTER_CLK) & GCC_BRANCH_CLK_OFF) {
        usbEnsureGccRcg(hw);
    }
    (void)usbEnableGccBranches(hw);

    // Ensure EUD_SW_ATTACH_DET (0x088E1018 bit 0) is set so EUD routes D+/D- to Type-C connector
    if (hw.eud) {
        const uint32_t attach = hw.eud[EUD_SW_ATTACH_DET / 4u];
        if ((attach & 1u) == 0u) {
            hw.eud[EUD_SW_ATTACH_DET / 4u] = attach | 1u;
            __asm__ volatile("dsb sy" ::: "memory");
            IODelay(200);
        }
    }

    // If Femto v2 HS PHY is already awake and out of POR from UEFI (and not forced into full re-init),
    // preserve its live calibration and just clear retention + affirm external VBUS valid.
    const bool hsPhyAlive = (hw.phyInitMode == 0u) &&
                            ((hw.prd(FEMTO_UTMI_CTRL0) & 1u) != 0u) &&
                            ((hw.prd(FEMTO_UTMI_CTRL5) & 3u) == 0u);
    if (hsPhyAlive) {
        hw.pmask(FEMTO_CFG0, 0x03u, 0x00u);
        hw.pmask(FEMTO_COMMON1, 0x10u, 0x10u);
        hw.pmask(FEMTO_CTRL1, 0x01u, 0x01u);
        hw.pmask(FEMTO_CTRL2, 0x04u, 0x04u);
    } else {
        usbInitFemtoHsPhy(hw);
    }
    return true;
}

#endif // J716F_USB_DEBUG_HW_H
