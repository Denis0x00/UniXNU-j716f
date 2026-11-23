// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef J716F_R41_UFS_TRANSPORT_H
#define J716F_R41_UFS_TRANSPORT_H

#include <stddef.h>
#include <stdint.h>
#include "R35Handoff.h"
#include "AppsSmmuOwner.h"
#include "Sm8250UfsIo.h"
#include "UfsObservation.h"

namespace j716f_r41 {

constexpr uint64_t kRamSlotPhysical = 0xB007E000ULL;
constexpr uint64_t kRamSlotBytes = 0x1000ULL;
constexpr uint64_t kOwnerPagePhysical = 0xB007F000ULL;
constexpr uint32_t kOwnerOffsetInPage = 0xFF4U;
constexpr uint32_t kOwnerMagic = 0x4E574F4AU;  // 'JOWN'
constexpr uint8_t kKnownLun = 4;
constexpr uint64_t kKnownLba = 155466;
constexpr uint8_t kUserLun = 0;
constexpr size_t kBlockBytes = 4096;
constexpr size_t kLogicalBlockBytes = 512;
constexpr size_t kDmaBytes = 139264;       // 8 KiB descriptors + 128 KiB data
constexpr size_t kDataOffset = 8192;
constexpr size_t kMaxDataBytes = 131072;   // 128 KiB = 32 x 4 KiB blocks
constexpr uint32_t kMaxChunkBlocks = 32;
constexpr unsigned kMaxRootWindows = 4;

enum class Phase : unsigned {
  Waiting = 0,
  ReadRecord,
  VerifyHardware,
  AllocateDma,
  InstallOwner,
  SetupQueue,
  CommandNop,
  CommandPowerMode,
  Lun4SelfTest,
  Lun0Capacity,
  Lun0GptScan,
  Online,
  Quarantined
};

enum class Error : unsigned {
  None = 0,
  Busy,
  AlreadyInitialized,
  InvalidPlatform,
  InvalidBudget,
  RamSlotReadFailed,
  InvalidOwnerTail,
  InvalidRecord,
  RecordEpochMismatch,
  RecordNotMaintained,
  RecordUnsupportedClaim,
  GccObservationFailed,
  HciReadFailed,
  HciStateMismatch,
  SmmuReadFailed,
  SmmuHeaderMismatch,
  SmmuStreamMismatch,
  SmmuContextMismatch,
  HciDrift,
  DmaAllocFailed,
  DmaRangeInvalid,
  OwnerFailed,
  QueueSetupFailed,
  NopFailed,
  PowerModeFailed,
  Lun4TurFailed,
  Lun4CapacityFailed,
  Lun4ReadFailed,
  Lun4Mismatch,
  Lun0TurFailed,
  Lun0CapacityFailed,
  Lun0GptReadFailed,
  Lun0GptInvalid,
  NotOnline,
  ReadOutOfRange,
  ReadTransferFailed,
  PostCheckFailed
};

struct Budget {
  uint64_t command_timeout_ticks = 0;
  uint32_t command_max_polls = 0;
  unsigned tur_retries = 2;
};

struct Platform {
  void *context = nullptr;
  bool (*read_ram_slot)(void *, R35Record *out_record, uint32_t *out_jown_magic,
                        uint32_t *out_jown_seq) = nullptr;
  bool (*read32)(void *, uint64_t physical, uint32_t *out) = nullptr;
  bool (*read64)(void *, uint64_t physical, uint64_t *out) = nullptr;
  bool (*write32)(void *, uint64_t physical, uint32_t value) = nullptr;
  bool (*barrier)(void *) = nullptr;
  bool (*ticks)(void *, uint64_t *out) = nullptr;
  bool (*cache)(void *, sm8250_ufs::CacheOp op, size_t offset, size_t bytes) = nullptr;
  bool (*allocate_dma)(void *, sm8250_ufs::DmaView *out) = nullptr;
  void (*lock)(void *) = nullptr;
  void (*unlock)(void *) = nullptr;
};

struct LunInfo {
  bool ready = false;
  uint64_t blocks = 0;
  uint32_t block_bytes = 0;
};

struct RootWindowSpec {
  char name[32] = {};
  uint64_t rel_offset = 0;    // offset within userdata partition (4 KiB aligned)
  uint64_t abs_offset = 0;    // absolute byte offset on LUN 0 (4 KiB aligned)
  uint64_t size_bytes = 0;    // window length (512 B multiple)
  uint8_t sha256[32] = {};
};

struct ShngRootManifest {
  bool valid = false;
  // 0=ok, 1=no-magic, 2=bad-version/count, 3=sha256-mismatch, 4=bounds, 5=name, 6=overlap, 7=missing-os-or-sysos
  uint32_t status = 1;
  uint32_t version = 0;
  uint32_t magic0 = 0;
  uint32_t magic1 = 0;
  uint64_t image_bytes = 0;
  uint32_t window_count = 0;
  RootWindowSpec windows[kMaxRootWindows] = {};
  int os_index = -1;
  int systemos_index = -1;
};

struct GptSummary {
  bool header_valid = false;
  bool entries_valid = false;
  uint32_t active_partitions = 0;
  bool found_userdata = false;
  uint64_t userdata_first_lba = 0;
  uint64_t userdata_last_lba = 0;
  ShngRootManifest manifest = {};
};

struct PostCheck {
  bool checked = false;
  bool display_slot4_unchanged = false;
  uint32_t cb2_fsr = 0;
  uint32_t cb3_fsr = 0;
  uint32_t cb2_tlbstatus = 0;
  uint32_t cb3_tlbstatus = 0;
  uint32_t utrlrsr = 0;
  uint32_t utrldbr = 0;
};

struct InitReport {
  Phase phase = Phase::Waiting;
  Error error = Error::None;
  uint64_t failed_address = 0;
  uint32_t jown_sequence = 0;
  uint32_t record_sequence = 0;
  uint32_t record_crc32 = 0;
  bool record_valid = false;
  bool hardware_matched = false;
  uint64_t dma_physical = 0;
  AppsSmmuResult owner_error = APPS_SMMU_OK;
  bool queue_started = false;
  bool lun4_matched = false;
  uint32_t lun4_crc32 = 0;
  LunInfo lun4 = {};
  LunInfo lun0 = {};
  GptSummary gpt = {};
  PostCheck post = {};
};

struct Stats {
  uint64_t read_calls = 0;
  uint64_t read_commands = 0;
  uint64_t blocks_read = 0;
  uint64_t read_errors = 0;
  uint32_t max_chunk_blocks = 0;
  bool quarantined = false;
};

const char *phaseName(Phase p);
const char *errorName(Error e);
uint32_t crc32Step(uint32_t crc, const uint8_t *data, size_t bytes);
uint32_t ieeeCrc32(const uint8_t *data, size_t bytes);

inline uint64_t hciAddress(unsigned index) {
  static constexpr unsigned kOffsets[R35_HCI_WORDS] = {
      0x00, 0x08, 0x34, 0x30, 0x20, 0x24, 0x50,
      0x54, 0x58, 0x60, 0x70, 0x74, 0x78, 0x80};
  return index < R35_HCI_WORDS ? R35_HCI_BASE + kOffsets[index] : 0;
}

inline uint64_t contextAddress(unsigned bank, unsigned word) {
  static constexpr unsigned kOffsets[R35_CONTEXT_WORDS] = {
      0x00, 0x04, 0x00, 0x00, 0x30, 0x10, 0x34, 0x38, 0x3C, 0x58, 0x7F4};
  if (bank < 2 || bank > 4 || word >= R35_CONTEXT_WORDS) return 0;
  if (word == R35Cbar) return R35_SMMU_BASE + 0x1000ULL + 4ULL * bank;
  if (word == R35Cba2r) return R35_SMMU_BASE + 0x1800ULL + 4ULL * bank;
  return R35_SMMU_BASE + 0x80000ULL + (static_cast<uint64_t>(bank) << 12) + kOffsets[word];
}

class Transport final {
 public:
  Transport() = default;
  Transport(const Transport &) = delete;
  Transport &operator=(const Transport &) = delete;

