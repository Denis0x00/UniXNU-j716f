// SPDX-License-Identifier: BSD-2-Clause-Patent
// TB-J716F (SM8250-AC / PM8150) wall-clock RTC driver for ArmDTPlatform.
//
// Combines the always-running PM8150 hardware RTC counter (SPMI PPID 0x060,
// verified read-only via observer channel) with a 64-byte CRC32-protected
// offset record persisted in the reserved tail (bytes 512..575) of the UFS
// SHNGDATA persistent header block (zero PMIC SDAM conflict).
//
// Boot sequence:
//   1. C++ static initializer registers clockRead/clockSet with
//      armdt_platform_clock_register() before IOKitInitializeTime().
//   2. When j716f-data-persist=1/2 opens the UFS Data store (before launchd),
//      j716f_rtc_on_store_ready() loads the persisted offset, reads the live
//      PM8150 hardware counter, and calibrates XNU's calendar via
//      clock_set_calendar_microtime() so userland boots to the true wall time.
//   3. Whenever iPadOS (timed or Settings) updates the wall clock, clockSet()
//      computes the new (wall - pm8150_counter) offset and schedules an async
//      worker thread to persist it with FUA to UFS (never blocking settime_lock).
//
// Boot-args:
//   j716f-rtc=<bits>          default 7 (full: measure + restore + persist)
//     bit 0 (1): observe PM8150 RTC counter (PPID 0x060) & record clockRead/clockSet
//     bit 1 (2): restore persisted wall time on boot (UFS record or optional SDAM)
//     bit 2 (4): persist updated offset when system sets wall time
//   j716f-rtc-sdam=<ppid>     optional SPMI PPID for SDAM cell override (default 0 = UFS)
//   j716f-rtc-cell=<offset>   optional byte offset inside SDAM (default 0x40)
//   j716f-rtc-raw=1           optional 4-byte untagged SDAM cell mode

#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <pexpert/pexpert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "J716fRtcBoard.h"

extern "C" {
void armdt_platform_clock_register(int (*read)(uint64_t *seconds),
                                   void (*set)(uint64_t seconds)) __attribute__((weak_import));
extern volatile uint64_t armdt_platform_clock_floor __attribute__((weak_import));
extern int j716f_ufs_rtc_load(uint8_t out64[64]) __attribute__((weak_import));
extern int j716f_ufs_rtc_save(const uint8_t in64[64]) __attribute__((weak_import));

volatile uint32_t j716f_rtc_state[16] = {};
void j716f_rtc_probe(void);
void j716f_rtc_on_store_ready(void);
int j716f_rtc_set_wall(uint64_t unix_secs);
void j716f_rtc_snapshot(char *out, size_t cap);
void j716f_rtc_scan(char *out, size_t cap);
}

