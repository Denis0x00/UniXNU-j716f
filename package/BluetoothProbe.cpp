/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <libkern/c++/OSData.h>
#include <kern/thread.h>
#include <kern/clock.h>
#include <kern/task.h>
#include <pexpert/pexpert.h>
#include <sys/errno.h>
#include <stdarg.h>
#include <string.h>
#include "qbt.h"
#include "qbt_geni.h"
#include "qbt_transport.h"
#include "Sm8250GeniUart.h"
#include "SmmuBypass.h"
#include "AppsSmmuLock.h"
#include "WcnPower.h"
#include "BluetoothFirmware.h"

extern "C" int armcpu_spi_attach(uint32_t, bool, void (*)(void *), void *, uint32_t *, uint64_t *) __attribute__((weak_import));
extern "C" int armcpu_spi_share(uint32_t, bool, void (*)(void *), void *, uint32_t *, uint64_t *) __attribute__((weak_import));
extern "C" void armcpu_spi_mask(uint32_t, bool) __attribute__((weak_import));
extern "C" volatile uint32_t native_wifi_stats[29];
extern "C" volatile uint32_t native_wifi_mhi[16];
extern "C" volatile int native_gpu_addon_stage;
extern "C" volatile int native_bt_addon_stage, native_bt_addon_status;
extern "C" volatile uint32_t qbt_transport_activity[4];
extern "C" volatile uint64_t native_bt_last_exit[2];
extern "C" {
volatile uint64_t native_bluetooth_stats[32];
uint8_t native_bluetooth_boot_rx[512];
volatile uint32_t native_bluetooth_boot_rx_length;
uint8_t native_bluetooth_boot_tail[256];
volatile uint32_t native_bluetooth_boot_tail_length;
volatile uint32_t native_bluetooth_boot_state[12];
volatile uint32_t native_bluetooth_processes[4];
char *volatile native_bluetooth_boot_text;
volatile uint32_t native_bluetooth_boot_text_length;

volatile unsigned native_bt_step;
char native_bt_rows[16][120];
}

