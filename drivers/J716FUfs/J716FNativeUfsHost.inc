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

  // 10. Verify LUN ready
  gRootDiagnostic.stage = "LUN ready";
  IOReturn result = kIOReturnNotReady;
  for (unsigned attempt = 0; attempt < 20; ++attempt) {
    uint8_t tur[6] = {}; uint32_t scsi = 0;
    result = hostScsi (0, tur, 6, false, 0, 200000, &scsi);
    if (result == kIOReturnSuccess || gHost.poisoned) break;
    IOSleep (10);
  }
  if (result != kIOReturnSuccess) { *why = "LUN ready"; return false; }

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