namespace {

constexpr uint64_t kSpmiCore   = 0x0C440000ULL;
constexpr uint64_t kSpmiChnls  = 0x0C600000ULL;
constexpr uint64_t kSpmiObsrvr = 0x0E600000ULL;
constexpr uint64_t kSpmiCnfg   = 0x0C40A000ULL;

enum : uint32_t {
    kModeMeasure   = 1u << 0,
    kModeUseStored = 1u << 1,
    kModeStore     = 1u << 2,
    kModeDefault   = kModeMeasure | kModeUseStored | kModeStore,
};

enum : uint32_t {
    kStRegistered  = 1u << 0,
    kStMapped      = 1u << 1,
    kStAsked       = 1u << 2,
    kStWasSet      = 1u << 3,
    kStNoArbiter   = 1u << 4,
    kStUfsRestored = 1u << 5,
    kStUfsSaved    = 1u << 6,
};

struct Window {
    IOMemoryMap *map = nullptr;
    volatile uint32_t *va = nullptr;
};

Window gCore, gCnfg, gObs, gSdamCh;
j716f_rtc_arbiter gArb = {};
uint32_t gSpmiVer = 0;
uint32_t gApids = 0;
int gRtcApid = -1, gRtcOwner = -1;
int gSdamApid = -1, gSdamOwner = -1;
uint32_t gMode = 0;
uint32_t gSdamPpid = 0;
uint32_t gCellOff = 0x40u;
int gTagged = 1;
bool gConfigRead = false;
bool gOpened = false;

int64_t gMemOffset = 0;
bool gHasMemOffset = false;
uint64_t gGeneration = 0;
uint8_t gLastCtrl = 0;
volatile uint8_t gBusy = 0;
volatile uint32_t gRestoring = 0;
volatile uint32_t gStoreReady = 0;

// Async UFS persistence queue (never blocks clockSet / settime_lock)
volatile uint32_t gSavePending = 0;
volatile uint32_t gSaveSeq = 0;
j716f_rtc_record gPendingRec = {};
volatile uint32_t gWorkerStarted = 0;
volatile int gUfsLoadRc = J716F_RTC_NOT_TRIED;
volatile int gUfsSaveRc = J716F_RTC_NOT_TRIED;
volatile uint32_t gUfsSaves = 0;
volatile uint64_t gUfsRestoredWall = 0;
volatile int gUfsCounterUsed = 0;

void setFlag(uint32_t bit) {
    j716f_rtc_state[0] = j716f_rtc_state[0] | bit;
}

bool mapWindow(Window &w, uint64_t phys, uint64_t bytes) {
    if (w.va) return true;
    IODeviceMemory *mem = IODeviceMemory::withRange(phys, bytes);
    w.map = mem ? mem->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
    if (mem) mem->release();
    w.va = w.map ? reinterpret_cast<volatile uint32_t *>(w.map->getVirtualAddress()) : nullptr;
    return w.va != nullptr;
}

bool enter() {
    for (unsigned i = 0; i < 2000u; ++i) {
        if (!__atomic_test_and_set(&gBusy, __ATOMIC_ACQUIRE)) return true;
        IODelay(10);
    }
    return false;
}

void leave() {
    __atomic_clear(&gBusy, __ATOMIC_RELEASE);
}

void readConfig() {
    if (gConfigRead) return;
    gConfigRead = true;
    uint32_t mode = kModeDefault;
    PE_parse_boot_argn("j716f-rtc", &mode, sizeof(mode));
    uint32_t sdamPpid = 0;
    PE_parse_boot_argn("j716f-rtc-sdam", &sdamPpid, sizeof(sdamPpid));
    uint32_t cellOff = 0x40u;
    PE_parse_boot_argn("j716f-rtc-cell", &cellOff, sizeof(cellOff));
    uint32_t rawMode = 0;
    PE_parse_boot_argn("j716f-rtc-raw", &rawMode, sizeof(rawMode));

    gMode = mode;
    gSdamPpid = sdamPpid;
    gCellOff = (cellOff >= J716F_RTC_SDAM_MEM_START && cellOff <= 0xF8u) ? cellOff : 0x40u;
    gTagged = rawMode ? 0 : 1;

    j716f_rtc_state[1] = gMode;
    j716f_rtc_state[4] = J716F_RTC_NOT_TRIED;
    j716f_rtc_state[12] = J716F_RTC_NOT_TRIED;
}

bool openArbiter() {
    if (gOpened) return gRtcApid >= 0;
    gOpened = true;
    gArb.ee = 0u;
    if (!mapWindow(gCore, kSpmiCore, 0x2000u) || !mapWindow(gCnfg, kSpmiCnfg, 0x2000u)) {
        setFlag(kStNoArbiter);
        return false;
    }
    gArb.core = gCore.va;
    gArb.cnfg = gCnfg.va;
    if (j716f_rtc_arbiter_check(&gArb, &gSpmiVer, &gApids) != 0) {
        setFlag(kStNoArbiter);
        return false;
    }
    const uint64_t obsBytes = ((uint64_t)gApids * J716F_RTC_SPMI_OBS_STRIDE + 0xFFFULL) & ~0xFFFULL;
    if (!mapWindow(gObs, kSpmiObsrvr, obsBytes)) {
        setFlag(kStNoArbiter);
        return false;
    }
    gRtcApid = j716f_rtc_find_ppid(&gArb, gApids, J716F_RTC_PPID, &gRtcOwner);
    if (gSdamPpid != 0u) {
        gSdamApid = j716f_rtc_find_ppid(&gArb, gApids, gSdamPpid, &gSdamOwner);
    }
    j716f_rtc_state[2] = gRtcApid < 0 ? 0xFFFFu : (static_cast<uint32_t>(gRtcApid) | (static_cast<uint32_t>(gRtcOwner) << 16));
    j716f_rtc_state[3] = gSdamApid < 0 ? 0xFFFFu : (static_cast<uint32_t>(gSdamApid) | (static_cast<uint32_t>(gSdamOwner) << 16));

    if ((gMode & kModeStore) && gSdamApid >= 0 && gSdamOwner == static_cast<int>(gArb.ee)) {
        (void)mapWindow(gSdamCh,
                        kSpmiChnls + static_cast<uint64_t>(gSdamApid) * J716F_RTC_SPMI_CHNL_STRIDE,
                        0x1000u);
    }
    setFlag(kStMapped);
    return gRtcApid >= 0;
}

void rtcPersistWorker(void *arg, wait_result_t waitResult) {
    (void)arg;
    (void)waitResult;
    for (;;) {
        if (__atomic_load_n(&gSavePending, __ATOMIC_ACQUIRE) &&
            __atomic_load_n(&gStoreReady, __ATOMIC_ACQUIRE) &&
            &j716f_ufs_rtc_save) {
            j716f_rtc_record rec = {};
            uint32_t seq = 0;
            bool have = false;
            if (enter()) {
                if (__atomic_load_n(&gSavePending, __ATOMIC_RELAXED)) {
                    rec = gPendingRec;
                    seq = __atomic_load_n(&gSaveSeq, __ATOMIC_RELAXED);
                    have = true;
                }
                leave();
            }
            if (have) {
                uint8_t buf[J716F_RTC_RECORD_BYTES];
                j716f_rtc_record_encode(buf, &rec);
                const int rc = j716f_ufs_rtc_save(buf);
                gUfsSaveRc = rc;
                if (rc == 0) {
                    setFlag(kStUfsSaved);
                    __atomic_fetch_add(&gUfsSaves, 1u, __ATOMIC_RELAXED);
                    j716f_rtc_state[12] = J716F_RTC_OK;
                    j716f_rtc_state[14] = j716f_rtc_state[14] + 1u;
                    IOLog("[J716F rtc] persisted offset=%lld cnt=%u wall=%llu gen=%llu to UFS\n",
                          static_cast<long long>(rec.offset), rec.saved_counter,
                          static_cast<unsigned long long>(rec.saved_wall),
                          static_cast<unsigned long long>(rec.generation));
                } else {
                    j716f_rtc_state[12] = static_cast<uint32_t>(rc < 0 ? (-rc) : rc);
                    IOLog("[J716F rtc] UFS persist failed rc=%d\n", rc);
                }
                if (enter()) {
                    if (__atomic_load_n(&gSaveSeq, __ATOMIC_RELAXED) == seq) {
                        __atomic_store_n(&gSavePending, 0u, __ATOMIC_RELEASE);
                    }
                    leave();
                }
            }
        }
        IOSleep(250);
    }
}

void ensurePersistWorker() {
    if (__atomic_exchange_n(&gWorkerStarted, 1u, __ATOMIC_ACQ_REL) != 0u) return;
    thread_t th = nullptr;
    if (kernel_thread_start(rtcPersistWorker, nullptr, &th) == KERN_SUCCESS && th) {
        thread_deallocate(th);
    } else {
        __atomic_store_n(&gWorkerStarted, 0u, __ATOMIC_RELEASE);
    }
}

int clockRead(uint64_t *seconds) {
    readConfig();
    setFlag(kStAsked);
    if (!gMode || !seconds || !enter()) return 1;

    const bool first = (j716f_rtc_state[4] == J716F_RTC_NOT_TRIED);
    uint8_t ctrl = 0;
    uint32_t counter = 0, cell = 0, tag = 0;
    uint64_t now = 0;

    int rc = openArbiter() ? j716f_rtc_read_counter(gObs.va, gRtcApid, &ctrl, &counter) : J716F_RTC_SPMI_ERR;
    if (!rc) gLastCtrl = ctrl;

    if (!rc && gHasMemOffset) {
        const int64_t sum = static_cast<int64_t>(counter) + gMemOffset;
        if (sum >= static_cast<int64_t>(J716F_RTC_EARLIEST) && static_cast<uint64_t>(sum) <= J716F_RTC_LATEST_WALL) {
            now = static_cast<uint64_t>(sum);
        } else {
            rc = J716F_RTC_RANGE;
        }
    } else if (!rc && gSdamApid >= 0) {
        rc = j716f_rtc_read_cell(gObs.va, gSdamApid, static_cast<uint8_t>(gCellOff), gTagged, &cell, &tag);
        if (!rc) rc = j716f_rtc_check_cell(counter, cell, tag, gTagged, UINT32_MAX);
        if (!rc) rc = j716f_rtc_wall_time(counter, cell, &now);
        if (!rc && !(gMode & kModeUseStored)) now = 0;
    } else if (!rc) {
        if ((gMode & kModeUseStored) && counter >= J716F_RTC_EARLIEST) {
            now = counter;
        } else {
            rc = J716F_RTC_NO_CELL;
        }
    }

    if (first) {
        j716f_rtc_state[4] = static_cast<uint32_t>(rc);
        j716f_rtc_state[5] = counter;
        j716f_rtc_state[6] = cell;
        j716f_rtc_state[7] = static_cast<uint32_t>(now);
    }
    leave();
    if (rc || !now) return 1;
    *seconds = now;
    return 0;
}

void clockSet(uint64_t seconds) {
    // Ignore re-entrant callback triggered by our own boot-time calendar restoration.
    if (__atomic_load_n(&gRestoring, __ATOMIC_ACQUIRE)) return;

    readConfig();
    setFlag(kStWasSet);
    j716f_rtc_state[8] = j716f_rtc_state[8] + 1u;
    j716f_rtc_state[9] = static_cast<uint32_t>(seconds);
    if (!gMode || seconds < J716F_RTC_EARLIEST || seconds > J716F_RTC_LATEST_WALL || !enter()) return;

    uint8_t ctrl = 0;
    uint32_t counter = 0;
    int rc = openArbiter() ? j716f_rtc_read_counter(gObs.va, gRtcApid, &ctrl, &counter) : J716F_RTC_SPMI_ERR;
    if (!rc) gLastCtrl = ctrl;
    j716f_rtc_state[10] = counter;
    if (rc) {
        leave();
        return;
    }

    const int64_t newOffset = static_cast<int64_t>(seconds) - static_cast<int64_t>(counter);
    const int64_t delta = gHasMemOffset ? (newOffset - gMemOffset) : 0;
    gMemOffset = newOffset;
    gHasMemOffset = true;
    j716f_rtc_state[11] = static_cast<uint32_t>(newOffset);
    j716f_rtc_state[15] = static_cast<uint32_t>(static_cast<int32_t>(delta));

    if (gMode & kModeStore) {
        // 1. Default persistent backend: schedule async UFS SHNGDATA header-tail save
        ++gGeneration;
        gPendingRec.offset = newOffset;
        gPendingRec.saved_counter = counter;
        gPendingRec.flags = 0u;
        gPendingRec.saved_wall = seconds;
        gPendingRec.generation = gGeneration;
        __atomic_fetch_add(&gSaveSeq, 1u, __ATOMIC_RELAXED);
        __atomic_store_n(&gSavePending, 1u, __ATOMIC_RELEASE);

        // 2. Optional PMIC SDAM backend if explicitly configured via j716f-rtc-sdam=
        uint32_t off32 = 0, cell = 0, tag = 0;
        if (gSdamApid >= 0 &&
            j716f_rtc_calc_offset(seconds, counter, &off32) == J716F_RTC_OK &&
            j716f_rtc_read_cell(gObs.va, gSdamApid, static_cast<uint8_t>(gCellOff), gTagged, &cell, &tag) == J716F_RTC_OK) {
            const int held = j716f_rtc_check_cell(counter, cell, tag, gTagged, seconds + J716F_RTC_SET_BACK);
            uint32_t after = cell;
            int stored = J716F_RTC_NOT_TRIED;
            if (held == J716F_RTC_FOREIGN) {
                stored = J716F_RTC_FOREIGN;
            } else if (cell == off32 && (!gTagged || tag == j716f_rtc_tag_for(off32))) {
                stored = J716F_RTC_CURRENT;
            } else if (gSdamOwner != static_cast<int>(gArb.ee)) {
                stored = J716F_RTC_NOT_OWNER;
            } else if (!gSdamCh.va) {
                stored = J716F_RTC_SPMI_ERR;
            } else {
                stored = j716f_rtc_store_cell(gObs.va, gSdamCh.va, gSdamApid, gSdamOwner, gArb.ee,
                                              static_cast<uint8_t>(gCellOff), gTagged, off32, &after);
            }
            j716f_rtc_state[12] = static_cast<uint32_t>(stored);
            j716f_rtc_state[13] = after;
            if (stored == J716F_RTC_OK) {
                j716f_rtc_state[14] = j716f_rtc_state[14] + 1u;
            }
        }
    }
    leave();
    if (gMode & kModeStore) {
        ensurePersistWorker();
    }
}

void registerClock() {
    if ((j716f_rtc_state[0] & kStRegistered) || !&armdt_platform_clock_register) return;
    armdt_platform_clock_register(clockRead, clockSet);
    setFlag(kStRegistered);
}

// Runs during kernel C++ static initialization, before ArmDTPlatform / IOKitInitializeTime().
struct Registration {
    Registration() { registerClock(); }
} gRegistration;

} // namespace

