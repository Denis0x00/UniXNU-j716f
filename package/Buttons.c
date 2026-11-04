// SPDX-License-Identifier: BSD-2-Clause-Patent
// TB-J716F power and volume keys (PM8150 PON + GPIO6 via SPMI observer), `pipa-buttons=1`.
// Also provides a non-blocking short-press `VolUp + VolDown` combo backup that injects a 300ms
// bottom-edge swipe-up unlock gesture without interfering with BootRamoops's 3-second hold-to-fastboot.
#include <stdint.h>
#include "ButtonsCore.h"

extern uint64_t ml_io_map(uint64_t phys_addr, uint64_t size);
extern int PE_parse_boot_argn(const char *arg_string, void *arg_ptr, int max_arg);
extern int kernel_thread_start(void (*continuation)(void *, int), void *parameter, void **new_thread);
extern void thread_deallocate(void *thread);
extern void IOSleep(unsigned milliseconds);
extern int snprintf(char *, unsigned long, const char *, ...);

// Shared touch state (TouchState.cpp & TouchProbe.cpp)
extern volatile uint32_t native_touch_seq, native_touch_count;
extern volatile int native_touch_xs[12], native_touch_ys[12], native_touch_ids[12];
extern void native_touch_published(void);
extern volatile uint32_t native_touch_injecting __attribute__((weak_import));

#define SPMI_CORE 0x0C440000ull
#define SPMI_OBSRVR 0x0E600000ull
#define SPMI_APID_MAP 0x900u
#define SPMI_MAX_APIDS 512u
#define SPMI_OBS_APID_STRIDE 0x80ull
#define SPMI_CMD 0x00u
#define SPMI_STATUS 0x08u
#define SPMI_RDATA0 0x18u
#define SPMI_POLLS 100000u
#define POLL_MS 10u
#define BTN_COMBO_MASK (BTN_VOLUME_UP | BTN_VOLUME_DOWN)

char native_buttons_row[120];
volatile uint32_t native_buttons_changes, native_buttons_mask;
volatile int native_buttons_error;
static volatile uint32_t swipe_busy;

static void (*volatile sink)(void *, uint32_t);
static void *volatile sink_context;

void native_buttons_set_sink(void (*callback)(void *, uint32_t), void *context) {
    __atomic_store_n(&sink, (void (*)(void *, uint32_t))0, __ATOMIC_RELEASE);
    __atomic_store_n(&sink_context, context, __ATOMIC_RELEASE);
    __atomic_store_n(&sink, callback, __ATOMIC_RELEASE);
}

static void publish_single_touch(unsigned count, int id, int x, int y) {
    native_touch_seq = native_touch_seq + 1u;
    __asm__ volatile("dmb ish" ::: "memory");
    native_touch_count = count;
    if (count > 0) {
        native_touch_ids[0] = id;
        native_touch_xs[0] = x;
        native_touch_ys[0] = y;
    }
    __asm__ volatile("dmb ish" ::: "memory");
    native_touch_seq = native_touch_seq + 1u;
    native_touch_published();
}

static void combo_swipe_thread(void *parameter, int wait_result) {
    (void)parameter; (void)wait_result;
    if (&native_touch_injecting) __atomic_store_n(&native_touch_injecting, 1u, __ATOMIC_RELEASE);
    // 300ms bottom-edge swipe-up: (15240, 20200) -> (15240, 9000) in 0..30480 x 0..20320 space
    for (int step = 0; step <= 30; ++step) {
        int y = 20200 - (11200 * step) / 30;
        publish_single_touch(1u, 9, 15240, y);
        IOSleep(10);
    }
    publish_single_touch(0u, 9, 15240, 9000);
    if (&native_touch_injecting) __atomic_store_n(&native_touch_injecting, 0u, __ATOMIC_RELEASE);
    snprintf(native_buttons_row, sizeof native_buttons_row, "combo unlock swipe (15240,20200)->(15240,9000) done");
    native_buttons_changes = native_buttons_changes + 1u;
    __atomic_store_n(&swipe_busy, 0u, __ATOMIC_RELEASE);
}

static volatile uint32_t *observer_for(volatile uint32_t *core, uint32_t apids, uint32_t ppid) {
    for (uint32_t i = 0; i < apids; ++i) {
        uint32_t map = core[(SPMI_APID_MAP + 4 * i) / 4];
        if (!map || ((map >> 8) & 0xFFF) != ppid) continue;
        uint64_t phys = SPMI_OBSRVR + SPMI_OBS_APID_STRIDE * i;
        uint64_t page = ml_io_map(phys & ~0xFFFull, 0x1000);
        return page ? (volatile uint32_t *)(uintptr_t)(page + (phys & 0xFFF)) : 0;
    }
    return 0;
}

static int read8(volatile uint32_t *observer, uint8_t address, uint8_t *value) {
    observer[SPMI_CMD / 4] = (1u << 27) | ((uint32_t)address << 4);
    for (uint32_t poll = 0; poll < SPMI_POLLS; ++poll) {
        uint32_t status = observer[SPMI_STATUS / 4];
        if (status & 1) {
            if (status & 0xE) return 2;
            *value = (uint8_t)observer[SPMI_RDATA0 / 4];
            return 0;
        }
    }
    return 1;
}

