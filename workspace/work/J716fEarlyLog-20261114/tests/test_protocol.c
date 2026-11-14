// SPDX-License-Identifier: BSD-2-Clause-Patent
/* Host model only. No physical mapping, reset, USB, storage or device access. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>

static unsigned writes, stop_after, barriers;
static unsigned trace_offset[2048], trace_barrier[2048];
static int interrupt_enabled;
static jmp_buf cut;
static void write_hook(unsigned offset, unsigned value)
{
    (void)value;
    if (writes < 2048) { trace_offset[writes] = offset; trace_barrier[writes] = barriers; }
    ++writes;
    if (interrupt_enabled && writes == stop_after) longjmp(cut, 1);
}
#define J716F_EL_WRITE_HOOK(offset, value) write_hook(offset, value)
#define J716F_EL_BARRIER() (++barriers)
#include "../include/J716fEarlyLog.h"

static unsigned assertions;
#define CHECK(expr) do { ++assertions; if (!(expr)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expr); exit(1); } } while (0)

static unsigned char arena[J716F_EL_REGION_BYTES + 64];
static unsigned char saved[J716F_EL_REGION_BYTES];
static unsigned char *mem = arena + 32;
static J716F_EL_STATE state;
static J716F_EL_REPORT report;
static unsigned char exported[104 * 240];
static unsigned exported_len, calls;
static J716F_EL_U64 exported_seq[240];

static void jlog(unsigned sequence)
{
    memset(arena, 0xA5, sizeof arena);
    J716fEarlyLogPut32(mem, J716F_EL_JLOG_MAGIC);
    J716fEarlyLogPut32(mem + 4, 100);
    J716fEarlyLogPut32(mem + 8, J716F_EL_JLOG_CAPACITY);
    J716fEarlyLogPut32(mem + 12, sequence);
    J716fEarlyLogPut32(mem + J716F_EL_OWNER_OFFSET, J716F_EL_JOWN_MAGIC);
    J716fEarlyLogPut32(mem + J716F_EL_OWNER_OFFSET + 4, sequence);
    J716fEarlyLogPut32(mem + J716F_EL_OWNER_OFFSET + 8, 7);
    memset(&state, 0, sizeof state);
    writes = barriers = 0;
    interrupt_enabled = 0;
}

static void canaries(void)
{
    unsigned i;
    for (i = 0; i != 32; ++i) {
        CHECK(arena[i] == 0xA5);
        CHECK(arena[sizeof arena - 32 + i] == 0xA5);
    }
    CHECK(memcmp(mem + J716F_EL_TOTAL_BYTES, saved + J716F_EL_TOTAL_BYTES,
                 J716F_EL_REGION_BYTES - J716F_EL_TOTAL_BYTES) == 0);
}

static void begin(void)
{
    jlog(7);
    memcpy(saved, mem, sizeof saved);
    CHECK(J716fEarlyLogBegin(&state, mem, J716F_EL_REGION_BYTES, 2,
          0x1122334455667788ULL, "r22-test") == 0);
}

static int collect(void *ctx, J716F_EL_U64 seq, const J716F_EL_U8 *data, J716F_EL_U32 n)
{
    (void)ctx;
    CHECK(calls < 240 && exported_len + n <= sizeof exported);
    exported_seq[calls++] = seq;
    memcpy(exported + exported_len, data, n);
    exported_len += n;
    return 0;
}

static void snapshot(void)
{
    calls = exported_len = 0;
    CHECK(J716fEarlyLogSnapshot(mem, J716F_EL_TOTAL_BYTES, collect, NULL, &report) == 0);
}

static int stop_callback(void *ctx, J716F_EL_U64 seq, const J716F_EL_U8 *p, J716F_EL_U32 n)
{
    (void)ctx; (void)seq; (void)p; (void)n; return 1;
}

static void basic(void)
{
    J716F_EL_HEADER header;
    CHECK(J716fEarlyLogCrc32((const unsigned char *)"123456789", 9) == 0xCBF43926U);
    begin();
    CHECK(J716fEarlyLogHeaderValidate(mem, 128, &header) == 0);
    CHECK(header.boot_sequence == 7 && header.source == 2 &&
          header.boot_id == 0x1122334455667788ULL && !strcmp(header.build_tag, "r22-test"));
    snapshot();
    CHECK(report.valid_records == 0 && report.empty_records == 240 && report.gaps == 0);
    CHECK(J716fEarlyLogAppend(&state, "abc", 3) == 0);
    snapshot(); CHECK(calls == 1 && exported_len == 3 && !memcmp(exported, "abc", 3));
    CHECK(J716fEarlyLogAppend(&state, "def", 3) == 0);
    snapshot(); CHECK(calls == 1 && exported_len == 6 && !memcmp(exported, "abcdef", 6));
    CHECK(J716fEarlyLogSnapshot(mem, J716F_EL_TOTAL_BYTES, stop_callback, NULL, &report) == J716F_EL_CALLBACK_STOP);
    canaries();
}

static void barrier_order(void)
{
    begin();
    CHECK(writes == 1092 && barriers == 3);
    CHECK(trace_offset[0] == 0 && trace_barrier[0] == 0);
    CHECK(trace_offset[4] == 252 && trace_barrier[4] == 1);
    CHECK(trace_offset[964] == 4 && trace_barrier[964] == 1);
    CHECK(trace_offset[1088] == 0 && trace_barrier[1088] == 2);
    writes = barriers = 0;
    CHECK(J716fEarlyLogAppend(&state, "a", 1) == 0);
    CHECK(writes == 132 && barriers == 3);
    CHECK(trace_offset[0] == 252 && trace_barrier[0] == 0);
    CHECK(trace_offset[4] == 128 && trace_barrier[4] == 1);
    CHECK(trace_offset[128] == 252 && trace_barrier[128] == 2);
}

static void arguments(void)
{
    unsigned before;
    char bad_tag[33];
    begin(); before = writes;
    CHECK(J716fEarlyLogAppend(&state, NULL, 0) == 0);
    CHECK(J716fEarlyLogAppend(&state, NULL, 1) == J716F_EL_BAD_ARGUMENT);
    CHECK(J716fEarlyLogAppend(&state, "x", J716F_EL_MAX_U64) == J716F_EL_OVERFLOW);
    CHECK(J716fEarlyLogAppend(NULL, "x", 1) == J716F_EL_BAD_ARGUMENT);
    CHECK(writes == before);
    CHECK(J716fEarlyLogHeaderValidate(NULL, 128, NULL) == J716F_EL_BAD_ARGUMENT);
    CHECK(J716fEarlyLogHeaderValidate(mem, 127, NULL) == J716F_EL_BAD_ARGUMENT);
    CHECK(J716fEarlyLogSnapshot(mem, J716F_EL_TOTAL_BYTES - 1, NULL, NULL, NULL) == J716F_EL_BAD_ARGUMENT);
    CHECK(J716fEarlyLogSnapshot(NULL, J716F_EL_TOTAL_BYTES, NULL, NULL, NULL) == J716F_EL_BAD_ARGUMENT);
    jlog(7); before = writes;
    CHECK(J716fEarlyLogBegin(NULL, mem, J716F_EL_REGION_BYTES, 2, 1, "ok") == J716F_EL_BAD_ARGUMENT);
    CHECK(J716fEarlyLogBegin(&state, NULL, J716F_EL_REGION_BYTES, 2, 1, "ok") == J716F_EL_BAD_ARGUMENT);
    CHECK(J716fEarlyLogBegin(&state, mem, J716F_EL_TOTAL_BYTES, 2, 1, "ok") == J716F_EL_BAD_ARGUMENT);
    CHECK(J716fEarlyLogBegin(&state, mem, J716F_EL_REGION_BYTES, 0, 1, "ok") == J716F_EL_BAD_ARGUMENT);
    CHECK(J716fEarlyLogBegin(&state, mem, J716F_EL_REGION_BYTES, 3, 1, "ok") == J716F_EL_BAD_ARGUMENT);
    CHECK(J716fEarlyLogBegin(&state, mem, J716F_EL_REGION_BYTES, 2, 0, "ok") == J716F_EL_BAD_ARGUMENT);
    CHECK(J716fEarlyLogBegin(&state, mem, J716F_EL_REGION_BYTES, 2, 1, "") == J716F_EL_BAD_ARGUMENT);
    CHECK(J716fEarlyLogBegin(&state, mem, J716F_EL_REGION_BYTES, 2, 1, "a\n") == J716F_EL_BAD_ARGUMENT);
    CHECK(J716fEarlyLogBegin(&state, mem, J716F_EL_REGION_BYTES, 2, 1, NULL) == J716F_EL_BAD_ARGUMENT);
    memset(bad_tag, 'x', 32); bad_tag[32] = 0;
    CHECK(J716fEarlyLogBegin(&state, mem, J716F_EL_REGION_BYTES, 2, 1, bad_tag) == J716F_EL_BAD_ARGUMENT);
    CHECK(writes == before);
    CHECK(J716fEarlyLogAppend(&state, "x", 1) == J716F_EL_INACTIVE);
}

static void bad_jlog(void)
{
    unsigned offsets[] = {0, 4, 8, 12, J716F_EL_OWNER_OFFSET, J716F_EL_OWNER_OFFSET + 4};
    unsigned values[] = {0, J716F_EL_JLOG_CAPACITY + 1, 1, 0, 0, 99};
    unsigned i;
    for (i = 0; i != sizeof offsets / sizeof offsets[0]; ++i) {
        jlog(7); J716fEarlyLogPut32(mem + offsets[i], values[i]);
        CHECK(J716fEarlyLogBegin(&state, mem, J716F_EL_REGION_BYTES, 2, 1, "ok") == J716F_EL_BAD_JLOG);
        CHECK(writes == 0);
    }
}

static void corruption(void)
{
    unsigned i;
    unsigned char header[128], first[128];
    begin(); memcpy(header, mem, 128);
    for (i = 0; i != 128; ++i) {
        memcpy(mem, header, 128); mem[i] ^= 1;
        CHECK(J716fEarlyLogHeaderValidate(mem, 128, NULL) == J716F_EL_BAD_HEADER);
    }
    memcpy(mem, header, 128);
    CHECK(J716fEarlyLogAppend(&state, "abc", 3) == 0); memcpy(first, mem + 128, 128);
    for (i = 0; i != 128; ++i) {
        memcpy(mem + 128, first, 128); mem[128 + i] ^= 1;
        snapshot(); CHECK(report.exported_records == 0);
    }
    memcpy(mem + 128, first, 128);
    mem[128 + 20] = 1;
    J716fEarlyLogPut32(mem + 128 + 120, J716fEarlyLogCrc32(mem + 128, 120));
    snapshot(); CHECK(report.invalid_records == 1);
}

static void wrap_and_epoch(void)
{
    unsigned i;
    unsigned char block[104];
    J716F_EL_STATE old;
    begin();
    for (i = 1; i <= 502; ++i) {
        memset(block, i & 255, sizeof block);
        CHECK(J716fEarlyLogAppend(&state, block, sizeof block) == 0);
    }
    snapshot();
    CHECK(calls == 240 && report.first_sequence == 263 && report.highest_sequence == 502);
    CHECK(report.gaps == 0 && report.exported_bytes == 240 * 104);
    for (i = 0; i != 240; ++i) {
        CHECK(exported_seq[i] == i + 263);
        CHECK(exported[i * 104] == ((i + 263) & 255));
    }
    old = state;
    /* A new firmware JLOG epoch leaves old record payloads in RAM. */
    J716fEarlyLogPut32(mem, J716F_EL_JLOG_MAGIC);
    J716fEarlyLogPut32(mem + 4, 0);
    J716fEarlyLogPut32(mem + 8, J716F_EL_JLOG_CAPACITY);
    J716fEarlyLogPut32(mem + 12, 8);
    J716fEarlyLogPut32(mem + J716F_EL_OWNER_OFFSET + 4, 8);
    CHECK(J716fEarlyLogBegin(&state, mem, J716F_EL_REGION_BYTES, 1, 2, "new-epoch") == 0);
    snapshot(); CHECK(calls == 0 && report.empty_records == 240);
    CHECK(J716fEarlyLogAppend(&old, "stale", 5) == J716F_EL_INACTIVE);
    CHECK(J716fEarlyLogAppend(&state, "new", 3) == 0);
    snapshot(); CHECK(calls == 1 && !memcmp(exported, "new", 3));
}