// Called from R41Entry native_ufs_store_open() before launchd starts so the calendar
// is calibrated to real wall time before any userland daemon runs.
extern "C" void j716f_rtc_on_store_ready(void) {
    registerClock();
    readConfig();
    __atomic_store_n(&gStoreReady, 1u, __ATOMIC_RELEASE);
    if (!(gMode & kModeUseStored) || !&j716f_ufs_rtc_load) return;

    uint8_t raw[J716F_RTC_RECORD_BYTES];
    memset(raw, 0, sizeof(raw));
    const int lrc = j716f_ufs_rtc_load(raw);
    if (lrc != 0) {
        gUfsLoadRc = lrc;
        return;
    }

    j716f_rtc_record rec = {};
    const int drc = j716f_rtc_record_decode(raw, &rec);
    gUfsLoadRc = drc;
    if (drc != J716F_RTC_OK) {
        IOLog("[J716F rtc] UFS record decode rc=%d (no stored offset yet)\n", drc);
        return;
    }

    uint8_t ctrl = 0;
    uint32_t liveCounter = 0;
    int crc = J716F_RTC_SPMI_ERR;
    if (enter()) {
        if (openArbiter()) {
            crc = j716f_rtc_read_counter(gObs.va, gRtcApid, &ctrl, &liveCounter);
            if (!crc) gLastCtrl = ctrl;
        }
        leave();
    }
    if (crc != J716F_RTC_OK) {
        IOLog("[J716F rtc] PM8150 counter read failed during UFS restore rc=%d\n", crc);
        return;
    }

    const uint64_t floorSec = &armdt_platform_clock_floor ? armdt_platform_clock_floor : 0ULL;
    uint64_t restoredWall = 0;
    int counterUsed = 0;
    const int rrc = j716f_rtc_record_restore(&rec, liveCounter, floorSec, &restoredWall, &counterUsed);
    if (rrc != J716F_RTC_OK) {
        gUfsLoadRc = rrc;
        return;
    }

    if (enter()) {
        gMemOffset = static_cast<int64_t>(restoredWall) - static_cast<int64_t>(liveCounter);
        gHasMemOffset = true;
        if (rec.generation > gGeneration) gGeneration = rec.generation;
        j716f_rtc_state[4] = J716F_RTC_OK;
        j716f_rtc_state[5] = liveCounter;
        j716f_rtc_state[6] = static_cast<uint32_t>(gMemOffset);
        j716f_rtc_state[7] = static_cast<uint32_t>(restoredWall);
        leave();
    }

    gUfsRestoredWall = restoredWall;
    gUfsCounterUsed = counterUsed;
    setFlag(kStUfsRestored);

    __atomic_store_n(&gRestoring, 1u, __ATOMIC_RELEASE);
    clock_set_calendar_microtime(static_cast<clock_sec_t>(restoredWall), 0);
    __atomic_store_n(&gRestoring, 0u, __ATOMIC_RELEASE);

    IOLog("[J716F rtc] restored wall=%llu from PM8150 cnt=%u (saved_cnt=%u off=%lld used_cnt=%d gen=%llu)\n",
          static_cast<unsigned long long>(restoredWall), liveCounter, rec.saved_counter,
          static_cast<long long>(rec.offset), counterUsed,
          static_cast<unsigned long long>(rec.generation));
}