namespace {
struct Region { IOMemoryMap *map; volatile uint32_t *va; };
struct Bluetooth {
    Region gcc, uart, wrapper, tlmm, smmu, gic;
    IOService *radio;
    IOBufferMemoryDescriptor *dma;
    struct smmu_plan domain;
    uint64_t dma_address;
    struct qbt_geni geni;
    struct qbt *core;
    uint64_t rx_sequence;
    uint32_t pending, rx_wake, power_request, started;
    unsigned log, baud;
    uint8_t address[6];
    bool resources, powered, gated, clocked, serial_irq, wake_irq, service, initializing, loopback;
    uint8_t loop_rx[64];
    unsigned loop_length, loop_us, clock_num, clock_den;
    char stats_str[256];
    char uart_str[256];
} bt;

/* SM8250 TLMM NORTH tile (0x0f900000):
 * UART6 (qupv3_0_se6 @ 0x998000): GPIO 16 (CTS), 17 (RFR), 18 (TX), 19 (RX)
 * Control pins (bt_qca6390): GPIO 21 (BT_EN), GPIO 124 (SW_CTRL) */
constexpr unsigned kUartCts = 16, kUartRfr = 17, kUartTx = 18, kRx = 19, kBtEnable = 21, kSwCtrl = 124;
constexpr uint32_t kBtEnableCfg = 1u | (3u << 6) | (1u << 9); // pull-down, 8 mA, output (matches WifiProbe kWlanEn & j716f.dts bt_en)
constexpr uint32_t kDmaBytes = 32768;

void recordRow(unsigned slot, const char *fmt, ...) {
    if (slot >= 16) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(native_bt_rows[slot], sizeof(native_bt_rows[slot]), fmt, ap);
    va_end(ap);
}

bool map(Region &r, uint64_t base, size_t bytes) {
    if (r.va) return true;
    IODeviceMemory *memory = IODeviceMemory::withRange(base, bytes);
    r.map = memory ? memory->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
    if (memory) memory->release();
    r.va = r.map ? reinterpret_cast<volatile uint32_t *>(r.map->getVirtualAddress()) : nullptr;
    return r.va != nullptr;
}
void barrier() { __asm__ volatile("dsb sy" ::: "memory"); }
uint64_t nowNs(void *) { uint64_t ns; absolutetime_to_nanoseconds(mach_absolute_time(), &ns); return ns; }
void wake(void *) { __atomic_store_n(&bt.pending, 1u, __ATOMIC_RELEASE); thread_wakeup((event_t)&bt.pending); }
void waitUntil(void *, uint64_t deadline) {
    if (__atomic_exchange_n(&bt.pending, 0u, __ATOMIC_ACQ_REL)) return;
    if (deadline == UINT64_MAX) assert_wait((event_t)&bt.pending, THREAD_UNINT);
    else {
        uint64_t absolute;
        nanoseconds_to_absolutetime(deadline, &absolute);
        assert_wait_deadline((event_t)&bt.pending, THREAD_UNINT, absolute);
    }
    if (__atomic_exchange_n(&bt.pending, 0u, __ATOMIC_ACQ_REL)) clear_wait(current_thread(), THREAD_AWAKENED);
    else thread_block(THREAD_CONTINUE_NULL);
}
uint32_t urd(void *, uint32_t reg) { return bt.uart.va[reg / 4]; }
void uwr(void *, uint32_t reg, uint32_t value) { bt.uart.va[reg / 4] = value; barrier(); }
uint32_t grd(void *, uint32_t reg) { return bt.gcc.va[reg / 4]; }
void gwr(void *, uint32_t reg, uint32_t value) { bt.gcc.va[reg / 4] = value; barrier(); }
void delay(void *, unsigned us) { IODelay(us); }
void dmaBarrier(void *) { barrier(); }
void dmaSync(void *, const struct qbt_dma_buffer *buffer, size_t bytes, bool device, bool tx) {
    if (bytes && (device || !tx)) {
        uint64_t ctr;
        __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
        size_t line = 4u << ((ctr >> 16) & 15);
        uintptr_t at = reinterpret_cast<uintptr_t>(buffer->bytes) & ~(line - 1);
        uintptr_t end = reinterpret_cast<uintptr_t>(buffer->bytes) + bytes;
        for (; at < end; at += line) {
            if (tx) __asm__ volatile("dc cvac, %0" :: "r"(at) : "memory");
            else if (device) __asm__ volatile("dc civac, %0" :: "r"(at) : "memory");
            else __asm__ volatile("dc ivac, %0" :: "r"(at) : "memory");
        }
    }
    barrier();
}
uint32_t srd(void *, uint32_t reg) { return bt.smmu.va[reg / 4]; }
void swr(void *, uint32_t reg, uint32_t value) { bt.smmu.va[reg / 4] = value; barrier(); }
void irqMask(void *, bool masked) {
    if (!bt.serial_irq) return;
    if (armcpu_spi_mask) {
        armcpu_spi_mask(SM8250_UART6_INTID, masked);
        return;
    }
    if (!bt.gic.va) return;
    const uint32_t word = (SM8250_UART6_INTID / 32u) * 4u;
    const uint32_t bit = 1u << (SM8250_UART6_INTID % 32u);
    bt.gic.va[((masked ? 0x180u : 0x100u) + word) / 4u] = bit;
    barrier();
}
void serialIrq(void *) { qbt_geni_irq(&bt.geni); }
void pin(unsigned gpio, uint32_t cfg, int output = -1) {
    if (output >= 0) bt.tlmm.va[(gpio * 4096 + 4) / 4] = output ? 2u : 0u;
    bt.tlmm.va[gpio * 4096 / 4] = cfg;
    if (output >= 0) bt.tlmm.va[(gpio * 4096 + 4) / 4] = output ? 2u : 0u;
    barrier();
}
void rxIrq(void *) {
    volatile uint32_t *r = bt.tlmm.va + kRx * 4096 / 4;
    if (!(r[3] & 1)) return;
    r[2] &= ~1u;
    r[3] = 0;
    barrier();
    __atomic_store_n(&bt.rx_wake, 1u, __ATOMIC_RELEASE);
    wake(nullptr);
}
void disarmRxWake() {
    bt.tlmm.va[(kRx * 4096 + 8) / 4] &= ~1u;
    bt.tlmm.va[(kRx * 4096 + 12) / 4] = 0;
    pin(kRx, 3u | (1u << 2));
}
bool serialClock(bool enabled) {
    if (!sm8250_geni_uart6_vote(bt.gcc.va + SM8250_GCC_APCS_CLOCK_BRANCH_ENA_VOTE / 4, enabled)) return false;
    if (!enabled) { bt.clocked = false; return true; }
    for (unsigned i = 0; i < 1000; i++) {
        if (!(grd(nullptr, SM8250_GCC_QUPV3_WRAP0_S6_CBCR) & (1u << 31))) {
            bt.clocked = true;
            return true;
        }
        IODelay(1);
    }
    return false;
}
size_t rxBytes(void *, const uint8_t *bytes, size_t n) {
    if (bt.loopback) {
        size_t count = sizeof(bt.loop_rx) - bt.loop_length;
        if (count > n) count = n;
        memcpy(bt.loop_rx + bt.loop_length, bytes, count);
        bt.loop_length += (unsigned)count;
        return n;
    }
    if (bt.initializing) {
        uint32_t at = __atomic_load_n(&native_bluetooth_boot_rx_length, __ATOMIC_RELAXED);
        size_t count = sizeof(native_bluetooth_boot_rx) - at;
        if (count > n) count = n;
        if (count) {
            memcpy(native_bluetooth_boot_rx + at, bytes, count);
            __atomic_store_n(&native_bluetooth_boot_rx_length, at + (uint32_t)count, __ATOMIC_RELEASE);
        }
        uint32_t tail = __atomic_load_n(&native_bluetooth_boot_tail_length, __ATOMIC_RELAXED);
        for (size_t i = 0; i < n; i++)
            native_bluetooth_boot_tail[(tail + i) % sizeof(native_bluetooth_boot_tail)] = bytes[i];
        __atomic_store_n(&native_bluetooth_boot_tail_length, tail + (uint32_t)n, __ATOMIC_RELEASE);
    }
    bt.rx_sequence += n;
    return qbt_receive(bt.core, bytes, n);
}
size_t nativeRx(void *, const uint8_t *bytes, size_t n) { return qbt_transport_receive(bytes, (uint32_t)n); }
size_t writeUart(void *, const uint8_t *p, size_t n) { return qbt_geni_write(&bt.geni, p, n); }
bool drained(void *) { return bt.gated || qbt_geni_drained(&bt.geni); }
int flow(void *, bool enabled) { qbt_geni_set_flow(&bt.geni, enabled); return 0; }
void pump(void *) {
    if (bt.log >= 2 && bt.initializing) {
        struct qbt_stats stats;
        qbt_get_stats(bt.core, &stats);
        native_bluetooth_stats[3] = stats.tx_bytes;
        native_bluetooth_stats[4] = stats.rx_bytes;
        native_bluetooth_boot_state[10] = (uint32_t)stats.tlv_acks | ((uint32_t)stats.tlv_sent << 16);
        native_bluetooth_boot_state[11] = (uint32_t)stats.hardware_errors | ((uint32_t)stats.last_hardware_error << 8) | ((uint32_t)stats.tlv_final_status << 16);
    }
    if (__atomic_exchange_n(&bt.rx_wake, 0u, __ATOMIC_ACQ_REL) && bt.powered) {
        qbt_rx_wake(bt.core);
        const uint8_t wakeByte = 0xfd;
        (void)qbt_receive(bt.core, &wakeByte, 1);
    }
    if (!bt.gated) {
        if (qbt_geni_service(&bt.geni)) wake(nullptr);
        if (bt.geni.state == QBT_GENI_FAILED) qbt_fail(bt.core, bt.geni.error);
    }
}
int quiesce() {
    uint64_t end = nowNs(nullptr) + 40000000ULL;
    for (;;) {
        int e = qbt_geni_quiesce(&bt.geni);
        if (!e) return 0;
        if (e != -EINPROGRESS && e != -EBUSY) return e;
        pump(nullptr);
        if (bt.geni.state == QBT_GENI_STOPPED) return 0;
        if (nowNs(nullptr) >= end) return -ETIMEDOUT;
        uint64_t next = qbt_geni_deadline(&bt.geni);
        if (next > end) next = end;
        waitUntil(nullptr, next);
    }
}
int clockVote(void *, bool enabled) {
    if (enabled) {
        if (!bt.gated) return 0;
        if (!serialClock(true)) return -EIO;
        disarmRxWake();
        IODelay(10);
        bt.gated = false;
        return qbt_geni_resume(&bt.geni);
    }
    if (bt.gated) return 0;
    uint64_t sequence = bt.rx_sequence;
    int e = quiesce();
    if (e) return e;
    if (sequence != bt.rx_sequence || qbt_transport_tx_pending()) {
        qbt_geni_resume(&bt.geni);
        return -EBUSY;
    }
    volatile uint32_t *r = bt.tlmm.va + kRx * 4096 / 4;
    r[2] = 0;
    r[3] = 0;
    pin(kRx, 3u);
    r[2] = (3u << 5) | (2u << 2) | (1u << 4) | 1u;
    barrier();
    if (!(r[1] & 1) || (r[3] & 1)) {
        disarmRxWake();
        qbt_geni_resume(&bt.geni);
        return -EBUSY;
    }
    uwr(nullptr, 0x2ac, 0x80000001u);
    bt.gated = true;
    if (!serialClock(false)) {
        bt.gated = false;
        disarmRxWake();
        qbt_geni_resume(&bt.geni);
        return -EIO;
    }
    return 0;
}
uint64_t rcgRateHz(uint32_t cfg, uint32_t mreg, uint32_t nreg) {
    unsigned src = (cfg >> 8) & 7u, mode = (cfg >> 12) & 3u, hid = cfg & 0x1fu;
    uint64_t prate = src == 0 ? 19200000ULL : src == 1 ? 600000000ULL : src == 6 ? 300000000ULL : 0ULL;
    if (hid) { prate *= 2ULL; prate /= (hid + 1ULL); }
    if (mode) {
        uint32_t m = mreg & 0xffffu;
        uint32_t n = ((~nreg) & 0xffffu) + m;
        if (n) { prate *= m; prate /= n; }
    }
    return prate;
}
uint64_t dfsLevelHz(unsigned l) {
    return rcgRateHz(grd(nullptr, SM8250_GCC_QUPV3_WRAP0_S6_PERF_DFSR(l)),
                     grd(nullptr, SM8250_GCC_QUPV3_WRAP0_S6_PERF_M_DFSR(l)),
                     grd(nullptr, SM8250_GCC_QUPV3_WRAP0_S6_PERF_N_DFSR(l)));
}
bool baudSupported(uint32_t baud) {
    if ((grd(nullptr, SM8250_GCC_QUPV3_WRAP0_S6_DFS_SR) & 1u) == 0) return true;
    uint64_t req = (uint64_t)baud * 16ULL;
    for (unsigned l = 0; l < 8u; l++) {
        uint64_t lhz = dfsLevelHz(l);
        if (bt.clock_num && bt.clock_den) lhz = (lhz * bt.clock_num) / bt.clock_den;
        if (lhz >= req && (lhz % req) == 0 && (lhz / req) <= 0xfffu) return true;
    }
    return false;
}
int configureBaud(uint32_t baud) {
    uint32_t hz = 0;
    unsigned clkSel = 0;
    uint32_t dfsr = grd(nullptr, SM8250_GCC_QUPV3_WRAP0_S6_DFS_SR);
    if (dfsr & 1u) {
        uint64_t req = (uint64_t)baud * 16ULL;
        bool found = false;
        for (unsigned l = 0; l < 8u; l++) {
            uint64_t raw_hz = dfsLevelHz(l);
            uint64_t eff_lhz = (bt.clock_num && bt.clock_den) ? ((raw_hz * bt.clock_num) / bt.clock_den) : raw_hz;
            if (eff_lhz >= req && (eff_lhz % req) == 0 && (eff_lhz / req) <= 0xfffu) {
                hz = (uint32_t)raw_hz;
                clkSel = l;
                found = true;
                break;
            }
        }
        if (!found) return -EINVAL;
    } else {
        struct sm8250_geni_clock_io clocks = { nullptr, grd, gwr, delay };
        int e = sm8250_geni_uart6_source(&clocks, baud, &hz);
        if (e) return e;
    }
    uint32_t eff_hz = (bt.clock_num && bt.clock_den) ? (uint32_t)(((uint64_t)hz * bt.clock_num) / bt.clock_den) : hz;
    int e = qbt_geni_set_rate(&bt.geni, baud, eff_hz, 16u);
    if (e) return e;
    if (dfsr & 1u) uwr(nullptr, 0x7c, clkSel);
    bt.baud = baud;
    return 0;
}
int setRate(void *, uint32_t baud) {
    int e = quiesce();
    if (e) return e;
    e = configureBaud(baud);
    if (!e) e = qbt_geni_resume(&bt.geni);
    return e;
}
void *allocate(void *, size_t n) { return IOMallocData(n); }
void release(void *, void *p, size_t n) { IOFreeData(p, n); }
int firmware(void *, const char *name, const uint8_t **data, size_t *size) {
    uint8_t *bytes = nullptr;
    int e = sheng_bt_firmware_read(name, &bytes, size);
    *data = bytes;
    return -e;
}
void firmwarePut(void *, const uint8_t *data, size_t size) {
    sheng_bt_firmware_release(const_cast<uint8_t *>(data), size);
}
void log(void *, int level, const char *line) {
    if (level <= 3 || bt.log) printf("[bluetooth] %s\n", line);
    recordRow(4, "log(%d): %s", level, line ? line : "");
}
bool address() {
    if (bt.radio) return true;
    IORegistryEntry *chosen = IORegistryEntry::fromPath("/chosen", gIODTPlane);
    OSData *id = chosen ? OSDynamicCast(OSData, chosen->getProperty("unique-chip-id")) : nullptr;
    if (!id || id->getLength() < 8) {
        if (chosen) chosen->release();
        return false;
    }
    uint64_t hash = 0xcbf29ce484222325ULL;
    static const char domain[] = "unixnu-bluetooth";
    for (unsigned i = 0; i < sizeof(domain) - 1; i++) hash = (hash ^ (uint8_t)domain[i]) * 0x100000001b3ULL;
    const uint8_t *seed = static_cast<const uint8_t *>(id->getBytesNoCopy());
    bool nonzero = false;
    for (unsigned i = 0; i < id->getLength(); i++) {
        nonzero |= seed[i] != 0;
        hash = (hash ^ seed[i]) * 0x100000001b3ULL;
    }
    bool published = false;
    if (nonzero) {
        for (unsigned i = 0; i < 6; i++) bt.address[i] = (uint8_t)(hash >> (i * 8));
        bt.address[0] = (bt.address[0] & 0xfe) | 2;
        IORegistryEntry *node = IORegistryEntry::fromPath("/arm-io/bluetooth", gIODTPlane);
        OSData *encoding = node ? OSDynamicCast(OSData, node->getProperty("transport-encoding")) : nullptr;
        OSData *speed = node ? OSDynamicCast(OSData, node->getProperty("transport-speed")) : nullptr;
        uint32_t wire = 1, baud = 3200000;
        if (encoding && encoding->getLength() == 4) memcpy(&wire, encoding->getBytesNoCopy(), 4);
        if (speed && speed->getLength() == 4) memcpy(&baud, speed->getBytesNoCopy(), 4);
        published = chosen->setProperty("mac-address-bluetooth0", bt.address, 6);
        if (node) (void)node->setProperty("local-mac-address", bt.address, 6);
        if (published && !bt.radio) {
            IOService *radio = new IOService;
            if (radio && radio->init()) {
                radio->setName("bluetooth");
                (void)radio->setProperty("device_type", "bluetooth");
                if (encoding) (void)radio->setProperty("transport-encoding", encoding);
                else (void)radio->setProperty("transport-encoding", &wire, 4);
                if (speed) (void)radio->setProperty("transport-speed", speed);
                else (void)radio->setProperty("transport-speed", &baud, 4);
                (void)radio->setProperty("local-mac-address", bt.address, 6);
                IOService *provider = IOService::getPlatform();
                if (provider && radio->attach(provider)) {
                    bt.radio = radio;
                    radio->registerService();
                } else {
                    radio->release();
                }
            } else if (radio) {
                radio->release();
            }
        }
        if (node) node->release();
    }
    chosen->release();
    return published;
}
int resources() {
    if (bt.resources) return 0;
    if (!map(bt.gcc, SM8250_GCC_BASE, SM8250_GCC_SIZE) ||
        !map(bt.uart, SM8250_UART6_BASE, SM8250_UART6_SIZE) ||
        !map(bt.wrapper, SM8250_QUP0_BASE, SM8250_QUP0_SIZE) ||
        !map(bt.tlmm, 0x0f900000ULL, 0x100000) ||
        !map(bt.smmu, 0x15000000ULL, 0xd0000) ||
        !map(bt.gic, 0x17a00000ULL, 0x1000))
        return -ENOMEM;
    if (!address()) return -ENOENT;
    const IOOptionBits options = static_cast<IOOptionBits>(kIODirectionInOut) |
                                 static_cast<IOOptionBits>(kIOMemoryPhysicallyContiguous) |
                                 static_cast<IOOptionBits>(kIOMemoryMapperNone);
    if (!bt.dma) bt.dma = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, options, kDmaBytes, 4096);
    if (!bt.dma) return -ENOMEM;
    IOByteCount contiguous = 0;
    uint64_t pa = bt.dma->getPhysicalSegment(0, &contiguous, kIOMemoryMapperNone);
    if (!pa || contiguous < kDmaBytes) return -EIO;
    AppsSmmuLock guard;
    if (!guard.ready()) return -ENOMEM;
    struct smmu_io io = { nullptr, srd, swr };
    bt.domain = smmu_plan_bypass(&io, SM8250_QUP0_SID, pa, kDmaBytes);
    const char *error = bt.domain.error ? bt.domain.error : smmu_configure_bypass(&io, &bt.domain, SM8250_QUP0_SID);
    if (error) {
        printf("[bluetooth dma] slot=%u bank=%u result=%s\n", bt.domain.slot, bt.domain.bank, error);
        recordRow(1, "smmu err=%s slot=%u bank=%u", error, bt.domain.slot, bt.domain.bank);
        return -EIO;
    }
    bt.dma_address = pa;
    printf("[bluetooth dma] bypass slot=%u bank=%u pa=%llx bytes=%u\n", bt.domain.slot, bt.domain.bank, pa, kDmaBytes);
    recordRow(1, "smmu ok slot=%u bank=%u pa=0x%llx", bt.domain.slot, bt.domain.bank, (unsigned long long)pa);
    bt.resources = true;
    return 0;
}
int stopEngine() {
    irqMask(nullptr, true);
    uwr(nullptr, 0xe1c, 0);
    if ((urd(nullptr, 0x40) & 0x1001) || (urd(nullptr, 0xe40) & 3)) {
        uwr(nullptr, 0x604, 2);
        uwr(nullptr, 0x634, 2);
        unsigned i = 0;
        for (; i < 1000 && (urd(nullptr, 0x40) & 0x1001); i++) IODelay(1);
        if (i == 1000) return -ETIMEDOUT;
        uwr(nullptr, 0xc58, 1);
        uwr(nullptr, 0xd58, 1);
        for (i = 0; i < 1000 && (urd(nullptr, 0xe40) & 3); i++) IODelay(1);
        if (i == 1000) return -ETIMEDOUT;
    }
    uwr(nullptr, 0x618, UINT32_MAX);
    uwr(nullptr, 0x648, UINT32_MAX);
    uwr(nullptr, 0xc44, UINT32_MAX);
    uwr(nullptr, 0xd44, UINT32_MAX);
    bt.geni.state = QBT_GENI_OFF;
    bt.geni.rx_busy = bt.geni.tx_busy = false;
    return 0;
}
uint32_t linePins() {
    static const uint8_t pins[] = { kUartCts, kUartRfr, kUartTx, kRx, kBtEnable, kSwCtrl };
    uint32_t bits = 0;
    for (unsigned i = 0; i < sizeof(pins); i++)
        bits |= (bt.tlmm.va[(pins[i] * 4096u + 4) / 4] & 1u) << i;
    return bits;
}
int prepareEngine() {
    uint8_t *bytes = static_cast<uint8_t *>(bt.dma->getBytesNoCopy());
    struct qbt_dma_buffer tx = { bytes, bt.dma_address, QBT_GENI_TX_SIZE }, rx[QBT_GENI_RX_COUNT];
    for (unsigned i = 0; i < QBT_GENI_RX_COUNT; i++)
        rx[i] = { bytes + 4096 + i * QBT_GENI_RX_SIZE, bt.dma_address + 4096 + i * QBT_GENI_RX_SIZE, QBT_GENI_RX_SIZE };
    struct qbt_geni_ops ops = { nullptr, urd, uwr, dmaBarrier, dmaSync, nowNs, irqMask, wake, rxBytes };
    int e = qbt_geni_prepare(&bt.geni, &ops, &tx, rx);
    if (e) return e;
    native_bluetooth_stats[14] = 5;
    return configureBaud(115200);
}
int loopbackCheck() {
    static const uint8_t pattern[64] = {
        0x01, 0x55, 0xaa, 0x00, 0xff, 0x04, 0x0e, 0x7e, 0x81, 0x42, 0x24, 0x18, 0x10, 0x20, 0x40, 0x80,
        0x01, 0x55, 0xaa, 0x00, 0xff, 0x04, 0x0e, 0x7e, 0x81, 0x42, 0x24, 0x18, 0x10, 0x20, 0x40, 0x80,
        0x01, 0x55, 0xaa, 0x00, 0xff, 0x04, 0x0e, 0x7e, 0x81, 0x42, 0x24, 0x18, 0x10, 0x20, 0x40, 0x80,
        0x01, 0x55, 0xaa, 0x00, 0xff, 0x04, 0x0e, 0x7e, 0x81, 0x42, 0x24, 0x18, 0x10, 0x20, 0x40, 0x80
    };
    uwr(nullptr, 0x22c, 3);
    bt.loopback = true;
    bt.loop_length = 0;
    int e = qbt_geni_start(&bt.geni);
    const uint64_t t0 = nowNs(nullptr);
    size_t sent = e ? 0 : qbt_geni_write(&bt.geni, pattern, sizeof(pattern));
    uint64_t end = t0 + 50000000ULL;
    while (sent && (bt.loop_length < sizeof(pattern) || !qbt_geni_drained(&bt.geni)) && bt.geni.state == QBT_GENI_RUN && nowNs(nullptr) < end) {
        if (!qbt_geni_service(&bt.geni)) IODelay(50);
    }
    bt.loop_us = (unsigned)((nowNs(nullptr) - t0) / 1000ULL);
    uint64_t first = 0;
    memcpy(&first, bt.loop_rx, bt.loop_length < 8 ? bt.loop_length : 8);
    native_bluetooth_stats[31] = first;
    native_bluetooth_stats[30] = bt.loop_length | (sent ? 1u << 16 : 0) |
        (bt.loop_length == sizeof(pattern) && !memcmp(bt.loop_rx, pattern, sizeof(pattern)) ? 1u << 17 : 0);
    e = bt.geni.state == QBT_GENI_RUN ? quiesce() : -EIO;
    bt.loopback = false;
    uwr(nullptr, 0x22c, 0);
    (void)e;
    e = stopEngine();
    if (e) return e;
    return prepareEngine();
}
int powerOn() {
    if (bt.powered) return 0;
    native_bluetooth_stats[14] = 1;
    native_bt_step = 2;
    int e = resources();
    if (e) { recordRow(0, "resources failed: %d", e); return e; }
    native_bluetooth_stats[14] = 2;
    e = sheng_wcn_power_get(SHENG_WCN_BLUETOOTH);
    if (e) { recordRow(0, "wcn_power failed: %d", e); return -e; }
    bt.powered = true;
    bt.gated = true;
    native_bluetooth_stats[14] = 3;
    native_bt_step = 3;
    /* Match j716f.dtb qupv3_se6_default_{cts,rtsrx,tx}: CTS no-pull, RFR+RX pull-down (QCA6390 bootstrap!), TX pull-up. */
    pin(kUartCts, 0u);
    pin(kUartRfr, 1u);
    pin(kUartTx, 3u);
    pin(kRx, 1u);
    pin(kSwCtrl, 1u);
    pin(kBtEnable, kBtEnableCfg, 0);
    IOSleep(100);
    const uint32_t pinsPre = linePins();
    if (!sm8250_gcc_update32(bt.gcc.va + SM8250_GCC_APCS_GPLL_ENA_VOTE / 4, 0, 1u)) return -EIO;
    for (unsigned i = 0; i < 1000 && !(grd(nullptr, 0) & (1u << 31)); i++) IODelay(1);
    if (!(grd(nullptr, 0) & (1u << 31)) || !serialClock(true)) return -EIO;
    bt.gated = false;
    native_bluetooth_stats[11] = urd(nullptr, 0x68);
    native_bluetooth_stats[12] = bt.wrapper.va[1];
    native_bluetooth_stats[13] = grd(nullptr, SM8250_GCC_APCS_CLOCK_BRANCH_ENA_VOTE);
    native_bluetooth_stats[14] = 4;
    native_bt_step = 4;
    if (((urd(nullptr, 0x68) >> 8) & 255) != 2) return -ENXIO;
    bt.clock_num = 1;
    bt.clock_den = 1;
    e = stopEngine();
    if (e) return e;
    e = prepareEngine();
    if (e) return e;
    if (!bt.serial_irq && armcpu_spi_attach) {
        e = armcpu_spi_attach(SM8250_UART6_INTID, false, serialIrq, nullptr, nullptr, nullptr);
        if (e) return -EIO;
        bt.serial_irq = true;
        irqMask(nullptr, true);
    }
    if (!bt.wake_irq) {
        auto attachWake = armcpu_spi_share ? armcpu_spi_share : armcpu_spi_attach;
        if (attachWake && attachWake(240, false, rxIrq, nullptr, nullptr, nullptr) == 0)
            bt.wake_irq = true;
    }
    e = loopbackCheck();
    if (e) return e;
    if (bt.loop_length == 64 && bt.loop_us > 1500 && bt.loop_us < 4200) {
        bt.clock_num = 2;
        bt.clock_den = 1;
        e = stopEngine();
        if (e) return e;
        e = prepareEngine();
        if (e) return e;
        e = loopbackCheck();
        if (e) return e;
    } else if (bt.loop_length == 64 && bt.loop_us > 8000 && bt.loop_us < 16000) {
        bt.clock_num = 1;
        bt.clock_den = 2;
        e = stopEngine();
        if (e) return e;
        e = prepareEngine();
        if (e) return e;
        e = loopbackCheck();
        if (e) return e;
    }
    pin(kBtEnable, kBtEnableCfg, 1);
    IOSleep(100);
    /* Match j716f.dtb qupv3_se6_{ctsrx,rts,tx}: function=qup6(1), CTS+RX no-pull, RFR pull-down, TX pull-up. */
    pin(kUartCts, (1u << 2) | 0u);
    pin(kUartRfr, (1u << 2) | 1u);
    pin(kUartTx, (1u << 2) | 3u);
    pin(kRx, (1u << 2) | 0u);
    const uint32_t pinsPost = linePins();
    recordRow(2, "cfg=0x%x dfsr=0x%x sel=%u mclk=0x%x bten=0x%x/0x%x pins=0x%x->0x%x loop=%u/%uus clk=%u/%u",
              grd(nullptr, SM8250_GCC_QUPV3_WRAP0_S6_CFG_RCGR),
              grd(nullptr, SM8250_GCC_QUPV3_WRAP0_S6_DFS_SR), urd(nullptr, 0x7c),
              urd(nullptr, 0x48), bt.tlmm.va[kBtEnable * 4096 / 4], bt.tlmm.va[(kBtEnable * 4096 + 4) / 4],
              pinsPre, pinsPost, bt.loop_length, bt.loop_us, bt.clock_num, bt.clock_den);
    recordRow(5, "dfs=%u,%u,%u,%u,%u,%u,%u,%u",
              (unsigned)(dfsLevelHz(0) / 1000ULL), (unsigned)(dfsLevelHz(1) / 1000ULL),
              (unsigned)(dfsLevelHz(2) / 1000ULL), (unsigned)(dfsLevelHz(3) / 1000ULL),
              (unsigned)(dfsLevelHz(4) / 1000ULL), (unsigned)(dfsLevelHz(5) / 1000ULL),
              (unsigned)(dfsLevelHz(6) / 1000ULL), (unsigned)(dfsLevelHz(7) / 1000ULL));
    disarmRxWake();
    e = qbt_geni_start(&bt.geni);
    if (e) return e;
    native_bluetooth_stats[14] = 6;
    native_bt_step = 6;
    for (unsigned i = 0; i < 20; i++) { pump(nullptr); IOSleep(1); }
    bt.geni.stats.line_errors = 0;
    __atomic_store_n(&native_bluetooth_boot_rx_length, 0u, __ATOMIC_RELEASE);
    bt.initializing = true;
    if (bt.log >= 2) {
        native_bluetooth_boot_state[0] = linePins();
        native_bluetooth_boot_state[1] = __atomic_load_n(&native_bluetooth_boot_rx_length, __ATOMIC_ACQUIRE);
    }
    uint32_t targetBaud = 115200u;
    if (baudSupported(3200000u)) targetBaud = 3200000u;
    else if (baudSupported(3000000u)) targetBaud = 3000000u;
    else if (baudSupported(4000000u)) targetBaud = 4000000u;
    e = qbt_boot(bt.core, targetBaud, bt.address);
    bt.initializing = false;
    native_bt_step = e ? 99 : 8;
    {
        struct qbt_stats stats;
        qbt_get_stats(bt.core, &stats);
        uint32_t rxlen = __atomic_load_n(&native_bluetooth_boot_rx_length, __ATOMIC_ACQUIRE);
        const uint8_t *r = native_bluetooth_boot_rx;
        recordRow(3, "rxlen=%u [%02x %02x %02x %02x %02x %02x %02x %02x] mal=%u unm=%u si=0x%x ri=0x%x",
                  rxlen, r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7],
                  stats.malformed, stats.unmatched_events, bt.geni.fault_irq[1], bt.geni.fault_irq[3]);
    }
    if (bt.log >= 2) {
        static const uint32_t regs[] = { 0x40, 0x610, 0x640, 0xc40, 0xd40, 0x800, 0xd54 };
        native_bluetooth_boot_state[2] = linePins();
        for (unsigned i = 0; i < 7; i++) native_bluetooth_boot_state[3 + i] = bt.clocked ? urd(nullptr, regs[i]) : 0;
        struct qbt_stats stats;
        qbt_get_stats(bt.core, &stats);
        native_bluetooth_boot_state[10] = (uint32_t)stats.tlv_acks | ((uint32_t)stats.tlv_sent << 16);
        native_bluetooth_boot_state[11] = (uint32_t)stats.hardware_errors | ((uint32_t)stats.last_hardware_error << 8) | ((uint32_t)stats.tlv_final_status << 16);
    }
    return e;
}
int powerOff() {
    if (!bt.powered) return 0;
    qbt_set_service(bt.core, false);
    bt.service = false;
    int e = 0;
    if (bt.geni.state != QBT_GENI_OFF) {
        if (bt.gated) { int resumed = clockVote(nullptr, true); if (!e) e = resumed; }
        if (bt.clocked) { int shutdown = qbt_shutdown(bt.core); if (!e) e = shutdown; }
    }
    if (bt.clocked && bt.geni.state != QBT_GENI_OFF) { int stopped = quiesce(); if (!e) e = stopped; }
    irqMask(nullptr, true);
    disarmRxWake();
    pin(kBtEnable, kBtEnableCfg, 0);
    IOSleep(100);
    int retired = bt.clocked ? stopEngine() : 0;
    if (!e) e = retired;
    if (!retired) bt.gated = serialClock(false);
    bt.powered = false;
    __atomic_store_n(&bt.rx_wake, 0u, __ATOMIC_RELEASE);
    int released = sheng_wcn_power_put(SHENG_WCN_BLUETOOTH);
    if (!e) e = -released;
    return e;
}
int requestPower(void *, bool enabled) {
    uint32_t expected = 0;
    if (!__atomic_compare_exchange_n(&bt.power_request, &expected, enabled ? 1u : 2u, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return EBUSY;
    wake(nullptr);
    return 0;
}
void powerResult(bool enabled, int error) {
    struct qbt_info info;
    qbt_get_info(bt.core, &info);
    native_bluetooth_stats[17] = info.product;
    native_bluetooth_stats[18] = info.soc;
    native_bluetooth_stats[19] = ((uint32_t)info.rom << 16) | info.patch;
    if (enabled && !error) native_bluetooth_stats[14] = 7;
    qbt_transport_power_result(enabled, error < 0 ? -error : error, info.firmware, info.baud);
    native_bluetooth_stats[20] = (unsigned)bt.powered | ((unsigned)bt.gated << 1) | ((unsigned)bt.clocked << 2);
    native_bluetooth_stats[21] = sheng_wcn_power_owners();
    native_bluetooth_stats[0] = error ? 3 : enabled ? 2 : 0;
    native_bluetooth_stats[1] = (uint64_t)(int64_t)error;
    printf("[bluetooth] power=%u result=%d baud=%u fw=%s\n", enabled, error, info.baud, info.firmware);
    recordRow(0, "power=%u result=%d baud=%u soc=0x%x fw=%s", enabled, error, info.baud, info.soc, info.firmware);
    if (error && bt.geni.error) {
        printf("[bluetooth uart] error=%d irq=%x/%x/%x/%x\n",
               bt.geni.error, bt.geni.fault_irq[0], bt.geni.fault_irq[1], bt.geni.fault_irq[2], bt.geni.fault_irq[3]);
        recordRow(3, "uart err=%d irq=%x/%x/%x/%x",
                  bt.geni.error, bt.geni.fault_irq[0], bt.geni.fault_irq[1], bt.geni.fault_irq[2], bt.geni.fault_irq[3]);
    }
}
void worker(void *, wait_result_t) {
    thread_set_thread_name(current_thread(), "sheng-bluetooth");
    struct qbt_platform ops = {
        nullptr, allocate, release, nowNs, writeUart, drained, setRate, flow,
        clockVote, pump, waitUntil, nativeRx, firmware, firmwarePut, log
    };
    bt.core = qbt_create(&ops);
    if (!bt.core) { native_bluetooth_stats[0] = 3; return; }
    struct qbt_transport_ops transport = { nullptr, wake, requestPower };
    int e = qbt_transport_init(&transport);
    if (e) { native_bluetooth_stats[0] = 3; return; }
    while (native_gpu_addon_stage < 1)
        waitUntil(nullptr, nowNs(nullptr) + 10000000ULL);
    /* Let Wi-Fi settle WLAN_EN, shared RPMh rails, and MHI SOC_GLOBAL_RESET / AMSS boot first when sheng-wifi >= 2 */
    unsigned wifi = 0;
    PE_parse_boot_argn("sheng-wifi", &wifi, sizeof(wifi));
    if (wifi >= 2) {
        uint64_t end = nowNs(nullptr) + 25000000000ULL;
        while (nowNs(nullptr) < end) {
            int32_t stage = (int32_t)native_wifi_stats[0];
            int32_t mhi = (int32_t)native_wifi_mhi[0];
            if (stage < 0) break;
            if (wifi < 3 && stage >= 16) break;
            if (wifi >= 3 && (mhi < 0 || mhi >= 7)) break;
            waitUntil(nullptr, nowNs(nullptr) + 50000000ULL);
        }
        IOSleep(200);
    } else {
        IOSleep(500);
    }
    e = powerOn();
    if (e) (void)powerOff();
    powerResult(true, e);
    uint64_t health = nowNs(nullptr) + 1000000000ULL, closeAt = 0;
    for (;;) {
        uint32_t command = __atomic_exchange_n(&bt.power_request, 0u, __ATOMIC_ACQ_REL);
        if (command) {
            e = command == 1 ? powerOn() : powerOff();
            if (e && command == 1) (void)powerOff();
            powerResult(command == 1, e);
        }
        if (bt.powered) {
            pump(nullptr);
            bool active = qbt_transport_active();
            if (active != bt.service) {
                qbt_set_service(bt.core, active);
                bt.service = active;
                closeAt = active ? 0 : nowNs(nullptr) + 500000000ULL;
            }
            uint8_t tx[2048];
            size_t room = qbt_transmit_capacity(bt.core);
            if (room > sizeof(tx)) room = sizeof(tx);
            if (room) {
                uint32_t n = qbt_transport_tx_take(tx, (uint32_t)room);
                if (n) {
                    /* Intercept HCI_LE_Read_Periodic_Advertiser_List_Size (0x204a) so QCA6390 does not
                     * latch into Bluetooth 5.0 Extended Advertising/Scanning-only mode (Vol 4 Part E 3.1.1)
                     * and reject bluetoothd's legacy advertising/scanning commands (0x2007/0x200b) with 0x0c. */
                    uint8_t filtered[2048];
                    uint32_t f_len = 0, off = 0;
                    while (off < n) {
                        if (tx[off] == 0x01 && off + 4 <= n) {
                            uint16_t op = (uint16_t)tx[off + 1] | ((uint16_t)tx[off + 2] << 8);
                            uint32_t pkt_len = 4u + tx[off + 3];
                            if (off + pkt_len > n) pkt_len = n - off;
                            if (op == 0x204a) {
                                static const uint8_t cc_204a[] = { 0x04, 0x0e, 0x05, 0x01, 0x4a, 0x20, 0x00, 0x00 };
                                (void)qbt_transport_receive(cc_204a, sizeof(cc_204a));
                            } else {
                                memcpy(filtered + f_len, tx + off, pkt_len);
                                f_len += pkt_len;
                            }
                            off += pkt_len;
                        } else {
                            memcpy(filtered + f_len, tx + off, n - off);
                            f_len += n - off;
                            break;
                        }
                    }
                    if (f_len && qbt_transmit(bt.core, filtered, f_len)) e = -EIO;
                }
            }
            qbt_step(bt.core);
            struct qbt_info info;
            qbt_get_info(bt.core, &info);
            if (info.state == 3 || bt.geni.state == QBT_GENI_FAILED || (closeAt && nowNs(nullptr) >= closeAt)) {
                int error = info.state == 3 ? info.error : bt.geni.error;
                (void)powerOff();
                powerResult(false, error);
                closeAt = 0;
            }
        }
        uint64_t t = nowNs(nullptr);
        if (t >= health) {
            struct qbt_stats stats;
            qbt_get_stats(bt.core, &stats);
            native_bluetooth_stats[2] = bt.baud;
            native_bluetooth_stats[3] = stats.tx_bytes;
            native_bluetooth_stats[4] = stats.rx_bytes;
            native_bluetooth_stats[5] = bt.geni.stats.interrupts;
            native_bluetooth_stats[6] = stats.timeouts;
            native_bluetooth_stats[7] = stats.clock_sleeps;
            native_bluetooth_stats[8] = bt.domain.slot;
            native_bluetooth_stats[9] = bt.domain.bank;
            native_bluetooth_stats[20] = (unsigned)bt.powered | ((unsigned)bt.gated << 1) | ((unsigned)bt.clocked << 2);
            native_bluetooth_stats[21] = sheng_wcn_power_owners();
            for (unsigned i = 0; i < 4; i++) native_bluetooth_stats[22 + i] = bt.geni.fault_irq[i];
            native_bluetooth_stats[26] = stats.malformed;
            native_bluetooth_stats[27] = stats.unmatched_events;
            native_bluetooth_stats[28] = bt.geni.stats.rx_dma;
            native_bluetooth_stats[29] = bt.geni.stats.tx_dma;
            native_bluetooth_stats[15] = bt.geni.stats.line_errors;
            native_bluetooth_stats[16] = bt.geni.stats.dma_errors;
            health = t + 1000000000ULL;
        }
        uint64_t deadline = bt.powered ? qbt_deadline(bt.core) : UINT64_MAX;
        uint64_t geni = bt.powered && !bt.gated ? qbt_geni_deadline(&bt.geni) : UINT64_MAX;
        if (geni < deadline) deadline = geni;
        if (health < deadline) deadline = health;
        if (closeAt && closeAt < deadline) deadline = closeAt;
        if (!deadline) { thread_yield_to_preemption(); continue; }
        waitUntil(nullptr, deadline);
    }
}
} // namespace

struct proc;
extern "C" {
void proc_iterate(unsigned, int (*)(struct proc *, void *), void *, int (*)(struct proc *, void *), void *);
int proc_pid(struct proc *);
}
static int processSeen(struct proc *process, void *found) {
    static const char *const names[] = { "bluetoothd", "BlueTool" };
    const char *name = proc_best_name(process);
    for (unsigned i = 0; name && i < 2; i++)
        if (!strcmp(name, names[i]))
            static_cast<int *>(found)[i] = proc_pid(process);
    return 0;
}
static void processWatch(void *, wait_result_t) {
    thread_set_thread_name(current_thread(), "sheng-bluetooth-procs");
    for (;;) {
        int found[2] = { 0, 0 };
        proc_iterate(1u | 4u, processSeen, found, nullptr, nullptr);
        for (unsigned i = 0; i < 2; i++) {
            if (found[i] && (uint32_t)found[i] != native_bluetooth_processes[i * 2])
                native_bluetooth_processes[i * 2 + 1] = native_bluetooth_processes[i * 2 + 1] + 1;
            native_bluetooth_processes[i * 2] = (uint32_t)found[i];
        }
        IOSleep(2000);
    }
}

extern "C" const char *native_bt_stats(void) {
    struct qbt_info info = {};
    struct qbt_stats st = {};
    if (bt.core) {
        qbt_get_info(bt.core, &info);
        qbt_get_stats(bt.core, &st);
    }
    snprintf(bt.stats_str, sizeof(bt.stats_str),
             "bt: state=%u err=%d baud=%u soc=0x%x rom=0x%x patch=0x%x fw=%s tx=%llu rx=%llu cmd=%llu to=%llu tlv=%llu/%llu mnt=%d/%d act=%u/%u/%u/%u proc=%u/%u/%u/%u exit=%llx/%llx",
             info.state, info.error, info.baud, info.soc, info.rom, info.patch, info.firmware,
             (unsigned long long)st.tx_bytes, (unsigned long long)st.rx_bytes,
             (unsigned long long)st.commands, (unsigned long long)st.timeouts,
             (unsigned long long)st.tlv_acks, (unsigned long long)st.tlv_sent,
             native_bt_addon_stage, native_bt_addon_status,
             qbt_transport_activity[0], qbt_transport_activity[1],
             qbt_transport_activity[2], qbt_transport_activity[3],
             native_bluetooth_processes[0], native_bluetooth_processes[1],
             native_bluetooth_processes[2], native_bluetooth_processes[3],
             (unsigned long long)native_bt_last_exit[0], (unsigned long long)native_bt_last_exit[1]);
    return bt.stats_str;
}

extern "C" const char *native_bt_uart(void) {
    snprintf(bt.uart_str, sizeof(bt.uart_str),
             "bt.uart: geni_state=%u baud=%u hz=%u irqs=%llu rx_dma=%llu tx_dma=%llu dma_err=%llu line_err=%llu pins=0x%x",
             (unsigned)bt.geni.state, bt.geni.baud, bt.geni.source_hz,
             (unsigned long long)bt.geni.stats.interrupts,
             (unsigned long long)bt.geni.stats.rx_dma,
             (unsigned long long)bt.geni.stats.tx_dma,
             (unsigned long long)bt.geni.stats.dma_errors,
             (unsigned long long)bt.geni.stats.line_errors,
             bt.tlmm.va ? linePins() : 0u);
    return bt.uart_str;
}

extern "C" void sheng_bluetooth_probe(void) {
    unsigned enabled = 0;
    if (!PE_parse_boot_argn("sheng-bluetooth", &enabled, sizeof(enabled)) || !enabled) return;
    uint32_t expected = 0;
    if (!__atomic_compare_exchange_n(&bt.started, &expected, 1u, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return;
    native_bluetooth_stats[0] = 1;
    native_bt_step = 1;
    if (!address()) {
        native_bluetooth_stats[0] = 3;
        native_bluetooth_stats[1] = (uint64_t)(int64_t)-ENOENT;
        return;
    }
    PE_parse_boot_argn("log-bluetooth", &bt.log, sizeof(bt.log));
    if (bt.log >= 2) native_bluetooth_boot_text = static_cast<char *>(IOMallocData(8192));
    thread_t thread = nullptr;
    if (kernel_thread_start(worker, nullptr, &thread) == KERN_SUCCESS) thread_deallocate(thread);
    else native_bluetooth_stats[0] = 3;
    if (kernel_thread_start(processWatch, nullptr, &thread) == KERN_SUCCESS) thread_deallocate(thread);
}
