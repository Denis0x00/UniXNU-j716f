// SPDX-License-Identifier: BSD-2-Clause-Patent
// SM8250 host adapter for Lenovo TB-J716F.
static bool hostBringUp (const char **why)
{
  if (gHost.poisoned) { *why = "transport quarantined"; return false; }
  if (gHost.ready) return true;

  // 1. Check SM8250 GCC clocks and power domain for UFS
  OSSharedPtr<IOMemoryMap> clocks;
  uintptr_t gcc = mapPhys (GCC_UFS_BASE, 4096, clocks);
  if (!gcc || !(read32 (gcc, GCC_UFS_PHY_GDSCR) & 0x80000000) ||
      !(read32 (gcc, GCC_UFS_PHY_AXI_ENABLE) & 1) ||
      !(read32 (gcc, GCC_UFS_PHY_AHB_ENABLE) & 1)) {
    *why = "UFS power/clocks disabled"; return false;
  }

  // 2. Map UFS host controller
  gRootDiagnostic.stage = "controller map";
  gHost.hc = mapPhys (UFS_HC_BASE, UFS_HC_SIZE, gHost.hcMap);
  if (!gHost.hc) { *why = "controller mapping"; return false; }
  if (gRootDiagnosticEnabled) {
    gRootDiagnostic.cap = read32 (gHost.hc, 0x00);
    gRootDiagnostic.version = read32 (gHost.hc, 0x08);
    gRootDiagnostic.hcs = read32 (gHost.hc, 0x30);
    gRootDiagnostic.hce = read32 (gHost.hc, 0x34);
  }

  // 3. Allocate DMA buffer
  gRootDiagnostic.stage = "DMA allocation";
  gHost.dma = IOBufferMemoryDescriptor::inTaskWithOptions (
    kernel_task, static_cast<IOOptionBits> (kIODirectionInOut) |
    static_cast<IOOptionBits> (kIOMemoryPhysicallyContiguous) |
    static_cast<IOOptionBits> (kIOMemoryMapperNone),
    kDmaBytes, 4096);
  gHost.lock = IOLockAlloc ();
  if (!gHost.dma || !gHost.lock) { *why = "DMA allocation"; return false; }
  gHost.virt = static_cast<uint8_t *> (gHost.dma->getBytesNoCopy ());
  IOByteCount contiguous = 0;
  gHost.phys = gHost.dma->getPhysicalSegment (0, &contiguous, kIOMemoryMapperNone);
  gRootDiagnostic.dma = gHost.phys;
  gRootDiagnostic.contiguous = contiguous;
  if (!gHost.virt || (gHost.phys & 4095) || contiguous < kDmaBytes) {
    *why = "DMA outside conventional RAM"; return false;
  }

  // 4. Map APPS SMMU and configure dual-stream bypass (0x0e0, 0x4e0)
  gRootDiagnostic.stage = "SMMU mapping";
  gHost.smmu = mapPhys (0x15000000ULL, 0xd0000, gHost.smmuMap);
  if (!gHost.smmu) { *why = "SMMU mapping"; return false; }
  auto read = [&] (unsigned at) { return read32 (gHost.smmu, at); };
  J716fSmmuPlan plan = planJ716fSmmu (gHost.phys, kDmaBytes, read);
  if (plan.error) { *why = "SMMU plan refused"; return false; }

  // 5. Stop old firmware queue
  gRootDiagnostic.stage = "stop old queue";
  if (!hostStopQueue (why)) return false;
  if (read32 (gHost.hc, 0x78) || read32 (gHost.hc, 0x80)) {
    *why = "firmware task queue active"; return false;
  }

  // 6. Apply SMMU takeover for both 0x0e0 and 0x4e0
  plan = planJ716fSmmu (gHost.phys, kDmaBytes, read);
  plan.error = configureJ716fSmmu (plan, read,
    [&] (unsigned at, uint32_t val) {
      write32 (gHost.smmu, at, val);
      __asm__ volatile ("dsb sy" ::: "memory");
    });
  if (plan.error) { *why = "SMMU dual-stream readback"; return false; }
  gHost.cb = gHost.smmu + (plan.pages + plan.bank) * 4096;

  // 6b. Set up PCIe0 WiFi endpoint SMMU Stage-1 stream (0x1c01 / 0x1c00) and 32-bit IOVA DMA arena
  uint32_t wifi = 4;
  PE_parse_boot_argn ("sheng-wifi", &wifi, sizeof (wifi));
  if (wifi >= 2 && !native_ufs_extra_va) {
    for (unsigned i = 0; i < plan.count && i < 128; ++i) {
      uint32_t smr = read32 (gHost.smmu, 0x800 + 4 * i);
      if (!(smr & 0x80000000U)) continue;
      uint32_t sid = smr & 0x7fffU;
      if (sid == 0x1c00U || sid == 0x1c01U) {
        uint32_t b = read32 (gHost.smmu, 0xc00 + 4 * i) & 0xffU;
        write32 (gHost.smmu, 0x800 + 4 * i, 0);
        if (b != plan.bank && (plan.pages + b) * 4096 < 0xd0000) {
          write32 (gHost.smmu + (plan.pages + b) * 4096, 0x0, 0);
          write32 (gHost.smmu, 0x1000 + 4 * b, 0x10000);
          write32 (gHost.smmu, 0x1800 + 4 * b, 1);
        }
        __asm__ volatile ("dsb sy" ::: "memory");
      }
    }
    UfsSmmuPlan wplan = planUfsSmmu (0x1c01, 0x9c900000ULL, 8ULL << 20, read);
    const char *werr = wplan.error ? wplan.error : configureUfsSmmu (wplan, 0x1c01, read,
      [&] (unsigned at, uint32_t val) {
        write32 (gHost.smmu, at, val);
        __asm__ volatile ("dsb sy" ::: "memory");
      });
    if (!werr && wplan.bank != plan.bank) {
      for (unsigned i = 0; i < plan.count && i < 128; ++i) {
        if (!(read32 (gHost.smmu, 0x800 + 4 * i) & 0x80000000U)) {
          write32 (gHost.smmu, 0xc00 + 4 * i, wplan.bank);
          __asm__ volatile ("dsb sy" ::: "memory");
          write32 (gHost.smmu, 0x800 + 4 * i, 0x80000000U | 0x1c00U);
          __asm__ volatile ("dsb sy" ::: "memory");
          break;
        }
      }
      uint32_t extraReport0 = wplan.slot | (wplan.bank << 16);
      constexpr uint32_t kExtraIova = 0x10000000U, kExtraBlocks = 32, kBlock = 2U << 20;
      IOBufferMemoryDescriptor *buffer = IOBufferMemoryDescriptor::inTaskWithOptions (
        kernel_task, static_cast<IOOptionBits> (kIODirectionInOut) | static_cast<IOOptionBits> (kIOMemoryPhysicallyContiguous) |
        static_cast<IOOptionBits> (kIOMemoryMapperNone), (kExtraBlocks + 1) * kBlock, kBlock);
      IOBufferMemoryDescriptor *tables = IOBufferMemoryDescriptor::inTaskWithOptions (
        kernel_task, static_cast<IOOptionBits> (kIODirectionInOut) | static_cast<IOOptionBits> (kIOMemoryPhysicallyContiguous) |
        static_cast<IOOptionBits> (kIOMemoryMapperNone), 8192, 4096);
      if (buffer && tables) {
        uint8_t *bva = static_cast<uint8_t *> (buffer->getBytesNoCopy ());
        uint64_t bpa = buffer->getPhysicalSegment (0, nullptr, kIOMemoryMapperNone);
        uint64_t shift = (kBlock - (bpa & (kBlock - 1))) & (kBlock - 1);
        bva += shift; bpa += shift;
        bzero (buffer->getBytesNoCopy (), (kExtraBlocks + 1) * kBlock);
        uint64_t *l1 = static_cast<uint64_t *> (tables->getBytesNoCopy ());
        uint64_t *l2 = l1 + 512;
        bzero (l1, 8192);
        const uint64_t tablesPa = tables->getPhysicalSegment (0, nullptr, kIOMemoryMapperNone);
        l1[0] = (tablesPa + 4096) | 3;
        uint32_t attr = 0;
        PE_parse_boot_argn ("sheng-wifi-attr", &attr, sizeof (attr));
        const uint64_t share = attr == 1 ? 0 : (3ULL << 8);
        for (uint32_t i = 0; i < kExtraBlocks; ++i)
          l2[(kExtraIova >> 21) + i] = (bpa + i * kBlock) | (1ULL << 11) | (1ULL << 10) | share | (1ULL << 6) | 1;
        for (uint32_t off = 0; off < 8192; off += 64) __asm__ volatile ("dc civac, %0" :: "r" (reinterpret_cast<uint8_t *> (l1) + off) : "memory");
        __asm__ volatile ("dsb sy" ::: "memory");
        uintptr_t smmuBase = gHost.smmu;
        uintptr_t cb = smmuBase + (plan.pages + wplan.bank) * 4096;
        write32 (cb, 0x0, 0);
        write32 (cb, 0x20, static_cast<uint32_t> (tablesPa)); write32 (cb, 0x24, static_cast<uint32_t> (tablesPa >> 32));
        write32 (cb, 0x10, 5);
        write32 (cb, 0x30, 32U | (1U << 23));
        write32 (cb, 0x38, attr == 1 ? 0x44 : 0xff);
        write32 (cb, 0x618, 0); write32 (cb, 0x7f0, 0);
        __asm__ volatile ("dsb sy" ::: "memory");
        write32 (cb, 0x0, 1U | (1U << 5) | (1U << 6));
        __asm__ volatile ("dsb sy; isb" ::: "memory");
        gExtraSmmu = smmuBase; gExtraCb = cb; gExtraBank = wplan.bank;
        {
          uint32_t tested = 0, faults = 0, wrong = 0, first = 0;
          for (uint64_t iova = kExtraIova; iova < kExtraIova + static_cast<uint64_t> (kExtraBlocks) * kBlock; iova += 4096) {
            write32 (cb, 0x800, static_cast<uint32_t> (iova)); write32 (cb, 0x804, 0);
            uint32_t spin = 0;
            while ((read32 (cb, 0x8f0) & 1) && ++spin < 100000) { }
            if (spin >= 100000) { native_ufs_extra_atos[3] = 0xdead0000U | tested; break; }
            const uint32_t parLo = read32 (cb, 0x50), parHi = read32 (cb, 0x54);
            ++tested;
            if (tested == 1) { native_ufs_extra_atos[4] = parLo; native_ufs_extra_atos[5] = parHi; native_ufs_extra_atos[6] = static_cast<uint32_t> (bpa); native_ufs_extra_atos[7] = static_cast<uint32_t> (bpa >> 32); }
            if (parLo & 1) { if (!faults++) first = static_cast<uint32_t> (iova); continue; }
            const uint64_t pa = ((static_cast<uint64_t> (parHi) << 32) | parLo) & 0x0000fffffffff000ULL;
            if (pa != ((bpa + (iova - kExtraIova)) & ~0xfffULL)) { if (!wrong++ && !first) first = static_cast<uint32_t> (iova); }
          }
          native_ufs_extra_atos[0] = tested; native_ufs_extra_atos[1] = faults; native_ufs_extra_atos[2] = wrong;
          if (native_ufs_extra_atos[3] == 0) native_ufs_extra_atos[3] = first;
        }
        for (uint64_t off = 0; off < (kExtraBlocks + 1) * static_cast<uint64_t> (kBlock); off += 64)
          __asm__ volatile ("dc civac, %0" :: "r" (static_cast<uint8_t *> (buffer->getBytesNoCopy ()) + off) : "memory");
        __asm__ volatile ("dsb sy" ::: "memory");
        IOMemoryMap *wc = buffer->createMappingInTask (kernel_task, 0, kIOMapAnywhere | kIOMapWriteCombineCache);
        native_ufs_extra_va = wc ? reinterpret_cast<uint8_t *> (wc->getVirtualAddress ()) + (bva - static_cast<uint8_t *> (buffer->getBytesNoCopy ())) : bva;
        native_ufs_extra_uncached = wc != nullptr;
        native_ufs_extra_bytes = kExtraBlocks * kBlock;
        native_ufs_extra_iova = kExtraIova;
      }
      native_ufs_extra_stream = 0x80000000U | extraReport0;
    }
  }
  if (gRootDiagnosticEnabled) {
    gRootDiagnostic.smmuId2 = read32 (gHost.smmu, 0x28);
    gRootDiagnostic.cba2r = read32 (gHost.smmu, 0x1800 + 4 * plan.bank);
    gRootDiagnostic.gfsrBefore = read32 (gHost.smmu, 0x48);
    gRootDiagnostic.fsrBefore = read32 (gHost.cb, 0x58);
  }

  // 7. Initialize buffer and start queue
  bzero (gHost.virt, kDmaBytes);
  cacheCivac (gHost.virt, kDmaBytes);
  if (!hostStartQueue (why)) return false;

  // 8. Test transport via NOP OUT
  gRootDiagnostic.stage = "NOP OUT";
  if (hostNop () != kIOReturnSuccess) { *why = "NOP OUT"; return false; }

  // 9. Query power state and wake if sleep
  uint32_t power = 0;
  gRootDiagnostic.stage = "power query";
  if (hostQueryRead (3, 2, &power) != kIOReturnSuccess) { *why = "power query"; return false; }
  gRootDiagnostic.power = power;
  if ((power & 255) != 0x11) {
    uint8_t wake[6] = { 0x1b, 0, 0, 0, 0x10, 0 }; uint32_t scsi = 0;
    if (hostScsi (0xd0, wake, 6, false, 0, 500000, &scsi) != kIOReturnSuccess) {
      *why = "wake"; return false;
    }
    IOSleep (50);
  }

  // 10. Verify LUN ready (and clear power-on Unit Attention on LUNs 1..5)
  gRootDiagnostic.stage = "LUN ready";
  IOReturn result = kIOReturnNotReady;
  for (unsigned attempt = 0; attempt < 20; ++attempt) {
    uint8_t tur[6] = {}; uint32_t scsi = 0;
    result = hostScsi (0, tur, 6, false, 0, 200000, &scsi);
    if (result == kIOReturnSuccess || gHost.poisoned) break;
    IOSleep (10);
  }
  if (result != kIOReturnSuccess) { *why = "LUN ready"; return false; }
  for (uint8_t l = 1; l < 6 && !gHost.poisoned; ++l) {
    for (unsigned attempt = 0; attempt < 4; ++attempt) {
      uint8_t tur[6] = {}; uint32_t scsi = 0;
      if (hostScsi (l, tur, 6, false, 0, 100000, &scsi) == kIOReturnSuccess || gHost.poisoned) break;
    }
  }

  // 11. Verify 4096-byte sector size
  gRootDiagnostic.stage = "LUN capacity";
  uint8_t capacity[10] = { 0x25 }; uint32_t scsi = 0;
  if (hostScsi (0, capacity, 10, true, 8, 200000, &scsi) != kIOReturnSuccess ||
      loadBe32 (gHost.virt + kDataOffset + 4) != 4096) {
    *why = "LUN geometry differs from hardware baseline"; return false;
  }

  gHost.ready = true;
  return true;
}