extern "C" void j716f_rtc_probe(void) {
    registerClock();
    readConfig();
    if (gMode & kModeStore) {
        ensurePersistWorker();
    }
    uint8_t ctrl = 0;
    uint32_t liveCounter = 0;
    int liveRc = J716F_RTC_NOT_TRIED;
    if (enter()) {
        if (openArbiter()) {
            liveRc = j716f_rtc_read_counter(gObs.va, gRtcApid, &ctrl, &liveCounter);
            if (!liveRc) gLastCtrl = ctrl;
        }
        leave();
    }
    IOLog("[J716F rtc] mode=%u st=0x%x spmi=0x%x/%u rtc=%u/%u live(rc=%d ctrl=0x%02x cnt=%u) ufs(load=%d wall=%llu used_cnt=%d save=%d n=%u)\n",
          j716f_rtc_state[1], j716f_rtc_state[0], gSpmiVer, gApids,
          j716f_rtc_state[2] & 0xFFFFu, j716f_rtc_state[2] >> 16,
          liveRc, static_cast<unsigned>(ctrl), liveCounter,
          gUfsLoadRc, static_cast<unsigned long long>(gUfsRestoredWall),
          gUfsCounterUsed, gUfsSaveRc, gUfsSaves);
}

extern "C" int j716f_rtc_set_wall(uint64_t unix_secs) {
    if (unix_secs < J716F_RTC_EARLIEST || unix_secs > J716F_RTC_LATEST_WALL) {
        return -22;
    }
    clock_set_calendar_microtime(static_cast<clock_sec_t>(unix_secs), 0);
    return 0;
}

