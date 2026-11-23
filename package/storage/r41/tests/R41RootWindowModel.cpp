// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host ASan/UBSan model test for r41 SHNGROOT manifest SHA-256 verification and
// 512-byte logical window slicing across 4 KiB and 128 KiB chunk boundaries.
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

#include "KnownBlockTrial.h"
#include "R41Sha256.h"
#include "R41UfsTransport.h"

using namespace j716f_r41;

namespace {

void putLe32(uint8_t *p, uint32_t v) {
  for (unsigned i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
}
void putLe64(uint8_t *p, uint64_t v) {
  for (unsigned i = 0; i < 8; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
}

struct AlignedFake {
  R35Record rec = {};
  uint32_t jown_magic = kOwnerMagic;
  uint32_t jown_seq = 1;

  uint32_t hci[R35_HCI_WORDS] = {};
  uint32_t headers[4] = {};
  uint32_t smr[R35_GROUPS] = {};
  uint32_t s2cr[R35_GROUPS] = {};
  R35Context cb[3] = {};

  uint8_t *aligned_dma = nullptr;
  uint64_t dma_pa = 0xD44C8000ULL;
  uint64_t ticks = 1000;
  uint32_t smmu_writes = 0;
  uint32_t lock_depth = 0;

  // Synthetic LUN 0: 512 x 4 KiB = 2 MiB; userdata starts at LBA 64 (1.75 MiB)
  static constexpr uint32_t kLun0Blocks = 512;
  static constexpr uint32_t kLun4Blocks = 524288;
  static constexpr uint64_t kUdFirstLba = 64;
  static constexpr uint64_t kUdLastLba = 495;
  std::vector<uint8_t> lun0_disk;

  bool corrupt_manifest_sha = false;
  bool overlap_windows = false;
  bool no_shngroot = false;

  ~AlignedFake() { free(aligned_dma); }

  void buildDisk() {
    lun0_disk.assign(static_cast<size_t>(kLun0Blocks) * kBlockBytes, 0);
    for (size_t i = 0; i < lun0_disk.size(); ++i) {
      lun0_disk[i] = static_cast<uint8_t>((i * 251U + (i >> 9) * 73U + 0x3DU) & 0xFFU);
    }
    // Primary GPT entries at LBA 2..5
    uint8_t *entries = lun0_disk.data() + 2 * kBlockBytes;
    memset(entries, 0, 128 * 128);
    uint8_t *e = entries;
    e[0] = 0xA2; e[1] = 0xA0; e[2] = 0xD0; e[3] = 0xEB;
    e[16] = 1;
    putLe64(e + 32, kUdFirstLba);
    putLe64(e + 40, kUdLastLba);
    const char *ud_name = "userdata";
    for (size_t i = 0; ud_name[i]; ++i) {
      e[56 + 2 * i] = static_cast<uint8_t>(ud_name[i]);
      e[56 + 2 * i + 1] = 0;
    }
    const uint32_t entries_crc = ieeeCrc32(entries, 128 * 128);

    uint8_t *hdr = lun0_disk.data() + kBlockBytes;
    memset(hdr, 0, kBlockBytes);
    putLe64(hdr + 0, 0x5452415020494645ULL);
    putLe32(hdr + 8, 0x00010000U);
    putLe32(hdr + 12, 92U);
    putLe64(hdr + 24, 1);
    putLe64(hdr + 32, kLun0Blocks - 1);
    putLe64(hdr + 40, 16);
    putLe64(hdr + 48, kLun0Blocks - 16);
    putLe64(hdr + 72, 2);
    putLe32(hdr + 80, 128);
    putLe32(hdr + 84, 128);
    putLe32(hdr + 88, entries_crc);
    putLe32(hdr + 16, ieeeCrc32(hdr, 92));

    // SHNGROOT manifest at kUdFirstLba
    uint8_t *m = lun0_disk.data() + static_cast<size_t>(kUdFirstLba) * kBlockBytes;
    memset(m, 0, kBlockBytes);
    if (no_shngroot) {
      m[0] = 0x63; m[1] = 0xb2; m[2] = 0x61; m[3] = 0x7e;
      return;
    }
    memcpy(m, "SHNGROOT", 8);
    putLe32(m + 8, 1);
    putLe32(m + 12, 2);
    // OS window: offset 0x10000 (64 KiB), size 300 KiB + 512 B (non-4K-multiple size!)
    // SystemOS window: offset 0x80000 (512 KiB) [or overlapping 0x5B000 if overlap_windows], size 256 KiB
    const uint64_t os_off = 0x10000ULL;
    const uint64_t os_sz = 300ULL * 1024ULL + 512ULL;  // 601 x 512B sectors (> 2 x 128 KiB!)
    const uint64_t sys_off = overlap_windows ? 0x5B000ULL : 0x80000ULL;
    const uint64_t sys_sz = 256ULL * 1024ULL;
    const uint64_t img_bytes = 0x100000ULL;  // 1 MiB
    putLe64(m + 16, img_bytes);

    auto writeWin = [&](unsigned idx, const char *name, uint64_t off, uint64_t sz) {
      uint8_t *w = m + 24 + 128 * idx;
      memcpy(w, name, strlen(name));
      putLe64(w + 32, off);
      putLe64(w + 40, sz);
      putLe32(w + 48, 512);
      putLe32(w + 52, 1);
    };
    writeWin(0, "OS", os_off, os_sz);
    writeWin(1, "SystemOS", sys_off, sys_sz);

    uint8_t digest[32] = {};
    sha256Compute(m, 4064, digest);
    if (corrupt_manifest_sha) digest[0] ^= 0x80U;
    memcpy(m + 4064, digest, 32);
  }

  void loadFromRecord(const R35Record &r) {
    rec = r;
    jown_seq = r.boot_sequence;
    memcpy(hci, r.after.hci, sizeof(hci));
    memcpy(headers, r.after.headers, sizeof(headers));
    for (unsigned i = 0; i < R35_GROUPS; ++i) {
      smr[i] = r.after.streams[i].smr;
      s2cr[i] = r.after.streams[i].s2cr;
    }
    memcpy(cb, r.after.context, sizeof(cb));
    buildDisk();
  }

  void handleDoorbell() {
    uint8_t *dma = aligned_dma;
    const uint8_t *cmd = dma + 4096;
    uint8_t *rsp = dma + 4608;
    uint8_t *data = dma + kDataOffset;
    memset(rsp, 0, 512);
    const uint8_t tc = cmd[0];
    const uint8_t lun = cmd[2];
    const uint8_t tag = cmd[3];
    if (tc == 0x00) {
      rsp[0] = 0x20; rsp[3] = tag;
    } else if (tc == 0x16) {
      rsp[0] = 0x36; rsp[3] = tag; rsp[5] = 1; rsp[12] = 3; rsp[13] = 2; rsp[23] = 0x11;
    } else if (tc == 0x01) {
      rsp[0] = 0x21; rsp[2] = lun; rsp[3] = tag;
      const uint8_t cdb0 = cmd[16];
      if (cdb0 == 0x00) {
      } else if (cdb0 == 0x25) {
        const uint32_t last = (lun == kUserLun ? kLun0Blocks : kLun4Blocks) - 1;
        data[0] = static_cast<uint8_t>(last >> 24);
        data[1] = static_cast<uint8_t>(last >> 16);
        data[2] = static_cast<uint8_t>(last >> 8);
        data[3] = static_cast<uint8_t>(last);
        data[4] = 0; data[5] = 0; data[6] = 0x10; data[7] = 0;
      } else if (cdb0 == 0x28) {
        const uint32_t lba = (uint32_t(cmd[18]) << 24) | (uint32_t(cmd[19]) << 16) |
                             (uint32_t(cmd[20]) << 8) | cmd[21];
        const uint32_t blks = (uint32_t(cmd[23]) << 8) | cmd[24];
        assert(blks >= 1 && blks <= kMaxChunkBlocks);
        if (lun == kKnownLun) {
          assert(lba == kKnownLba && blks == 1);
          memcpy(data, j716f_first_block::reference, kBlockBytes);
        } else {
          assert(lun == kUserLun && lba + blks <= kLun0Blocks);
          memcpy(data, lun0_disk.data() + static_cast<size_t>(lba) * kBlockBytes,
                 static_cast<size_t>(blks) * kBlockBytes);
        }
      } else {
        assert(false && "Forbidden opcode");
      }
    } else {
      assert(false && "Unsupported UPIU");
    }
    dma[8] = dma[9] = dma[10] = dma[11] = 0;
    hci[R35TransferDoorbell] = 0;
    hci[R35Is] |= 1U;
  }
};

bool cbReadRam(void *ctx, R35Record *out, uint32_t *magic, uint32_t *seq) {
  auto &hw = *static_cast<AlignedFake *>(ctx);
  *out = hw.rec; *magic = hw.jown_magic; *seq = hw.jown_seq;
  return true;
}
bool cbRead32(void *ctx, uint64_t pa, uint32_t *out) {
  auto &hw = *static_cast<AlignedFake *>(ctx);
  if (pa >= 0x00177000ULL && pa < 0x00178000ULL) {
    *out = (pa == 0x00177004ULL) ? 0x80000000U : (pa == 0x00177000ULL ? 0U : 1U);
    return true;
  }
  for (unsigned i = 0; i < R35_HCI_WORDS; ++i)
    if (pa == hciAddress(i)) { *out = hw.hci[i]; return true; }
  static const unsigned hdr_off[4] = {0x20, 0x24, 0x28, 0x00};
  for (unsigned i = 0; i < 4; ++i)
    if (pa == R35_SMMU_BASE + hdr_off[i]) { *out = hw.headers[i]; return true; }
  if (pa >= R35_SMMU_BASE + 0x800 && pa < R35_SMMU_BASE + 0x800 + 4U * R35_GROUPS) {
    *out = hw.smr[(pa - (R35_SMMU_BASE + 0x800)) / 4]; return true;
  }
  if (pa >= R35_SMMU_BASE + 0xC00 && pa < R35_SMMU_BASE + 0xC00 + 4U * R35_GROUPS) {
    *out = hw.s2cr[(pa - (R35_SMMU_BASE + 0xC00)) / 4]; return true;
  }
  for (unsigned bank = 2; bank <= 3; ++bank)
    for (unsigned w = 0; w < R35_CONTEXT_WORDS; ++w)
      if (pa == contextAddress(bank, w)) { *out = hw.cb[bank - 2].words[w]; return true; }
  return false;
}
bool cbRead64(void *ctx, uint64_t pa, uint64_t *out) {
  auto &hw = *static_cast<AlignedFake *>(ctx);
  for (unsigned bank = 2; bank <= 3; ++bank) {
    const uint64_t base = R35_SMMU_BASE + 0x80000ULL + (static_cast<uint64_t>(bank) << 12);
    if (pa == base + 0x20ULL) { *out = hw.cb[bank - 2].ttbr[0]; return true; }
    if (pa == base + 0x28ULL) { *out = hw.cb[bank - 2].ttbr[1]; return true; }
  }
  return false;
}
bool cbWrite32(void *ctx, uint64_t pa, uint32_t val) {
  auto &hw = *static_cast<AlignedFake *>(ctx);
  if (pa >= R35_SMMU_BASE && pa < R35_SMMU_BASE + 0x100000ULL) { ++hw.smmu_writes; return false; }
  if (pa == R35_HCI_BASE + 0x20) { hw.hci[R35Is] &= ~val; return true; }
  if (pa == R35_HCI_BASE + 0x50) { hw.hci[R35TransferBaseLow] = val; return true; }
  if (pa == R35_HCI_BASE + 0x54) { hw.hci[R35TransferBaseHigh] = val; return true; }
  if (pa == R35_HCI_BASE + 0x60) { hw.hci[R35TransferRunStop] = val; return true; }
  if (pa == R35_HCI_BASE + 0x58) {
    hw.hci[R35TransferDoorbell] = val;
    if (val & 1U) hw.handleDoorbell();
    return true;
  }
  return false;
}
bool cbBarrier(void *) { return true; }
bool cbTicks(void *ctx, uint64_t *out) {
  auto &hw = *static_cast<AlignedFake *>(ctx);
  hw.ticks += 10; *out = hw.ticks; return true;
}
bool cbCache(void *, sm8250_ufs::CacheOp, size_t, size_t) { return true; }
bool cbAllocDma(void *ctx, sm8250_ufs::DmaView *out) {
  auto &hw = *static_cast<AlignedFake *>(ctx);
  void *mem = nullptr;
  if (posix_memalign(&mem, 4096, kDmaBytes) != 0 || !mem) return false;
  memset(mem, 0, kDmaBytes);
  hw.aligned_dma = static_cast<uint8_t *>(mem);
  out->cpu = hw.aligned_dma; out->physical = hw.dma_pa; out->bytes = kDmaBytes;
  return true;
}
void cbLock(void *ctx) { auto &hw = *static_cast<AlignedFake *>(ctx); assert(hw.lock_depth == 0); ++hw.lock_depth; }
void cbUnlock(void *ctx) { auto &hw = *static_cast<AlignedFake *>(ctx); assert(hw.lock_depth == 1); --hw.lock_depth; }

R35Record loadRecordFile(const char *path) {
  FILE *f = fopen(path, "rb");
  assert(f);
  R35Record r = {};
  assert(fread(&r, 1, sizeof(r), f) == sizeof(r));
  fclose(f);
  return r;
}

}  // namespace

int main(int argc, char **argv) {
  assert(argc == 3);
  const R35Record rec = loadRecordFile(argv[1]);
  const char *mode = argv[2];

  // Quick SHA-256 KAT ("abc")
  uint8_t kat[32] = {};
  sha256Compute(reinterpret_cast<const uint8_t *>("abc"), 3, kat);
  assert(kat[0] == 0xba && kat[1] == 0x78 && kat[2] == 0x16 && kat[31] == 0xad);

  AlignedFake hw;
  if (strcmp(mode, "bad-manifest-sha") == 0) hw.corrupt_manifest_sha = true;
  if (strcmp(mode, "overlap-windows") == 0) hw.overlap_windows = true;
  if (strcmp(mode, "no-shngroot") == 0) hw.no_shngroot = true;
  hw.loadFromRecord(rec);

  const Platform plat{&hw, cbReadRam, cbRead32, cbRead64, cbWrite32,
                      cbBarrier, cbTicks, cbCache, cbAllocDma, cbLock, cbUnlock};
  const Budget budget{1000000ULL, 500, 2};
  Transport t;
  const InitReport r = t.initialize(plat, budget, j716f_first_block::reference);
  assert(r.error == Error::None && t.online());

  if (strcmp(mode, "happy-path") == 0) {
    assert(r.gpt.manifest.valid && r.gpt.manifest.status == 0);
    assert(r.gpt.manifest.os_index == 0 && r.gpt.manifest.systemos_index == 1);
    const auto &os_w = r.gpt.manifest.windows[r.gpt.manifest.os_index];
    assert(os_w.size_bytes == 300ULL * 1024ULL + 512ULL);

    // Exhaustive 512-byte window slicing tests across 4 KiB & 128 KiB boundaries
    const struct { uint64_t blk; uint64_t cnt; } cases[] = {
        {0, 1}, {1, 1}, {7, 2}, {3, 13}, {0, 8}, {5, 64},
        {3, 257},     // crosses 128 KiB chunk boundary + unaligned start & end
        {0, 601},     // entire 300.5 KiB window (> 2 x 128 KiB chunks)
        {600, 1},     // exact final 512B sector
        {599, 2},     // final 2 sectors
    };
    for (const auto &c : cases) {
      std::vector<uint8_t> got(c.cnt * 512ULL, 0xCC);
      auto vec_sink = [](void *ctx, uint64_t off, const uint8_t *src, size_t n) -> bool {
        auto &v = *static_cast<std::vector<uint8_t> *>(ctx);
        assert(off + n <= v.size());
        memcpy(v.data() + off, src, n);
        return true;
      };
      uint64_t transferred = 0;
      const bool ok = t.readWindow512WithSink(os_w.abs_offset, os_w.size_bytes,
                                              c.blk, c.cnt, vec_sink, &got, &transferred);
      assert(ok && transferred == c.cnt * 512ULL);
      const uint8_t *expected = hw.lun0_disk.data() + os_w.abs_offset + c.blk * 512ULL;
      assert(memcmp(got.data(), expected, got.size()) == 0);
    }
    // Out-of-bounds must be refused
    std::vector<uint8_t> dummy(1024);
    auto noop = [](void *, uint64_t, const uint8_t *, size_t) -> bool { return true; };
    assert(!t.readWindow512WithSink(os_w.abs_offset, os_w.size_bytes, 600, 2, noop, &dummy));
    assert(!t.readWindow512WithSink(os_w.abs_offset, os_w.size_bytes, 601, 1, noop, &dummy));
    assert(hw.smmu_writes == 0 && hw.lock_depth == 0);
    printf("PASS happy-path: SHNGROOT SHA-256 valid + 10 window-512 slicing cases + OOB refused (maxch=%u)\n",
           t.stats().max_chunk_blocks);
  } else if (strcmp(mode, "bad-manifest-sha") == 0) {
    assert(!r.gpt.manifest.valid && r.gpt.manifest.status == 3);
    printf("PASS bad-manifest-sha: rejected with status=3\n");
  } else if (strcmp(mode, "overlap-windows") == 0) {
    assert(!r.gpt.manifest.valid && r.gpt.manifest.status == 6);
    printf("PASS overlap-windows: rejected with status=6\n");
  } else if (strcmp(mode, "no-shngroot") == 0) {
    assert(!r.gpt.manifest.valid && r.gpt.manifest.status == 1);
    printf("PASS no-shngroot: cleanly reported status=1 without quarantine\n");
  } else {
    assert(false);
  }
  return 0;
}
