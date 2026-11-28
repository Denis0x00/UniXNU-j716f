/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* Host ASan/UBSan test for TB-J716F iPadOS 17.7.11 (21H461) Type0 capability publisher.
 *
 * Usage:
 *   test_bt_type0                       -> uses built-in synthetic 265-entry descriptor table (for public CI)
 *   test_bt_type0 --synthetic           -> same as above
 *   test_bt_type0 <descriptors-21H461>  -> validates against real 21H461 bluetoothd descriptor table
 */
#define QBT_TYPE0_TEST 1
#define QBT_LAYOUT_HEADER "qbt_ios17_layout.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../bt/Type0.c"

#define BASE 0x100000000ULL
#define MEMORY_BYTES 0x950000u

static uint8_t *memory_image;
static uint8_t model_object[0x6d0];

static struct {
    bool ext_adv, spec_adv, scan, scan_hci, connection, coded, periodic_adv, periodic_sync;
    uint32_t count, submit_error, submit_calls, advances, status, failures;
    const char *step;
    void *context;
    complete_fn complete;
} observed;

void *qbt_test_memory(uintptr_t address)
{
    assert(address >= BASE && address < BASE + MEMORY_BYTES);
    return memory_image + address - BASE;
}

static void set_ext(bool value) { observed.ext_adv = value; }
static void set_spec(bool value) { observed.spec_adv = value; }
static void set_scan(bool value) { observed.scan = value; }
static void set_scan_hci(bool value) { observed.scan_hci = value; }
static void set_conn(bool value) { observed.connection = value; }
static void set_coded(bool value) { observed.coded = value; }
static void set_per_adv(bool value) { observed.periodic_adv = value; }
static void set_per_sync(bool value) { observed.periodic_sync = value; }
static void set_handles(uint32_t count) { observed.count = count; }
static void *model(void) { return model_object; }

static void next(uint32_t status, const char *step_name)
{
    ++observed.advances;
    observed.status = status;
    observed.step = step_name;
}

static uint32_t submit(uint32_t id, complete_fn complete)
{
    assert(id == QBT_ID_ADV_SETS && *(uint8_t *)AT(QBT_INIT_SUBMIT_FLAG) == 1);
    ++observed.submit_calls;
    observed.complete = complete;
    return observed.submit_error;
}

static void fail(void *context, uint32_t status)
{
    ++observed.failures;
    observed.context = context;
    observed.status = status;
}

void *qbt_test_function(uintptr_t address)
{
    switch (address) {
    case QBT_MODEL: return model;
    case QBT_ADVANCE: return next;
    case QBT_SUBMIT: return submit;
    case QBT_CHAIN_FAIL: return fail;
    case QBT_SET_EXT_ADV: return set_ext;
    case QBT_SET_SPEC_ADV: return set_spec;
    case QBT_SET_ADV_HANDLES: return set_handles;
    case QBT_SET_EXT_SCAN: return set_scan;
    case QBT_SET_EXT_SCAN_HCI: return set_scan_hci;
    case QBT_SET_ENH_CONNECTION: return set_conn;
    case QBT_SET_CODED_SCAN: return set_coded;
    case QBT_SET_PERIODIC_ADV: return set_per_adv;
    case QBT_SET_PERIODIC_SYNC: return set_per_sync;
    default: assert(!"unmapped native function"); return NULL;
    }
}

static const uint16_t required[] = {
    0x2035, 0x2036, 0x2037, 0x2038, 0x2039, 0x203a, 0x203b, 0x203c, 0x203d,
    0x2041, 0x2042, 0x2043, 0x203e, 0x203f, 0x2040, 0x2044, 0x2045, 0x2046,
    0x205a, 0x205b, 0x2059, 0x205c, 0x205d, 0x2030, 0x2032,
};

#define FULL_FEATURES ((1ULL << 8) | (1ULL << 11) | (1ULL << 12) | (1ULL << 13) | (1ULL << 24) | (1ULL << 25))

static void populate_synthetic_descriptors(void)
{
    uint8_t *base = AT(QBT_DESCRIPTORS);
    memset(base, 0, QBT_DESCRIPTOR_COUNT * 24);
    /* Place each required opcode at a distinct descriptor slot & distinct support bitmap bit. */
    for (unsigned i = 0; i < sizeof(required) / sizeof(required[0]); ++i) {
        unsigned slot = (required[i] == 0x203b) ? (unsigned)QBT_ID_ADV_SETS : (10u + i);
        uint16_t op = required[i];
        uint16_t bit = (required[i] == 0x203b)
                           ? 0x2480u
                           : (uint16_t)(((38u + (i / 8u)) << 8) | (1u << (i % 8u)));
        memcpy(base + slot * 24 + 2, &op, sizeof(op));
        memcpy(base + slot * 24 + 4, &bit, sizeof(bit));
    }
}

