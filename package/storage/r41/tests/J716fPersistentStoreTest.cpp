// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host ASan/UBSan test (Test 14) for R41UfsTransport opt-in persistent Data
// store write-guard, WRITE(10) FUA/cached + SYNCHRONIZE CACHE(10) transport,
// realistic TB-J716F LUN 0 userdata geometry (128 GiB sparse backing), and
// quarantine permanent write lockout.
#include <array>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unordered_map>
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

struct SparseUfsFake {
  // Real TB-J716F LUN 0 geometry (128 GiB UFS 3.1, 4 KiB blocks).
  static constexpr uint32_t kLun0Blocks = 30693376U;
  static constexpr uint32_t kLun4Blocks = 524288U;
  static constexpr uint64_t kUdFirstLba = 2315048ULL;
  static constexpr uint64_t kUdLastLba = 30693370ULL;
  // Realistic ~10 GiB SHNGROOT image (2,621,440 x 4 KiB blocks).
  static constexpr uint64_t kShngImageBytes = 10ULL * 1024ULL * 1024ULL * 1024ULL;

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
  uint32_t lock_depth = 0;

  std::unordered_map<uint64_t, std::array<uint8_t, kBlockBytes>> sparse_lun0;
  uint64_t write_cmds = 0;
  uint64_t fua_write_cmds = 0;
  uint64_t cached_write_cmds = 0;
  uint64_t sync_cmds = 0;
  uint64_t clean_inv_data_ops = 0;
  bool inject_fatal_hci = false;

  ~SparseUfsFake() { free(aligned_dma); }

  std::array<uint8_t, kBlockBytes> &blockRef(uint64_t lba) {
    auto it = sparse_lun0.find(lba);
    if (it != sparse_lun0.end()) return it->second;
    std::array<uint8_t, kBlockBytes> blk{};
    blk.fill(0);
    return sparse_lun0.emplace(lba, blk).first->second;
  }

  void buildDisk() {
    sparse_lun0.clear();

    // Primary GPT entries at LBA 2..5 (128 x 128 B = 16 KiB = 4 blocks).
    std::vector<uint8_t> entries(128 * 128, 0);
    uint8_t *e = entries.data();
    e[0] = 0xA2; e[1] = 0xA0; e[2] = 0xD0; e[3] = 0xEB;
    e[16] = 1;
    putLe64(e + 32, kUdFirstLba);
    putLe64(e + 40, kUdLastLba);
    const char *ud_name = "userdata";
    for (size_t i = 0; ud_name[i]; ++i) {
      e[56 + 2 * i] = static_cast<uint8_t>(ud_name[i]);
      e[56 + 2 * i + 1] = 0;
    }
    const uint32_t entries_crc = ieeeCrc32(entries.data(), entries.size());
    for (uint64_t b = 0; b < 4; ++b) {
      memcpy(blockRef(2 + b).data(), entries.data() + b * kBlockBytes, kBlockBytes);
    }

    auto &hdr_arr = blockRef(1);
    uint8_t *hdr = hdr_arr.data();
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

    // Valid SHNGROOT manifest at kUdFirstLba covering 10 GiB.
    auto &m_arr = blockRef(kUdFirstLba);
    uint8_t *m = m_arr.data();
    memcpy(m, "SHNGROOT", 8);
    putLe32(m + 8, 1);
    putLe32(m + 12, 2);
    putLe64(m + 16, kShngImageBytes);
    auto writeWin = [&](unsigned idx, const char *name, uint64_t off, uint64_t sz) {
      uint8_t *w = m + 24 + 128 * idx;
      memcpy(w, name, strlen(name));
      putLe64(w + 32, off);
      putLe64(w + 40, sz);
      putLe32(w + 48, 512);
      putLe32(w + 52, 1);
    };
    writeWin(0, "OS", 0x10000ULL, 4ULL * 1024ULL * 1024ULL * 1024ULL);
    writeWin(1, "SystemOS", 5ULL * 1024ULL * 1024ULL * 1024ULL, 4ULL * 1024ULL * 1024ULL * 1024ULL);

    uint8_t digest[32] = {};
    sha256Compute(m, 4064, digest);
    memcpy(m + 4064, digest, 32);
  }

