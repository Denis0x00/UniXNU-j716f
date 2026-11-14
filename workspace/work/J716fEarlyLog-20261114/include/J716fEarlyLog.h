// SPDX-License-Identifier: BSD-2-Clause-Patent
#ifndef J716F_EARLY_LOG_H
#define J716F_EARLY_LOG_H

/* Board-private retained RAM protocol. No allocation, libc, locks or MMIO.
 * Callers provide one serialized CPU and a frozen reader snapshot. All integer
 * encoding is little endian, independent of the compiler's host byte order.
 */
typedef unsigned char J716F_EL_U8;
typedef unsigned short J716F_EL_U16;
typedef unsigned int J716F_EL_U32;
typedef unsigned long long J716F_EL_U64;
typedef char J716F_EL_WIDTH_CHECK[(sizeof(J716F_EL_U8) == 1 &&
    sizeof(J716F_EL_U16) == 2 && sizeof(J716F_EL_U32) == 4 &&
    sizeof(J716F_EL_U64) == 8) ? 1 : -1];

#define J716F_EL_BASE 0xB0000000ULL
#define J716F_EL_REGION_BYTES 0x80000U
#define J716F_EL_HEADER_BYTES 128U
#define J716F_EL_RECORD_BYTES 128U
#define J716F_EL_RECORD_COUNT 240U
#define J716F_EL_PAYLOAD_BYTES 104U
#define J716F_EL_TOTAL_BYTES 30848U
#define J716F_EL_OWNER_OFFSET 0x7FFF4U
#define J716F_EL_MAGIC 0x474C584AU
#define J716F_EL_VERSION 1U
#define J716F_EL_COMMIT 0x58434D54U
#define J716F_EL_JLOG_MAGIC 0x474F4C4AU
#define J716F_EL_JLOG_CAPACITY (J716F_EL_REGION_BYTES - 32U)
#define J716F_EL_JOWN_MAGIC 0x4E574F4AU
#define J716F_EL_SOURCE_PROBE 1U
#define J716F_EL_SOURCE_XNU 2U
#define J716F_EL_MAX_U64 (~(J716F_EL_U64)0)
/* A bounded single append, not the lifetime stream capacity. */
#define J716F_EL_MAX_APPEND 0xFFFFFFFFULL
typedef char J716F_EL_GEOMETRY_CHECK[(J716F_EL_TOTAL_BYTES ==
    J716F_EL_HEADER_BYTES + J716F_EL_RECORD_COUNT * J716F_EL_RECORD_BYTES &&
    J716F_EL_TOTAL_BYTES < J716F_EL_OWNER_OFFSET) ? 1 : -1];

#ifndef J716F_EL_BARRIER
# if defined(__aarch64__) || defined(__arm64__)
#  define J716F_EL_BARRIER() __asm__ volatile("dsb sy" ::: "memory")
# elif defined(__GNUC__) || defined(__clang__)
#  define J716F_EL_BARRIER() __atomic_thread_fence(__ATOMIC_SEQ_CST)
# else
#  error "Define J716F_EL_BARRIER() for this platform"
# endif
#endif

/* Tests may interrupt after any byte store. Production has no hook. */
#ifndef J716F_EL_WRITE_HOOK
# define J716F_EL_WRITE_HOOK(offset, value) ((void)0)
#endif

enum {
    J716F_EL_OK = 0,
    J716F_EL_BAD_ARGUMENT = -1,
    J716F_EL_BAD_HEADER = -2,
    J716F_EL_BAD_JLOG = -3,
    J716F_EL_INACTIVE = -4,
    J716F_EL_OVERFLOW = -5,
    J716F_EL_CALLBACK_STOP = -6
};

typedef struct {
    J716F_EL_U32 boot_sequence;
    J716F_EL_U32 source;
    J716F_EL_U64 boot_id;
    char build_tag[32];
} J716F_EL_HEADER;

typedef struct {
    volatile J716F_EL_U8 *memory;
    J716F_EL_U64 boot_id;
    J716F_EL_U64 sequence;
    J716F_EL_U32 boot_sequence;
    J716F_EL_U32 source;
    J716F_EL_U32 tail_length;
    J716F_EL_U32 active;
    J716F_EL_U8 tail[J716F_EL_PAYLOAD_BYTES];
} J716F_EL_STATE;

typedef struct {
    J716F_EL_HEADER header;
    J716F_EL_U64 first_sequence;
    J716F_EL_U64 highest_sequence;
    J716F_EL_U64 exported_bytes;
    J716F_EL_U32 valid_records;
    J716F_EL_U32 invalid_records;
    J716F_EL_U32 empty_records;
    J716F_EL_U32 exported_records;
    J716F_EL_U32 gaps;
} J716F_EL_REPORT;

