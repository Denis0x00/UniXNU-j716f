/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2026 daimile2 and contributors. All rights reserved.
 * Copyright (c) 2026 UniXNU contributors. All rights reserved.
 * Native Type-0 capability publication for TB-J716F iPadOS 17.7.11 (21H461) arm64
 * bluetoothd (non-PAC). Adapted from UniXNU QcaBluetooth/userland/Type0.c. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef QBT_LAYOUT_HEADER
#include QBT_LAYOUT_HEADER
#else
#include "qbt_ios17_layout.h"
#endif

#ifdef QBT_TYPE0_TEST
extern void *qbt_test_memory(uintptr_t);
extern void *qbt_test_function(uintptr_t);
#define AT(address) qbt_test_memory(address)
#define FN(address, type) ((type)qbt_test_function(address))
#define INFO(...) ((void)0)
#else
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <os/log.h>
#include <stdlib.h>
static intptr_t slide;
static os_log_t logger;
#define AT(address) ((void *)((uintptr_t)(address) + slide))
#if defined(__has_feature) && __has_feature(ptrauth_calls)
#include <ptrauth.h>
#define FN(address, type) ((type)ptrauth_sign_unauthenticated(AT(address), ptrauth_key_function_pointer, 0))
#else
#define FN(address, type) ((type)AT(address))
#endif
#define INFO(...) os_log(logger, __VA_ARGS__)
#endif

typedef void (*set_bool_fn)(bool);
typedef void (*set_count_fn)(uint32_t);
typedef void *(*model_fn)(void);
typedef void (*advance_fn)(uint32_t, const char *);
typedef void (*complete_fn)(uint32_t, uint32_t);
typedef uint32_t (*submit_fn)(uint32_t, complete_fn);
typedef void (*fail_fn)(void *, uint32_t);

static struct qbt_native_state *state(void) { return AT(QBT_STATE); }
static bool query_pending;
static const char *step(void) { return AT(state()->kind ? QBT_ALL_STEP : QBT_LEGACY_STEP); }

static bool supports(uint16_t opcode)
{
    const uint8_t *descriptors = AT(QBT_DESCRIPTORS);
    const uint8_t *bitmap = AT(QBT_SUPPORT_TABLE);
    for (unsigned index = 0; index < QBT_DESCRIPTOR_COUNT; ++index) {
        uint16_t op, bit;
        memcpy(&op, descriptors + index * 24 + 2, sizeof(op));
        if (op != opcode) continue;
        memcpy(&bit, descriptors + index * 24 + 4, sizeof(bit));
        return (bit >> 8) < QBT_SUPPORT_BYTES && (bitmap[bit >> 8] & (bit & 255));
    }
    return false;
}

static bool support_all(const uint16_t *opcodes, unsigned count)
{
    for (unsigned index = 0; index < count; ++index)
        if (!supports(opcodes[index])) return false;
    return true;
}

static void advance(uint32_t status)
{
    FN(QBT_ADVANCE, advance_fn)(status, step());
}

static void cache(uintptr_t address, bool value)
{
    *(uint8_t *)AT(address) = value;
}

/* Native configuration caches can have been populated before HCI discovery.
 * Publish the same standard-command selection to the model, upper caches and
 * lower stack. Vendor scan extensions remain disabled for type zero. */
static void publish_flags(void)
{
    struct qbt_native_state *s = state();
    uint8_t *model = FN(QBT_MODEL, model_fn)();
    model[QBT_MODEL_MULTIPLE_ADV] = s->ext_adv;
    cache(QBT_CACHE_EXT_ADV, s->ext_adv);
    cache(QBT_CACHE_SPEC_ADV, s->spec_adv);
    cache(QBT_CACHE_EXT_SCAN, s->ext_scan);
    cache(QBT_CACHE_ENH_CONNECTION, s->enhanced_connection);
    cache(QBT_CACHE_PERIODIC_ADV, s->periodic_adv);
    cache(QBT_CACHE_PERIODIC_SYNC, s->periodic_sync);
    cache(QBT_CACHE_PAST_SEND, s->past_sender);
    cache(QBT_CACHE_PAST_RECV, s->past_recipient);
    cache(QBT_CACHE_CODED_PHY, s->coded_phy);
    cache(QBT_CACHE_PHY_2M, s->phy_2m);
    FN(QBT_SET_EXT_ADV, set_bool_fn)(s->ext_adv);
    FN(QBT_SET_SPEC_ADV, set_bool_fn)(s->spec_adv);
    FN(QBT_SET_ADV_HANDLES, set_count_fn)(s->adv_sets);
    /* This flag starts Apple's scan extension VSC after scan enable, which has no type zero
     * backend. Standard extended scanning is selected only by the HCI flag below. */
    FN(QBT_SET_EXT_SCAN, set_bool_fn)(false);
    /* Selects 0x2041/0x2042 over 0x200b/0x200c. The controller answers Command Disallowed
     * to legacy scan commands once extended advertising commands have been used. */
    FN(QBT_SET_EXT_SCAN_HCI, set_bool_fn)(s->ext_scan);
    FN(QBT_SET_ENH_CONNECTION, set_bool_fn)(s->enhanced_connection);
    FN(QBT_SET_CODED_SCAN, set_bool_fn)(s->coded_phy);
    FN(QBT_SET_PERIODIC_ADV, set_bool_fn)(s->periodic_adv);
    FN(QBT_SET_PERIODIC_SYNC, set_bool_fn)(s->periodic_sync);
    INFO("type0 capabilities: extended scan %u, connection %u, advertising %u sets %u, periodic %u/%u, PHY coded %u 2M %u",
         s->ext_scan, s->enhanced_connection, s->ext_adv, s->adv_sets,
         s->periodic_adv, s->periodic_sync, s->coded_phy, s->phy_2m);
}

