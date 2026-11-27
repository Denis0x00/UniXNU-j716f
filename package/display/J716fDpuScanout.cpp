// SPDX-License-Identifier: BSD-2-Clause-Patent
// SM8250 DPU 6.0 & Display APPS SMMU observation + Stage-1 direct scanout mapper
// for Lenovo TB-J716F.
//
// Boot-args:
//   (default)       Read-only observation only (zero MMIO writes); `COM9` `dpu` dumps live state.
//   dpu-scanout=1   Enable Stage-1 translation on the firmware Display SMMU context bank (CB 4,
//                   SID 0x820 mask 0x402) with identity blocks for 0x80000000..0xFFFFFFFF
//                   (preserving 0x9C000000 boot GOP) and 4 x 32 MiB 4 KiB windows at 0x10000000.
//                   Watches CB_FSR for 50 ms and automatically reverts on any fault.
//   dpu-scanout=2   Same as =1 plus NativeLegacyDisplay zero-copy direct scanout on VIG0/VIG1
//                   (0x4000 / 0x6000) via CTL0_FLUSH (0x1018 = 0x3).
#include "J716fDpuSmmu.h"

#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <libkern/libkern.h>
#include <pexpert/pexpert.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>

extern "C" {
extern volatile uint32_t native_legacy_direct[14] __attribute__((weak_import));

// Strong definitions overriding the weak fallback stubs in J716fLegacyDisplayProbe.cpp.
volatile uint32_t native_ufs_dpu_state = 0;
volatile uint32_t j716f_dpu_check[8] = {};
}

