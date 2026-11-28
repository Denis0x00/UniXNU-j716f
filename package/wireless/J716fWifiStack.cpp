// SPDX-License-Identifier: BSD-2-Clause-Patent
// TB-J716F QCA6390 ath11k + mac80211/cfg80211 board runtime glue.
// Bridges SM8250 PCIe0 (BAR0/CFG/DBI iMSI SPI 141), 64 MiB <4 GiB Normal-NC DMA arena,
// embedded QCA6390 firmware (amss20.bin/m3.bin/bdwlan.elf), and org.unixnu.wifi.control
// into libJ716fAth11kWifi.a and J716fWifiNet.cpp (en0).
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IORegistryEntry.h>
#include <IOKit/IOService.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSMetaClass.h>
#include <kern/clock.h>
#include <kern/sched_prim.h>
#include <kern/thread.h>
#include <mach/kmod.h>
#include <pexpert/pexpert.h>
#include <sys/errno.h>
#include <sys/kern_control.h>
#include <sys/kpi_mbuf.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>

#include "J716fWcnBoard.h"
#include "nw_abi.h"
#include "nw_control_endpoint.h"

extern "C" {
extern kmod_info_t g_kernel_kmod_info;
extern void _disable_preemption(void);
extern void _enable_preemption(void);
extern void thread_yield_to_preemption(void);
extern void thread_set_thread_name(thread_t thread, const char *name);
extern int armcpu_spi_attach(uint32_t intid, bool edge, void (*fn)(void *), void *ctx,
                             void (*fn2)(void *), void *ctx2) __attribute__((weak_import));
extern bool j716f_wcn_dma_arena(uint64_t *pa_out, uint64_t *size_out) __attribute__((weak_import));
extern int j716f_wcn_wifi_prepare_for_ath11k(uint32_t *bar_addr_out, uint32_t *bar_size_out);

extern const uint8_t j716f_wcn_wifi_amss_fw[];
extern const uint32_t j716f_wcn_wifi_amss_fw_size;
extern const uint8_t j716f_wcn_wifi_m3_fw[];
extern const uint32_t j716f_wcn_wifi_m3_fw_size;
extern const uint8_t j716f_wcn_wifi_bdf_fw[];
extern const uint32_t j716f_wcn_wifi_bdf_fw_size;

extern void __GLOBAL__sub_I_J716fWifiNet(void) __asm__("__GLOBAL__sub_I_J716fWifiNet.cpp");
extern void j716f_wifi_net_start(const uint8_t mac[6]);
extern void j716f_wifi_net_stats(uint32_t *up, uint32_t *ioctls, uint32_t *scanReqs, uint32_t *scanRes);
extern void j716f_wifi_net_stats_ext(char *ip_out, size_t ip_cap,
                                     uint64_t *pkts_in, uint64_t *pkts_out,
                                     uint32_t *rx_ok, uint32_t *rx_err, uint32_t *tx_drop);
extern void j716f_wifi_net_kinds(char *out, size_t cap);
extern void j716f_wifi_net_diag(char *out, size_t cap);
extern volatile int native_cryptex_os_stage __attribute__((weak_import));
extern volatile int native_cryptex_os_status __attribute__((weak_import));
extern volatile int native_ls_store_status __attribute__((weak_import));
extern volatile int native_gestalt_cache_status __attribute__((weak_import));
}