  InitReport initialize(const Platform &platform, const Budget &budget,
                        const uint8_t (&lun4_reference)[kBlockBytes]);

  bool readBlocksWithSink(uint8_t lun, uint64_t lba, uint32_t block_count,
                          bool (*sink)(void *ctx, uint64_t byte_off,
                                       const uint8_t *src, size_t bytes),
                          void *sink_ctx);

  // Strictly read-only 512-byte logical window read mapped onto 4 KiB LUN 0 blocks.
  // Handles arbitrary alignment and transfers larger than 128 KiB without extra bounce buffers.
  bool readWindow512WithSink(uint64_t abs_offset_bytes, uint64_t window_bytes,
                             uint64_t block512, uint64_t count512,
                             bool (*sink)(void *ctx, uint64_t dst_off,
                                          const uint8_t *src, size_t bytes),
                             void *sink_ctx, uint64_t *out_bytes = nullptr);

  PostCheck postCheck();

  bool online() const {
    return __atomic_load_n(&online_, __ATOMIC_ACQUIRE) != 0 &&
           __atomic_load_n(&quarantined_, __ATOMIC_ACQUIRE) == 0;
  }
  const LunInfo &lun0() const { return report_.lun0; }
  const LunInfo &lun4() const { return report_.lun4; }
  const InitReport &initReport() const { return report_; }
  Stats stats() const;

