// SPDX-License-Identifier: BSD-2-Clause-Patent
// No-op definitions of the probe hooks and diagnostic counters referenced by
// XNU, BootRamoops, and NativeLegacyDisplay while their optional drivers are unlinked.

typedef unsigned int u32;
typedef unsigned long long u64;

// --- Basic Probes (weak fallbacks or unlinked hardware probes) ---
void native_legacy_display_probe(void);
__attribute__((weak)) void native_legacy_display_probe(void) {}

// native_touch_probe is provided by TouchProbe.cpp in libNativeLegacyDisplay.late.a

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

int boot_log_export_put(u32 generation, u32 total, u32 offset, const void *data, u32 size);
__attribute__((weak)) int boot_log_export_put(u32 generation, u32 total, u32 offset, const void *data, u32 size)
{
  (void)generation; (void)total; (void)offset; (void)data; (void)size;
  return -4;
}

// --- Touch Diagnostics (BootRamoops) ---
char native_touch_rows[4][120];
volatile unsigned native_touch_stage = 0;
volatile unsigned native_touch_stage_req = 0;
volatile unsigned native_touch_stroke_count = 0;
volatile int native_touch_strokes[16][7];
char native_touch_trace[8][120];
volatile int native_touch_extent[4] = {0x7fffffff, -1, 0x7fffffff, -1};

// --- Battery Diagnostics (BootRamoops) ---
char native_battery_row[120];
volatile unsigned native_battery_changes = 0;
volatile int native_battery_error = 0;
volatile unsigned native_battery_published = 0;

// --- Gestures Diagnostics (BootRamoops) ---
char native_gesture_rows[8][120];
volatile unsigned native_gesture_written = 0;
volatile int native_gesture_error = 0;
char native_gesture_timeline[4096];
volatile unsigned native_gesture_timeline_seq = 0;

// --- UFS / DPU / Scanout ---
u32 native_ufs_dpu_map(unsigned slot, const u64 *pages, unsigned count);
__attribute__((weak)) u32 native_ufs_dpu_map(unsigned slot, const u64 *pages, unsigned count)
{
  (void)slot; (void)pages; (void)count;
  return 0;
}
volatile u32 native_ufs_dpu_state = 0;

struct native_ufs_reader_view {
  char name[20];
  unsigned long blocks;
  unsigned long mb;
};
struct native_ufs_reader_view native_ufs_readers[12];
volatile u64 native_ufs_verify_ticks = 0;

void native_mdss_surface_offered(void *memory);
__attribute__((weak)) void native_mdss_surface_offered(void *memory) { (void)memory; }

void native_flip_frame_offered(void);
__attribute__((weak)) void native_flip_frame_offered(void) {}

// --- GPU / MDSS / Flip Diagnostics (BootRamoops) ---
char native_gpu_rows[31][120];
volatile unsigned native_gpu_changes = 0;
volatile unsigned native_gpu_step = 0;

char native_mdss_rows[31][120];
volatile unsigned native_mdss_changes = 0;
volatile unsigned native_mdss_step = 0;

char native_flip_rows[16][120];
volatile unsigned native_flip_changes = 0;

// --- Adreno Diagnostics / Fences (BootRamoops & NativeLegacyDisplay) ---
int native_adreno_fences(u32 *submitted, u32 *retired);
__attribute__((weak)) int native_adreno_fences(u32 *submitted, u32 *retired)
{
  if (submitted) *submitted = 0;
  if (retired) *retired = 0;
  return 0;
}

struct native_adreno_client_record {
  char name[20];
  u32 opens, refused, closes, slot, peak_mib, last_refusal;
};
struct native_adreno_client_record native_adreno_client_log[8];

struct native_adreno_client_text_view {
  char name[20];
  char text[64];
};
struct native_adreno_client_text_view native_adreno_client_text[4];

struct native_adreno_metal_diag_view {
  u32 opens, busy, bad, closes, reports, property_reads, foreign_opens, foreign_type, name_reads, calls, last_selector, last_status,
      ext_entries, ext_selector, ext_status, ext_in, ext_out, tunnel_calls, probes, sel_bits[8], mb_maps, mb_calls, marks_total, marks[16],
      chk_macf, chk_res, chk_type, type_pass_lo, type_pass_hi, attach_status, inflight, inflight_since_calls, forced_unmaps;
  char opener[3][17], reader[3][17], name_reader[17], starter[3][17], report[64], report2[64];
};
struct native_adreno_metal_diag_view native_adreno_metal_diag;

struct native_adreno_open_record {
  char name[20];
  u32 type, macf, res, attach, foreign;
};
struct native_adreno_open_record native_adreno_open_log[8];
volatile u32 native_adreno_open_log_count = 0;

struct native_adreno_asker_record {
  char name[20];
  u32 reads;
};
struct native_adreno_asker_record native_adreno_metal_askers[12];

volatile u32 native_adreno_fence_wait[8];
char native_adreno_metal_rows[48][96];