namespace {

IOMemoryMap *gMdssMap = nullptr;
IOMemoryMap *gSmmuMap = nullptr;
volatile uint32_t *gMdssVa = nullptr;
volatile uint32_t *gSmmuVa = nullptr;

IOBufferMemoryDescriptor *gDpuTableDesc = nullptr;
uint64_t *gDpuTables = nullptr;
volatile uint32_t *gDpuCbVa = nullptr;
IOLock *gDpuLock = nullptr;
bool gProbed = false;

uint32_t mmioRead(void *ctx, uint32_t off) {
    return reinterpret_cast<volatile uint32_t *>(ctx)[off / 4u];
}

inline uint32_t cbRead(volatile uint32_t *cb, uint32_t off) {
    return cb[off / 4u];
}

inline void cbWrite(volatile uint32_t *cb, uint32_t off, uint32_t val) {
    cb[off / 4u] = val;
}

void dpuClean(const void *start, size_t length) {
    const uint8_t *p = static_cast<const uint8_t *>(start);
    for (size_t off = 0; off < length; off += 64u) {
        __asm__ volatile("dc civac, %0" :: "r"(p + off) : "memory");
    }
    __asm__ volatile("dsb sy" ::: "memory");
}

bool dpuTlbFlush(volatile uint32_t *cb) {
    cbWrite(cb, J716F_CB_TLBIALL, 0u);
    cbWrite(cb, J716F_CB_TLBSYNC, 0u);
    __asm__ volatile("dsb sy" ::: "memory");
    for (unsigned spin = 0; spin < 100000u; ++spin) {
        if ((cbRead(cb, J716F_CB_TLBSTATUS) & 1u) == 0u) return true;
    }
    return false;
}

void appendf(char *out, size_t cap, size_t &pos, const char *fmt, ...) {
    if (!out || pos >= cap) return;
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(out + pos, cap - pos, fmt, ap);
    va_end(ap);
    if (n > 0) {
        const size_t adv = static_cast<size_t>(n);
        pos = (pos + adv < cap) ? (pos + adv) : (cap - 1u);
    }
}

void ensureMapped() {
    if (!gMdssVa) {
        IODeviceMemory *mem = IODeviceMemory::withRange(J716F_MDSS_PHYS_BASE, J716F_MDSS_PHYS_SIZE);
        gMdssMap = mem ? mem->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
        if (mem) mem->release();
        if (gMdssMap) {
            gMdssVa = reinterpret_cast<volatile uint32_t *>(gMdssMap->getVirtualAddress());
        }
    }
    if (!gSmmuVa) {
        IODeviceMemory *mem = IODeviceMemory::withRange(J716F_APPS_SMMU_PHYS_BASE, J716F_APPS_SMMU_PHYS_SIZE);
        gSmmuMap = mem ? mem->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
        if (mem) mem->release();
        if (gSmmuMap) {
            gSmmuVa = reinterpret_cast<volatile uint32_t *>(gSmmuMap->getVirtualAddress());
        }
    }
}

void dpuEnableStage1(const J716fDpuSmmuReport &smmu) {
    if (smmu.match_count == 0u) {
        native_ufs_dpu_state = 0xDEAD0001u;
        return;
    }
    const J716fDpuSmmuMatch &m = smmu.matches[0];
    const uint32_t s2Type = (m.s2cr >> 16) & 3u;
    const uint32_t cbarType = (m.cbar >> 16) & 3u;
    if (!m.valid_cb || s2Type != 0u || (m.sctlr & 1u) != 0u ||
        cbarType != 1u || (m.cba2r & 1u) == 0u) {
        native_ufs_dpu_state = 0xDEAD0001u;
        IOLog("[J716F dpu] stage1 precondition failed bank=%u s2=%u sctlr=0x%08x cbar=0x%08x cba2r=0x%08x\n",
              m.bank, s2Type, m.sctlr, m.cbar, m.cba2r);
        return;
    }

    const size_t bytes = J716F_DPU_TOTAL_TABLE_PAGES * 4096u;
    IOBufferMemoryDescriptor *tables = IOBufferMemoryDescriptor::inTaskWithOptions(
        kernel_task,
        static_cast<IOOptionBits>(kIODirectionInOut) |
        static_cast<IOOptionBits>(kIOMemoryPhysicallyContiguous) |
        static_cast<IOOptionBits>(kIOMemoryMapperNone),
        bytes, 4096u);
    gDpuLock = IOLockAlloc();
    if (!tables || !gDpuLock) {
        if (tables) tables->release();
        native_ufs_dpu_state = 0xDEAD0002u;
        return;
    }
    uint64_t *l1 = static_cast<uint64_t *>(tables->getBytesNoCopy());
    const uint64_t pa = tables->getPhysicalSegment(0, nullptr, kIOMemoryMapperNone);
    if (!j716f_dpu_build_stage1_tables(l1, pa)) {
        tables->release();
        native_ufs_dpu_state = 0xDEAD0002u;
        return;
    }
    dpuClean(l1, bytes);

    volatile uint32_t *cb = gSmmuVa + ((smmu.pages + m.bank) * 4096u) / 4u;
    const uint32_t oldSctlr = m.sctlr;
    const uint32_t old[5] = {
        cbRead(cb, J716F_CB_TTBR0_LO),
        cbRead(cb, J716F_CB_TTBR0_HI),
        cbRead(cb, J716F_CB_TCR2),
        cbRead(cb, J716F_CB_TCR),
        cbRead(cb, J716F_CB_MAIR0),
    };

    const uint32_t oas = (smmu.idr2 >> 4) & 7u;
    const uint32_t wantTcr2 = oas | (7u << 15);        // PASIZE=OAS (36-bit on SM8250), SEP=7 (upstream)
    const uint32_t wantTcr  = 32u | (1u << 23);        // T0SZ=32, non-cacheable walks, 4 KiB, EPD1=1

    cbWrite(cb, J716F_CB_TTBR0_LO, static_cast<uint32_t>(pa));
    cbWrite(cb, J716F_CB_TTBR0_HI, static_cast<uint32_t>(pa >> 32));
    cbWrite(cb, J716F_CB_TCR2, wantTcr2);
    cbWrite(cb, J716F_CB_TCR, wantTcr);
    cbWrite(cb, J716F_CB_MAIR0, 0x44u);
    __asm__ volatile("dsb sy" ::: "memory");

    j716f_dpu_check[0] = cbRead(cb, J716F_CB_TTBR0_LO);
    j716f_dpu_check[1] = cbRead(cb, J716F_CB_TCR);
    j716f_dpu_check[2] = cbRead(cb, J716F_CB_FSR);

    const auto undo = [&](uint32_t why) {
        cbWrite(cb, J716F_CB_SCTLR, oldSctlr);
        __asm__ volatile("dsb sy; isb" ::: "memory");
        cbWrite(cb, J716F_CB_TTBR0_LO, old[0]);
        cbWrite(cb, J716F_CB_TTBR0_HI, old[1]);
        cbWrite(cb, J716F_CB_TCR2, old[2]);
        cbWrite(cb, J716F_CB_TCR, old[3]);
        cbWrite(cb, J716F_CB_MAIR0, old[4]);
        const uint32_t fsr = cbRead(cb, J716F_CB_FSR);
        if (fsr & (1u << 30)) cbWrite(cb, J716F_CB_RESUME, 1u);
        if (fsr & J716F_DPU_CB_FAULTS) cbWrite(cb, J716F_CB_FSR, fsr);
        (void)dpuTlbFlush(cb);
        tables->release();
        native_ufs_dpu_state = why;
    };

    if (j716f_dpu_check[0] != static_cast<uint32_t>(pa) || j716f_dpu_check[1] != wantTcr) {
        undo(0xDEAD0003u);
        IOLog("[J716F dpu] stage1 readback mismatch ttbr0=0x%08x tcr=0x%08x\n",
              j716f_dpu_check[0], j716f_dpu_check[1]);
        return;
    }
    if (!dpuTlbFlush(cb)) {
        undo(0xDEAD0004u);
        IOLog("[J716F dpu] stage1 tlb flush timeout\n");
        return;
    }

    // Enable Stage-1 translation: M | CFRE | CFIE (terminate faults cleanly instead of stalling DPU).
    cbWrite(cb, J716F_CB_SCTLR, 1u | (1u << 5) | (1u << 6));
    __asm__ volatile("dsb sy; isb" ::: "memory");

    // Watch live 90 Hz GOP fetches for 50 ms (~4.5 frames); revert immediately on any fault.
    uint32_t fsr = 0u;
    uint32_t us = 0u;
    for (; us < 50000u; us += 500u) {
        fsr = cbRead(cb, J716F_CB_FSR);
        if (fsr & J716F_DPU_CB_FAULTS) break;
        IODelay(500);
    }
    j716f_dpu_check[3] = fsr;
    j716f_dpu_check[7] = us;
    if (fsr & J716F_DPU_CB_FAULTS) {
        j716f_dpu_check[4] = cbRead(cb, J716F_CB_FAR_LO);
        j716f_dpu_check[5] = cbRead(cb, J716F_CB_FAR_HI);
        j716f_dpu_check[6] = cbRead(cb, J716F_CB_FSYNR0);
        undo(0xDEAD0005u);
        IOLog("[J716F dpu] stage1 faulted at %uus fsr=0x%08x far=%08x:%08x fsynr0=0x%08x (reverted)\n",
              us, fsr, j716f_dpu_check[5], j716f_dpu_check[4], j716f_dpu_check[6]);
        return;
    }

    gDpuTableDesc = tables;
    gDpuTables = l1;
    gDpuCbVa = cb;
    native_ufs_dpu_state = 0x80000000u | m.bank;
    IOLog("[J716F dpu] stage1 active on CB %u pa=0x%llx oas=%u tcr2=0x%08x fsr=0x%08x (50ms clean)\n",
          m.bank, static_cast<unsigned long long>(pa), oas, wantTcr2, fsr);
}

}  // namespace