typedef int (*J716F_EL_EXPORT)(void *context, J716F_EL_U64 sequence,
    const J716F_EL_U8 *payload, J716F_EL_U32 length);

static inline J716F_EL_U16
J716fEarlyLogGet16(const volatile J716F_EL_U8 *p)
{
    return (J716F_EL_U16)((J716F_EL_U16)p[0] | ((J716F_EL_U16)p[1] << 8));
}

static inline J716F_EL_U32
J716fEarlyLogGet32(const volatile J716F_EL_U8 *p)
{
    return (J716F_EL_U32)p[0] | ((J716F_EL_U32)p[1] << 8) |
        ((J716F_EL_U32)p[2] << 16) | ((J716F_EL_U32)p[3] << 24);
}

static inline J716F_EL_U64
J716fEarlyLogGet64(const volatile J716F_EL_U8 *p)
{
    return (J716F_EL_U64)J716fEarlyLogGet32(p) |
        ((J716F_EL_U64)J716fEarlyLogGet32(p + 4) << 32);
}

static inline void
J716fEarlyLogPut32(J716F_EL_U8 *p, J716F_EL_U32 v)
{
    J716F_EL_U32 i;
    for (i = 0; i != 4; ++i) p[i] = (J716F_EL_U8)(v >> (i * 8));
}

static inline void
J716fEarlyLogPut64(J716F_EL_U8 *p, J716F_EL_U64 v)
{
    J716fEarlyLogPut32(p, (J716F_EL_U32)v);
    J716fEarlyLogPut32(p + 4, (J716F_EL_U32)(v >> 32));
}

static inline J716F_EL_U32
J716fEarlyLogCrc32(const volatile J716F_EL_U8 *p, J716F_EL_U32 n)
{
    J716F_EL_U32 crc = 0xFFFFFFFFU, i, bit;
    for (i = 0; i != n; ++i) {
        crc ^= p[i];
        for (bit = 0; bit != 8; ++bit)
            crc = (crc >> 1) ^ (0xEDB88320U & (0U - (crc & 1U)));
    }
    return crc ^ 0xFFFFFFFFU;
}

static inline void
J716fEarlyLogStore(volatile J716F_EL_U8 *memory, J716F_EL_U32 offset,
    J716F_EL_U8 value)
{
    memory[offset] = value;
    J716F_EL_WRITE_HOOK(offset, value);
}

static inline void
J716fEarlyLogStore32(volatile J716F_EL_U8 *memory, J716F_EL_U32 offset,
    J716F_EL_U32 value)
{
    J716F_EL_U32 i;
    for (i = 0; i != 4; ++i)
        J716fEarlyLogStore(memory, offset + i, (J716F_EL_U8)(value >> (8 * i)));
}

static inline int
J716fEarlyLogTagValid(const volatile J716F_EL_U8 *tag)
{
    J716F_EL_U32 i;
    int ended = 0;
    if (tag[0] == 0) return 0;
    for (i = 0; i != 32; ++i) {
        J716F_EL_U8 c = tag[i];
        if (c == 0) ended = 1;
        else if (ended || c < 0x20 || c > 0x7E) return 0;
    }
    return ended;
}

static inline int
J716fEarlyLogHeaderValidate(const volatile J716F_EL_U8 *memory,
    J716F_EL_U64 bytes, J716F_EL_HEADER *out_header)
{
    J716F_EL_U32 i, source;
    if (!memory || bytes < J716F_EL_HEADER_BYTES) return J716F_EL_BAD_ARGUMENT;
    source = J716fEarlyLogGet32(memory + 24);
    if (J716fEarlyLogGet32(memory) != J716F_EL_MAGIC ||
        J716fEarlyLogGet16(memory + 4) != J716F_EL_VERSION ||
        J716fEarlyLogGet16(memory + 6) != J716F_EL_HEADER_BYTES ||
        J716fEarlyLogGet32(memory + 8) != J716F_EL_RECORD_BYTES ||
        J716fEarlyLogGet32(memory + 12) != J716F_EL_RECORD_COUNT ||
        J716fEarlyLogGet32(memory + 16) != J716F_EL_TOTAL_BYTES ||
        J716fEarlyLogGet32(memory + 20) == 0 ||
        (source != J716F_EL_SOURCE_PROBE && source != J716F_EL_SOURCE_XNU) ||
        J716fEarlyLogGet32(memory + 28) != 0 ||
        J716fEarlyLogGet64(memory + 32) == 0 ||
        J716fEarlyLogGet64(memory + 40) != J716F_EL_BASE ||
        J716fEarlyLogGet32(memory + 48) != J716F_EL_REGION_BYTES ||
        J716fEarlyLogGet32(memory + 52) != 0 ||
        !J716fEarlyLogTagValid(memory + 56)) return J716F_EL_BAD_HEADER;
    for (i = 88; i != 124; ++i)
        if (memory[i] != 0) return J716F_EL_BAD_HEADER;
    if (J716fEarlyLogGet32(memory + 124) != J716fEarlyLogCrc32(memory, 124))
        return J716F_EL_BAD_HEADER;
    if (out_header) {
        out_header->boot_sequence = J716fEarlyLogGet32(memory + 20);
        out_header->source = source;
        out_header->boot_id = J716fEarlyLogGet64(memory + 32);
        for (i = 0; i != 32; ++i) out_header->build_tag[i] = (char)memory[56 + i];
    }
    return J716F_EL_OK;
}