static void boundaries(void)
{
    unsigned before;
    unsigned char block[104] = {0};
    begin(); state.sequence = J716F_EL_MAX_U64; state.tail_length = 0;
    CHECK(J716fEarlyLogAppend(&state, block, sizeof block) == 0);
    snapshot(); CHECK(report.highest_sequence == J716F_EL_MAX_U64 && report.exported_records == 1);
    CHECK(report.gaps == 239);
    before = writes;
    CHECK(J716fEarlyLogAppend(&state, "x", 1) == J716F_EL_OVERFLOW && writes == before);
    begin(); state.sequence = J716F_EL_COMMIT; before = writes;
    CHECK(J716fEarlyLogAppend(&state, "x", 1) == J716F_EL_OVERFLOW && writes == before);
    begin(); state.sequence = J716F_EL_COMMIT - 1; state.tail_length = 103; before = writes;
    CHECK(J716fEarlyLogAppend(&state, "xy", 2) == J716F_EL_OVERFLOW && writes == before);
    CHECK(J716fEarlyLogAppend(&state, "x", 1) == 0);
    begin(); state.sequence = (0x1234ULL << 32) | (0x1234U ^ J716F_EL_COMMIT); before = writes;
    CHECK(J716fEarlyLogAppend(&state, "x", 1) == J716F_EL_OVERFLOW && writes == before);
}