  void loadRecord(const R35Record &r) {
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
    if (inject_fatal_hci) {
      hci[R35Is] |= 0x10000U;  // System bus fatal error -> triggers quarantine
      return;
    }
    uint8_t *dma = aligned_dma;
    const uint32_t utrd0 = static_cast<uint32_t>(dma[0]) |
                           (static_cast<uint32_t>(dma[1]) << 8) |
                           (static_cast<uint32_t>(dma[2]) << 16) |
                           (static_cast<uint32_t>(dma[3]) << 24);
    const uint32_t dir = (utrd0 >> 25) & 3U;
    const uint8_t *cmd = dma + 4096;
    uint8_t *rsp = dma + 4608;
    uint8_t *data = dma + kDataOffset;
    memset(rsp, 0, 512);
    const uint8_t tc = cmd[0];
    const uint8_t lun = cmd[2];
    const uint8_t tag = cmd[3];

    if (tc == 0x00) {
      assert(dir == 0U);
      rsp[0] = 0x20; rsp[3] = tag;
    } else if (tc == 0x16) {
      assert(dir == 0U);
      rsp[0] = 0x36; rsp[3] = tag; rsp[5] = 1; rsp[12] = 3; rsp[13] = 2; rsp[23] = 0x11;
    } else if (tc == 0x01) {
      rsp[0] = 0x21; rsp[2] = lun; rsp[3] = tag;
      const uint8_t cdb0 = cmd[16];
      if (cdb0 == 0x00) {
        assert(dir == 0U);
      } else if (cdb0 == 0x25) {
        assert(dir == 2U);
        const uint32_t last = (lun == kUserLun ? kLun0Blocks : kLun4Blocks) - 1;
        data[0] = static_cast<uint8_t>(last >> 24);
        data[1] = static_cast<uint8_t>(last >> 16);
        data[2] = static_cast<uint8_t>(last >> 8);
        data[3] = static_cast<uint8_t>(last);
        data[4] = 0; data[5] = 0; data[6] = 0x10; data[7] = 0;
      } else if (cdb0 == 0x28) {
        assert(dir == 2U && cmd[1] == 0x40);
        const uint32_t lba = (uint32_t(cmd[18]) << 24) | (uint32_t(cmd[19]) << 16) |
                             (uint32_t(cmd[20]) << 8) | cmd[21];
        const uint32_t blks = (uint32_t(cmd[23]) << 8) | cmd[24];
        assert(blks >= 1 && blks <= kMaxChunkBlocks);
        if (lun == kKnownLun) {
          assert(lba == kKnownLba && blks == 1);
          memcpy(data, j716f_first_block::reference, kBlockBytes);
        } else {
          assert(lun == kUserLun && uint64_t(lba) + blks <= kLun0Blocks);
          for (uint32_t i = 0; i < blks; ++i) {
            auto it = sparse_lun0.find(uint64_t(lba) + i);
            if (it != sparse_lun0.end()) {
              memcpy(data + size_t(i) * kBlockBytes, it->second.data(), kBlockBytes);
            } else {
              memset(data + size_t(i) * kBlockBytes, 0, kBlockBytes);
            }
          }
        }
      } else if (cdb0 == 0x2A) {
        // WRITE(10): verify UTRD DD=01 (Host-to-Device), UPIU flags=0x20, LUN 0.
        assert(dir == 1U && cmd[1] == 0x20 && lun == kUserLun);
        assert((cmd[17] & ~0x08U) == 0);
        const uint32_t lba = (uint32_t(cmd[18]) << 24) | (uint32_t(cmd[19]) << 16) |
                             (uint32_t(cmd[20]) << 8) | cmd[21];
        const uint32_t blks = (uint32_t(cmd[23]) << 8) | cmd[24];
        assert(blks >= 1 && blks <= kMaxChunkBlocks);
        // Hardware-level check: GPT and SHNGROOT must NEVER be written.
        assert(lba >= kUdFirstLba + (kShngImageBytes / kBlockBytes) + kDataStoreGuardBlocks);
        assert(uint64_t(lba) + blks <= kUdLastLba + 1ULL - kDataStoreTailReserveBlocks);
        for (uint32_t i = 0; i < blks; ++i) {
          memcpy(blockRef(uint64_t(lba) + i).data(), data + size_t(i) * kBlockBytes, kBlockBytes);
        }
        ++write_cmds;
        if (cmd[17] & 0x08U) ++fua_write_cmds;
        else ++cached_write_cmds;
      } else if (cdb0 == 0x35) {
        // SYNCHRONIZE CACHE(10): verify DD=00, no data transfer, LUN 0.
        assert(dir == 0U && cmd[1] == 0x00 && lun == kUserLun);
        ++sync_cmds;
      } else {
        assert(false && "Unexpected SCSI opcode");
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
  auto &hw = *static_cast<SparseUfsFake *>(ctx);
  *out = hw.rec; *magic = hw.jown_magic; *seq = hw.jown_seq;
  return true;
}
bool cbRead32(void *ctx, uint64_t pa, uint32_t *out) {
  auto &hw = *static_cast<SparseUfsFake *>(ctx);
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
  auto &hw = *static_cast<SparseUfsFake *>(ctx);
  for (unsigned bank = 2; bank <= 3; ++bank) {
    const uint64_t base = R35_SMMU_BASE + 0x80000ULL + (static_cast<uint64_t>(bank) << 12);
    if (pa == base + 0x20ULL) { *out = hw.cb[bank - 2].ttbr[0]; return true; }
    if (pa == base + 0x28ULL) { *out = hw.cb[bank - 2].ttbr[1]; return true; }
  }
  return false;
}
bool cbWrite32(void *ctx, uint64_t pa, uint32_t val) {
  auto &hw = *static_cast<SparseUfsFake *>(ctx);
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
  auto &hw = *static_cast<SparseUfsFake *>(ctx);
  hw.ticks += 10; *out = hw.ticks; return true;
}
bool cbCache(void *ctx, sm8250_ufs::CacheOp op, size_t off, size_t) {
  auto &hw = *static_cast<SparseUfsFake *>(ctx);
  if (off == kDataOffset && op == sm8250_ufs::CacheOp::CleanInvalidate) {
    ++hw.clean_inv_data_ops;
  }
  return true;
}
bool cbAllocDma(void *ctx, sm8250_ufs::DmaView *out) {
  auto &hw = *static_cast<SparseUfsFake *>(ctx);
  void *mem = nullptr;
  if (posix_memalign(&mem, 4096, kDmaBytes) != 0 || !mem) return false;
  memset(mem, 0, kDmaBytes);
  hw.aligned_dma = static_cast<uint8_t *>(mem);
  out->cpu = hw.aligned_dma; out->physical = hw.dma_pa; out->bytes = kDmaBytes;
  return true;
}
void cbLock(void *ctx) { auto &hw = *static_cast<SparseUfsFake *>(ctx); assert(hw.lock_depth == 0); ++hw.lock_depth; }
void cbUnlock(void *ctx) { auto &hw = *static_cast<SparseUfsFake *>(ctx); assert(hw.lock_depth == 1); --hw.lock_depth; }

R35Record makeRecord() {
  R35Record r = {};
  r.magic = R35_MAGIC;
  r.version = R35_VERSION;
  r.bytes = R35_RECORD_BYTES;
  r.profile = R35_PROFILE;
  r.source = 1;
  r.boot_sequence = 7;
  r.mode = R35StopAndMaintain;
  r.boot_id = 0x4A37313646504552ULL;
  r.status = R35Maintained;
  r.phase = R35Done;
  r.host_stopped = 1;
  r.disabled_mask = 0xCU;
  r.synchronized_mask = 0xCU;
  r.routes_unchanged = 1;
  r.display_config_unchanged = 1;
  r.old_namespace_captured = 1;
  r.detacher_policy = R35PreserveUfsBanks;
  r.preserve_banks = 0x1CU;
  r.selected_slots[0] = 2;
  r.selected_slots[1] = 3;
  r.selected_banks[0] = 2;
  r.selected_banks[1] = 3;
  r.finished = 1;
  r.post_detacher_recorded = 1;
  r.post_detacher_preserved = 1;
  r.maintenance_record_valid = 1;

  auto fill = [](R35Snapshot &s) {
    s.hci_valid = (1U << R35_HCI_WORDS) - 1U;
    s.hci[R35Cap] = 0x0100001FU;
    s.hci[R35Ver] = 0x00000300U;
    s.hci[R35Hce] = 1U;
    s.hci[R35Hcs] = 0x0000000FU;
    s.header_valid = 0xFU;
    s.headers[0] = 0x48001E57U;
    s.headers[1] = 0x30000008U;
    s.headers[2] = 0x00001222U;
    s.headers[3] = 0x00000000U;
    s.stream_count = R35_GROUPS;
    for (unsigned i = 0; i < R35_GROUPS; ++i) s.streams[i].valid = 3U;
    s.streams[2].smr = 0x800004E0U;
    s.streams[2].s2cr = 2U;
    s.streams[3].smr = 0x800000E0U;
    s.streams[3].s2cr = 3U;
    s.streams[4].smr = 0x84020820U;
    s.streams[4].s2cr = 0x00000004U;
    for (unsigned idx = 0; idx < 3; ++idx) {
      s.context[idx].bank = idx + 2;
      s.context[idx].valid32 = R35_CONTEXT_ALL;
      s.context[idx].valid64 = 3U;
      s.context[idx].words[R35Cbar] = 0x00010000U;
      s.context[idx].words[R35Cba2r] = 1U;
    }
  };
  fill(r.before);
  fill(r.after);
  r.crc32 = r35_record_crc(&r);
  assert(r35_record_validate(&r));
  return r;
}

bool copySink(void *ctx, uint64_t off, const uint8_t *src, size_t len) {
  memcpy(static_cast<uint8_t *>(ctx) + off, src, len);
  return true;
}

}  // namespace

int main() {
  SparseUfsFake fake;
  fake.loadRecord(makeRecord());

  Platform plat{&fake, cbReadRam, cbRead32, cbRead64, cbWrite32, cbBarrier,
                cbTicks, cbCache, cbAllocDma, cbLock, cbUnlock, nullptr, nullptr};
  Budget budget{1000000ULL, 10000U, 2};

  Transport tr;
  InitReport rep = tr.initialize(plat, budget, j716f_first_block::reference);
  assert(rep.error == Error::None);
  assert(tr.online());
  assert(rep.gpt.manifest.valid);
  assert(!tr.writeWindow().armed);

  // Save untouched GPT LBA 1 and SHNGROOT LBA 0 copies to assert immutability at the end.
  const auto orig_gpt_hdr = fake.blockRef(1);
  const auto orig_shngroot = fake.blockRef(SparseUfsFake::kUdFirstLba);

  // 1. Bounds check on real TB-J716F 128 GiB geometry.
  uint64_t min_lba = 0, max_excl = 0;
  assert(tr.computeDataStoreBounds(&min_lba, &max_excl));
  const uint64_t expected_min =
      SparseUfsFake::kUdFirstLba + (SparseUfsFake::kShngImageBytes / kBlockBytes) +
      kDataStoreGuardBlocks;
  const uint64_t expected_max =
      SparseUfsFake::kUdLastLba + 1ULL - kDataStoreTailReserveBlocks;
  assert(min_lba == expected_min);
  assert(max_excl == expected_max);

  // 2. Unarmed transport must reject all writes and syncs with 0 HCI write commands.
  std::vector<uint8_t> one_block(kBlockBytes, 0x5AU);
  assert(!tr.writeBlocks(kUserLun, min_lba, 1, one_block.data(), true));
  assert(!tr.syncCache(kUserLun));
  assert(fake.write_cmds == 0 && fake.sync_cmds == 0);

  // 3. Reject arming inside GPT, SHNGROOT, 64 MiB guard, 1 GiB tail reserve, or zero/overflow.
  assert(!tr.armWriteWindow(1, 16));
  assert(!tr.armWriteWindow(SparseUfsFake::kUdFirstLba, 16));
  assert(!tr.armWriteWindow(min_lba - 1, 16));
  assert(!tr.armWriteWindow(max_excl - 16, 17));
  assert(!tr.armWriteWindow(max_excl, 16));
  assert(!tr.armWriteWindow(min_lba, 0));
  assert(!tr.armWriteWindow(min_lba, UINT64_MAX));
  assert(!tr.writeWindow().armed);

  // 4. Arm realistic 16 GiB Data store (4,194,433 blocks) at end of allowed range.
  const uint64_t store_blocks = 4194433ULL;
  const uint64_t first_lba = max_excl - store_blocks;
  assert(tr.armWriteWindow(first_lba, store_blocks));
  assert(tr.writeWindow().armed);
  assert(tr.writeWindow().first_lba == first_lba);
  assert(tr.writeWindow().block_count == store_blocks);

  // 5. Immutable one-shot: second armWriteWindow must be rejected.
  assert(!tr.armWriteWindow(min_lba, 64));
  assert(tr.writeWindow().first_lba == first_lba);

  // 6. Reject out-of-window writes and wrong-LUN writes even after arming.
  assert(!tr.writeBlocks(kUserLun, 1, 1, one_block.data(), true));
  assert(!tr.writeBlocks(kUserLun, SparseUfsFake::kUdFirstLba, 1, one_block.data(), true));
  assert(!tr.writeBlocks(kUserLun, first_lba - 1, 1, one_block.data(), true));
  assert(!tr.writeBlocks(kUserLun, first_lba - 1, 2, one_block.data(), true));
  assert(!tr.writeBlocks(kUserLun, first_lba + store_blocks, 1, one_block.data(), true));
  assert(!tr.writeBlocks(kUserLun, first_lba + store_blocks - 1, 2, one_block.data(), true));
  assert(!tr.writeBlocks(kKnownLun, first_lba, 1, one_block.data(), true));
  assert(!tr.syncCache(kKnownLun));
  assert(fake.write_cmds == 0 && fake.sync_cmds == 0);

  // 7. Multi-chunk FUA write (70 blocks = 32 + 32 + 6 -> 3 WRITE(10) commands) + read-back.
  const uint32_t multi_blocks = 70;
  std::vector<uint8_t> tx(static_cast<size_t>(multi_blocks) * kBlockBytes);
  for (size_t i = 0; i < tx.size(); ++i) {
    tx[i] = static_cast<uint8_t>((i * 131U + (i >> 12) * 17U + 0xA5U) & 0xFFU);
  }
  assert(tr.writeBlocks(kUserLun, first_lba, multi_blocks, tx.data(), /*fua=*/true));
  assert(fake.write_cmds == 3);
  assert(fake.fua_write_cmds == 3);
  assert(fake.cached_write_cmds == 0);
  assert(fake.clean_inv_data_ops >= 3);

  std::vector<uint8_t> rx(tx.size(), 0);
  assert(tr.readBlocksWithSink(kUserLun, first_lba, multi_blocks, copySink, rx.data()));
  assert(memcmp(tx.data(), rx.data(), tx.size()) == 0);

  // 8. Cached write at the very end of the window + SYNCHRONIZE CACHE(10) + read-back.
  std::vector<uint8_t> tail_tx(3 * kBlockBytes, 0xC3U);
  const uint64_t tail_lba = first_lba + store_blocks - 3;
  assert(tr.writeBlocks(kUserLun, tail_lba, 3, tail_tx.data(), /*fua=*/false));
  assert(fake.cached_write_cmds == 1);
  assert(tr.syncCache(kUserLun));
  assert(fake.sync_cmds == 1);

  std::vector<uint8_t> tail_rx(tail_tx.size(), 0);
  assert(tr.readBlocksWithSink(kUserLun, tail_lba, 3, copySink, tail_rx.data()));
  assert(memcmp(tail_tx.data(), tail_rx.data(), tail_tx.size()) == 0);

  Stats st = tr.stats();
  assert(st.write_armed);
  assert(!st.quarantined);
  assert(st.write_calls == 2);
  assert(st.write_commands == 4);
  assert(st.blocks_written == 73);
  assert(st.sync_commands == 1);

  // 9. Hardware fault triggers quarantine -> permanently disables writes and syncs.
  fake.inject_fatal_hci = true;
  assert(!tr.writeBlocks(kUserLun, first_lba, 1, one_block.data(), true));
  assert(!tr.online());
  assert(!tr.writeWindow().armed);
  st = tr.stats();
  assert(st.quarantined);
  assert(!st.write_armed);

  fake.inject_fatal_hci = false;
  assert(!tr.writeBlocks(kUserLun, first_lba, 1, one_block.data(), true));
  assert(!tr.syncCache(kUserLun));
  assert(!tr.armWriteWindow(first_lba, 16));

  // 10. GPT header and SHNGROOT manifest remained bit-for-bit untouched.
  assert(fake.blockRef(1) == orig_gpt_hdr);
  assert(fake.blockRef(SparseUfsFake::kUdFirstLba) == orig_shngroot);

  printf("PASS J716fPersistentStoreTest: bounds=[%llu..%llu) win=%llu+%llu wr=%llu/%llub sync=%llu quar=ok\n",
         static_cast<unsigned long long>(min_lba),
         static_cast<unsigned long long>(max_excl),
         static_cast<unsigned long long>(first_lba),
         static_cast<unsigned long long>(store_blocks),
         static_cast<unsigned long long>(st.write_commands),
         static_cast<unsigned long long>(st.blocks_written),
         static_cast<unsigned long long>(st.sync_commands));
  return 0;
}