static inline int
J716fEarlyLogBegin(J716F_EL_STATE *state, volatile J716F_EL_U8 *memory,
    J716F_EL_U64 bytes, J716F_EL_U32 source, J716F_EL_U64 boot_id,
    const char *build_tag)
{
    J716F_EL_U8 header[J716F_EL_HEADER_BYTES];
    J716F_EL_U32 i, tag_length = 0, boot_sequence;
    if (state) state->active = 0;
    if (!state || !memory || bytes < J716F_EL_REGION_BYTES || !build_tag || !boot_id ||
        (source != J716F_EL_SOURCE_PROBE && source != J716F_EL_SOURCE_XNU))
        return J716F_EL_BAD_ARGUMENT;
    while (tag_length < 32 && build_tag[tag_length]) {
        J716F_EL_U8 c = (J716F_EL_U8)build_tag[tag_length];
        if (c < 0x20 || c > 0x7E) return J716F_EL_BAD_ARGUMENT;
        ++tag_length;
    }
    if (!tag_length || tag_length == 32) return J716F_EL_BAD_ARGUMENT;
    boot_sequence = J716fEarlyLogGet32(memory + 12);
    if (J716fEarlyLogGet32(memory) != J716F_EL_JLOG_MAGIC ||
        J716fEarlyLogGet32(memory + 8) != J716F_EL_JLOG_CAPACITY ||
        J716fEarlyLogGet32(memory + 4) > J716F_EL_JLOG_CAPACITY || !boot_sequence ||
        J716fEarlyLogGet32(memory + J716F_EL_OWNER_OFFSET) != J716F_EL_JOWN_MAGIC ||
        J716fEarlyLogGet32(memory + J716F_EL_OWNER_OFFSET + 4) != boot_sequence)
        return J716F_EL_BAD_JLOG;
    for (i = 0; i != J716F_EL_HEADER_BYTES; ++i) header[i] = 0;
    J716fEarlyLogPut32(header, J716F_EL_MAGIC);
    header[4] = J716F_EL_VERSION;
    header[6] = J716F_EL_HEADER_BYTES;
    J716fEarlyLogPut32(header + 8, J716F_EL_RECORD_BYTES);
    J716fEarlyLogPut32(header + 12, J716F_EL_RECORD_COUNT);
    J716fEarlyLogPut32(header + 16, J716F_EL_TOTAL_BYTES);
    J716fEarlyLogPut32(header + 20, boot_sequence);
    J716fEarlyLogPut32(header + 24, source);
    J716fEarlyLogPut64(header + 32, boot_id);
    J716fEarlyLogPut64(header + 40, J716F_EL_BASE);
    J716fEarlyLogPut32(header + 48, J716F_EL_REGION_BYTES);
    for (i = 0; i != tag_length; ++i) header[56 + i] = (J716F_EL_U8)build_tag[i];
    J716fEarlyLogPut32(header + 124, J716fEarlyLogCrc32(header, 124));

    /* Invalidate the epoch before removing any old record's commit. */
    J716fEarlyLogStore32(memory, 0, 0);
    J716F_EL_BARRIER();
    for (i = 0; i != J716F_EL_RECORD_COUNT; ++i)
        J716fEarlyLogStore32(memory, J716F_EL_HEADER_BYTES + i * J716F_EL_RECORD_BYTES + 124, 0);
    for (i = 4; i != J716F_EL_HEADER_BYTES; ++i)
        J716fEarlyLogStore(memory, i, header[i]);
    J716F_EL_BARRIER();
    J716fEarlyLogStore32(memory, 0, J716F_EL_MAGIC);
    J716F_EL_BARRIER();
    state->memory = memory;
    state->boot_id = boot_id;
    state->sequence = 1;
    state->boot_sequence = boot_sequence;
    state->source = source;
    state->tail_length = 0;
    for (i = 0; i != J716F_EL_PAYLOAD_BYTES; ++i) state->tail[i] = 0;
    state->active = 1;
    return J716F_EL_OK;
}

