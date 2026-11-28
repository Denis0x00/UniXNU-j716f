// SPDX-License-Identifier: BSD-2-Clause-Patent
// TB-J716F QCA6390 Wi-Fi network bridge (en0 + IO80211Interface + Apple80211 ioctls) for iPadOS 17.
// Adapted from UniXNU sheng WifiNet.cpp; late-init staged (zero __mod_init_func).
#include <IOKit/IOLib.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOService.h>
#include <IOKit/IORegistryEntry.h>
#include <IOKit/IOUserClient.h>
#include <libkern/OSAtomic.h>
#include <libkern/c++/OSData.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSSerialize.h>
#include <sys/kern_event.h>
#include <sys/socket.h>
#include <sys/sockio.h>
#include <net/ethernet.h>
#include <net/if.h>
#include <net/if_media.h>
#include <net/if_ether.h>
#include <net/kpi_interface.h>
extern "C" {
#include <net/bpf.h>
}
#include <net/if_types.h>
#include <netinet/in.h>
#include <sys/fcntl.h>
#include <sys/vnode.h>
#include <sys/kpi_mbuf.h>
extern "C" {
extern u_int32_t necp_drop_all_order __attribute__((weak_import));
extern u_int32_t necp_drop_all_level __attribute__((weak_import));
extern u_int32_t necp_drop_unentitled_order __attribute__((weak_import));
extern u_int32_t necp_session_count __attribute__((weak_import));
extern void rtalloc_scoped(void *, unsigned int) __attribute__((weak_import));
extern void rtfree(void *) __attribute__((weak_import));
struct proc;
extern void proc_iterate(unsigned, int (*)(struct proc *, void *), void *,
                         int (*)(struct proc *, void *), void *) __attribute__((weak_import));
extern int proc_pid(struct proc *) __attribute__((weak_import));
}
#include <kern/locks.h>
#include <kern/thread.h>
#include <kern/thread_call.h>
#include <sys/systm.h>
#include <stdarg.h>
#include "nw_abi.h"
#include "nw_control_endpoint.h"
#include "WifiScan.h"
#include "WifiScanResult.h"
#include "WifiScanPolicy.h"

static_assert(SHENG_WIFI_SCAN_FREQUENCIES == NW_WIFI_SCAN_FREQUENCIES_MAX, "scan frequencies");

extern "C" void j716f_wifi_logf(int level, const char *fmt, ...);
extern "C" void j716f_wifi_wake(void);

#define SIOCSA80211 0x802869c8UL
#define SIOCGA80211 0xc02869c9UL

extern "C" int j716f_kev_post(struct kev_msg *) __asm__("_kev_post_msg");