 private:
  unsigned busy_ = 0;
  unsigned initialized_ = 0;
  unsigned online_ = 0;
  unsigned quarantined_ = 0;
  uint8_t next_tag_ = 0;

  Platform platform_ = {};
  Budget budget_ = {};
  R35Record record_ = {};
  sm8250_ufs::DmaView dma_ = {};
  AppsSmmuClaim claim_ = {};
  AppsSmmuToken token_ = {};
  InitReport report_ = {};

  uint64_t read_calls_ = 0;
  uint64_t read_commands_ = 0;
  uint64_t blocks_read_ = 0;
  uint64_t read_errors_ = 0;
  uint32_t max_chunk_blocks_ = 0;

  bool failInit(Error e, uint64_t addr = 0);
  void quarantine(uint32_t reason);
  bool read32(uint64_t pa, uint32_t &out);
  bool read64(uint64_t pa, uint64_t &out);
  bool writeHci32(uint32_t offset, uint32_t val);
  bool cacheOp(sm8250_ufs::CacheOp op, size_t off, size_t bytes);

  bool verifyRecord();
  bool verifyHardware();
  bool verifyDmaRange(const sm8250_ufs::DmaView &dma);
  bool setupOwnerAndQueue();

  struct CmdOutcome {
    bool ok = false;
    uint32_t ocs = 15;
    uint8_t rsp_type = 0;
    uint8_t rsp_status = 0;
    uint8_t scsi_status = 0;
    uint8_t sense_key = 0;
  };
  CmdOutcome submitLocked(const uint8_t upiu[32], uint32_t data_bytes, bool from_device);
  bool cmdNopLocked();
  bool cmdPowerModeLocked();
  bool cmdTurLocked(uint8_t lun);
  bool cmdReadCapacityLocked(uint8_t lun, LunInfo &out);
  bool cmdRead10ChunkLocked(uint8_t lun, uint64_t lba, uint32_t blocks, uint8_t *dst);
  bool scanLun0GptLocked();
  void parseShngRootLocked();

  static bool observeReadThunk(void *ctx, uint64_t pa, uint32_t *out);
};

}  // namespace j716f_r41

#endif