static inline J716F_EL_U32
J716fEarlyLogRecordCommit(J716F_EL_U64 sequence)
{
    return J716F_EL_COMMIT ^ (J716F_EL_U32)sequence ^ (J716F_EL_U32)(sequence >> 32);
}

static inline void
J716fEarlyLogPublish(J716F_EL_STATE *state)
{
    J716F_EL_U8 record[J716F_EL_RECORD_BYTES];
    J716F_EL_U32 i, offset = J716F_EL_HEADER_BYTES +
        (J716F_EL_U32)((state->sequence - 1) % J716F_EL_RECORD_COUNT) * J716F_EL_RECORD_BYTES;
    for (i = 0; i != J716F_EL_RECORD_BYTES; ++i) record[i] = 0;
    J716fEarlyLogPut64(record, state->sequence);
    J716fEarlyLogPut32(record + 8, state->tail_length);
    for (i = 0; i != state->tail_length; ++i) record[16 + i] = state->tail[i];
    J716fEarlyLogPut32(record + 120, J716fEarlyLogCrc32(record, 120));
    J716fEarlyLogStore32(state->memory, offset + 124, 0);
    J716F_EL_BARRIER();
    for (i = 0; i != 124; ++i) J716fEarlyLogStore(state->memory, offset + i, record[i]);
    J716F_EL_BARRIER();
    J716fEarlyLogStore32(state->memory, offset + 124, J716fEarlyLogRecordCommit(state->sequence));
    J716F_EL_BARRIER();
}

static inline int
J716fEarlyLogAppend(J716F_EL_STATE *state, const void *data, J716F_EL_U64 bytes)
{
    const J716F_EL_U8 *input = (const J716F_EL_U8 *)data;
    J716F_EL_U64 remaining = bytes, additional = 0;
    J716F_EL_U32 available, n, i;
    if (!state) return J716F_EL_BAD_ARGUMENT;
    if (!state->active || !state->memory || !state->sequence ||
        state->tail_length > J716F_EL_PAYLOAD_BYTES) return J716F_EL_INACTIVE;
    if (J716fEarlyLogGet32(state->memory) != J716F_EL_MAGIC ||
        J716fEarlyLogGet64(state->memory + 32) != state->boot_id ||
        J716fEarlyLogGet32(state->memory + 20) != state->boot_sequence ||
        J716fEarlyLogGet32(state->memory + 24) != state->source) return J716F_EL_INACTIVE;
    if (!bytes) return J716F_EL_OK;
    if (!input) return J716F_EL_BAD_ARGUMENT;
    if (bytes > J716F_EL_MAX_APPEND) return J716F_EL_OVERFLOW;
    available = J716F_EL_PAYLOAD_BYTES - state->tail_length;
    if (bytes > available) {
        J716F_EL_U64 beyond = bytes - available;
        additional = beyond / J716F_EL_PAYLOAD_BYTES + (beyond % J716F_EL_PAYLOAD_BYTES != 0);
    }
    if (additional > J716F_EL_MAX_U64 - state->sequence) return J716F_EL_OVERFLOW;
    /* Zero is reserved for an uncommitted slot. The specified XOR commit has
     * one unrepresentable sequence per high 32-bit word. MAX_APPEND bounds
     * this search to at most two words, without touching caller data or RAM.
     */
    {
        J716F_EL_U64 last = state->sequence + additional;
        J716F_EL_U64 high = state->sequence >> 32;
        J716F_EL_U64 last_high = last >> 32;
        for (;;) {
            J716F_EL_U64 zero_commit_sequence = (high << 32) |
                (J716F_EL_U64)((J716F_EL_U32)high ^ J716F_EL_COMMIT);
            if (zero_commit_sequence >= state->sequence && zero_commit_sequence <= last)
                return J716F_EL_OVERFLOW;
            if (high == last_high) break;
            ++high;
        }
    }
    while (remaining) {
        if (state->tail_length == J716F_EL_PAYLOAD_BYTES) {
            ++state->sequence;
            state->tail_length = 0;
        }
        available = J716F_EL_PAYLOAD_BYTES - state->tail_length;
        n = remaining < available ? (J716F_EL_U32)remaining : available;
        for (i = 0; i != n; ++i) state->tail[state->tail_length + i] = *input++;
        state->tail_length += n;
        remaining -= n;
        J716fEarlyLogPublish(state);
    }
    return J716F_EL_OK;
}