static void support(uint16_t opcode, bool enabled)
{
    const uint8_t *d = AT(QBT_DESCRIPTORS);
    for (unsigned index = 0; index < QBT_DESCRIPTOR_COUNT; ++index, d += 24) {
        uint16_t op, bit;
        memcpy(&op, d + 2, 2);
        if (op != opcode) continue;
        memcpy(&bit, d + 4, 2);
        assert((bit >> 8) < QBT_SUPPORT_BYTES && (bit & 255));
        uint8_t *value = (uint8_t *)AT(QBT_SUPPORT_TABLE) + (bit >> 8);
        if (enabled) *value |= (uint8_t)(bit & 255);
        else *value &= (uint8_t)~(bit & 255);
        return;
    }
    assert(!"missing native opcode descriptor");
}

static void reset(uint64_t features, bool commands)
{
    memset(&observed, 0, sizeof(observed));
    memset(model_object, 0, sizeof(model_object));
    memset(AT(QBT_STATE), 0, QBT_STATE_BYTES);
    memset(AT(QBT_SUPPORT_TABLE), 0, QBT_SUPPORT_BYTES);
    memcpy(AT(QBT_LE_FEATURES), &features, 8);
    *(uint8_t *)AT(QBT_INIT_SUBMIT_FLAG) = 0;
    *(void **)AT(QBT_CHAIN_CONTEXT) = model_object;
    state()->parameters = 0x1234;
    query_pending = false;
    if (commands) {
        for (unsigned index = 0; index < sizeof(required) / sizeof(required[0]); ++index)
            support(required[index], true);
    }
}

static void consistent(void)
{
    struct qbt_native_state *s = state();
    assert(observed.ext_adv == s->ext_adv && observed.spec_adv == s->spec_adv);
    assert(!observed.scan && observed.scan_hci == s->ext_scan && observed.connection == s->enhanced_connection);
    assert(observed.coded == s->coded_phy);
    assert(observed.periodic_adv == s->periodic_adv && observed.periodic_sync == s->periodic_sync);
    assert(observed.count == s->adv_sets && s->adv_sets <= QBT_ADV_CAPACITY);
    assert(model_object[QBT_MODEL_MULTIPLE_ADV] == s->ext_adv);
    assert(*(uint8_t *)AT(QBT_CACHE_EXT_ADV) == s->ext_adv);
    assert(*(uint8_t *)AT(QBT_CACHE_SPEC_ADV) == s->spec_adv);
    assert(*(uint8_t *)AT(QBT_CACHE_EXT_SCAN) == s->ext_scan);
    assert(*(uint8_t *)AT(QBT_CACHE_ENH_CONNECTION) == s->enhanced_connection);
    assert(*(uint8_t *)AT(QBT_CACHE_PERIODIC_ADV) == s->periodic_adv);
    assert(*(uint8_t *)AT(QBT_CACHE_PERIODIC_SYNC) == s->periodic_sync);
    assert(*(uint8_t *)AT(QBT_CACHE_PAST_SEND) == s->past_sender);
    assert(*(uint8_t *)AT(QBT_CACHE_PAST_RECV) == s->past_recipient);
    assert(*(uint8_t *)AT(QBT_CACHE_CODED_PHY) == s->coded_phy);
    assert(*(uint8_t *)AT(QBT_CACHE_PHY_2M) == s->phy_2m);
}

static void response(uint32_t status, uint32_t count)
{
    assert(observed.complete && query_pending && !observed.advances);
    observed.complete(status, count);
    assert(!query_pending && observed.advances == 1);
    /* Duplicate completion must be ignored. */
    observed.complete(status, count);
    assert(observed.advances == 1);
    consistent();
}

