// SPDX-License-Identifier: BSD-2-Clause-Patent
// Host fixtures exercise the actual state writer; no sysregs/IRQ are emulated.
#include "J716fArmCpuState.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

static void emit(const char *directory, const char *name, unsigned mode)
{
  volatile uint64_t t[J716F_TRACE_WORDS] = {J716F_TRACE_MAGIC, 1, J716F_TRACE_WORDS};
  uint64_t r[16] = {};
  r[4] = 19200000; r[5] = 100; r[6] = 10000; r[7] = 1;
  r[8] = 0x3c0; r[9] = 27; r[10] = 1; r[11] = 1;
  uint32_t gic[8] = {};
  for (unsigned phase = 1; phase <= 7; ++phase) {
    if (mode != 9 || phase != 4) { j716fTraceSetup(t, phase, 0, r, gic, 0, 0); }
  }
  unsigned callbacks = 0;
  for (unsigned irq = 1; irq <= 8; ++irq) {
    r[13] = irq;
    if (mode == 6) { r[10] = 0; }
    if (mode == 7) { r[9] = 0; }
    for (unsigned stage = 1; stage <= 3; ++stage) {
      if (mode == 1 && stage != 1) { continue; }
      if (mode == 2 && stage == 3) { continue; }
      unsigned phase = (mode == 3 && stage != 3) ? 3-stage : stage;
      if (mode != 4) { r[5] = r[5]+100; }
      r[14] = phase == 3 ? 6 : 2;
      if (j716fTraceEvent(t, phase, mode == 5 ? 1 : 0, r)) { ++callbacks; }
    }
  }
  if (mode == 8) { j716fTraceSetup(t, 8, 0, r, gic, 1, 0); }
  if (mode == 0 || mode == 8) {
    if (callbacks != 1) { std::fprintf(stderr, "missing callback\n"); std::abort(); }
  } else if (callbacks) { std::fprintf(stderr, "false checkpoint\n"); std::abort(); }
  // Ninth interrupt must leave all words unchanged.
  if (mode == 0) {
    uint64_t frozen[J716F_TRACE_WORDS];
    for (unsigned i = 0; i < J716F_TRACE_WORDS; ++i) { frozen[i] = t[i]; }
    if (j716fTraceEvent(t, 1, 0, r)) { std::abort(); }
    for (unsigned i = 0; i < J716F_TRACE_WORDS; ++i) { if (frozen[i] != t[i]) { std::abort(); } }
  }
  std::string path = std::string(directory)+"/"+name+".bin";
  FILE *file = std::fopen(path.c_str(), "wb");
  if (!file) { std::abort(); }
  for (unsigned i = 0; i < J716F_TRACE_WORDS; ++i) {
    uint64_t value = t[i];
    if (std::fwrite(&value, sizeof(value), 1, file) != 1) { std::abort(); }
  }
  std::fclose(file);
}
int main(int argc, char **argv)
{
  if (argc != 2) { return 2; }
  const char *names[] = {"complete", "entry_only", "before_eoi", "out_of_order",
                         "counter_frozen", "wrong_cpu", "provider_absent", "wrong_intid",
                         "quiesced", "setup_missing"};
  for (unsigned i = 0; i < 10; ++i) { emit(argv[1], names[i], i); }
  return 0;
}