extern "C" void j716f_rtc_snapshot(char *out, size_t cap) {
    if (!out || cap < 2u) return;
    clock_sec_t calSec = 0;
    clock_usec_t calUsec = 0;
    clock_get_calendar_microtime(&calSec, &calUsec);

    uint8_t ctrl = gLastCtrl;
    uint32_t liveCounter = 0;
    int liveRc = J716F_RTC_NOT_TRIED;
    if (enter()) {
        if (openArbiter()) {
            liveRc = j716f_rtc_read_counter(gObs.va, gRtcApid, &ctrl, &liveCounter);
            if (!liveRc) gLastCtrl = ctrl;
        }
        leave();
    }
    const uint64_t floorSec = &armdt_platform_clock_floor ? armdt_platform_clock_floor : 0ULL;
    snprintf(out, cap,
             "[usb] RTC: mode=%u st=0x%x cal=%llu floor=%llu rtc=%u/%u live(rc=%d ctrl=0x%02x cnt=%u) "
             "ufs(rdy=%u load=%d wall=%llu used_cnt=%d save=%d n=%u pend=%u) "
             "set(n=%u sec=%u cnt=%u off=%lld)\r\n",
             j716f_rtc_state[1], j716f_rtc_state[0],
             static_cast<unsigned long long>(calSec),
             static_cast<unsigned long long>(floorSec),
             j716f_rtc_state[2] & 0xFFFFu, j716f_rtc_state[2] >> 16,
             liveRc, static_cast<unsigned>(ctrl), liveCounter,
             __atomic_load_n(&gStoreReady, __ATOMIC_RELAXED),
             gUfsLoadRc, static_cast<unsigned long long>(gUfsRestoredWall),
             gUfsCounterUsed, gUfsSaveRc, gUfsSaves,
             __atomic_load_n(&gSavePending, __ATOMIC_RELAXED),
             j716f_rtc_state[8], j716f_rtc_state[9], j716f_rtc_state[10],
             static_cast<long long>(gMemOffset));
}