/* Returns 1 for a valid record, 0 for an uncommitted slot, -1 for corruption. */
static inline int
J716fEarlyLogRecordValidate(const volatile J716F_EL_U8 *record,
    J716F_EL_U32 slot, J716F_EL_U64 *out_sequence, J716F_EL_U32 *out_length)
{
    J716F_EL_U64 sequence;
    J716F_EL_U32 length, commit, i;
    if (!record || slot >= J716F_EL_RECORD_COUNT) return -1;
    commit = J716fEarlyLogGet32(record + 124);
    if (!commit) return 0;
    sequence = J716fEarlyLogGet64(record);
    length = J716fEarlyLogGet32(record + 8);
    if (!sequence || slot >= J716F_EL_RECORD_COUNT ||
        (sequence - 1) % J716F_EL_RECORD_COUNT != slot ||
        length == 0 || length > J716F_EL_PAYLOAD_BYTES ||
        J716fEarlyLogGet32(record + 12) != 0 ||
        commit != J716fEarlyLogRecordCommit(sequence)) return -1;
    for (i = length; i != J716F_EL_PAYLOAD_BYTES; ++i)
        if (record[16 + i] != 0) return -1;
    if (J716fEarlyLogGet32(record + 120) != J716fEarlyLogCrc32(record, 120)) return -1;
    if (out_sequence) *out_sequence = sequence;
    if (out_length) *out_length = length;
    return 1;
}

static inline int
J716fEarlyLogSnapshot(const volatile J716F_EL_U8 *memory, J716F_EL_U64 bytes,
    J716F_EL_EXPORT callback, void *context, J716F_EL_REPORT *out_report)
{
    J716F_EL_REPORT report = {0};
    J716F_EL_U8 record[J716F_EL_RECORD_BYTES];
    J716F_EL_U32 slot, i, n, length, offset;
    J716F_EL_U64 sequence, expected;
    int result;
    if (!memory || bytes < J716F_EL_TOTAL_BYTES) return J716F_EL_BAD_ARGUMENT;
    result = J716fEarlyLogHeaderValidate(memory, bytes, &report.header);
    if (result != J716F_EL_OK) return result;
    for (slot = 0; slot != J716F_EL_RECORD_COUNT; ++slot) {
        offset = J716F_EL_HEADER_BYTES + slot * J716F_EL_RECORD_BYTES;
        for (i = 0; i != J716F_EL_RECORD_BYTES; ++i) record[i] = memory[offset + i];
        result = J716fEarlyLogRecordValidate(record, slot, &sequence, &length);
        if (result == 0) ++report.empty_records;
        else if (result < 0) ++report.invalid_records;
        else {
            ++report.valid_records;
            if (sequence > report.highest_sequence) report.highest_sequence = sequence;
        }
    }
    if (report.highest_sequence) {
        report.first_sequence = report.highest_sequence >= J716F_EL_RECORD_COUNT ?
            report.highest_sequence - (J716F_EL_RECORD_COUNT - 1) : 1;
        n = (J716F_EL_U32)(report.highest_sequence - report.first_sequence + 1);
        /* Bounded by 240 even if a valid record has sequence UINT64_MAX. */
        for (i = 0; i != n; ++i) {
            J716F_EL_U32 byte;
            expected = report.first_sequence + i;
            slot = (J716F_EL_U32)((expected - 1) % J716F_EL_RECORD_COUNT);
            offset = J716F_EL_HEADER_BYTES + slot * J716F_EL_RECORD_BYTES;
            for (byte = 0; byte != J716F_EL_RECORD_BYTES; ++byte) record[byte] = memory[offset + byte];
            result = J716fEarlyLogRecordValidate(record, slot, &sequence, &length);
            if (result != 1 || sequence != expected) { ++report.gaps; continue; }
            if (callback && callback(context, sequence, record + 16, length)) {
                if (out_report) *out_report = report;
                return J716F_EL_CALLBACK_STOP;
            }
            ++report.exported_records;
            report.exported_bytes += length;
        }
    }
    if (out_report) *out_report = report;
    return J716F_EL_OK;
}

#endif
