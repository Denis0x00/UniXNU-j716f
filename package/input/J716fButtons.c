// SPDX-License-Identifier: BSD-2-Clause-Patent
// TB-J716F hardware buttons (PM8150 PON Power/VolDown + GPIO6 VolUp via SPMI observer).
// Enables when j716f-buttons=1, buttons=1, or pipa-buttons=1 is present.
#include <stdint.h>
#include "J716fButtonsCore.h"

extern uint64_t ml_io_map(uint64_t phys_addr, uint64_t size);
extern int PE_parse_boot_argn(const char *arg_string, void *arg_ptr, int max_arg);
extern int kernel_thread_start(void (*continuation)(void *, int), void *parameter, void **new_thread);
extern void thread_deallocate(void *thread);
extern void IOSleep(unsigned milliseconds);
extern void IOLog(const char *format, ...);

extern volatile uint32_t native_touch_seq, native_touch_count;
extern volatile int native_touch_xs[12], native_touch_ys[12], native_touch_ids[12];
extern void native_touch_published(void);
extern volatile uint32_t native_touch_injecting;

#define SPMI_CORE            0x0C440000ULL
#define SPMI_OBSRVR          0x0E600000ULL
#define SPMI_APID_MAP        0x900U
#define SPMI_MAX_APIDS       512U
#define SPMI_OBS_APID_STRIDE 0x80ULL
#define SPMI_CMD             0x00U
#define SPMI_STATUS          0x08U
#define SPMI_RDATA0          0x18U
#define SPMI_POLLS           100000U
#define POLL_MS              10U
#define BTN_COMBO_MASK       (BTN_VOLUME_UP | BTN_VOLUME_DOWN)

volatile uint32_t j716f_buttons_changes = 0;
volatile uint32_t j716f_buttons_mask = 0;
volatile int j716f_buttons_error = 0;
static volatile uint32_t g_swipe_busy = 0;

static void (*volatile g_btn_sink)(void *, uint32_t) = 0;
static void *volatile g_btn_sink_context = 0;

void native_buttons_set_sink(void (*callback)(void *, uint32_t), void *context) {
  __atomic_store_n(&g_btn_sink, (void (*)(void *, uint32_t))0, __ATOMIC_RELEASE);
  __atomic_store_n(&g_btn_sink_context, context, __ATOMIC_RELEASE);
  __atomic_store_n(&g_btn_sink, callback, __ATOMIC_RELEASE);
}

static void publish_single_touch(unsigned count, int id, int x, int y) {
  native_touch_seq = native_touch_seq + 1U;
  __asm__ volatile("dmb ish" ::: "memory");
  native_touch_count = count;
  if (count > 0) {
    native_touch_ids[0] = id;
    native_touch_xs[0] = x;
    native_touch_ys[0] = y;
  }
  __asm__ volatile("dmb ish" ::: "memory");
  native_touch_seq = native_touch_seq + 1U;
  native_touch_published();
}

static void combo_swipe_thread(void *parameter, int wait_result) {
  (void)parameter;
  (void)wait_result;
  __atomic_store_n(&native_touch_injecting, 1U, __ATOMIC_RELEASE);
  for (int step = 0; step <= 30; ++step) {
    int y = 20200 - (11200 * step) / 30;
    publish_single_touch(1U, 9, 15240, y);
    IOSleep(10);
  }
  publish_single_touch(0U, 9, 15240, 9000);
  __atomic_store_n(&native_touch_injecting, 0U, __ATOMIC_RELEASE);
  IOLog("[J716F btn] VolUp+VolDown swipe-up gesture injected\n");
  j716f_buttons_changes = j716f_buttons_changes + 1U;
  __atomic_store_n(&g_swipe_busy, 0U, __ATOMIC_RELEASE);
}

static volatile uint32_t *observer_for(volatile uint32_t *core, uint32_t apids, uint32_t ppid) {
  for (uint32_t i = 0; i < apids; ++i) {
    uint32_t map = core[(SPMI_APID_MAP + 4U * i) / 4U];
    if (!map || ((map >> 8) & 0xFFFU) != ppid) continue;
    uint64_t phys = SPMI_OBSRVR + SPMI_OBS_APID_STRIDE * i;
    uint64_t page = ml_io_map(phys & ~0xFFFULL, 0x1000U);
    return page ? (volatile uint32_t *)(uintptr_t)(page + (phys & 0xFFFU)) : 0;
  }
  return 0;
}

static int read8(volatile uint32_t *observer, uint8_t address, uint8_t *value) {
  observer[SPMI_CMD / 4U] = (1U << 27) | ((uint32_t)address << 4);
  for (uint32_t poll = 0; poll < SPMI_POLLS; ++poll) {
    uint32_t status = observer[SPMI_STATUS / 4U];
    if (status & 1U) {
      if (status & 0xEU) return 2;
      *value = (uint8_t)observer[SPMI_RDATA0 / 4U];
      return 0;
    }
  }
  return 1;
}