// Read-only SPMI scan of PM8150/PM8150B/PM8150L RTC and SDAM peripherals (zero writes).
extern "C" void j716f_rtc_scan(char *out, size_t cap) {
    if (!out || cap < 64u) return;
    size_t pos = 0;
    auto append = [&](const char *line) {
        if (pos + 1u >= cap) return;
        const int n = snprintf(out + pos, cap - pos, "%s", line);
        if (n > 0) pos += static_cast<size_t>(n) < (cap - pos) ? static_cast<size_t>(n) : (cap - pos - 1u);
    };

    if (!enter()) {
        append("[usb] RTC.SCAN: busy\r\n");
        return;
    }
    if (!openArbiter() && !gObs.va) {
        leave();
        append("[usb] RTC.SCAN: SPMI arbiter unavailable\r\n");
        return;
    }

    char line[256];
    snprintf(line, sizeof(line), "[usb] RTC.SCAN: spmi=0x%08x apids=%u\r\n", gSpmiVer, gApids);
    append(line);

    for (uint32_t ppid = 0x060u; ppid <= 0x061u; ++ppid) {
        int owner = -1;
        const int apid = j716f_rtc_find_ppid(&gArb, gApids, ppid, &owner);
        if (apid < 0) {
            snprintf(line, sizeof(line), "[usb]   PPID 0x%03x: absent\r\n", ppid);
            append(line);
            continue;
        }
        uint32_t type = 0, sub = 0, ctrl = 0, rw = 0, rd = 0;
        (void)j716f_rtc_obs_read(gObs.va, apid, 0x04u, 1u, &type);
        (void)j716f_rtc_obs_read(gObs.va, apid, 0x05u, 1u, &sub);
        (void)j716f_rtc_obs_read(gObs.va, apid, 0x46u, 1u, &ctrl);
        (void)j716f_rtc_obs_read(gObs.va, apid, 0x40u, 4u, &rw);
        (void)j716f_rtc_obs_read(gObs.va, apid, 0x48u, 4u, &rd);
        snprintf(line, sizeof(line),
                 "[usb]   PPID 0x%03x (RTC): apid=%d own=%d type=%02x/%02x ctrl=0x%02x r40=0x%08x r48=%u(0x%08x)\r\n",
                 ppid, apid, owner, type, sub, ctrl, rw, rd, rd);
        append(line);
    }

    static const uint32_t kUsids[3] = {0u, 2u, 4u};
    for (unsigned u = 0; u < 3u; ++u) {
        for (uint32_t per = 0xB0u; per <= 0xBFu; ++per) {
            const uint32_t ppid = (kUsids[u] << 8) | per;
            int owner = -1;
            const int apid = j716f_rtc_find_ppid(&gArb, gApids, ppid, &owner);
            if (apid < 0) continue;
            uint32_t type = 0, sub = 0, msz = 0;
            (void)j716f_rtc_obs_read(gObs.va, apid, J716F_RTC_SDAM_TYPE_REG, 1u, &type);
            (void)j716f_rtc_obs_read(gObs.va, apid, J716F_RTC_SDAM_SUBTYPE_REG, 1u, &sub);
            (void)j716f_rtc_obs_read(gObs.va, apid, J716F_RTC_SDAM_MEM_SIZE_REG, 1u, &msz);
            const uint32_t memBytes = (msz > 0u && msz <= 6u) ? (msz * 32u) : 128u;
            unsigned nonZeroWords = 0;
            uint32_t firstNzOff = 0, firstNzVal = 0;
            uint32_t lastZeroRunOff = 0, lastZeroRunBytes = 0;
            uint32_t curZeroStart = 0, curZeroBytes = 0;
            for (uint32_t off = J716F_RTC_SDAM_MEM_START;
                 off + 4u <= J716F_RTC_SDAM_MEM_START + memBytes && off + 4u <= 0x100u;
                 off += 4u) {
                uint32_t w = 0;
                if (j716f_rtc_obs_read(gObs.va, apid, static_cast<uint8_t>(off), 4u, &w) != J716F_RTC_OK) {
                    curZeroBytes = 0;
                    continue;
                }
                if (w != 0u) {
                    if (nonZeroWords == 0u) {
                        firstNzOff = off;
                        firstNzVal = w;
                    }
                    ++nonZeroWords;
                    curZeroBytes = 0;
                } else {
                    if (curZeroBytes == 0u) curZeroStart = off;
                    curZeroBytes += 4u;
                    if (curZeroBytes >= lastZeroRunBytes) {
                        lastZeroRunOff = curZeroStart;
                        lastZeroRunBytes = curZeroBytes;
                    }
                }
            }
            snprintf(line, sizeof(line),
                     "[usb]   PPID 0x%03x (SDAM): apid=%d own=%d type=%02x/%02x sz=%uB nz=%u(first@0x%02x=0x%08x) max_zero=%uB@0x%02x\r\n",
                     ppid, apid, owner, type, sub, memBytes, nonZeroWords,
                     firstNzOff, firstNzVal, lastZeroRunBytes, lastZeroRunOff);
            append(line);
        }
    }
    leave();
}
