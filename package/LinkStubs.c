// SPDX-License-Identifier: BSD-2-Clause-Patent
// No-op weak definitions of the probe hooks and diagnostic counters referenced by
// XNU, BootRamoops, and NativeLegacyDisplay while optional components are unlinked.

typedef unsigned int u32;
typedef unsigned long long u64;

// --- Basic Probes (weak fallbacks when optional probes are not linked) ---
void native_legacy_display_probe(void);
__attribute__((weak)) void native_legacy_display_probe(void) {}

void native_battery_probe(void);
__attribute__((weak)) void native_battery_probe(void) {}

void native_gpu_probe(void);
__attribute__((weak)) void native_gpu_probe(void) {}

void native_mdss_probe(void);
__attribute__((weak)) void native_mdss_probe(void) {}

void native_flip_probe(void);
__attribute__((weak)) void native_flip_probe(void) {}

void native_gestures_probe(void);
__attribute__((weak)) void native_gestures_probe(void) {}

void native_buttons_set_sink(void (*sink)(void *, unsigned), void *context);
__attribute__((weak)) void native_buttons_set_sink(void (*sink)(void *, unsigned), void *context)
{
  (void)sink; (void)context;
}

int boot_log_export_put(u32 generation, u32 total, u32 offset, const void *data, u32 size);
__attribute__((weak)) int boot_log_export_put(u32 generation, u32 total, u32 offset, const void *data, u32 size)
{
  (void)generation; (void)total; (void)offset; (void)data; (void)size;
  return -4;
}

// --- Touch Diagnostics (TouchProbe.cpp / BootRamoops.c) ---
char native_touch_rows[4][120];
volatile unsigned native_touch_stage = 0;
volatile unsigned native_touch_stage_req = 0;
volatile unsigned native_touch_stroke_count = 0;
volatile int native_touch_strokes[16][7];
char native_touch_trace[8][120];
volatile int native_touch_extent[4];

// --- Gestures Diagnostics (BootRamoops) ---
__attribute__((weak)) char native_gesture_rows[8][120];
__attribute__((weak)) volatile unsigned native_gesture_written = 0;
__attribute__((weak)) volatile int native_gesture_error = 0;
__attribute__((weak)) char native_gesture_timeline[4096];
__attribute__((weak)) volatile unsigned native_gesture_timeline_seq = 0;

// --- UFS / DPU / Scanout ---
u32 native_ufs_dpu_map(unsigned slot, const u64 *pages, unsigned count);
__attribute__((weak)) u32 native_ufs_dpu_map(unsigned slot, const u64 *pages, unsigned count)
{
  (void)slot; (void)pages; (void)count;
  return 0;
}
__attribute__((weak)) volatile u32 native_ufs_dpu_state = 0;

struct native_ufs_reader_view {
  char name[20];
  unsigned long blocks;
  unsigned long mb;
};
__attribute__((weak)) struct native_ufs_reader_view native_ufs_readers[12];
__attribute__((weak)) volatile u64 native_ufs_verify_ticks = 0;

void native_mdss_surface_offered(void *memory);
__attribute__((weak)) void native_mdss_surface_offered(void *memory) { (void)memory; }

void native_flip_frame_offered(void);
__attribute__((weak)) void native_flip_frame_offered(void) {}

// --- Flip Diagnostics (BootRamoops) ---
__attribute__((weak)) char native_flip_rows[16][120];
__attribute__((weak)) volatile unsigned native_flip_changes = 0;

// --- Kernel Thread / CPU Diagnostic Hooks (WifiNet.cpp) ---
int boot_kthread_report(const char *name, char *out, unsigned size);
__attribute__((weak)) int boot_kthread_report(const char *name, char *out, unsigned size)
{
  (void)name;
  if (out && size) out[0] = 0;
  return 0;
}

unsigned boot_cpu_kick(char *out, unsigned size);
__attribute__((weak)) unsigned boot_cpu_kick(char *out, unsigned size)
{
  if (out && size) out[0] = 0;
  return 0;
}