static void clear_flags(void)
{
    struct qbt_native_state *s = state();
    memset(&s->ext_adv, 0, offsetof(struct qbt_native_state, reserved_tail) - offsetof(struct qbt_native_state, ext_adv));
    publish_flags();
}

static void advertising_complete(uint32_t status, uint32_t sets)
{
    if (!query_pending) return;
    query_pending = false;
    if (status) {
        clear_flags();
        advance(status);
        return;
    }
    struct qbt_native_state *s = state();
    s->adv_sets = sets < QBT_ADV_CAPACITY ? sets : QBT_ADV_CAPACITY;
    if (!s->adv_sets) {
        s->ext_adv = s->spec_adv = s->periodic_adv = 0;
    }
    publish_flags();
    advance(0);
}

static void publish(void)
{
    struct qbt_native_state *s = state();
    if (s->status || !s->parameters) {
        clear_flags();
        advance(s->status ? s->status : 0x67);
        return;
    }
    static const uint16_t advertising[] = {0x2035, 0x2036, 0x2037, 0x2038, 0x2039, 0x203a, 0x203b, 0x203c, 0x203d};
    static const uint16_t scanning[] = {0x2041, 0x2042};
    static const uint16_t periodic_adv[] = {0x203e, 0x203f, 0x2040};
    static const uint16_t periodic_sync[] = {0x2044, 0x2045, 0x2046};
    static const uint16_t past_sender[] = {0x205a, 0x205b};
    static const uint16_t past_recipient[] = {0x2059, 0x205c, 0x205d};
    uint64_t features;
    memcpy(&features, AT(QBT_LE_FEATURES), sizeof(features));
    clear_flags();
    s->ext_adv = s->spec_adv = (features >> 12 & 1) && support_all(advertising, 9);
    s->ext_scan = support_all(scanning, 2);
    s->enhanced_connection = supports(0x2043);
    s->periodic_adv = s->ext_adv && (features >> 13 & 1) && support_all(periodic_adv, 3);
    s->periodic_sync = (features >> 13 & 1) && support_all(periodic_sync, 3);
    s->past_sender = (features >> 24 & 1) && support_all(past_sender, 2);
    s->past_recipient = (features >> 25 & 1) && support_all(past_recipient, 3);
    s->coded_phy = s->ext_scan && (features >> 11 & 1);
    s->phy_2m = (features >> 8 & 1) && supports(0x2030) && supports(0x2032);
    if (!s->ext_adv) {
        publish_flags();
        advance(0);
        return;
    }
    query_pending = true;
    volatile uint8_t *flag = AT(QBT_INIT_SUBMIT_FLAG);
    uint8_t previous = *flag;
    *flag = 1;
    uint32_t error = FN(QBT_SUBMIT, submit_fn)(QBT_ID_ADV_SETS, advertising_complete);
    *flag = previous;
    if (error) {
        query_pending = false;
        clear_flags();
        FN(QBT_CHAIN_FAIL, fail_fn)(*(void **)AT(QBT_CHAIN_CONTEXT), error);
    }
}

static void prepare(void *broadcaster)
{
    uint32_t mode = state()->ext_adv ? 0 : 1;
    memcpy((uint8_t *)broadcaster + QBT_ADV_MODE, &mode, sizeof(mode));
}

#ifndef QBT_TYPE0_TEST
__attribute__((constructor)) static void initialize(void)
{
    static const uint8_t expected[16] = QBT_NATIVE_UUID;
    logger = os_log_create("org.unixnu.bluetooth", "Type0");
    const struct mach_header_64 *header = (const void *)_dyld_get_image_header(0);
    const uint8_t *cursor = (const void *)(header + 1);
    const uint8_t *end = cursor + header->sizeofcmds;
    bool match = false;
    for (uint32_t index = 0; index < header->ncmds; ++index) {
        if ((size_t)(end - cursor) < sizeof(struct load_command)) abort();
        const struct load_command *command = (const void *)cursor;
        if (command->cmdsize < sizeof(*command) || command->cmdsize > (size_t)(end - cursor)) abort();
        if (command->cmd == LC_UUID && command->cmdsize == sizeof(struct uuid_command))
            match = !memcmp(((const struct uuid_command *)command)->uuid, expected, sizeof(expected));
        cursor += command->cmdsize;
    }
    if (!match) abort();
    slide = _dyld_get_image_vmaddr_slide(0);
    memset(AT(QBT_STATE), 0, QBT_STATE_BYTES);
    state()->publish = publish;
    state()->prepare = prepare;
}
#endif