static void buttons_thread(void *parameter, int wait_result) {
  (void)parameter;
  (void)wait_result;
  volatile uint32_t *core = (volatile uint32_t *)(uintptr_t)ml_io_map(SPMI_CORE, 0x2000U);
  if (!core) {
    j716f_buttons_error = -1;
    IOLog("[J716F btn] SPMI core map failed\n");
    return;
  }
  uint32_t version = core[0], apids = core[1] & 0x7FFU;
  if (version < 0x50000000U || version >= 0x70000000U || !apids || apids > SPMI_MAX_APIDS) {
    j716f_buttons_error = -2;
    IOLog("[J716F btn] unexpected SPMI version=0x%08x apids=%u\n", version, apids);
    return;
  }
  volatile uint32_t *pon = observer_for(core, apids, BTN_PON_PPID);
  volatile uint32_t *gpio = observer_for(core, apids, BTN_GPIO6_PPID);
  if (!pon || !gpio) {
    j716f_buttons_error = pon ? -4 : -3;
    IOLog("[J716F btn] missing observer pon=%d gpio6=%d\n", pon != 0, gpio != 0);
    return;
  }
  uint8_t type = 0, subtype = 0, mode = 0, pull = 0, enable = 0, pon_sts = 0, gpio_sts = 0;
  int rc = read8(gpio, BTN_GPIO_TYPE, &type) |
           read8(gpio, BTN_GPIO_SUBTYPE, &subtype) |
           read8(gpio, BTN_GPIO_MODE_CTL, &mode) |
           read8(gpio, BTN_GPIO_DIG_PULL, &pull) |
           read8(gpio, BTN_GPIO_EN_CTL, &enable) |
           read8(pon, BTN_RT_STS, &pon_sts) |
           read8(gpio, BTN_RT_STS, &gpio_sts);
  IOLog("[J716F btn] started rc=%d gpio6=%02x/%02x mode=%02x pull=%02x en=%02x sts=%02x pon=%02x\n",
        rc, type, subtype, mode, pull, enable, gpio_sts, pon_sts);
  j716f_buttons_changes = 1U;
  if (rc) {
    j716f_buttons_error = -5;
    return;
  }

  struct buttons_debounce d = {0, 0, 0};
  uint32_t errors = 0;
  uint32_t combo_ticks = 0;
  for (;;) {
    IOSleep(POLL_MS);
    if (read8(pon, BTN_RT_STS, &pon_sts) || read8(gpio, BTN_RT_STS, &gpio_sts)) {
      ++errors;
      continue;
    }
    int changed = buttons_debounce_step(&d, buttons_mask(pon_sts, gpio_sts), 2U);
    j716f_buttons_mask = d.stable;

    if ((d.stable & BTN_COMBO_MASK) == BTN_COMBO_MASK) {
      if (combo_ticks < 1000U) ++combo_ticks;
      if (changed) {
        void (*cb)(void *, uint32_t) = __atomic_load_n(&g_btn_sink, __ATOMIC_ACQUIRE);
        if (cb) cb(__atomic_load_n(&g_btn_sink_context, __ATOMIC_ACQUIRE), d.stable & ~BTN_COMBO_MASK);
      }
      continue;
    }

    if (combo_ticks > 0U) {
      uint32_t held = combo_ticks;
      combo_ticks = 0U;
      if (held < 150U && __atomic_exchange_n(&g_swipe_busy, 1U, __ATOMIC_ACQ_REL) == 0U) {
        void *sthread = 0;
        if (kernel_thread_start(combo_swipe_thread, 0, &sthread) == 0) {
          thread_deallocate(sthread);
        } else {
          __atomic_store_n(&g_swipe_busy, 0U, __ATOMIC_RELEASE);
        }
      }
    }

    if (!changed) continue;
    void (*cb)(void *, uint32_t) = __atomic_load_n(&g_btn_sink, __ATOMIC_ACQUIRE);
    if (cb) cb(__atomic_load_n(&g_btn_sink_context, __ATOMIC_ACQUIRE), d.stable);
    if (j716f_buttons_changes < 16U) {
      IOLog("[J716F btn] mask=0x%x pon=%02x gpio6=%02x sink=%d errs=%u\n",
            d.stable, pon_sts, gpio_sts, cb != 0, errors);
    }
    j716f_buttons_changes = j716f_buttons_changes + 1U;
  }
}

void j716f_buttons_probe(void) {
  static int started = 0;
  unsigned on = 0;
  int enabled = (PE_parse_boot_argn("j716f-buttons", &on, sizeof(on)) && on) ||
                (PE_parse_boot_argn("buttons", &on, sizeof(on)) && on) ||
                (PE_parse_boot_argn("pipa-buttons", &on, sizeof(on)) && on);
  if (started || !enabled) return;
  started = 1;
  void *thread = 0;
  if (kernel_thread_start(buttons_thread, 0, &thread) == 0) {
    thread_deallocate(thread);
  } else {
    j716f_buttons_error = -12;
  }
}