extern "C" uint32_t native_ufs_dpu_map(unsigned slot, const uint64_t *pages, unsigned count) {
    if (!gDpuTableDesc || !gDpuTables || !gDpuCbVa || !gDpuLock) return 0u;
    IOLockLock(gDpuLock);
    const uint32_t iova = j716f_dpu_fill_slot_l3(gDpuTables, slot, pages, count);
    bool flushed = false;
    if (iova != 0u) {
        uint64_t *l3 = gDpuTables + (2u + slot * J716F_DPU_SLOT_TABLES) * 512u;
        dpuClean(l3, J716F_DPU_SLOT_TABLES * 4096u);
        flushed = dpuTlbFlush(gDpuCbVa);
    }
    IOLockUnlock(gDpuLock);
    return flushed ? iova : 0u;
}

extern "C" void j716f_dpu_probe(void) {
    if (gProbed) return;
    gProbed = true;

    ensureMapped();
    if (!gMdssVa || !gSmmuVa) {
        IOLog("[J716F dpu] map failed mdss=%p smmu=%p\n",
              const_cast<uint32_t *>(gMdssVa), const_cast<uint32_t *>(gSmmuVa));
        return;
    }

    const J716fDpuRegIo mdssIo = {mmioRead, nullptr, const_cast<uint32_t *>(gMdssVa)};
    const J716fDpuRegIo smmuIo = {mmioRead, nullptr, const_cast<uint32_t *>(gSmmuVa)};
    J716fDpuMdpReport mdp = {};
    J716fDpuSmmuReport smmu = {};
    j716f_dpu_observe_mdp(&mdssIo, &mdp);
    j716f_dpu_smmu_inspect(&smmuIo, &smmu);

    unsigned activePipes = 0;
    for (unsigned i = 0; i < J716F_MDP_SSPP_COUNT; ++i) {
        if (mdp.pipes[i].src0_addr != 0u || mdp.pipes[i].src_size != 0u) ++activePipes;
    }
    IOLog("[J716F dpu] obs mdss=0x%08x mdp=0x%08x pipes=%u smmu(g=%u v=%u b=%u p=%u match=%u)\n",
          mdp.mdss_hw_ver, mdp.mdp_hw_ver, activePipes,
          smmu.groups, smmu.valid_smrs, smmu.banks, smmu.pages, smmu.match_count);

    unsigned scanout = 0;
    PE_parse_boot_argn("dpu-scanout", &scanout, sizeof(scanout));
    if (scanout >= 1u) {
        dpuEnableStage1(smmu);
    }
}