static void torn_begin(void)
{
    unsigned total, stop;
    begin(); total = writes;
    for (stop = 1; stop <= total; ++stop) {
        jlog(7); stop_after = stop; interrupt_enabled = 1;
        if (!setjmp(cut)) (void)J716fEarlyLogBegin(&state, mem, J716F_EL_REGION_BYTES, 2, 1, "r22-test");
        interrupt_enabled = 0;
        if (stop < total) CHECK(J716fEarlyLogHeaderValidate(mem, 128, NULL) != 0);
        else { snapshot(); CHECK(calls == 0 && report.empty_records == 240); }
    }
}

static void torn_append(void)
{
    unsigned total, stop;
    begin(); CHECK(J716fEarlyLogAppend(&state, "abc", 3) == 0);
    writes = 0; CHECK(J716fEarlyLogAppend(&state, "def", 3) == 0); total = writes;
    for (stop = 1; stop <= total; ++stop) {
        begin(); CHECK(J716fEarlyLogAppend(&state, "abc", 3) == 0);
        writes = 0; stop_after = stop; interrupt_enabled = 1;
        if (!setjmp(cut)) (void)J716fEarlyLogAppend(&state, "def", 3);
        interrupt_enabled = 0; snapshot();
        CHECK(calls <= 1);
        if (calls) CHECK(exported_len == 6 && !memcmp(exported, "abcdef", 6));
        canaries();
    }
}

int main(int argc, char **argv)
{
    basic(); barrier_order(); arguments(); bad_jlog(); corruption(); wrap_and_epoch(); boundaries(); torn_begin(); torn_append();
    begin(); CHECK(J716fEarlyLogAppend(&state, "C writer cross-language fixture\n", 32) == 0);
    if (argc == 2) {
        FILE *f = fopen(argv[1], "wb"); CHECK(f != NULL);
        CHECK(fwrite(mem, 1, J716F_EL_TOTAL_BYTES, f) == J716F_EL_TOTAL_BYTES); CHECK(fclose(f) == 0);
    }
    printf("PASS: %u assertions; protocol byte-store interruption tests complete\n", assertions);
    return 0;
}