namespace {

struct MmioWin {
    IOMemoryMap *map = nullptr;
    volatile uint32_t *va = nullptr;
    size_t len = 0;
};

bool mapMmio(MmioWin &w, uint64_t pa, size_t len) {
    if (w.va) return true;
    IODeviceMemory *mem = IODeviceMemory::withRange(pa, len);
    w.map = mem ? mem->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
    if (mem) mem->release();
    if (!w.map) return false;
    w.va = reinterpret_cast<volatile uint32_t *>(w.map->getVirtualAddress());
    w.len = len;
    return true;
}

constexpr unsigned kArenaBlocks = 32768u;
struct Arena {
    IOMemoryMap *map = nullptr;
    uint8_t *va = nullptr;
    uint64_t pa = 0;
    size_t bytes = 0;
    struct Block { uint32_t off, size; bool used; } *blocks = nullptr;
    unsigned count = 0;
    size_t used = 0, peak = 0;
    unsigned failures = 0;
} gArena;

constexpr unsigned kLogRingSize = 64u;
struct StackState {
    bool started = false;
    volatile uint32_t state = 0; /* 0=off, 1=init, 2=running, 3=ready(en0), 4=err */
    volatile int lastErr = 0;
    uint32_t barAddr = 0;
    uint32_t barSize = 0;
    int msiAttachRc = -999;
    bool netRegistered = false;
    uint8_t dtChosenMac = 0;
    uint8_t dtWlanMac = 0;
    uint64_t firstLogMs = 0;
    volatile uint32_t logCount = 0;
    char logs[kLogRingSize][128];
} gStk;

MmioWin gDbi, gCfg, gBar;
uint32_t gPcieCap = 0, gMsiCap = 0;
volatile uint32_t gMsiPending = 0, gMsiCount = 0, gMsiBits = 0, gMsiWake = 0;
bool gMsiAttached = false;

constexpr uint32_t kMsiAddrLo = 0x820u;
constexpr uint32_t kMsiAddrHi = 0x824u;
constexpr uint32_t kMsiEnable0 = 0x828u;
constexpr uint32_t kMsiMask0 = 0x82Cu;
constexpr uint32_t kMsiStatus0 = 0x830u;
constexpr uint32_t kMsiSpi = 141u; /* SM8250 sm8250.dtsi pcie@1c00000 msi0 */

uint64_t pNowNs() {
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
    return ns;
}
uint64_t pBootNs() {
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(mach_continuous_time(), &ns);
    return ns;
}
uint64_t pNow() { return pNowNs() / 1000000ULL; }
void pSleep(unsigned ms) { IOSleep(ms); }
void pBusyDelayUs(unsigned us) { IODelay(us); }
void pHalt() { for (;;) IOSleep(1000); }

bool strContains(const char *s, const char *w) {
    for (; *s; ++s) {
        size_t k = 0;
        while (w[k] && s[k] == w[k]) ++k;
        if (!w[k]) return true;
    }
    return false;
}

void pLog(int level, const char *line) {
    if (!line || !line[0]) return;
    if (level >= 7 && (strContains(line, "htc rx") || strContains(line, "rx ce") ||
                       strContains(line, "ext irq") || strContains(line, "mgmt rx") ||
                       strContains(line, "chan info") || strContains(line, "scan event foreign"))) {
        return;
    }
    static uint32_t recent[12];
    uint32_t h = 2166136261u ^ static_cast<uint32_t>(level);
    for (const char *c = line; *c; ++c) h = (h ^ static_cast<uint8_t>(*c)) * 16777619u;
    for (uint32_t r : recent) if (r == h) return;
    memmove(&recent[1], &recent[0], sizeof(recent) - sizeof(recent[0]));
    recent[0] = h;

    const uint64_t now = pNow();
    if (!gStk.firstLogMs) gStk.firstLogMs = now;
    const uint32_t idx = __atomic_fetch_add(&gStk.logCount, 1u, __ATOMIC_RELAXED) % kLogRingSize;
    snprintf(gStk.logs[idx], sizeof(gStk.logs[idx]), "%u [%d] %.108s",
             static_cast<uint32_t>(now - gStk.firstLogMs), level, line);
    if (level <= 6) {
        IOLog("[J716F wifi] %s\n", gStk.logs[idx]);
    }
}

bool arenaInit() {
    if (gArena.va) return true;
    uint64_t pa = 0ULL, bytes = 0ULL;
    for (unsigned waited = 0u; waited < 8000u; waited += 50u) {
        if (&j716f_wcn_dma_arena && j716f_wcn_dma_arena(&pa, &bytes) &&
            pa && bytes >= 0x1000000ULL && pa + bytes <= 0x100000000ULL) {
            break;
        }
        IOSleep(50);
    }
    if (!pa || bytes < 0x1000000ULL || pa + bytes > 0x100000000ULL) {
        return false;
    }
    IODeviceMemory *mem = IODeviceMemory::withRange(pa, bytes);
    gArena.map = mem ? mem->map(kIOMapAnywhere | kIOMapWriteCombineCache) : nullptr;
    if (mem) mem->release();
    if (!gArena.map) return false;
    gArena.va = reinterpret_cast<uint8_t *>(gArena.map->getVirtualAddress());
    gArena.pa = pa;
    gArena.bytes = static_cast<size_t>(bytes);
    gArena.blocks = static_cast<Arena::Block *>(IOMallocData(kArenaBlocks * sizeof(Arena::Block)));
    if (!gArena.blocks) return false;
    /* Reserve page 0 (4 KiB) for DesignWare iMSI target address (gArena.pa). */
    gArena.blocks[0] = {4096u, static_cast<uint32_t>(gArena.bytes - 4096u), false};
    gArena.count = 1u;
    gArena.used = gArena.peak = 4096u;
    gArena.failures = 0u;
    bzero(gArena.va, 4096u);
    return true;
}

void *pDmaAlloc(size_t n, uint64_t *bus) {
    n = (n + 63u) & ~static_cast<size_t>(63u);
    if (n >= 4096u) n = (n + 4095u) & ~static_cast<size_t>(4095u);
    uint32_t align = 64u;
    if (n >= 4096u) {
        align = 4096u;
        while (align < n && align < 0x800000u) align <<= 1;
    }
    for (unsigned i = 0; i < gArena.count; ++i) {
        Arena::Block &b = gArena.blocks[i];
        if (b.used) continue;
        const uint32_t start = (b.off + align - 1u) & ~(align - 1u);
        const uint32_t pad = start - b.off;
        if (b.size < pad + n) continue;
        const bool tail = b.size > pad + n;
        const unsigned extra = (pad ? 1u : 0u) + (tail ? 1u : 0u);
        if (gArena.count + extra > kArenaBlocks) continue;
        const Arena::Block whole = b;
        memmove(&gArena.blocks[i + extra], &gArena.blocks[i],
                (gArena.count - i) * sizeof(Arena::Block));
        gArena.count += extra;
        unsigned at = i;
        if (pad) { gArena.blocks[at] = {whole.off, pad, false}; ++at; }
        gArena.blocks[at] = {start, static_cast<uint32_t>(n), true};
        if (tail) gArena.blocks[at + 1] = {start + static_cast<uint32_t>(n),
                                           whole.size - pad - static_cast<uint32_t>(n), false};
        uint8_t *p = gArena.va + start;
        gArena.used += n;
        if (gArena.used > gArena.peak) gArena.peak = gArena.used;
        bzero(p, n);
        __asm__ volatile("dsb sy" ::: "memory");
        if (bus) *bus = gArena.pa + start;
        return p;
    }
    ++gArena.failures;
    char msg[96];
    snprintf(msg, sizeof(msg), "dma arena full: want %zu used %zu/%zu", n, gArena.used, gArena.bytes);
    pLog(3, msg);
    return nullptr;
}

void pDmaFree(size_t, void *p, uint64_t) {
    if (!p || static_cast<uint8_t *>(p) < gArena.va ||
        static_cast<uint8_t *>(p) >= gArena.va + gArena.bytes) return;
    const uint32_t off = static_cast<uint32_t>(static_cast<uint8_t *>(p) - gArena.va);
    for (unsigned i = 0; i < gArena.count; ++i) {
        if (gArena.blocks[i].off != off || !gArena.blocks[i].used) continue;
        gArena.used -= gArena.blocks[i].size;
        gArena.blocks[i].used = false;
        if (i + 1 < gArena.count && !gArena.blocks[i + 1].used) {
            gArena.blocks[i].size += gArena.blocks[i + 1].size;
            memmove(&gArena.blocks[i + 1], &gArena.blocks[i + 2],
                    (gArena.count - i - 2) * sizeof(Arena::Block));
            --gArena.count;
        }
        if (i > 0 && !gArena.blocks[i - 1].used) {
            gArena.blocks[i - 1].size += gArena.blocks[i].size;
            memmove(&gArena.blocks[i], &gArena.blocks[i + 1],
                    (gArena.count - i - 1) * sizeof(Arena::Block));
            --gArena.count;
        }
        return;
    }
}

uint64_t pDmaMap(void *p, size_t, int) {
    if (static_cast<uint8_t *>(p) < gArena.va ||
        static_cast<uint8_t *>(p) >= gArena.va + gArena.bytes) return 0ULL;
    return gArena.pa + static_cast<uint64_t>(static_cast<uint8_t *>(p) - gArena.va);
}

void pDmaSync(uint64_t, size_t, int) { __asm__ volatile("dsb sy" ::: "memory"); }

void *hAlloc(size_t n) {
    uint8_t *p = static_cast<uint8_t *>(IOMallocData(n + 16u));
    if (!p) return nullptr;
    *reinterpret_cast<size_t *>(p) = n + 16u;
    return p + 16u;
}
void hFree(void *q) {
    if (!q) return;
    uint8_t *p = static_cast<uint8_t *>(q) - 16u;
    IOFreeData(p, *reinterpret_cast<size_t *>(p));
}

bool boardMac(uint8_t m[6]) {
    uint64_t h = 0xcbf29ce484222325ULL;
    static const char kDomain[] = "unixnu-j716f-wifi0";
    for (unsigned i = 0; i < sizeof(kDomain) - 1u; ++i) {
        h = (h ^ static_cast<uint8_t>(kDomain[i])) * 0x100000001b3ULL;
    }
    if (IORegistryEntry *chosen = IORegistryEntry::fromPath("/chosen", gIODTPlane)) {
        if (OSData *id = OSDynamicCast(OSData, chosen->getProperty("unique-chip-id"))) {
            const uint8_t *seed = static_cast<const uint8_t *>(id->getBytesNoCopy());
            for (unsigned i = 0; i < id->getLength(); ++i) h = (h ^ seed[i]) * 0x100000001b3ULL;
        }
        chosen->release();
    }
    for (unsigned i = 0; i < 6u; ++i) m[i] = static_cast<uint8_t>(h >> (8u * i));
    m[0] = static_cast<uint8_t>((m[0] & 0xFEu) | 0x02u);
    return true;
}

void publishWifiMac() {
    uint8_t m[6];
    if (!boardMac(m)) return;
    if (IORegistryEntry *chosen = IORegistryEntry::fromPath("/chosen", gIODTPlane)) {
        if (!chosen->getProperty("mac-address-wifi0")) {
            if (chosen->setProperty("mac-address-wifi0", m, 6u)) gStk.dtChosenMac = 1u;
        } else {
            gStk.dtChosenMac = 1u;
        }
        chosen->release();
    }
    if (IORegistryEntry *wlan = IORegistryEntry::fromPath("/arm-io/wlan", gIODTPlane)) {
        if (!wlan->getProperty("local-mac-address")) {
            if (wlan->setProperty("local-mac-address", m, 6u)) gStk.dtWlanMac = 1u;
        } else {
            gStk.dtWlanMac = 1u;
        }
        wlan->release();
    }
}

int pFwGet(const char *path, const uint8_t **data, size_t *size) {
    const char *base = path;
    for (const char *c = path; *c; ++c) if (*c == '/') base = c + 1;
    if (strcmp(base, "wlan_mac.bin") == 0) {
        static char text[40];
        static size_t len = 0;
        if (!len) {
            uint8_t m[6];
            boardMac(m);
            len = static_cast<size_t>(snprintf(text, sizeof(text), "address=%02x%02x%02x%02x%02x%02x\n",
                                               m[0], m[1], m[2], m[3], m[4], m[5]));
        }
        *data = reinterpret_cast<const uint8_t *>(text);
        *size = len;
        return 0;
    }
    if (strcmp(base, "amss.bin") == 0 && j716f_wcn_wifi_amss_fw_size >= 524288u) {
        *data = j716f_wcn_wifi_amss_fw;
        *size = j716f_wcn_wifi_amss_fw_size;
        return 0;
    }
    if (strcmp(base, "m3.bin") == 0 && j716f_wcn_wifi_m3_fw_size >= 4096u) {
        *data = j716f_wcn_wifi_m3_fw;
        *size = j716f_wcn_wifi_m3_fw_size;
        return 0;
    }
    if (strcmp(base, "board.bin") == 0 && j716f_wcn_wifi_bdf_fw_size >= 4096u) {
        *data = j716f_wcn_wifi_bdf_fw;
        *size = j716f_wcn_wifi_bdf_fw_size;
        return 0;
    }
    return -1;
}

bool findCaps() {
    gPcieCap = 0;
    gMsiCap = 0;
    uint8_t cap = static_cast<uint8_t>(gCfg.va[0x34u / 4u] & 0xFFu);
    for (unsigned guard = 0; cap >= 0x40u && guard < 48u; ++guard) {
        const uint32_t w = gCfg.va[cap / 4u];
        if (static_cast<uint8_t>(w) == 0x10u) gPcieCap = cap;
        if (static_cast<uint8_t>(w) == 0x05u) gMsiCap = cap;
        cap = static_cast<uint8_t>(w >> 8);
    }
    return gPcieCap != 0 && gMsiCap != 0;
}

int pCfgRead(int off, int size, uint32_t *v) {
    uint32_t o = static_cast<uint32_t>(off);
    if (o & 0x80000000u) o = gPcieCap + (o & 0x7FFFFFFFu);
    const uint32_t w = gCfg.va[(o & ~3u) / 4u];
    *v = size == 4 ? w : size == 2 ? ((w >> ((o & 2u) * 8u)) & 0xFFFFu) : ((w >> ((o & 3u) * 8u)) & 0xFFu);
    return 0;
}

int pCfgWrite(int off, int size, uint32_t v) {
    uint32_t o = static_cast<uint32_t>(off);
    if (o & 0x80000000u) o = gPcieCap + (o & 0x7FFFFFFFu);
    if (size == 4) {
        gCfg.va[o / 4u] = v;
        return 0;
    }
    const unsigned shift = (size == 2 ? (o & 2u) : (o & 3u)) * 8u;
    const uint32_t mask = (size == 2 ? 0xFFFFu : 0xFFu) << shift;
    gCfg.va[(o & ~3u) / 4u] = (gCfg.va[(o & ~3u) / 4u] & ~mask) | ((v << shift) & mask);
    return 0;
}

int pMsiTarget(uint32_t *lo, uint32_t *hi, uint32_t *data) {
    *lo = static_cast<uint32_t>(gArena.pa);
    *hi = static_cast<uint32_t>(gArena.pa >> 32);
    *data = 0u;
    return 0;
}

void msiIrq(void *) {
    const uint32_t st = gDbi.va[kMsiStatus0 / 4u];
    if (st) gDbi.va[kMsiStatus0 / 4u] = st;
    gMsiCount = gMsiCount + 1u;
    gMsiBits = gMsiBits | st;
    __atomic_fetch_or(&gMsiPending, st, __ATOMIC_RELEASE);
    static uint32_t empty = 0u;
    empty = st ? 0u : empty + 1u;
    if (empty > 100000u) {
        gDbi.va[kMsiEnable0 / 4u] = 0u;
        gMsiAttached = false;
    }
    __atomic_store_n(&gMsiWake, 1u, __ATOMIC_RELEASE);
    thread_wakeup(reinterpret_cast<event_t>(const_cast<uint32_t *>(&gMsiWake)));
}

int msiAttach() {
    uint32_t forcePoll = 0u;
    PE_parse_boot_argn("j716f-wifi-poll", &forcePoll, sizeof(forcePoll));
    if (forcePoll) return -1;
    if (!&armcpu_spi_attach || !gDbi.va) return -78;
    gDbi.va[kMsiAddrLo / 4u] = static_cast<uint32_t>(gArena.pa);
    gDbi.va[kMsiAddrHi / 4u] = static_cast<uint32_t>(gArena.pa >> 32);
    gDbi.va[kMsiMask0 / 4u] = 0u;
    gDbi.va[kMsiEnable0 / 4u] = 0xFFFFFFFFu;
    __asm__ volatile("dsb sy" ::: "memory");
    const int rc = armcpu_spi_attach(32u + kMsiSpi, false, msiIrq, nullptr, nullptr, nullptr);
    gMsiAttached = (rc == 0);
    return rc;
}

void pPoll() {
    nw_wifi_irq_mode(gMsiAttached ? 1 : 0);
    const uint32_t pending = __atomic_exchange_n(&gMsiPending, 0u, __ATOMIC_ACQ_REL);
    if (pending) nw_wifi_irq_pending(pending);
    nw_wifi_poll();
}

void msiWait(unsigned ms) {
    if (__atomic_exchange_n(&gMsiWake, 0u, __ATOMIC_ACQ_REL)) return;
    assert_wait_timeout(reinterpret_cast<event_t>(const_cast<uint32_t *>(&gMsiWake)),
                        THREAD_UNINT, ms, NSEC_PER_MSEC);
    if (__atomic_exchange_n(&gMsiWake, 0u, __ATOMIC_ACQ_REL)) {
        clear_wait(current_thread(), THREAD_AWAKENED);
        return;
    }
    thread_block(THREAD_CONTINUE_NULL);
    __atomic_store_n(&gMsiWake, 0u, __ATOMIC_RELEASE);
}

/* Kernel control socket (org.unixnu.wifi.control) for future wpa_supplicant. */
struct ControlClient { uint64_t session; uint32_t unit; };
kern_ctl_ref gControlRef = nullptr;
IOLock *gControlLock = nullptr;
ControlClient *gControlClient = nullptr;

errno_t controlError(int status) {
    switch (status) {
    case NW_CONTROL_OK: return 0;
    case NW_CONTROL_INVALID: return EINVAL;
    case NW_CONTROL_BUSY: return EWOULDBLOCK;
    case NW_CONTROL_NOT_READY: return ENXIO;
    case NW_CONTROL_AUTH_FAILED: return EACCES;
    case NW_CONTROL_TIMED_OUT: return ETIMEDOUT;
    case NW_CONTROL_UNSUPPORTED: return EOPNOTSUPP;
    case NW_CONTROL_NO_MEMORY: return ENOBUFS;
    case NW_CONTROL_NOT_FOUND: return ENOENT;
    case NW_CONTROL_STALE: return ESTALE;
    default: return EIO;
    }
}
errno_t controlConnect(kern_ctl_ref, sockaddr_ctl *address, void **information) {
    auto *owner = static_cast<ControlClient *>(IOMallocData(sizeof(ControlClient)));
    if (!owner) return ENOMEM;
    const int status = nw_wifi_control_attach(&owner->session);
    if (status) { IOFreeData(owner, sizeof(*owner)); return controlError(status); }
    owner->unit = address->sc_unit;
    IOLockLock(gControlLock); gControlClient = owner; IOLockUnlock(gControlLock);
    *information = owner;
    return 0;
}
errno_t controlDisconnect(kern_ctl_ref, uint32_t, void *information) {
    if (!information) return 0;
    auto *owner = static_cast<ControlClient *>(information);
    nw_wifi_control_detach(owner->session);
    IOLockLock(gControlLock); if (gControlClient == owner) gControlClient = nullptr; IOLockUnlock(gControlLock);
    IOFreeData(owner, sizeof(*owner));
    return 0;
}
int controlReadMbuf(void *source, void *destination, size_t length) {
    return mbuf_copydata(static_cast<mbuf_t>(source), 0, length, destination);
}
errno_t controlReceive(kern_ctl_ref, uint32_t, void *information, mbuf_t packet, int) {
    auto *owner = static_cast<ControlClient *>(information);
    nw_control_header hdr{};
    if (mbuf_pkthdr_len(packet) >= sizeof(hdr)) {
        (void)mbuf_copydata(packet, 0, sizeof(hdr), &hdr);
    }
    const int status = nw_wifi_control_receive(owner->session, mbuf_pkthdr_len(packet), controlReadMbuf, packet);
    mbuf_freem(packet);
    if (hdr.operation != NW_CONTROL_GET_BSS && hdr.operation != NW_CONTROL_SCAN && hdr.operation != 0) {
        char msg[96];
        snprintf(msg, sizeof(msg), "wifi-ctl: rx op=%u gen=%llu st=%d -> %d",
                 hdr.operation, static_cast<unsigned long long>(hdr.generation), hdr.status, status);
        pLog(6, msg);
    }
    return controlError(status);
}
void controlReceived(kern_ctl_ref, uint32_t, void *information, int) {
    nw_wifi_control_writable(static_cast<ControlClient *>(information)->session);
}
errno_t controlGetOption(kern_ctl_ref, uint32_t, void *information, int option, void *bytes, size_t *length) {
    if (option != NW_CONTROL_GET_SESSION) return ENOPROTOOPT;
    if (!length) return EINVAL;
    if (!bytes || *length < sizeof(uint64_t)) { *length = sizeof(uint64_t); return EMSGSIZE; }
    const uint64_t session = static_cast<ControlClient *>(information)->session;
    memcpy(bytes, &session, sizeof(session));
    *length = sizeof(session);
    return 0;
}
int controlSend(void *, uint64_t session, const void *bytes, size_t length) {
    IOLockLock(gControlLock);
    const bool active = gControlClient && gControlClient->session == session;
    const uint32_t unit = active ? gControlClient->unit : 0u;
    IOLockUnlock(gControlLock);
    if (!active) return NW_CONTROL_SEND_CLOSED;
    if (length >= sizeof(nw_control_header)) {
        nw_control_header hdr{};
        memcpy(&hdr, bytes, sizeof(hdr));
        if (hdr.operation != NW_CONTROL_GET_BSS && hdr.operation != NW_CONTROL_SCAN &&
            hdr.operation != NW_CONTROL_SCAN_STARTED && hdr.operation != NW_CONTROL_SCAN_DONE &&
            hdr.operation != NW_CONTROL_SCAN_REQUEST) {
            char msg[96];
            snprintf(msg, sizeof(msg), "wifi-ctl: tx k=%u op=%u gen=%llu st=%d len=%u",
                     hdr.kind, hdr.operation, static_cast<unsigned long long>(hdr.generation),
                     hdr.status, static_cast<unsigned>(length));
            pLog(6, msg);
        }
    }
    const errno_t r = ctl_enqueuedata(gControlRef, unit, const_cast<void *>(bytes), length, CTL_DATA_EOR);
    if (!r) return NW_CONTROL_SEND_OK;
    if (r == ENOBUFS || r == EWOULDBLOCK) return NW_CONTROL_SEND_AGAIN;
    return NW_CONTROL_SEND_CLOSED;
}
void controlWake(void *) {
    __atomic_store_n(&gMsiWake, 1u, __ATOMIC_RELEASE);
    thread_wakeup(reinterpret_cast<event_t>(const_cast<uint32_t *>(&gMsiWake)));
}
int controlStart() {
    if (gControlRef) return 0;
    if (!gControlLock) gControlLock = IOLockAlloc();
    if (!gControlLock) return ENOMEM;
    kern_ctl_reg reg{};
    snprintf(reg.ctl_name, sizeof(reg.ctl_name), "%s", NW_CONTROL_NAME);
    reg.ctl_flags = CTL_FLAG_PRIVILEGED | CTL_FLAG_REG_EXTENDED;
    reg.ctl_sendsize = 256u * 1024u;
    reg.ctl_recvsize = 512u * 1024u;
    reg.ctl_connect = controlConnect;
    reg.ctl_disconnect = controlDisconnect;
    reg.ctl_send = controlReceive;
    reg.ctl_getopt = controlGetOption;
    reg.ctl_rcvd = controlReceived;
    const errno_t r = ctl_register(&reg, &gControlRef);
    if (r) return r;
    const nw_control_transport tr{nullptr, controlSend, controlWake};
    const int st = nw_wifi_control_transport_set(&tr);
    if (st) { ctl_deregister(gControlRef); gControlRef = nullptr; return controlError(st); }
    return 0;
}

void netStartOnce() {
    if (gStk.netRegistered) return;
    gStk.netRegistered = true;
    nw_wifi_status status{};
    nw_wifi_status_get(&status);
    void *handle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
    if (!handle) { pLog(3, "wifi-net: preModLoad failed"); return; }
    __GLOBAL__sub_I_J716fWifiNet();
    const OSReturn r = OSMetaClass::postModLoad(handle);
    if (r) {
        char msg[64];
        snprintf(msg, sizeof(msg), "wifi-net: postModLoad err %d", static_cast<int>(r));
        pLog(3, msg);
        return;
    }
    j716f_wifi_net_start(status.mac);
    gStk.state = 3u;
}

void wifiWorker(void *, wait_result_t) {
    thread_set_thread_name(current_thread(), "j716f-wifi-ath11k");
    gStk.state = 1u;
    publishWifiMac();

    uint32_t barAddr = 0u, barSize = 0u;
    const int prc = j716f_wcn_wifi_prepare_for_ath11k(&barAddr, &barSize);
    if (prc != 0 || !barAddr) {
        gStk.lastErr = prc ? prc : -19;
        gStk.state = 4u;
        pLog(3, "wifi: PCIe0/SMMU prepare failed");
        thread_terminate(current_thread());
        return;
    }
    gStk.barAddr = barAddr;
    gStk.barSize = barSize;

    const size_t mapLen = barSize < 0x200000u ? barSize : 0x200000u;
    if (!arenaInit() ||
        !mapMmio(gDbi, J716F_PCIE0_DBI_BASE, 0x1000u) ||
        !mapMmio(gCfg, J716F_PCIE0_CFG_BASE, 0x1000u) ||
        !mapMmio(gBar, barAddr, mapLen) ||
        !findCaps()) {
        gStk.lastErr = -12;
        gStk.state = 4u;
        pLog(3, "wifi: MMIO/arena/caps init failed");
        thread_terminate(current_thread());
        return;
    }

    static const struct lx_platform plat = {
        pNow, pSleep, pPoll, pLog, pHalt, hAlloc, hFree,
        pDmaAlloc, pDmaFree, pDmaMap, pDmaSync, pFwGet,
        pNowNs, pBootNs, pBusyDelayUs, _disable_preemption, _enable_preemption,
    };
    nw_wifi_set_platform(&plat);

    static struct nw_board board{};
    board.bar = const_cast<uint32_t *>(gBar.va);
    board.bar_len = mapLen;
    board.device_id = static_cast<uint16_t>(gCfg.va[0] >> 16);
    board.msi_cap = static_cast<uint16_t>(gMsiCap);
    board.irq_base = 1000;
    board.cfg_read = pCfgRead;
    board.cfg_write = pCfgWrite;
    board.msi_target = pMsiTarget;
    board.map_phys = nullptr;
    uint32_t wifiDiag = 0u;
    PE_parse_boot_argn("log-wifi", &wifiDiag, sizeof(wifiDiag));
    board.diagnostics = wifiDiag ? 1u : 0u;

    (void)controlStart();
    pLog(6, "wifi: starting Linux ath11k + mac80211/cfg80211 stack");
    const int r = nw_wifi_start(&plat, &board);
    if (r != 0) {
        gStk.lastErr = r;
        gStk.state = 4u;
        char msg[80];
        snprintf(msg, sizeof(msg), "wifi: nw_wifi_start failed: %d", r);
        pLog(3, msg);
        thread_terminate(current_thread());
        return;
    }

    gStk.msiAttachRc = msiAttach();
    nw_wifi_irq_mode(gMsiAttached ? 1 : 0);
    {
        char msg[80];
        snprintf(msg, sizeof(msg), "wifi: ath11k running, msi spi141 rc=%d att=%u",
                 gStk.msiAttachRc, gMsiAttached ? 1u : 0u);
        pLog(6, msg);
    }
    gStk.state = 2u;

    uint64_t sliceStarted = pNowNs();
    uint64_t nextLinkPollMs = pNow() + 2000ULL;
    for (;;) {
        const int busy = nw_wifi_step();
        if (!gStk.netRegistered && nw_wifi_stats.stage >= 4u) {
            netStartOnce();
        }
        if (gStk.netRegistered) {
            const uint64_t nowMs = pNow();
            if (nowMs >= nextLinkPollMs) {
                nextLinkPollMs = nowMs + 2000ULL;
                char link[96];
                nw_wifi_link_state(link, sizeof(link));
            }
        }
        if (!busy) {
            if (gMsiAttached) {
                const unsigned wait = nw_wifi_wait_ms(1000u);
                if (wait) msiWait(wait);
            } else {
                IOSleep(1);
            }
            sliceStarted = pNowNs();
        } else if (pNowNs() - sliceStarted >= 2000000ULL) {
            thread_yield_to_preemption();
            sliceStarted = pNowNs();
        }
    }
}

void appendf(char *out, size_t cap, size_t &pos, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
void appendf(char *out, size_t cap, size_t &pos, const char *fmt, ...) {
    if (pos >= cap) return;
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(out + pos, cap - pos, fmt, ap);
    va_end(ap);
    if (n > 0) {
        const size_t add = static_cast<size_t>(n);
        pos = (pos + add < cap) ? (pos + add) : (cap - 1u);
    }
}

}  // namespace

extern "C" void j716f_wifi_logf(int level, const char *fmt, ...) {
    char line[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    pLog(level, line);
}

extern "C" void j716f_wifi_wake(void) {
    __atomic_store_n(&gMsiWake, 1u, __ATOMIC_RELEASE);
    thread_wakeup(reinterpret_cast<event_t>(const_cast<uint32_t *>(&gMsiWake)));
}

extern "C" void j716f_wifi_publish_early_dt(void) {
    publishWifiMac();
}

extern "C" void j716f_wifi_stack_start_async(void) {
    publishWifiMac();
    if (!gStk.started || gStk.state == 4u) {
        gStk.started = true;
        gStk.state = 1u;
        gStk.lastErr = 0;
        thread_t th = nullptr;
        if (kernel_thread_start(wifiWorker, nullptr, &th) != KERN_SUCCESS || !th) {
            gStk.state = 4u;
            gStk.lastErr = -12;
            return;
        }
        thread_deallocate(th);
    }
}

extern "C" int j716f_wifi_stack_start(void) {
    j716f_wifi_stack_start_async();
    /* Wait up to 10 s for GPU arena + ath11k + mac80211 to reach stage 4 (en0 registered). */
    for (unsigned i = 0; i < 1000u && gStk.state < 3u && gStk.state != 4u; ++i) {
        IOSleep(10);
    }
    return gStk.state == 3u ? 0 : (gStk.lastErr ? gStk.lastErr : -110);
}

extern "C" int j716f_wifi_stack_scan_now(void) {
    if (!gStk.started || gStk.state < 3u) {
        const int rc = j716f_wifi_stack_start();
        if (rc != 0) return rc;
    }
    const uint32_t before = nw_wifi_stats.scans;
    const int sr = nw_wifi_scan(nullptr, 0u);
    if (sr != 0) return sr;
    j716f_wifi_wake();
    for (unsigned i = 0; i < 1200u && nw_wifi_stats.scans == before; ++i) {
        IOSleep(10);
    }
    return nw_wifi_bss_count();
}

extern "C" void j716f_wifi_stack_snapshot(char *out, size_t cap) {
    if (!out || !cap) return;
    out[0] = '\0';
    size_t pos = 0;
    nw_wifi_status st{};
    if (gStk.started) nw_wifi_status_get(&st);
    uint32_t en0Up = 0, ioctls = 0, sreqs = 0, sres = 0;
    j716f_wifi_net_stats(&en0Up, &ioctls, &sreqs, &sres);
    char ipStr[32] = "none";
    uint64_t pktsIn = 0, pktsOut = 0;
    uint32_t rxOk = 0, rxErr = 0, txDrop = 0;
    j716f_wifi_net_stats_ext(ipStr, sizeof(ipStr), &pktsIn, &pktsOut, &rxOk, &rxErr, &txDrop);
    nw_control_stats ctl{};
    if (gStk.started) nw_wifi_control_stats_get(&ctl);

    appendf(out, cap, pos,
            "[usb] WCN_WIFI_STACK: st=%u err=%d nw_stg=%u(lerr=%d) sta=%u dt=%u/%u msi=%u(att=%u,rc=%d,bits=0x%08x) dma=%uK/%uK(fail=%u) scans=%u bss=%u ch=%u mac=%02x:%02x:%02x:%02x:%02x:%02x\r\n",
            gStk.state, gStk.lastErr, nw_wifi_stats.stage, nw_wifi_stats.last_err, st.state,
            gStk.dtChosenMac, gStk.dtWlanMac,
            gMsiCount, gMsiAttached ? 1u : 0u, gStk.msiAttachRc, gMsiBits,
            static_cast<unsigned>(gArena.used / 1024u), static_cast<unsigned>(gArena.peak / 1024u),
            gArena.failures, nw_wifi_stats.scans, nw_wifi_stats.bss_seen, st.channels,
            st.mac[0], st.mac[1], st.mac[2], st.mac[3], st.mac[4], st.mac[5]);
    char kinds[240] = {};
    j716f_wifi_net_kinds(kinds, sizeof(kinds));
    appendf(out, cap, pos,
            "[usb] WCN_WIFI_NET: en0=%u ip=%s freq=%u rssi=%d rate=%u.%u in=%llu out=%llu(rx=%u/%u,txdrop=%u) io=%u sr=%u res=%u | ctl=sess:%llu/conn:%llu/disc:%llu rx=%u tx=%u acc=%llu disp=%llu last=%u(%u)\r\n",
            en0Up, ipStr, st.freq_mhz, static_cast<int>(st.signal_dbm),
            st.tx_rate_100kbps / 10u, st.tx_rate_100kbps % 10u,
            static_cast<unsigned long long>(pktsIn), static_cast<unsigned long long>(pktsOut),
            rxOk, rxErr, txDrop, ioctls, sreqs, sres,
            static_cast<unsigned long long>(ctl.active_session),
            static_cast<unsigned long long>(ctl.connections),
            static_cast<unsigned long long>(ctl.disconnections),
            ctl.rx_items, ctl.tx_items,
            static_cast<unsigned long long>(ctl.accepted),
            static_cast<unsigned long long>(ctl.dispatched),
            ctl.last_operation, ctl.last_status);
    appendf(out, cap, pos,
            "[usb] WCN_WIFI_PKTS: %s | cryptex=%d/%d ls=%d gestalt=%d\r\n",
            kinds,
            &native_cryptex_os_stage ? native_cryptex_os_stage : -99,
            &native_cryptex_os_status ? native_cryptex_os_status : -99,
            &native_ls_store_status ? native_ls_store_status : -99,
            &native_gestalt_cache_status ? native_gestalt_cache_status : -99);
    char diag[512] = {};
    j716f_wifi_net_diag(diag, sizeof(diag));
    appendf(out, cap, pos, "[usb] WCN_WIFI_DIAG: %s\r\n", diag);

    const int nBss = gStk.started ? nw_wifi_bss_count() : 0;
    for (int i = 0; i < nBss && i < 32; ++i) {
        nw_bss_info b{};
        if (nw_wifi_bss_get(i, &b) != 0) continue;
        char ssid[33];
        const unsigned sl = b.ssid_len < 32u ? b.ssid_len : 32u;
        for (unsigned k = 0; k < sl; ++k) {
            const uint8_t c = b.ssid[k];
            ssid[k] = (c >= 0x20u && c < 0x7Fu) ? static_cast<char>(c) : '?';
        }
        ssid[sl] = '\0';
        const char *sec = (b.caps & 0x0010u) ? "WEP" : "OPEN";
        bool hasWpa = false, hasRsn = false, hasPsk = false, hasSae = false;
        for (unsigned o = 0; o + 2u <= b.ie_len && o + 2u + b.ie[o + 1u] <= b.ie_len; o += 2u + b.ie[o + 1u]) {
            const uint8_t id = b.ie[o], len = b.ie[o + 1u];
            const uint8_t *v = b.ie + o + 2u;
            if (id == 48u && len >= 8u) {
                hasRsn = true;
                unsigned p = 6u;
                if (p + 2u <= len) {
                    const unsigned pc = static_cast<unsigned>(v[p]) | (static_cast<unsigned>(v[p + 1u]) << 8);
                    p += 2u + pc * 4u;
                    if (p + 2u <= len) {
                        const unsigned ac = static_cast<unsigned>(v[p]) | (static_cast<unsigned>(v[p + 1u]) << 8);
                        p += 2u;
                        for (unsigned a = 0; a < ac && p + 4u <= len; ++a, p += 4u) {
                            if (v[p] == 0x00u && v[p + 1u] == 0x0Fu && v[p + 2u] == 0xACu) {
                                if (v[p + 3u] == 2u || v[p + 3u] == 6u) hasPsk = true;
                                if (v[p + 3u] == 8u) hasSae = true;
                            }
                        }
                    }
                }
            } else if (id == 221u && len >= 4u && v[0] == 0x00u && v[1] == 0x50u && v[2] == 0xF2u && v[3] == 0x01u) {
                hasWpa = true;
            }
        }
        if (hasRsn && hasPsk && hasSae) sec = "WPA2/3";
        else if (hasRsn && hasSae) sec = "WPA3";
        else if (hasRsn && hasWpa) sec = "WPA/2";
        else if (hasRsn) sec = "WPA2";
        else if (hasWpa) sec = "WPA";
        appendf(out, cap, pos,
                "[usb] WCN_WIFI_BSS[%d]: %uMHz %ddBm %02x:%02x:%02x:%02x:%02x:%02x caps=0x%04x %s \"%s\"\r\n",
                i, b.freq_mhz, static_cast<int>(b.signal_dbm),
                b.bssid[0], b.bssid[1], b.bssid[2], b.bssid[3], b.bssid[4], b.bssid[5],
                b.caps, sec, ssid);
    }

    const uint32_t totalLogs = gStk.logCount;
    const uint32_t show = totalLogs < 24u ? totalLogs : 24u;
    const uint32_t start = totalLogs - show;
    for (uint32_t i = 0; i < show; ++i) {
        appendf(out, cap, pos, "[usb] WCN_WIFI_LOG: %s\r\n", gStk.logs[(start + i) % kLogRingSize]);
    }
}