static void buttons_thread(void *parameter, int wait_result) {
    (void)parameter; (void)wait_result;
    volatile uint32_t *core = (volatile uint32_t *)(uintptr_t)ml_io_map(SPMI_CORE, 0x2000);
    if (!core) {native_buttons_error = -1; return;}
    uint32_t version = core[0], apids = core[1] & 0x7FF;
    if (version < 0x50000000u || version >= 0x70000000u || !apids || apids > SPMI_MAX_APIDS) {native_buttons_error = -2; return;}
    volatile uint32_t *pon = observer_for(core, apids, BTN_PON_PPID), *gpio = observer_for(core, apids, BTN_GPIO6_PPID);
    if (!pon || !gpio) {native_buttons_error = pon ? -4 : -3; return;}
    uint8_t type = 0, subtype = 0, mode = 0, pull = 0, enable = 0, pon_sts = 0, gpio_sts = 0;
    int rc = read8(gpio, BTN_GPIO_TYPE, &type) | read8(gpio, BTN_GPIO_SUBTYPE, &subtype) | read8(gpio, BTN_GPIO_MODE_CTL, &mode) |
             read8(gpio, BTN_GPIO_DIG_PULL, &pull) | read8(gpio, BTN_GPIO_EN_CTL, &enable) | read8(pon, BTN_RT_STS, &pon_sts) |
             read8(gpio, BTN_RT_STS, &gpio_sts);
    snprintf(native_buttons_row, sizeof native_buttons_row,
             "start rc%d gpio6 type%02x/%02x mode%02x pull%02x en%02x sts%02x pon sts%02x", rc, type, subtype, mode, pull, enable,
             gpio_sts, pon_sts);
    native_buttons_changes = 1;
    if (rc) {native_buttons_error = -5; return;}
    struct buttons_debounce d = {0, 0, 0};
    uint32_t errors = 0;
    unsigned powerAt = 0;
    PE_parse_boot_argn("pipa-power-at", &powerAt, sizeof powerAt);
    uint32_t ticks = 0;
    unsigned powerSent = 0;
    uint32_t comboTicks = 0;
    for (;;) {
        IOSleep(POLL_MS);
        if (powerAt && !powerSent && ++ticks >= powerAt * (1000u / POLL_MS)) {
            powerSent = 1;
            void (*cb)(void *, uint32_t) = __atomic_load_n(&sink, __ATOMIC_ACQUIRE);
            void *ctx = __atomic_load_n(&sink_context, __ATOMIC_ACQUIRE);
            if (cb) {
                cb(ctx, BTN_POWER);
                IOSleep(150);
                cb(ctx, 0);
            }
            snprintf(native_buttons_row, sizeof native_buttons_row, "power press at %u s%s", powerAt, cb ? "" : " (no sink)");
            native_buttons_changes = native_buttons_changes + 1;
        }
        if (read8(pon, BTN_RT_STS, &pon_sts) || read8(gpio, BTN_RT_STS, &gpio_sts)) {++errors; continue;}
        int changed = buttons_debounce_step(&d, buttons_mask(pon_sts, gpio_sts), 2);
        native_buttons_mask = d.stable;

        if ((d.stable & BTN_COMBO_MASK) == BTN_COMBO_MASK) {
            if (comboTicks < 1000u) ++comboTicks;
            if (changed) {
                // Release consumer report so volume HUD does not block lock screen swipe
                void (*callback)(void *, uint32_t) = __atomic_load_n(&sink, __ATOMIC_ACQUIRE);
                if (callback) callback(__atomic_load_n(&sink_context, __ATOMIC_ACQUIRE), d.stable & ~BTN_COMBO_MASK);
            }
            continue;
        }

        // Combo released: if held briefly (< 1.5s, well below BootRamoops's 3s fastboot trigger), inject unlock swipe!
        if (comboTicks > 0u) {
            uint32_t held = comboTicks;
            comboTicks = 0;
            if (held < 150u && __atomic_exchange_n(&swipe_busy, 1u, __ATOMIC_ACQ_REL) == 0u) {
                void *sthread = 0;
                if (kernel_thread_start(combo_swipe_thread, 0, &sthread) == 0) thread_deallocate(sthread);
                else __atomic_store_n(&swipe_busy, 0u, __ATOMIC_RELEASE);
            }
        }

        if (!changed) continue;
        void (*callback)(void *, uint32_t) = __atomic_load_n(&sink, __ATOMIC_ACQUIRE);
        if (callback) callback(__atomic_load_n(&sink_context, __ATOMIC_ACQUIRE), d.stable);
        snprintf(native_buttons_row, sizeof native_buttons_row, "keys %s%s%s(mask %x)%s, pon %02x gpio6 %02x, read errors %u",
                 (d.stable & BTN_POWER) ? "power " : "", (d.stable & BTN_VOLUME_UP) ? "volume-up " : "",
                 (d.stable & BTN_VOLUME_DOWN) ? "volume-down " : "", d.stable, callback ? "" : " no sink", pon_sts, gpio_sts, errors);
        native_buttons_changes = native_buttons_changes + 1;
    }
}

void native_buttons_probe(void) {
    static int started;
    unsigned on = 0;
    if (started || !PE_parse_boot_argn("pipa-buttons", &on, sizeof on) || !on) return;
    started = 1;
    void *thread = 0;
    if (kernel_thread_start(buttons_thread, 0, &thread) == 0) thread_deallocate(thread);
    else native_buttons_error = -12;
}
