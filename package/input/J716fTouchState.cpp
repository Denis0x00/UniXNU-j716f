// SPDX-License-Identifier: BSD-2-Clause-Patent
// Seqlock finger buffer shared between J716fTouchProbe / J716fButtons and upstream NativeHidInput.
#include <stdint.h>

extern "C" {
volatile uint32_t native_touch_seq = 0;
volatile uint32_t native_touch_count = 0;
volatile uint32_t native_touch_hid_reports = 0;
volatile int native_touch_xs[12] = {};
volatile int native_touch_ys[12] = {};
volatile int native_touch_ids[12] = {};

void native_touch_set_sink(void (*sink)(void *), void *context);
void native_touch_published(void);
}

namespace {
void (*volatile g_sink)(void *) = nullptr;
void *volatile g_sink_context = nullptr;
}  // namespace

void native_touch_set_sink(void (*sink)(void *), void *context) {
  g_sink = nullptr;
  __asm__ volatile("dmb ish" ::: "memory");
  g_sink_context = context;
  __asm__ volatile("dmb ish" ::: "memory");
  g_sink = sink;
}

void native_touch_published(void) {
  void (*sink)(void *) = g_sink;
  __asm__ volatile("dmb ish" ::: "memory");
  if (sink) sink(g_sink_context);
}