int main(int argc, char **argv)
{
    memory_image = calloc(1, MEMORY_BYTES);
    assert(memory_image);

    const char *mode = "synthetic";
    if (argc == 2 && strcmp(argv[1], "--synthetic") != 0) {
        FILE *input = fopen(argv[1], "rb");
        assert(input);
        assert(fread(AT(QBT_DESCRIPTORS), 24, QBT_DESCRIPTOR_COUNT, input) == QBT_DESCRIPTOR_COUNT);
        assert(fgetc(input) == EOF);
        fclose(input);
        mode = argv[1];
    } else {
        assert(argc == 1 || (argc == 2 && strcmp(argv[1], "--synthetic") == 0));
        populate_synthetic_descriptors();
    }

    unsigned cases = 0;

    /* Baseline: zero features, zero commands. */
    reset(0, false);
    publish();
    assert(!observed.submit_calls && observed.advances == 1 && observed.status == 0);
    assert(!state()->ext_adv && !state()->ext_scan && !state()->phy_2m);
    consistent();
    ++cases;

    /* Capacity clamp against iOS 17 ADV_CAPACITY == 5 and INIT_SUBMIT_FLAG preservation. */
    for (unsigned kind = 0; kind < 2; ++kind) {
        for (unsigned prior = 0; prior < 2; ++prior) {
            const uint32_t capacities[] = {0, 1, 5, 6, 16, 255};
            for (unsigned index = 0; index < sizeof(capacities) / sizeof(capacities[0]); ++index) {
                reset(FULL_FEATURES, true);
                state()->kind = kind;
                *(uint8_t *)AT(QBT_INIT_SUBMIT_FLAG) = (uint8_t)prior;
                publish();
                assert(observed.submit_calls == 1 && !observed.advances);
                assert(*(uint8_t *)AT(QBT_INIT_SUBMIT_FLAG) == prior);
                response(0, capacities[index]);
                assert(observed.status == 0 && !observed.failures);
                uint32_t expected_sets = capacities[index] > QBT_ADV_CAPACITY
                                             ? (uint32_t)QBT_ADV_CAPACITY
                                             : capacities[index];
                assert(observed.count == expected_sets);
                assert(observed.step == AT(kind ? QBT_ALL_STEP : QBT_LEGACY_STEP));
                assert(state()->ext_adv == (capacities[index] != 0));
                assert(state()->ext_scan && state()->enhanced_connection && state()->phy_2m);
                ++cases;
            }
        }
    }

    /* Missing each of the 25 required HCI opcodes individually. */
    for (unsigned index = 0; index < sizeof(required) / sizeof(required[0]); ++index) {
        uint16_t missing = required[index];
        reset(FULL_FEATURES, true);
        support(missing, false);
        publish();
        bool adv = !(missing >= 0x2035 && missing <= 0x203d);
        if (adv) response(0, (uint32_t)QBT_ADV_CAPACITY);
        else assert(!observed.submit_calls && observed.advances == 1);
        assert(state()->ext_adv == adv);
        assert(state()->periodic_adv == (adv && !(missing >= 0x203e && missing <= 0x2040)));
        assert(state()->ext_scan == (missing != 0x2041 && missing != 0x2042));
        assert(state()->enhanced_connection == (missing != 0x2043));
        assert(state()->periodic_sync == !(missing >= 0x2044 && missing <= 0x2046));
        assert(state()->past_sender == (missing != 0x205a && missing != 0x205b));
        assert(state()->past_recipient == (missing != 0x2059 && missing != 0x205c && missing != 0x205d));
        assert(state()->coded_phy == state()->ext_scan);
        assert(state()->phy_2m == (missing != 0x2030 && missing != 0x2032));
        consistent();
        ++cases;
    }

    /* Missing each of the 6 LE feature bits individually. */
    const unsigned feature_bits[] = {8, 11, 12, 13, 24, 25};
    for (unsigned index = 0; index < 6; ++index) {
        unsigned missing = feature_bits[index];
        reset(FULL_FEATURES & ~(1ULL << missing), true);
        publish();
        if (missing != 12) response(0, (uint32_t)QBT_ADV_CAPACITY);
        assert(state()->ext_adv == (missing != 12));
        assert(state()->periodic_adv == (missing != 12 && missing != 13));
        assert(state()->periodic_sync == (missing != 13));
        assert(state()->past_sender == (missing != 24));
        assert(state()->past_recipient == (missing != 25));
        assert(state()->coded_phy == (missing != 11));
        assert(state()->phy_2m == (missing != 8));
        consistent();
        ++cases;
    }

    /* Non-zero HCI status or NULL parameters pointer clears flags and advances with error. */
    for (unsigned status = 0; status < 2; ++status) {
        reset(FULL_FEATURES, true);
        state()->status = status ? 0x12 : 0;
        state()->parameters = status ? 0x1234 : 0;
        publish();
        assert(observed.advances == 1 && observed.status == (status ? 0x12u : 0x67u));
        assert(!observed.submit_calls && !state()->ext_adv);
        consistent();
        ++cases;
    }

    /* Async advertising-sets response error clears flags and advances with error. */
    reset(FULL_FEATURES, true);
    publish();
    response(0x12, (uint32_t)QBT_ADV_CAPACITY);
    assert(observed.status == 0x12 && !state()->ext_scan);
    ++cases;

    /* Synchronous submit failure triggers chain_fail and clears state. */
    reset(FULL_FEATURES, true);
    observed.submit_error = 0x198;
    publish();
    assert(observed.failures == 1 && observed.context == model_object && observed.status == 0x198);
    assert(!observed.advances && !query_pending && !state()->ext_adv);
    assert(*(uint8_t *)AT(QBT_INIT_SUBMIT_FLAG) == 0);
    consistent();
    ++cases;

    /* LeBroadcaster::stackDidStart prepare() writes only [broadcaster + 0x88]. */
    uint8_t broadcaster[256];
    for (unsigned enabled = 0; enabled < 2; ++enabled) {
        memset(broadcaster, 0xa5, sizeof(broadcaster));
        state()->ext_adv = (uint8_t)enabled;
        prepare(broadcaster);
        uint32_t mode_val;
        memcpy(&mode_val, broadcaster + QBT_ADV_MODE, 4);
        assert(mode_val == (enabled ? 0u : 1u));
        for (unsigned index = 0; index < sizeof(broadcaster); ++index) {
            if (index < QBT_ADV_MODE || index >= QBT_ADV_MODE + 4)
                assert(broadcaster[index] == 0xa5);
        }
        ++cases;
    }

    free(memory_image);
    printf("J716F 21H461 Type0 capability publication (%s): %u cases passed\n", mode, cases);
    return 0;
}