extern "C" void j716f_dpu_snapshot(char *out, size_t cap) {
    if (!out || cap == 0u) return;
    out[0] = '\0';
    size_t pos = 0;

    if (!gMdssVa || !gSmmuVa) {
        appendf(out, cap, pos, "[usb] DPU: unmapped (mdss=%p smmu=%p)\r\n",
                const_cast<uint32_t *>(gMdssVa), const_cast<uint32_t *>(gSmmuVa));
        return;
    }

    const J716fDpuRegIo mdssIo = {mmioRead, nullptr, const_cast<uint32_t *>(gMdssVa)};
    const J716fDpuRegIo smmuIo = {mmioRead, nullptr, const_cast<uint32_t *>(gSmmuVa)};
    J716fDpuMdpReport mdp = {};
    J716fDpuSmmuReport smmu = {};
    j716f_dpu_observe_mdp(&mdssIo, &mdp);
    j716f_dpu_smmu_inspect(&smmuIo, &smmu);

    appendf(out, cap, pos,
            "[usb] DPU: mdss=0x%08x mdp=0x%08x state=0x%08x chk=[%08x %08x %08x %08x far=%08x:%08x syn=%08x us=%u]\r\n",
            mdp.mdss_hw_ver, mdp.mdp_hw_ver, native_ufs_dpu_state,
            j716f_dpu_check[0], j716f_dpu_check[1], j716f_dpu_check[2], j716f_dpu_check[3],
            j716f_dpu_check[5], j716f_dpu_check[4], j716f_dpu_check[6], j716f_dpu_check[7]);

    for (unsigned i = 0; i < J716F_MDP_SSPP_COUNT; ++i) {
        const J716fDpuPipeInfo &p = mdp.pipes[i];
        if (p.src0_addr == 0u && p.src_size == 0u && p.out_size == 0u) continue;
        appendf(out, cap, pos,
                "[usb] DPU_PIPE %s(0x%05x): addr=0x%08x stride=0x%04x src=%ux%u@(%u,%u) out=%ux%u@(%u,%u) fmt=0x%08x op=0x%08x\r\n",
                p.name, p.off, p.src0_addr, p.stride0 & 0xFFFFu,
                p.src_size & 0xFFFFu, p.src_size >> 16,
                p.src_xy & 0xFFFFu, p.src_xy >> 16,
                p.out_size & 0xFFFFu, p.out_size >> 16,
                p.out_xy & 0xFFFFu, p.out_xy >> 16,
                p.format, p.op_mode);
    }

    for (unsigned c = 0; c < J716F_MDP_CTL_COUNT; ++c) {
        const J716fDpuCtlInfo &ci = mdp.ctls[c];
        if (ci.layer0 == 0u && ci.layer1 == 0u && ci.intf_active == 0u &&
            ci.fetch_pipe_active == 0u && ci.flush == 0u) {
            continue;
        }
        appendf(out, cap, pos,
                "[usb] DPU_CTL%u(0x%04x): l0=0x%08x l1=0x%08x top=0x%08x flush=0x%08x intf_act=0x%08x fetch_act=0x%08x flush_act=0x%08x\r\n",
                ci.index, ci.off, ci.layer0, ci.layer1, ci.top, ci.flush,
                ci.intf_active, ci.fetch_pipe_active, ci.flush_active);
    }

    appendf(out, cap, pos,
            "[usb] DPU_SMMU: scr0=0x%08x idr0=0x%08x idr1=0x%08x idr2=0x%08x gfsr=0x%08x groups=%u valid=%u banks=%u pages=%u matches=%u\r\n",
            smmu.scr0, smmu.idr0, smmu.idr1, smmu.idr2, smmu.sgfsr,
            smmu.groups, smmu.valid_smrs, smmu.banks, smmu.pages, smmu.match_count);

    if (smmu.match_count > 0u) {
        const J716fDpuSmmuMatch &m = smmu.matches[0];
        appendf(out, cap, pos,
                "[usb] DPU_CB sid=0x%04x slot=%u smr=0x%08x s2cr=0x%08x bank=%u cbar=0x%08x cba2r=0x%08x sctlr=0x%08x tcr2=0x%08x tcr=0x%08x ttbr0=%08x:%08x mair0=0x%08x fsr=0x%08x\r\n",
                m.sid, m.slot, m.smr, m.s2cr, m.bank,
                m.cbar, m.cba2r, m.sctlr, m.tcr2, m.tcr,
                m.ttbr0_hi, m.ttbr0_lo, m.mair0, m.fsr);
    }

    if (&native_legacy_direct) {
        appendf(out, cap, pos,
                "[usb] DPU_DIRECT: flips=%u mapped=%u map_err=%u slow50=%u latch_max=%uus latch_tot=%uus drop=%u fb=%u boot=%u wait=%u wait_tot=%uus wait_max=%uus wait250=%u wait8=%u\r\n",
                native_legacy_direct[0], native_legacy_direct[1], native_legacy_direct[2],
                native_legacy_direct[3], native_legacy_direct[4], native_legacy_direct[5],
                native_legacy_direct[6], native_legacy_direct[7], native_legacy_direct[8],
                native_legacy_direct[9], native_legacy_direct[10], native_legacy_direct[11],
                native_legacy_direct[12], native_legacy_direct[13]);
    }
}