namespace {

struct apple80211req {
    char req_if_name[16];
    int32_t req_type;
    int32_t req_val;
    uint32_t req_len;
    uint32_t req_kernel;
    user_addr_t req_data;
};
static_assert(sizeof(apple80211req) == 40 && __builtin_offsetof(apple80211req, req_data) == 32, "req layout");

enum {
    kSsid = 1, kAuthType = 2, kCipherKey = 3, kChannel = 4, kPowerSave = 5, kTxPower = 7, kRate = 8,
    kBssid = 9, kScanReq = 10, kScanResult = 11, kCardCaps = 12, kState = 13, kPhyMode = 14,
    kOpMode = 15, kRssi = 16, kNoise = 17, kPower = 19, kAssociate = 20, kAssociateResult = 21,
    kDisassociate = 22, kSupportedChannels = 27, kLocale = 28, kDeauth = 29, kDriverVersion = 43,
    kHardwareVersion = 44, kCountryCode = 51, kRadioInfo = 54, kScanReqMultiple = 86,
    kScanCacheClear = 90, kCurrentNetwork = 103, kRssiBounds = 106, kChannelsInfo = 207,
    kHwSupportedChannels = 254, kNss = 353, kAbortScan = 382,
};
enum { kScanTypeCached = 3 };
enum {
    kEvPowerChanged = 1, kEvSsidChanged = 2, kEvBssidChanged = 3, kEvLinkChanged = 4,
    kEvAssocDone = 9, kEvScanDone = 10, kEvRssiChanged = 39,
};
enum { kAssocPending = 0, kAssocOk = 1, kAssocFailed = 2, kAssocTimeout = 9, kAssocBadKey = 21 };

struct assoc_event27 {
    uint8_t attempts[10][52];
    uint32_t reserved[2], reason, result, has_result;
};
using channel = sheng_wifi_channel;
struct channels_list { uint32_t version, count; channel list[401]; uint32_t unused; };
using scan_result = sheng_wifi_scan_result;
struct assoc_parameters {
    uint32_t ssid_length; uint8_t ssid[32]; uint8_t bssid[6], unused[2];
    uint32_t key_version, key_length, key_cipher; uint16_t key_flags, key_index; uint8_t key[64];
};
struct assoc_request {
    uint32_t version; uint16_t mode, auth_lower, auth_upper, unused;
    assoc_parameters network; uint8_t rest[0x250 - 0x88];
};
struct assoc_request17 {
    uint32_t version; uint16_t mode, auth_lower, auth_upper, unused;
    assoc_parameters network; uint8_t rest[0x358 - 0x88];
};
struct assoc_request27 {
    uint32_t version; uint16_t mode, unused; uint32_t auth_lower, auth_upper;
    assoc_parameters network; uint8_t rest[0x290 - 0x8c];
};
static_assert(sizeof(assoc_request) == 592 && sizeof(assoc_request17) == 856 && sizeof(assoc_request27) == 656,
              "apple80211_assoc_data layouts");
struct link_event {
    uint8_t down, unused0[3]; int32_t rssi; uint16_t snr, noise; uint8_t unused1[4];
    uint8_t involuntary, unused2[3]; uint32_t reason, subreason; uint8_t unused3[4];
};

ifnet_t gIf;
uint8_t gMac[6];
lck_grp_t *gGrp;
lck_mtx_t *gLock;
bool gPower = true;
bool gLinkSuppressed;
bool gKeyed;
uint32_t gRxFrames, gRxErrors, gTxRefused;
uint32_t gIoctlCount, gScanReqCount, gScanResultCount;
uint32_t gLogged[512 / 32];
struct FrameKinds {
    uint32_t arp, ip4, dhcp, dns, tcp, tcp_syn, tcp_synack, icmp, ip6, other;
};
FrameKinds gKindsIn, gKindsOut;
mbuf_t gRxHead, gRxTail, gRxSpare, gRxSpareParked;
uint32_t gRxPendingPackets, gRxPendingBytes;
int gAssocResult = kAssocPending;
bool gAssocModern;
bool gJoinPending;
unsigned gScanCursor, gScanning;
sheng_wifi_air gAir;
uint64_t gLinkUpMs;
volatile uint32_t gLinkSettleSeq;
thread_call_t gLinkSettleCall;

uint64_t continuous_ms() {
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(mach_continuous_time(), &ns);
    return ns / 1000000ULL;
}

void logf(int level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void logf(int level, const char *fmt, ...) {
    char line[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    j716f_wifi_logf(level, "%s", line);
}

void post_event(uint32_t code, const void *payload, size_t length) {
    char name[16] = "en0";
    struct kev_msg msg;
    memset(&msg, 0, sizeof(msg));
    msg.vendor_code = KEV_VENDOR_APPLE;
    msg.kev_class = 6;
    msg.kev_subclass = 1;
    msg.event_code = code;
    msg.dv[0].data_length = sizeof(name);
    msg.dv[0].data_ptr = name;
    msg.dv[1].data_length = static_cast<uint32_t>(length);
    msg.dv[1].data_ptr = const_cast<void *>(payload);
    const int error = j716f_kev_post(&msg);
    if (error || code == kEvAssocDone || code == kEvPowerChanged) {
        logf(6, "wifi-net: ev %u len %u -> %d", code, static_cast<unsigned>(length), error);
    }
}

void post_link(bool up) {
    gLinkUpMs = up ? continuous_ms() : 0;
    if (!gIf) return;
    ifnet_set_flags(gIf, up ? IFF_RUNNING : 0, IFF_RUNNING);
    struct { struct net_event_data data; } payload;
    memset(&payload, 0, sizeof(payload));
    payload.data.if_family = IFNET_FAMILY_ETHERNET;
    payload.data.if_unit = 0;
    snprintf(payload.data.if_name, sizeof(payload.data.if_name), "en");
    struct kev_msg msg;
    memset(&msg, 0, sizeof(msg));
    msg.vendor_code = KEV_VENDOR_APPLE;
    msg.kev_class = KEV_NETWORK_CLASS;
    msg.kev_subclass = KEV_DL_SUBCLASS;
    msg.event_code = up ? KEV_DL_LINK_ON : KEV_DL_LINK_OFF;
    msg.dv[0].data_length = sizeof(payload);
    msg.dv[0].data_ptr = &payload;
    j716f_kev_post(&msg);
}

void post_link_down(bool involuntary, int reason) {
    post_link(false);
    link_event ev{};
    ev.down = 1;
    ev.involuntary = involuntary;
    ev.reason = static_cast<uint32_t>(reason);
    post_event(kEvLinkChanged, &ev, sizeof(ev));
    post_event(kEvSsidChanged, nullptr, 0);
    post_event(kEvBssidChanged, nullptr, 0);
}

void post_association_result() {
    if (gAssocModern) {
        assoc_event27 event{};
        event.result = gAssocResult;
        event.has_result = 1;
        nw_wifi_status current;
        nw_wifi_status_get(&current);
        memcpy(event.attempts[0], current.bssid, 6);
        post_event(kEvAssocDone, &event, sizeof(event));
    } else {
        post_event(kEvAssocDone, nullptr, 0);
    }
}

void disconnect_user() {
    const bool pending = __atomic_exchange_n(&gJoinPending, false, __ATOMIC_ACQ_REL);
    __atomic_fetch_add(&gLinkSettleSeq, 1u, __ATOMIC_ACQ_REL);
    nw_wifi_status current;
    nw_wifi_status_get(&current);
    const bool wasRunning = gIf && (ifnet_flags(gIf) & IFF_RUNNING) != 0;
    const bool wasActive = current.state >= NW_STA_AUTH || pending || wasRunning;
    gLinkSuppressed = true;
    gAssocResult = kAssocFailed;
    if (wasActive || !gPower) {
        nw_wifi_disconnect();
        j716f_wifi_wake();
        post_link_down(false, 0);
    }
    if (pending) post_association_result();
}

int drain_disconnect() {
    disconnect_user();
    j716f_wifi_wake();
    for (unsigned waited = 0; waited < 2000; waited += 10) {
        nw_wifi_status current;
        nw_wifi_status_get(&current);
        if (current.state < NW_STA_AUTH) return 0;
        IOSleep(10);
    }
    return ETIMEDOUT;
}

void visible_status(nw_wifi_status &st) {
    nw_wifi_status_get(&st);
    if (!gPower || gLinkSuppressed) {
        st.state = NW_STA_IDLE;
        st.ssid_len = 0;
        memset(st.ssid, 0, sizeof(st.ssid));
        memset(st.bssid, 0, sizeof(st.bssid));
        st.freq_mhz = 0;
        st.signal_dbm = 0;
    }
}

void on_scan_done() {
    __atomic_store_n(&gScanning, 0u, __ATOMIC_RELEASE);
    gScanCursor = 0;
    post_event(kEvScanDone, nullptr, 0);
}

thread_call_t gCachedScan;
void cached_scan_done(thread_call_param_t, thread_call_param_t) { on_scan_done(); }

int effective_rssi(const nw_wifi_status &st) {
    if (st.signal_dbm < 0 && st.signal_dbm >= -127) return st.signal_dbm;
    nw_bss_info b;
    const int count = nw_wifi_bss_count();
    for (int i = 0; i < count; ++i) {
        if (!nw_wifi_bss_get(i, &b) && !memcmp(b.bssid, st.bssid, 6) &&
            b.signal_dbm < 0 && b.signal_dbm >= -127) {
            return b.signal_dbm;
        }
    }
    return -50;
}

bool en0_has_ipv4() {
    if (!gIf) return false;
    ifaddr_t *list = nullptr;
    bool ok = false;
    if (!ifnet_get_address_list_family(gIf, &list, AF_INET) && list) {
        ok = (list[0] != nullptr);
        ifnet_free_address_list(list);
    }
    return ok;
}

bool has_fresh_bss(const uint8_t *ssid, uint32_t ssidLen) {
    nw_bss_info b;
    const int count = nw_wifi_bss_count();
    for (int i = 0; i < count; ++i) {
        if (nw_wifi_bss_get(i, &b) || sheng_wifi_held_stale(b.age_ms)) continue;
        if (!ssid || !ssidLen) return true;
        if (b.ssid_len == ssidLen && !memcmp(b.ssid, ssid, ssidLen)) return true;
    }
    return false;
}

void post_link_rssi(const nw_wifi_status &st) {
    const int rssi = effective_rssi(st);
    link_event ev{};
    ev.rssi = rssi;
    ev.noise = static_cast<uint16_t>(-95);
    ev.snr = rssi > -95 ? static_cast<uint16_t>(rssi + 95) : 0;
    post_event(kEvLinkChanged, &ev, sizeof(ev));
}

void link_settle_fire(thread_call_param_t, thread_call_param_t pArg) {
    const uintptr_t arg = reinterpret_cast<uintptr_t>(pArg);
    const uint32_t seq = static_cast<uint32_t>(arg >> 4);
    const uintptr_t step = arg & 0xFu;
    if (__atomic_load_n(&gLinkSettleSeq, __ATOMIC_ACQUIRE) != seq) return;
    if (!gPower || gLinkSuppressed) return;
    nw_wifi_status st;
    nw_wifi_status_get(&st);
    if (st.state != NW_STA_UP) return;
    if (step == 8u) {
        post_event(kEvSsidChanged, nullptr, 0);
        post_event(kEvBssidChanged, nullptr, 0);
        post_link_rssi(st);
        return;
    }
    if (en0_has_ipv4()) {
        if (gLinkSettleCall) {
            uint64_t at;
            clock_interval_to_deadline(800, kMillisecondScale, &at);
            const uintptr_t nextArg = (static_cast<uintptr_t>(seq) << 4) | 8u;
            thread_call_enter1_delayed(gLinkSettleCall,
                                       reinterpret_cast<thread_call_param_t>(nextArg),
                                       at);
        }
        return;
    }
    if (step < 7u && gLinkSettleCall) {
        uint64_t at;
        clock_interval_to_deadline(1000, kMillisecondScale, &at);
        const uintptr_t nextArg = (static_cast<uintptr_t>(seq) << 4) | (step + 1u);
        thread_call_enter1_delayed(gLinkSettleCall,
                                   reinterpret_cast<thread_call_param_t>(nextArg),
                                   at);
    }
}

void on_connect_done(int result) {
    logf(6, "wifi-net: on_connect_done res=%d pwr=%u supp=%u pend=%u",
         result, gPower ? 1u : 0u, gLinkSuppressed ? 1u : 0u,
         __atomic_load_n(&gJoinPending, __ATOMIC_ACQUIRE) ? 1u : 0u);
    if (!gPower || gLinkSuppressed) return;
    nw_wifi_status snapshot;
    nw_wifi_status_get(&snapshot);
    if (result == NW_RES_OK && snapshot.state != NW_STA_UP) return;
    gAssocResult = result == NW_RES_OK ? kAssocOk
                   : result == NW_RES_BAD_KEY ? kAssocBadKey
                   : result == NW_RES_TIMEOUT ? kAssocTimeout
                   : kAssocFailed;
    const bool pending = __atomic_exchange_n(&gJoinPending, false, __ATOMIC_ACQ_REL);
    post_link(result == NW_RES_OK);
    if (result == NW_RES_OK) {
        post_event(kEvSsidChanged, nullptr, 0);
        post_event(kEvBssidChanged, nullptr, 0);
        nw_wifi_status st;
        nw_wifi_status_get(&st);
        post_link_rssi(st);
        if (pending) post_association_result();
        const uint32_t seq = __atomic_add_fetch(&gLinkSettleSeq, 1u, __ATOMIC_ACQ_REL);
        if (!gLinkSettleCall) gLinkSettleCall = thread_call_allocate(link_settle_fire, nullptr);
        if (gLinkSettleCall) {
            uint64_t at;
            clock_interval_to_deadline(1000, kMillisecondScale, &at);
            const uintptr_t firstArg = (static_cast<uintptr_t>(seq) << 4) | 0u;
            thread_call_enter1_delayed(gLinkSettleCall,
                                       reinterpret_cast<thread_call_param_t>(firstArg),
                                       at);
        }
    } else if (pending) {
        post_association_result();
    }
}

void on_link_lost(int reason) {
    __atomic_fetch_add(&gLinkSettleSeq, 1u, __ATOMIC_ACQ_REL);
    gAssocResult = kAssocFailed;
    post_link_down(true, reason);
}

void on_link_update(const nw_wifi_status *status) {
    if (!gPower || gLinkSuppressed || status->state != NW_STA_UP || status->signal_dbm >= 0) return;
    post_link_rssi(*status);
}

void free_rx_list(mbuf_t list) {
    while (list) {
        mbuf_t p = list;
        list = mbuf_nextpkt(p);
        mbuf_setnextpkt(p, nullptr);
        mbuf_freem(p);
    }
}

void flush_rx() {
    if (gRxHead) {
        mbuf_t packets = gRxHead;
        const uint32_t count = gRxPendingPackets, bytes = gRxPendingBytes;
        gRxHead = gRxTail = nullptr;
        gRxPendingPackets = gRxPendingBytes = 0;
        struct ifnet_stat_increment_param inc{};
        inc.packets_in = count;
        inc.bytes_in = bytes;
        const errno_t error = ifnet_input(gIf, packets, &inc);
        if (error) gRxErrors += count;
        else gRxFrames += count;
    }
    if (gRxSpare) {
        free_rx_list(__atomic_exchange_n(&gRxSpareParked, gRxSpare, __ATOMIC_ACQ_REL));
        gRxSpare = nullptr;
    }
}

mbuf_t allocate_rx(size_t length) {
    if (length <= 2048) {
        if (!gRxSpare) gRxSpare = __atomic_exchange_n(&gRxSpareParked, static_cast<mbuf_t>(nullptr), __ATOMIC_ACQ_REL);
        if (!gRxSpare) {
            unsigned chunks = 1;
            (void)mbuf_allocpacket_list(16, MBUF_DONTWAIT, 2048, &chunks, &gRxSpare);
        }
        if (gRxSpare) {
            mbuf_t p = gRxSpare;
            gRxSpare = mbuf_nextpkt(p);
            mbuf_setnextpkt(p, nullptr);
            return p;
        }
    }
    mbuf_t p = nullptr;
    if (mbuf_allocpacket(MBUF_DONTWAIT, length, nullptr, &p)) return nullptr;
    return p;
}

void note_frame(const uint8_t *e, size_t n, bool out) {
    FrameKinds &k = out ? gKindsOut : gKindsIn;
    const unsigned type = n >= 14 ? (static_cast<unsigned>(e[12]) << 8 | e[13]) : 0;
    const char *dir = out ? "out" : "in";
    if (type == 0x0806) {
        ++k.arp;
        static unsigned shown[2];
        if (n >= 42 && shown[out]++ < 10) {
            logf(6, "wifi-net: arp %s op=%u %u.%u.%u.%u->%u.%u.%u.%u",
                 dir, e[21], e[28], e[29], e[30], e[31], e[38], e[39], e[40], e[41]);
        }
        return;
    }
    if (type == 0x86dd) { ++k.ip6; return; }
    if (type != 0x0800 || n < 34) { ++k.other; return; }
    ++k.ip4;
    const uint8_t *ip = e + 14;
    const unsigned ihl = (ip[0] & 15u) * 4u;
    if (ihl < 20 || n < 14 + ihl) return;
    const uint8_t proto = ip[9];
    if (proto == 1) {
        ++k.icmp;
        return;
    }
    if (proto == 6 && n >= 14 + ihl + 14) {
        ++k.tcp;
        const uint8_t fl = ip[ihl + 13];
        if ((fl & 0x12u) == 0x02u) ++k.tcp_syn;
        if ((fl & 0x12u) == 0x12u) ++k.tcp_synack;
        return;
    }
    if (proto != 17 || n < 14 + ihl + 8) return;
    const uint8_t *udp = ip + ihl;
    const unsigned sp = (static_cast<unsigned>(udp[0]) << 8) | udp[1];
    const unsigned dp = (static_cast<unsigned>(udp[2]) << 8) | udp[3];
    if (sp == 53 || dp == 53) {
        ++k.dns;
        return;
    }
    if (!((sp == 67 || sp == 68) && (dp == 67 || dp == 68))) return;
    ++k.dhcp;
    const uint8_t *b = udp + 8;
    const size_t bl = n - static_cast<size_t>(b - e);
    unsigned msg = 0;
    for (size_t o = 240; bl >= 240 && o + 2 <= bl && b[o] != 255; o += b[o] ? 2u + b[o + 1] : 1u) {
        if (b[o] == 53 && o + 2 < bl) { msg = b[o + 2]; break; }
    }
    static unsigned dhcpShown[2];
    if (dhcpShown[out]++ < 16 && bl >= 240) {
        logf(6, "wifi-net: dhcp %s t=%u xid=%02x%02x%02x%02x yi=%u.%u.%u.%u",
             dir, msg, b[4], b[5], b[6], b[7], b[16], b[17], b[18], b[19]);
    }
    if (out || (msg != 2 && msg != 5)) return;
    char opts[120];
    int at = 0;
    for (size_t o = 240; o + 2 <= bl && b[o] != 255 && at >= 0 && at < static_cast<int>(sizeof(opts)) - 22;
         o += b[o] ? 2u + b[o + 1] : 1u) {
        if (!b[o]) continue;
        const unsigned code = b[o], len = b[o + 1];
        const uint8_t *v = b + o + 2;
        if (o + 2 + len > bl) break;
        if ((code == 1 || code == 3 || code == 6 || code == 54) && len >= 4) {
            for (unsigned i = 0; i + 4 <= len && at < static_cast<int>(sizeof(opts)) - 22; i += 4) {
                at += snprintf(opts + at, sizeof(opts) - static_cast<size_t>(at), " %u=%u.%u.%u.%u",
                               code, v[i], v[i + 1], v[i + 2], v[i + 3]);
            }
        } else if (code == 51 && len == 4) {
            at += snprintf(opts + at, sizeof(opts) - static_cast<size_t>(at), " 51=%u",
                           (static_cast<unsigned>(v[0]) << 24) | (static_cast<unsigned>(v[1]) << 16) |
                           (static_cast<unsigned>(v[2]) << 8) | v[3]);
        }
    }
    logf(6, "wifi-net: dhcp in t=%u opts:%s", msg, opts);
}

void note_packet(void *context, size_t length, nw_packet_reader reader, bool out) {
    uint8_t header[128];
    const size_t peek = length < sizeof(header) ? length : sizeof(header);
    if (reader(context, 0, header, peek)) return;
    bool dhcp = false;
    if (peek >= 42 && header[12] == 8 && header[13] == 0 && header[23] == 17) {
        const unsigned off = 14 + (header[14] & 15u) * 4u;
        if (off + 4 <= peek) {
            const unsigned sp = (static_cast<unsigned>(header[off]) << 8) | header[off + 1];
            const unsigned dp = (static_cast<unsigned>(header[off + 2]) << 8) | header[off + 3];
            dhcp = (sp == 67 || sp == 68) && (dp == 67 || dp == 68);
        }
    }
    if (dhcp && length <= 2000) {
        uint8_t frame[2000];
        if (!reader(context, 0, frame, length)) note_frame(frame, length, out);
    } else {
        note_frame(header, peek, out);
    }
}

void on_rx_packet(void *context, size_t length, nw_packet_reader reader) {
    if (!gIf || length < 14) return;
    enum : size_t { Align = 2 };
    mbuf_t m = allocate_rx(length + Align);
    if (!m && gRxHead) {
        flush_rx();
        m = allocate_rx(length + Align);
    }
    if (!m) return;
    size_t done = 0;
    bool first = true;
    for (mbuf_t n = m; n; n = mbuf_next(n)) {
        size_t room = mbuf_maxlen(n) - mbuf_leadingspace(n);
        if (first) {
            if (room < Align + 14) break;
            mbuf_setdata(n, static_cast<uint8_t *>(mbuf_datastart(n)) + mbuf_leadingspace(n) + Align, 0);
            room -= Align;
        }
        const size_t take = length - done < room ? length - done : room;
        if (reader(context, done, mbuf_data(n), take)) { done = 0; break; }
        mbuf_setlen(n, take);
        done += take;
        first = false;
    }
    if (done != length) { mbuf_freem(m); return; }
    note_frame(static_cast<const uint8_t *>(mbuf_data(m)), mbuf_len(m), false);
    mbuf_pkthdr_setlen(m, length);
    mbuf_pkthdr_setrcvif(m, gIf);
    void *ethHdr = mbuf_data(m);
    mbuf_pkthdr_setheader(m, ethHdr);
    mbuf_adj(m, 14);
    bpf_tap_in(gIf, DLT_EN10MB, m, ethHdr, 14);
    mbuf_setnextpkt(m, nullptr);
    if (gRxTail) mbuf_setnextpkt(gRxTail, m);
    else gRxHead = m;
    gRxTail = m;
    gRxPendingPackets++;
    gRxPendingBytes += static_cast<uint32_t>(length);
    if (gRxPendingPackets >= 64) flush_rx();
}

int flat_read(void *context, size_t offset, void *destination, size_t length) {
    memcpy(destination, static_cast<uint8_t *>(context) + offset, length);
    return 0;
}
void on_rx(const uint8_t *eth, size_t length) { on_rx_packet(const_cast<uint8_t *>(eth), length, flat_read); }

const struct nw_wifi_events gEvents = {
    on_scan_done, on_connect_done, on_link_lost, on_rx, flush_rx, on_rx_packet, on_link_update,
};

int mbuf_read(void *context, size_t offset, void *destination, size_t length) {
    return mbuf_copydata(static_cast<mbuf_t>(context), offset, length, destination);
}
void mbuf_release(void *context) { mbuf_freem(static_cast<mbuf_t>(context)); }

errno_t if_output(ifnet_t, mbuf_t m) {
    const size_t length = mbuf_pkthdr_len(m);
    if (length >= 14 && length <= 2000) {
        if (gIf) bpf_tap_out(gIf, DLT_EN10MB, m, nullptr, 0);
        if (mbuf_len(m) >= length) note_frame(static_cast<const uint8_t *>(mbuf_data(m)), length, true);
        else note_packet(m, length, mbuf_read, true);
        if (nw_wifi_tx_packet(m, length, mbuf_read, mbuf_release)) {
            ++gTxRefused;
        } else {
            ifnet_stat_increment_out(gIf, 1, static_cast<uint32_t>(length), 0);
            j716f_wifi_wake();
            return 0;
        }
    }
    mbuf_freem(m);
    return 0;
}
void if_event_stub(ifnet_t, const struct kev_msg *) {}

void fill_channel(channel &c, uint32_t mhz) {
    c.version = 1;
    if (mhz >= 4900) {
        c.channel = (mhz - 5000) / 5;
        c.flags = 0x2 | 0x10;
    } else {
        c.channel = mhz == 2484 ? 14 : (mhz - 2407) / 5;
        c.flags = 0x2 | 0x8;
    }
}

void fill_result(scan_result &r, const nw_bss_info &b, uint32_t requestBytes) {
    memset(&r, 0, sizeof(r));
    r.version = 1;
    fill_channel(r.ch, b.freq_mhz);
    r.noise_known = 1;
    r.noise = -95;
    r.rssi = b.signal_dbm;
    r.snr = b.signal_dbm + 95;
    r.beacon_interval = b.beacon_int;
    r.capability = b.caps;
    memcpy(r.bssid, b.bssid, 6);
    r.ssid_length = b.ssid_len;
    memcpy(r.ssid, b.ssid, 32);
    uint64_t now = 0;
    absolutetime_to_nanoseconds(mach_continuous_time(), &now);
    sheng_wifi_scan_result_stamp(&r, b.ie, b.ie_len, b.age_ms, now, requestBytes);
    r.rate_count = 0;
}

int copy_out(const apple80211req &rq, const void *src, size_t size) {
    if (rq.req_len == 0 || !rq.req_data) return EINVAL;
    const size_t n = rq.req_len < size ? rq.req_len : size;
    if (rq.req_kernel) { memcpy(reinterpret_cast<void *>(rq.req_data), src, n); return 0; }
    return copyout(src, rq.req_data, n);
}

int answer_value(apple80211req &rq, uint32_t value) {
    if (rq.req_len == 0 || !rq.req_data) { rq.req_val = static_cast<int32_t>(value); return 0; }
    const uint32_t w[2] = {1, value};
    return copy_out(rq, w, sizeof(w));
}

int answer_bytes(apple80211req &rq, const void *src, uint32_t size) {
    if (!rq.req_data) return EINVAL;
    const uint32_t n = rq.req_len < size ? rq.req_len : size;
    rq.req_len = size;
    if (rq.req_kernel) { memcpy(reinterpret_cast<void *>(rq.req_data), src, n); return 0; }
    return copyout(src, rq.req_data, n);
}

int copy_in(const apple80211req &rq, void *dst, size_t size) {
    if (!rq.req_data) return EINVAL;
    memset(dst, 0, size);
    const size_t n = rq.req_len < size ? rq.req_len : size;
    if (rq.req_kernel) { memcpy(dst, reinterpret_cast<const void *>(rq.req_data), n); return 0; }
    return copyin(rq.req_data, dst, n);
}

int copy_association(const apple80211req &rq, assoc_parameters &out) {
    if (rq.req_len != sizeof(assoc_request) &&
        rq.req_len != sizeof(assoc_request17) &&
        rq.req_len != sizeof(assoc_request27)) {
        uint32_t hdr[8] = {};
        (void)copy_in(rq, hdr, sizeof(hdr));
        logf(4, "wifi-net: assoc bad len=%u hdr=%08x %08x %08x %08x %08x %08x",
             rq.req_len, hdr[0], hdr[1], hdr[2], hdr[3], hdr[4], hdr[5]);
        return EINVAL;
    }
    union { assoc_request legacy; assoc_request17 ios17; assoc_request27 current; } wire;
    int error = copy_in(rq, &wire, sizeof(wire));
    if (!error) {
        if (rq.req_len == sizeof(assoc_request27)) {
            if (wire.current.version != 1) {
                logf(4, "wifi-net: assoc27 bad ver=%u", wire.current.version);
                error = EINVAL;
            } else {
                out = wire.current.network;
                logf(6, "wifi-net: assoc27 auth=%x/%x mode=%u", wire.current.auth_lower, wire.current.auth_upper, wire.current.mode);
            }
        } else {
            if (wire.legacy.version != 1) {
                logf(4, "wifi-net: assoc bad ver=%u", wire.legacy.version);
                error = EINVAL;
            } else {
                out = wire.legacy.network;
                logf(6, "wifi-net: assoc%u auth=%x/%x mode=%u", rq.req_len,
                     wire.legacy.auth_lower, wire.legacy.auth_upper, wire.legacy.mode);
            }
        }
    }
    memset(&wire, 0, sizeof(wire));
    return error;
}

void country(char cc[3]) {
    cc[0] = 'X'; cc[1] = 'X'; cc[2] = 0;
    int best = -200;
    nw_bss_info b;
    for (int i = 0, n = nw_wifi_bss_count(); i < n; ++i) {
        if (nw_wifi_bss_get(i, &b) || b.signal_dbm <= best) continue;
        for (unsigned o = 0; o + 2 <= b.ie_len && o + 2 + b.ie[o + 1] <= b.ie_len; o += 2 + b.ie[o + 1]) {
            if (b.ie[o] == 7 && b.ie[o + 1] >= 2 &&
                b.ie[o + 2] >= 'A' && b.ie[o + 2] <= 'Z' &&
                b.ie[o + 3] >= 'A' && b.ie[o + 3] <= 'Z') {
                cc[0] = static_cast<char>(b.ie[o + 2]);
                cc[1] = static_cast<char>(b.ie[o + 3]);
                best = b.signal_dbm;
                break;
            }
        }
    }
}

int map_state(unsigned s) {
    switch (s) {
    case NW_STA_SCANNING: return 1;
    case NW_STA_AUTH: return 2;
    case NW_STA_ASSOC:
    case NW_STA_KEYS: return 3;
    case NW_STA_UP: return 4;
    default: return 0;
    }
}

int apple80211(bool set, apple80211req &rq) {
    nw_wifi_status st;
    visible_status(st);
    const int type = rq.req_type;
    union Out { uint8_t raw[5120]; channels_list chans; scan_result res; };
    Out *outp = static_cast<Out *>(IOMallocData(sizeof(Out)));
    if (!outp) return ENOMEM;
    memset(outp, 0, sizeof(Out));
    struct Free { Out *p; ~Free() { IOFreeData(p, sizeof(Out)); } } freeOut{outp};
    Out &out = *outp;
    uint32_t *w = reinterpret_cast<uint32_t *>(out.raw);

    if (set) {
        switch (type) {
        case kPower: {
            uint32_t in[6];
            if (copy_in(rq, in, sizeof(in))) return EFAULT;
            gPower = in[2] != 0;
            if (!gPower) disconnect_user();
            nw_wifi_power(gPower);
            j716f_wifi_wake();
            post_event(kEvPowerChanged, nullptr, 0);
            return 0;
        }
        case kScanReq:
        case kScanReqMultiple: {
            ++gScanReqCount;
            if (!gPower) return ENETDOWN;
            if (gLinkSuppressed) {
                nw_wifi_status current;
                nw_wifi_status_get(&current);
                if (current.state >= NW_STA_AUTH) return EBUSY;
            }
            uint32_t ssidLength = 0, scanType = 0, frequencies = 0, mhz[SHENG_WIFI_SCAN_FREQUENCIES];
            const uint8_t *ssid = nullptr;
            if (type == kScanReq) {
                const uint32_t copied = SHENG_WIFI_SCAN_ONE_CHANNELS + SHENG_WIFI_SCAN_CHANNELS_READ * SHENG_WIFI_SCAN_CHANNEL_ENTRY;
                if (copy_in(rq, out.raw, copied)) return EFAULT;
                memcpy(&ssidLength, out.raw + 16, 4);
                ssid = out.raw + 20;
                memcpy(&scanType, out.raw + 0x34, 4);
                frequencies = sheng_wifi_scan_frequencies(out.raw, rq.req_len < copied ? rq.req_len : copied,
                                                          SHENG_WIFI_SCAN_ONE_CHANNEL_COUNT, SHENG_WIFI_SCAN_ONE_CHANNELS, mhz);
            } else {
                const uint32_t copied = SHENG_WIFI_SCAN_MULTIPLE_CHANNELS + SHENG_WIFI_SCAN_CHANNELS_READ * SHENG_WIFI_SCAN_CHANNEL_ENTRY;
                if (copy_in(rq, out.raw, copied)) return EFAULT;
                auto *names = reinterpret_cast<sheng_wifi_scan_multiple *>(out.raw + 0x600);
                const uint32_t held = rq.req_len < copied ? rq.req_len : copied;
                if (sheng_wifi_scan_multiple_parse(out.raw, held, names)) return EINVAL;
                frequencies = sheng_wifi_scan_frequencies(out.raw, held,
                                                          SHENG_WIFI_SCAN_MULTIPLE_CHANNEL_COUNT, SHENG_WIFI_SCAN_MULTIPLE_CHANNELS, mhz);
                scanType = names->scan_type;
                sheng_wifi_scan_multiple_target(names, &ssid, &ssidLength);
            }
            const uint8_t *wantSsid = (ssid && ssidLength && ssidLength <= 32) ? ssid : nullptr;
            const uint32_t wantSsidLen = wantSsid ? ssidLength : 0;
            const bool fresh = has_fresh_bss(wantSsid, wantSsidLen);
            if (scanType == kScanTypeCached) {
                gScanCursor = 0;
                return 0;
            }
            const uint64_t now = continuous_ms();
            if (st.state != NW_STA_UP) gLinkUpMs = 0;
            else if (!gLinkUpMs) gLinkUpMs = now;
            const sheng_wifi_link link = st.state == NW_STA_UP ? ShengWifiLinkUp
                                         : st.state >= NW_STA_AUTH ? ShengWifiLinkJoining
                                         : ShengWifiLinkNone;
            if (!sheng_wifi_scan_on_air(&gAir, link, now - gLinkUpMs, fresh, mhz, frequencies, now)) {
                if (!gCachedScan) gCachedScan = thread_call_allocate(cached_scan_done, nullptr);
                gScanning = 1;
                gScanCursor = 0;
                uint64_t at;
                clock_interval_to_deadline(2000, kMillisecondScale, &at);
                thread_call_enter_delayed(gCachedScan, at);
                return 0;
            }
            const bool merged = __atomic_load_n(&gScanning, __ATOMIC_ACQUIRE) != 0;
            gScanning = 1;
            gScanCursor = 0;
            const int result = nw_wifi_scan_frequencies(
                wantSsid, wantSsidLen,
                frequencies ? mhz : nullptr, frequencies);
            if (result) {
                __atomic_store_n(&gScanning, 0u, __ATOMIC_RELEASE);
                return ENETDOWN;
            }
            if (!merged) sheng_wifi_air_mark(&gAir, mhz, frequencies, now);
            j716f_wifi_wake();
            return 0;
        }
        case kAssociate: {
            assoc_parameters a{};
            struct Clear { assoc_parameters &p; ~Clear() { memset(&p, 0, sizeof(p)); } } clear{a};
            int error = copy_association(rq, a);
            if (error) return error;
            if (!a.ssid_length || a.ssid_length > 32 || a.key_length > 64) {
                logf(4, "wifi-net: assoc invalid ssid_len=%u key_len=%u", a.ssid_length, a.key_length);
                return EINVAL;
            }
            if (a.key_length && !((a.key_cipher == 6 && a.key_length == 32) ||
                                  (a.key_cipher == 10 && a.key_length >= 8 && a.key_length <= 63))) {
                logf(4, "wifi-net: assoc unsupported key_cipher=%u key_len=%u key_ver=%u",
                     a.key_cipher, a.key_length, a.key_version);
                return EINVAL;
            }
            const bool hasBssid = (a.bssid[0] | a.bssid[1] | a.bssid[2] | a.bssid[3] | a.bssid[4] | a.bssid[5]) != 0;
            const bool hasKey = a.key_length != 0 && a.key_length <= 64;
            logf(6, "wifi-net: join len=%u ssid_len=%u bssid=%u key_len=%u cipher=%u",
                 rq.req_len, a.ssid_length, hasBssid ? 1u : 0u, a.key_length, a.key_cipher);
            if (!gPower) return ENETDOWN;
            nw_wifi_status current;
            nw_wifi_status_get(&current);
            if (current.state >= NW_STA_AUTH) {
                error = drain_disconnect();
                if (error) return error;
                if (!gPower) return ENETDOWN;
            }
            char pass[65];
            memset(pass, 0, sizeof(pass));
            if (hasKey) memcpy(pass, a.key, a.key_length);
            unsigned keyLength = a.key_length;
            if (hasKey && a.key_cipher == 6 && a.key_length == 32) {
                static const char hex[] = "0123456789abcdef";
                for (int i = 0; i < 32; ++i) {
                    pass[2 * i] = hex[a.key[i] >> 4];
                    pass[2 * i + 1] = hex[a.key[i] & 15];
                }
                keyLength = 64;
            }
            gAssocResult = kAssocPending;
            gAssocModern = rq.req_len == sizeof(assoc_request27);
            gLinkSuppressed = false;
            __atomic_store_n(&gJoinPending, true, __ATOMIC_RELEASE);
            gKeyed = hasKey;
            const int r = nw_wifi_connect(a.ssid, a.ssid_length, hasBssid ? a.bssid : nullptr,
                                          hasKey ? pass : nullptr, hasKey ? keyLength : 0);
            logf(6, "wifi-net: nw_wifi_connect -> %d", r);
            if (!r) j716f_wifi_wake();
            else { gAssocResult = kAssocFailed; __atomic_store_n(&gJoinPending, false, __ATOMIC_RELEASE); }
            memset(pass, 0, sizeof(pass));
            return r ? EINVAL : 0;
        }
        case kDisassociate:
        case kDeauth:
            disconnect_user();
            return 0;
        case kScanCacheClear:
        case kLocale:
        case kCountryCode:
        case kPowerSave:
        case kAuthType:
        case kCipherKey:
        case kAbortScan:
            return 0;
        default:
            return EOPNOTSUPP;
        }
    }

    switch (type) {
    case kPower:
        w[0] = 1; w[1] = 1; w[2] = gPower && st.state != NW_STA_OFF ? 1 : 0;
        return copy_out(rq, w, 24);
    case kState:
        return answer_value(rq, static_cast<uint32_t>(gPower ? map_state(st.state) : 0));
    case kOpMode:
        return answer_value(rq, 1);
    case kAuthType:
        w[0] = 1; w[1] = 1; w[2] = gKeyed ? 0x10 : 0; w[3] = 0;
        return copy_out(rq, w, 16);
    case kPowerSave:
        return answer_value(rq, 0);
    case kCardCaps:
        sheng_wifi_caps_fill(out.raw);
        return answer_bytes(rq, out.raw, SHENG_WIFI_CAPS_BYTES);
    case kPhyMode:
        w[0] = 1; w[1] = 0x1 | 0x2 | 0x4 | 0x8 | 0x10 | 0x20 | 0x40; w[2] = st.state == NW_STA_UP ? 0x8 : 0;
        return copy_out(rq, w, 12);
    case kSsid:
        if (st.state < NW_STA_ASSOC) return ENOENT;
        return answer_bytes(rq, st.ssid, st.ssid_len);
    case kBssid:
        if (st.state < NW_STA_ASSOC) return ENOENT;
        return answer_bytes(rq, st.bssid, 6);
    case kChannel:
        if (st.state < NW_STA_AUTH) return ENOENT;
        w[0] = 1;
        fill_channel(*reinterpret_cast<channel *>(out.raw + 4), st.freq_mhz);
        return copy_out(rq, w, 16);
    case kRssi:
    case kNoise: {
        if (type == kRssi && st.state != NW_STA_UP) return ENOENT;
        const int32_t sv = type == kRssi ? static_cast<int32_t>(effective_rssi(st)) : -95;
        const uint32_t v = static_cast<uint32_t>(sv);
        w[0] = 1; w[1] = 1; w[2] = 0;
        w[3] = v; w[7] = v; w[8] = v; w[12] = v;
        return copy_out(rq, w, 13 * 4);
    }
    case kNss:
        return answer_value(rq, st.state == NW_STA_UP ? 2u : 0u);
    case kRate: {
        if (st.state != NW_STA_UP) return answer_value(rq, 0);
        const uint32_t mbps = st.tx_rate_100kbps >= 10u
                                  ? (st.tx_rate_100kbps / 10u)
                                  : (st.freq_mhz >= 4900u ? 433u : 144u);
        return answer_value(rq, mbps);
    }
    case kTxPower:
        return answer_value(rq, 15);
    case kDriverVersion:
    case kHardwareVersion: {
        const char *v = type == kDriverVersion ? "ath11k-j716f 1" : "QCA6390";
        strlcpy(reinterpret_cast<char *>(out.raw), v, 256);
        return copy_out(rq, out.raw, 256);
    }
    case kCountryCode: {
        char cc[3];
        country(cc);
        return answer_bytes(rq, cc, 3);
    }
    case kAssociateResult:
        return answer_value(rq, static_cast<uint32_t>(gAssocResult));
    case kSupportedChannels:
    case kHwSupportedChannels: {
        out.chans.version = 1;
        unsigned n = 0;
        static const uint16_t known[] = {
            2412, 2417, 2422, 2427, 2432, 2437, 2442, 2447, 2452, 2457, 2462,
            5180, 5200, 5220, 5240, 5745, 5765, 5785, 5805, 5825,
        };
        for (uint16_t f : known) {
            fill_channel(out.chans.list[n], f);
            out.chans.list[n].flags |= 0x80;
            ++n;
        }
        out.chans.count = n;
        return copy_out(rq, &out.chans, sizeof(out.chans));
    }
    case kScanResult: {
        nw_bss_info b;
        const int count = nw_wifi_bss_count();
        do {
            if (gScanCursor >= static_cast<unsigned>(count) || nw_wifi_bss_get(static_cast<int>(gScanCursor), &b)) {
                gScanCursor = 0;
                return EIO;
            }
            ++gScanCursor;
        } while (sheng_wifi_held_stale(b.age_ms));
        ++gScanResultCount;
        fill_result(out.res, b, rq.req_len);
        return copy_out(rq, &out.res, sizeof(out.res));
    }
    case kRssiBounds: {
        const int32_t bounds[4] = {1, -95, -30, -75};
        return copy_out(rq, bounds, sizeof(bounds));
    }
    case kCurrentNetwork: {
        if (st.state < NW_STA_ASSOC) return ENOENT;
        nw_bss_info b;
        const int count = nw_wifi_bss_count();
        for (int i = 0; i < count; ++i) {
            if (!nw_wifi_bss_get(i, &b) && !memcmp(b.bssid, st.bssid, 6)) {
                fill_result(out.res, b, rq.req_len);
                const int rssi = effective_rssi(st);
                out.res.rssi = static_cast<int16_t>(rssi);
                out.res.snr = rssi > -95 && rssi < 0 ? static_cast<int16_t>(rssi + 95) : 0;
                return copy_out(rq, &out.res, sizeof(out.res));
            }
        }
        return ENOENT;
    }
    default:
        return EOPNOTSUPP;
    }
}

errno_t if_ioctl(ifnet_t, unsigned long cmd, void *data) {
    switch (cmd) {
    case SIOCGA80211:
    case SIOCSA80211: {
        ++gIoctlCount;
        auto *rq = static_cast<apple80211req *>(data);
        rq->req_kernel = 0;
        const int e = apple80211(cmd == SIOCSA80211, *rq);
        static unsigned shown;
        const int t = rq->req_type;
        const bool first = t >= 0 && t < 512 && !(gLogged[t / 32] & (1u << (t % 32)));
        if (t >= 0 && t < 512) gLogged[t / 32] |= 1u << (t % 32);
        const bool important = (t == kAssociate || t == kAssociateResult ||
                                t == kDisassociate || t == kDeauth || t == kPower);
        if (shown < 40 || first || important) {
            if (shown < 40) ++shown;
            logf(6, "wifi-net: ioctl %s t=%d len=%u val=%d -> %d",
                 cmd == SIOCSA80211 ? "S" : "G", t, rq->req_len, rq->req_val, e);
        }
        return e;
    }
    case SIOCGIFMEDIA: {
        auto *m = static_cast<struct ifmediareq *>(data);
        nw_wifi_status st;
        visible_status(st);
        m->ifm_current = IFM_IEEE80211 | IFM_AUTO;
        m->ifm_mask = 0;
        m->ifm_status = IFM_AVALID | (st.state == NW_STA_UP ? IFM_ACTIVE : 0);
        m->ifm_active = IFM_IEEE80211 | IFM_AUTO;
        m->ifm_count = 1;
        static uint32_t lastMediaSt = 0xFFFFFFFFu;
        if (m->ifm_status != lastMediaSt) {
            lastMediaSt = m->ifm_status;
            logf(6, "wifi-net: SIOCGIFMEDIA st=0x%x (sta=%u)", m->ifm_status, st.state);
        }
        return 0;
    }
    case SIOCSIFFLAGS:
    case SIOCSIFMTU:
    case SIOCADDMULTI:
    case SIOCDELMULTI:
    case SIOCSIFADDR:
    case SIOCSIFMEDIA:
        return 0;
    default: {
        static unsigned long seen[16];
        static unsigned nseen = 0;
        bool known = false;
        for (unsigned i = 0; i < nseen; ++i) if (seen[i] == cmd) { known = true; break; }
        if (!known && nseen < 16u) {
            seen[nseen++] = cmd;
            logf(6, "wifi-net: unhandled sioc 0x%lx -> EOPNOTSUPP", cmd);
        }
        return EOPNOTSUPP;
    }
    }
}

}  // namespace

class IO80211Controller : public IOService { OSDeclareDefaultStructors(IO80211Controller); };
OSDefineMetaClassAndStructors(IO80211Controller, IOService);
class IONetworkInterface : public IOService { OSDeclareDefaultStructors(IONetworkInterface); };
OSDefineMetaClassAndStructors(IONetworkInterface, IOService);
class IO80211Interface : public IONetworkInterface {
    OSDeclareDefaultStructors(IO80211Interface);
  public:
    IOReturn newUserClient(task_t, void *, UInt32, OSDictionary *, IOUserClient **) APPLE_KEXT_OVERRIDE {
        return kIOReturnUnsupported;
    }
};
OSDefineMetaClassAndStructors(IO80211Interface, IONetworkInterface);

class IONetworkStackUserClient : public IOUserClient {
    OSDeclareDefaultStructors(IONetworkStackUserClient);
  public:
    IOReturn setProperties(OSObject *) APPLE_KEXT_OVERRIDE { return kIOReturnSuccess; }
    IOReturn clientClose() APPLE_KEXT_OVERRIDE { terminate(); return kIOReturnSuccess; }
};
OSDefineMetaClassAndStructors(IONetworkStackUserClient, IOUserClient);

class IONetworkStack : public IOService {
    OSDeclareDefaultStructors(IONetworkStack);
  public:
    IOReturn newUserClient(task_t task, void *security, UInt32 type, OSDictionary *, IOUserClient **handler) APPLE_KEXT_OVERRIDE {
        auto *client = OSTypeAlloc(IONetworkStackUserClient);
        if (!client || !client->initWithTask(task, security, type)) { OSSafeReleaseNULL(client); return kIOReturnNoMemory; }
        if (!client->attach(this)) { client->release(); return kIOReturnError; }
        if (!client->start(this)) { client->detach(this); client->release(); return kIOReturnError; }
        *handler = client;
        return kIOReturnSuccess;
    }
};
OSDefineMetaClassAndStructors(IONetworkStack, IOService);

namespace {

IOService *publishSvc(IOService *service, IOService *provider, const char *name) {
    if (!service || !service->attach(provider)) { OSSafeReleaseNULL(service); return nullptr; }
    service->setName(name);
    if (!service->start(provider)) { service->detach(provider); service->release(); return nullptr; }
    service->registerService();
    return service;
}

void publish_registry(const char *ifname) {
    IOService *platform = IOService::getPlatform();
    if (!platform) return;
    if (auto *stack = OSTypeAlloc(IONetworkStack)) {
        if (stack->init() && publishSvc(stack, platform, "IONetworkStack")) stack->release();
    }
    auto *controller = OSTypeAlloc(IO80211Controller);
    auto *wlan = OSTypeAlloc(IO80211Interface);
    if (!controller || !wlan || !controller->init() || !wlan->init()) {
        OSSafeReleaseNULL(controller);
        OSSafeReleaseNULL(wlan);
        return;
    }
    if (OSData *mac = OSData::withBytes(gMac, 6)) { controller->setProperty("IOMACAddress", mac); mac->release(); }
    controller->setProperty("IOBuiltin", true);
    controller = static_cast<IO80211Controller *>(publishSvc(controller, platform, "J716fWLANController"));
    if (!controller) { wlan->release(); return; }
    wlan->setProperty("IOInterfaceName", ifname);
    wlan->setProperty("BSD Name", ifname);
    wlan->setProperty("IOInterfaceNamePrefix", "en");
    if (OSNumber *n = OSNumber::withNumber(static_cast<unsigned long long>(IFT_ETHER), 32)) { wlan->setProperty("IOInterfaceType", n); n->release(); }
    if (OSNumber *n = OSNumber::withNumber(0ULL, 32)) { wlan->setProperty("IOInterfaceUnit", n); n->release(); }
    wlan->setProperty("IOBuiltin", true);
    wlan->setProperty("IOPrimaryInterface", true);
    wlan->setProperty("IO80211InterfaceRole", "Infrastructure");
    if (publishSvc(wlan, controller, "J716fWLAN")) wlan->release();
    controller->release();
}

}  // namespace

extern "C" void j716f_wifi_net_start(const uint8_t mac[6]) {
    if (gIf) return;
    memcpy(gMac, mac, 6);
    gGrp = lck_grp_alloc_init("wifi-net", LCK_GRP_ATTR_NULL);
    gLock = lck_mtx_alloc_init(gGrp, LCK_ATTR_NULL);

    struct ifnet_init_eparams p;
    memset(&p, 0, sizeof(p));
    p.ver = IFNET_INIT_CURRENT_VERSION;
    p.len = sizeof(p);
    p.flags = IFNET_INIT_LEGACY | IFNET_INIT_NX_NOAUTO;
    p.name = "en";
    p.unit = 0;
    p.family = IFNET_FAMILY_ETHERNET;
    p.subfamily = IFNET_SUBFAMILY_WIFI;
    p.type = IFT_ETHER;
    p.output = if_output;
    p.demux = ether_demux;
    p.add_proto = ether_add_proto;
    p.del_proto = ether_del_proto;
    p.check_multi = ether_check_multi;
    p.framer = ether_frameout;
    p.ioctl = if_ioctl;
    p.event = if_event_stub;
    static const uint8_t bcast[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
    p.broadcast_addr = bcast;
    p.broadcast_len = 6;

    ifnet_t ifp = nullptr;
    errno_t e = ifnet_allocate_extended(&p, &ifp);
    if (e || !ifp) { logf(3, "wifi-net: alloc failed %d", e); return; }
    ifnet_set_mtu(ifp, 1500);
    ifnet_set_flags(ifp, IFF_BROADCAST | IFF_SIMPLEX | IFF_MULTICAST, 0xffff);
    ifnet_set_baudrate(ifp, 54000000ULL);
    ifnet_set_hdrlen(ifp, 14);
    ifnet_set_addrlen(ifp, 6);
    ifnet_set_eflags(ifp, IFEF_ENQUEUE_MULTI, IFEF_ENQUEUE_MULTI);
    e = ifnet_attach(ifp, nullptr);
    if (e) { logf(3, "wifi-net: attach failed %d", e); ifnet_release(ifp); return; }
    ifnet_set_lladdr(ifp, mac, 6);
    (void)bpf_attach(ifp, DLT_EN10MB, 14, nullptr, nullptr);
    gIf = ifp;
    ifnet_set_flags(ifp, IFF_UP, IFF_UP);
    nw_wifi_set_events(&gEvents);
    publish_registry("en0");
    logf(6, "wifi-net: en0 up mac=%02x:%02x:%02x:%02x:%02x:%02x",
         mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

extern "C" void j716f_wifi_net_kinds(char *out, size_t cap) {
    if (!out || !cap) return;
    const uint16_t fl = gIf ? ifnet_flags(gIf) : 0;
    snprintf(out, cap,
             "fl=0x%x in(arp=%u ip4=%u dhcp=%u dns=%u tcp=%u/%u/%u icmp=%u ip6=%u oth=%u) "
             "out(arp=%u ip4=%u dhcp=%u dns=%u tcp=%u/%u/%u icmp=%u ip6=%u oth=%u)",
             fl,
             gKindsIn.arp, gKindsIn.ip4, gKindsIn.dhcp, gKindsIn.dns,
             gKindsIn.tcp, gKindsIn.tcp_syn, gKindsIn.tcp_synack, gKindsIn.icmp, gKindsIn.ip6, gKindsIn.other,
             gKindsOut.arp, gKindsOut.ip4, gKindsOut.dhcp, gKindsOut.dns,
             gKindsOut.tcp, gKindsOut.tcp_syn, gKindsOut.tcp_synack, gKindsOut.icmp, gKindsOut.ip6, gKindsOut.other);
}

extern "C" void j716f_wifi_net_stats(uint32_t *up, uint32_t *ioctls, uint32_t *scanReqs, uint32_t *scanRes) {
    if (up) *up = gIf ? 1u : 0u;
    if (ioctls) *ioctls = gIoctlCount;
    if (scanReqs) *scanReqs = gScanReqCount;
    if (scanRes) *scanRes = gScanResultCount;
}

extern "C" void j716f_wifi_net_stats_ext(char *ip_out, size_t ip_cap,
                                         uint64_t *pkts_in, uint64_t *pkts_out,
                                         uint32_t *rx_ok, uint32_t *rx_err, uint32_t *tx_drop) {
    if (ip_out && ip_cap) strlcpy(ip_out, "none", ip_cap);
    if (pkts_in) *pkts_in = 0;
    if (pkts_out) *pkts_out = 0;
    if (rx_ok) *rx_ok = gRxFrames;
    if (rx_err) *rx_err = gRxErrors;
    if (tx_drop) *tx_drop = gTxRefused;
    if (!gIf) return;
    if (ip_out && ip_cap) {
        ifaddr_t *list = nullptr;
        if (!ifnet_get_address_list_family(gIf, &list, AF_INET) && list) {
            if (list[0]) {
                struct sockaddr_in a;
                memset(&a, 0, sizeof(a));
                if (!ifaddr_address(list[0], reinterpret_cast<struct sockaddr *>(&a), sizeof(a))) {
                    const uint8_t *b = reinterpret_cast<const uint8_t *>(&a.sin_addr);
                    snprintf(ip_out, ip_cap, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
                }
            }
            ifnet_free_address_list(list);
        }
    }
    struct ifnet_stats_param stats;
    memset(&stats, 0, sizeof(stats));
    if (!ifnet_stat(gIf, &stats)) {
        if (pkts_in) *pkts_in = stats.packets_in;
        if (pkts_out) *pkts_out = stats.packets_out;
    }
}

namespace {

int wifiDiagProcCb(struct proc *p, void *arg) {
    auto *pids = static_cast<int *>(arg);
    if (!&proc_pid) return 0;
    const char *name = proc_best_name(p);
    if (!name) return 0;
    const int pid = proc_pid(p);
    if (!strcmp(name, "configd")) pids[0] = pid;
    else if (!strcmp(name, "wifid")) pids[1] = pid;
    else if (!strcmp(name, "mDNSResponder")) pids[2] = pid;
    else if (!strcmp(name, "nehelper")) pids[3] = pid;
    else if (!strcmp(name, "nesessionmanager")) pids[4] = pid;
    else if (!strcmp(name, "captiveagent")) pids[5] = pid;
    else if (!strcmp(name, "systemstatusd")) pids[6] = pid;
    else if (!strcmp(name, "CommCenter")) pids[7] = pid;
    else if (!strcmp(name, "MobileSafari")) pids[8] = pid;
    else if (!strncmp(name, "com.apple.WebKit", 16)) pids[9] = pid;
    return 0;
}

bool memContains(const uint8_t *hay, size_t hlen, const char *needle) {
    const size_t nlen = strlen(needle);
    if (!nlen || hlen < nlen) return false;
    for (size_t i = 0; i + nlen <= hlen; ++i) {
        if (hay[i] == static_cast<uint8_t>(needle[0]) && !memcmp(hay + i, needle, nlen)) return true;
    }
    return false;
}

void probePlistFile(const char *path, char *out, size_t cap) {
    vnode_t vp = nullptr;
    vfs_context_t ctx = vfs_context_current();
    const int err = vnode_open(path, O_RDONLY, 0, 0, &vp, ctx);
    if (err || !vp) {
        snprintf(out, cap, "err=%d", err);
        return;
    }
    struct vnode_attr va;
    VATTR_INIT(&va);
    VATTR_WANTED(&va, va_data_size);
    const uint64_t sz = (!vnode_getattr(vp, &va, ctx) && VATTR_IS_SUPPORTED(&va, va_data_size))
                            ? va.va_data_size
                            : 0ULL;
    const size_t want = sz > 16384u ? 16384u : static_cast<size_t>(sz);
    uint8_t *buf = want ? static_cast<uint8_t *>(IOMallocData(want)) : nullptr;
    int resid = static_cast<int>(want);
    bool hasEn0 = false, hasAir = false, hasCC = false;
    if (buf && !vn_rdwr(UIO_READ, vp, reinterpret_cast<caddr_t>(buf), static_cast<int>(want), 0,
                        UIO_SYSSPACE, IO_NODELOCKED, vfs_context_ucred(ctx), &resid, current_proc())) {
        const size_t got = want - static_cast<size_t>(resid > 0 ? resid : 0);
        hasEn0 = memContains(buf, got, "en0");
        hasAir = memContains(buf, got, "AirPort") || memContains(buf, got, "Wi-Fi");
        hasCC = memContains(buf, got, "CommCenter");
    }
    if (buf) IOFreeData(buf, want);
    vnode_close(vp, O_RDONLY, ctx);
    snprintf(out, cap, "%lluB(en0=%u,air=%u,cc=%u)", static_cast<unsigned long long>(sz),
             hasEn0 ? 1u : 0u, hasAir ? 1u : 0u, hasCC ? 1u : 0u);
}

struct j716f_route {
    void *ro_rt;
    void *ro_srcia;
    uint32_t ro_flags;
    struct sockaddr_in ro_dst;
    uint8_t ro_pad[16];
};

void probeRouteIp(uint32_t ipHost, unsigned int scope, char *out, size_t cap) {
    if (!&rtalloc_scoped || !&rtfree) {
        strlcpy(out, "nosym", cap);
        return;
    }
    j716f_route ro;
    memset(&ro, 0, sizeof(ro));
    ro.ro_dst.sin_len = sizeof(ro.ro_dst);
    ro.ro_dst.sin_family = AF_INET;
    ro.ro_dst.sin_addr.s_addr = htonl(ipHost);
    rtalloc_scoped(&ro, scope);
    if (!ro.ro_rt) {
        strlcpy(out, "NONE", cap);
        return;
    }
    strlcpy(out, "FOUND", cap);
    rtfree(ro.ro_rt);
}

void probeRoute(unsigned int scope, char *out, size_t cap) {
    probeRouteIp(0x01010101u, scope, out, cap);
}

void appendfDiag(char *out, size_t cap, size_t &pos, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
void appendfDiag(char *out, size_t cap, size_t &pos, const char *fmt, ...) {
    if (pos + 1u >= cap) return;
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(out + pos, cap - pos, fmt, ap);
    va_end(ap);
    if (n > 0) pos += (static_cast<size_t>(n) < cap - pos) ? static_cast<size_t>(n) : (cap - pos - 1u);
}

void dumpPlistText(const char *tag, const char *path, char *out, size_t cap, size_t &pos) {
    vnode_t vp = nullptr;
    vfs_context_t ctx = vfs_context_current();
    const int err = vnode_open(path, O_RDONLY, 0, 0, &vp, ctx);
    if (err || !vp) {
        appendfDiag(out, cap, pos, "[usb] %s: err=%d\r\n", tag, err);
        return;
    }
    struct vnode_attr va;
    VATTR_INIT(&va);
    VATTR_WANTED(&va, va_data_size);
    const uint64_t sz = (!vnode_getattr(vp, &va, ctx) && VATTR_IS_SUPPORTED(&va, va_data_size))
                            ? va.va_data_size : 0ULL;
    const size_t want = sz > 4096u ? 4096u : static_cast<size_t>(sz);
    uint8_t *buf = want ? static_cast<uint8_t *>(IOMallocData(want)) : nullptr;
    int resid = static_cast<int>(want);
    size_t got = 0;
    if (buf && !vn_rdwr(UIO_READ, vp, reinterpret_cast<caddr_t>(buf), static_cast<int>(want), 0,
                        UIO_SYSSPACE, IO_NODELOCKED, vfs_context_ucred(ctx), &resid, current_proc())) {
        got = want - static_cast<size_t>(resid > 0 ? resid : 0);
    }
    vnode_close(vp, O_RDONLY, ctx);
    const bool isBin = (got >= 8u && !memcmp(buf, "bplist00", 8));
    appendfDiag(out, cap, pos, "[usb] %s (%lluB,%s): ", tag,
                static_cast<unsigned long long>(sz), isBin ? "bplist" : "xml");
    if (buf && got) {
        if (isBin) {
            size_t i = 8u;
            bool first = true;
            while (i < got && pos + 64u < cap) {
                if (buf[i] >= 0x20u && buf[i] < 0x7Fu) {
                    size_t j = i;
                    while (j < got && buf[j] >= 0x20u && buf[j] < 0x7Fu) ++j;
                    if (j - i >= 2u) {
                        if (!first) appendfDiag(out, cap, pos, "|");
                        first = false;
                        for (size_t k = i; k < j && pos + 32u < cap; ++k) {
                            out[pos++] = static_cast<char>(buf[k]);
                        }
                        out[pos] = '\0';
                    }
                    i = j;
                } else {
                    ++i;
                }
            }
        } else {
            bool sp = false;
            for (size_t i = 0; i < got && pos + 32u < cap; ++i) {
                const uint8_t c = buf[i];
                if (c == '\r' || c == '\n' || c == '\t') {
                    if (!sp) { out[pos++] = ' '; sp = true; }
                } else if (c >= 0x20u && c < 0x7Fu) {
                    out[pos++] = static_cast<char>(c);
                    sp = (c == ' ');
                }
            }
            out[pos] = '\0';
        }
    }
    appendfDiag(out, cap, pos, "\r\n");
    if (buf) IOFreeData(buf, want);
}

struct AllProcsCtx {
    char *out;
    size_t cap;
    size_t pos;
    unsigned count;
};

int allProcsCb(struct proc *p, void *arg) {
    auto *ctx = static_cast<AllProcsCtx *>(arg);
    if (!&proc_pid || ctx->pos + 40u >= ctx->cap) return 0;
    const char *name = proc_best_name(p);
    if (!name) return 0;
    const int pid = proc_pid(p);
    appendfDiag(ctx->out, ctx->cap, ctx->pos, "%s%d:%s", ctx->count ? "," : "", pid, name);
    ++ctx->count;
    return 0;
}

}  // namespace

extern "C" void j716f_wifi_net_diag_full(char *out, size_t cap) {
    if (!out || !cap) return;
    out[0] = '\0';
    size_t pos = 0;

    // 1. All network interfaces
    ifnet_t *iflist = nullptr;
    uint32_t ifcnt = 0;
    if (!ifnet_list_get(IFNET_FAMILY_ANY, &iflist, &ifcnt) && iflist) {
        for (uint32_t i = 0; i < ifcnt; ++i) {
            ifnet_t ifp = iflist[i];
            if (!ifp) continue;
            char ip[32] = "-";
            ifaddr_t *al = nullptr;
            if (!ifnet_get_address_list_family(ifp, &al, AF_INET) && al) {
                if (al[0]) {
                    struct sockaddr_in a;
                    memset(&a, 0, sizeof(a));
                    if (!ifaddr_address(al[0], reinterpret_cast<struct sockaddr *>(&a), sizeof(a))) {
                        const uint8_t *b = reinterpret_cast<const uint8_t *>(&a.sin_addr);
                        snprintf(ip, sizeof(ip), "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
                    }
                }
                ifnet_free_address_list(al);
            }
            appendfDiag(out, cap, pos,
                        "[usb] WCN_DIAG_IF: idx=%u %s%u fl=0x%x efl=0x%x ip=%s\r\n",
                        ifnet_index(ifp), ifnet_name(ifp) ? ifnet_name(ifp) : "?",
                        ifnet_unit(ifp), ifnet_flags(ifp), ifnet_eflags(ifp), ip);
        }
        ifnet_list_free(iflist);
    }

    // 2. Routes matrix (unscoped vs en0-scoped)
    const unsigned idx = gIf ? ifnet_index(gIf) : 0u;
    char g1[16], s1[16], ggw[16], sgw[16], gip[16], sip[16];
    probeRouteIp(0x01010101u, 0u, g1, sizeof(g1));
    probeRouteIp(0x01010101u, idx, s1, sizeof(s1));
    probeRouteIp(0x0A010201u, 0u, ggw, sizeof(ggw));
    probeRouteIp(0x0A010201u, idx, sgw, sizeof(sgw));
    probeRouteIp(0x0A0102C7u, 0u, gip, sizeof(gip));
    probeRouteIp(0x0A0102C7u, idx, sip, sizeof(sip));
    appendfDiag(out, cap, pos,
                "[usb] WCN_DIAG_RT: 1.1.1.1(g=%s,s=%s) 10.1.2.1(g=%s,s=%s) 10.1.2.199(g=%s,s=%s)\r\n",
                g1, s1, ggw, sgw, gip, sip);

    // 3. Full contents of preferences.plist & NetworkInterfaces.plist
    dumpPlistText("WCN_DIAG_PREF", "/private/var/preferences/SystemConfiguration/preferences.plist", out, cap, pos);
    dumpPlistText("WCN_DIAG_NETIF", "/private/var/preferences/SystemConfiguration/NetworkInterfaces.plist", out, cap, pos);

    // 4. Other SC/network plists presence & sizes
    static const char *const kScFiles[] = {
        "/private/var/preferences/SystemConfiguration/com.apple.wifi.plist",
        "/private/var/preferences/SystemConfiguration/com.apple.wifi-private-mac-networks.plist",
        "/private/var/preferences/SystemConfiguration/com.apple.airport.preferences.plist",
        "/private/var/preferences/SystemConfiguration/com.apple.captive.plist",
        "/private/var/preferences/SystemConfiguration/com.apple.network.identification.plist",
        "/private/var/preferences/com.apple.networkextension.plist",
    };
    appendfDiag(out, cap, pos, "[usb] WCN_DIAG_SC:");
    for (const char *path : kScFiles) {
        const char *base = path;
        for (const char *c = path; *c; ++c) if (*c == '/') base = c + 1;
        char info[48] = {};
        probePlistFile(path, info, sizeof(info));
        appendfDiag(out, cap, pos, " %s=%s", base, info);
    }
    appendfDiag(out, cap, pos, "\r\n");

    // 5. All running processes
    appendfDiag(out, cap, pos, "[usb] WCN_DIAG_PROCS: ");
    AllProcsCtx pctx{out, cap, pos, 0};
    if (&proc_iterate) proc_iterate(1u | 4u, allProcsCb, &pctx, nullptr, nullptr);
    pos = pctx.pos;
    appendfDiag(out, cap, pos, " (n=%u)\r\n", pctx.count);
}

extern "C" void j716f_wifi_net_diag(char *out, size_t cap) {
    if (!out || !cap) return;
    char rtGlob[64] = "n/a", rtScoped[64] = "n/a";
    const unsigned idx = gIf ? ifnet_index(gIf) : 0u;
    const uint32_t efl = gIf ? ifnet_eflags(gIf) : 0u;
    probeRoute(0u, rtGlob, sizeof(rtGlob));
    if (idx) probeRoute(idx, rtScoped, sizeof(rtScoped));
    int pids[10] = {};
    if (&proc_iterate) proc_iterate(1u | 4u, wifiDiagProcCb, pids, nullptr, nullptr);
    char pref[64] = {}, netif[64] = {}, dis[64] = {};
    probePlistFile("/private/var/preferences/SystemConfiguration/preferences.plist", pref, sizeof(pref));
    probePlistFile("/private/var/preferences/SystemConfiguration/NetworkInterfaces.plist", netif, sizeof(netif));
    probePlistFile("/private/var/db/com.apple.xpc.launchd/disabled.plist", dis, sizeof(dis));
    snprintf(out, cap,
             "idx=%u efl=0x%x rt_glob=%s rt_en0=%s | necp(lvl=%u,ord=%u,unent=%u,sess=%u)\r\n"
             "[usb] WCN_WIFI_PROCS: cfgd=%d wifid=%d mdns=%d nehlp=%d nesess=%d cap=%d sstat=%d cc=%d saf=%d wk=%d\r\n"
             "[usb] WCN_WIFI_PLISTS: pref=%s netif=%s dis=%s",
             idx, efl, rtGlob, rtScoped,
             &necp_drop_all_level ? necp_drop_all_level : 999u,
             &necp_drop_all_order ? necp_drop_all_order : 999u,
             &necp_drop_unentitled_order ? necp_drop_unentitled_order : 999u,
             &necp_session_count ? necp_session_count : 999u,
             pids[0], pids[1], pids[2], pids[3], pids[4], pids[5], pids[6], pids[7], pids[8], pids[9],
             pref, netif, dis);
}
