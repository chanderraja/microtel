/* Copyright (c) 2026 The microtel Authors.
 * SPDX-License-Identifier: Apache-2.0
 */

#include "leaf_vectors.h"

#include <string.h>

/* The record buffer every vector builds in. The Cortex-M0 target runner
 * (tests/leaf/target/), which has 16 KiB of RAM, builds this file with a
 * smaller one; the largest vector, max_strings, needs a little over 3 KiB. */
#ifndef MICROTEL_LEAF_VECTORS_RECORD_BYTES
#define MICROTEL_LEAF_VECTORS_RECORD_BYTES 16384
#endif

enum
{
    RECORD_BUFFER_SIZE = MICROTEL_LEAF_VECTORS_RECORD_BYTES,
    LONG_STRING_SIZE = 1000,
    TRACE_ID_SIZE = 16,
    SPAN_ID_SIZE = 8,
    MAX_STRINGS_ATTRS = 16
};

#define CLOCK_START 1000000000ULL
#define CLOCK_STEP 1000ULL
#define RANDOM_SEED 0x9e3779b97f4a7c15ULL
#define SYNC_UNIX_NS 1700000000000000000ULL
#define VECTOR_BOOT_ID 77u

/* Fixed clock: every read advances by CLOCK_STEP. */
static uint64_t g_clock_now;

static uint64_t read_clock(void* ctx)
{
    const uint64_t t = g_clock_now;
    (void)ctx;
    g_clock_now += CLOCK_STEP;
    return t;
}

/* Fixed random source: xorshift64 from a constant seed. */
static uint64_t g_random_state;

static void fill_random(void* ctx, uint8_t* out, size_t len)
{
    size_t i;
    (void)ctx;
    for (i = 0; i < len; ++i)
    {
        g_random_state ^= g_random_state << 13u;
        g_random_state ^= g_random_state >> 7u;
        g_random_state ^= g_random_state << 17u;
        out[i] = (uint8_t)(g_random_state & 0xffu);
    }
}

static microtel_leaf_kv_t str_kv(const char* key, const char* value, size_t value_len)
{
    microtel_leaf_kv_t kv;
    memset(&kv, 0, sizeof(kv));
    kv.key = key;
    kv.key_len = strlen(key);
    kv.type = MICROTEL_LEAF_VALUE_STRING;
    kv.value.s.ptr = value;
    kv.value.s.len = value_len;
    return kv;
}

static microtel_leaf_kv_t int_kv(const char* key, int64_t value)
{
    microtel_leaf_kv_t kv;
    memset(&kv, 0, sizeof(kv));
    kv.key = key;
    kv.key_len = strlen(key);
    kv.type = MICROTEL_LEAF_VALUE_INT64;
    kv.value.i = value;
    return kv;
}

static microtel_leaf_kv_t double_kv(const char* key, double value)
{
    microtel_leaf_kv_t kv;
    memset(&kv, 0, sizeof(kv));
    kv.key = key;
    kv.key_len = strlen(key);
    kv.type = MICROTEL_LEAF_VALUE_DOUBLE;
    kv.value.d = value;
    return kv;
}

static microtel_leaf_kv_t bool_kv(const char* key, int value)
{
    microtel_leaf_kv_t kv;
    memset(&kv, 0, sizeof(kv));
    kv.key = key;
    kv.key_len = strlen(key);
    kv.type = MICROTEL_LEAF_VALUE_BOOL;
    kv.value.b = value;
    return kv;
}

/* Starts, optionally decorates, and ends one span. */
static microtel_leaf_status_t simple_span(microtel_leaf_t* leaf,
                                          const char* name,
                                          microtel_leaf_span_t* out)
{
    microtel_leaf_status_t st = microtel_leaf_span_start(
        leaf, out, name, strlen(name), MICROTEL_LEAF_SPAN_KIND_INTERNAL, NULL);
    if (st == MICROTEL_LEAF_OK)
    {
        st = microtel_leaf_span_end(leaf, *out);
    }
    return st;
}

static microtel_leaf_status_t build_empty(microtel_leaf_t* leaf)
{
    (void)leaf;
    return MICROTEL_LEAF_OK;
}

static microtel_leaf_status_t build_one_span(microtel_leaf_t* leaf)
{
    microtel_leaf_span_t s = 0;
    return simple_span(leaf, "sensor.read", &s);
}

static microtel_leaf_status_t build_attribute_types(microtel_leaf_t* leaf)
{
    microtel_leaf_span_t s = 0;
    microtel_leaf_kv_t kvs[8];
    size_t i;
    microtel_leaf_status_t st =
        microtel_leaf_span_start(leaf, &s, "attrs", 5, MICROTEL_LEAF_SPAN_KIND_CLIENT, NULL);
    kvs[0] = str_kv("s", "text", 4);
    kvs[1] = int_kv("i.pos", 300);
    kvs[2] = int_kv("i.neg", -1);
    kvs[3] = int_kv("i.zero", 0);
    kvs[4] = double_kv("d", 1.5);
    kvs[5] = double_kv("d.negzero", -0.0);
    kvs[6] = bool_kv("b.true", 1);
    kvs[7] = bool_kv("b.false", 0);
    for (i = 0; i < 8u && st == MICROTEL_LEAF_OK; ++i)
    {
        st = microtel_leaf_span_set_attribute(leaf, s, &kvs[i]);
    }
    if (st == MICROTEL_LEAF_OK)
    {
        st = microtel_leaf_span_end(leaf, s);
    }
    return st;
}

static microtel_leaf_status_t build_events(microtel_leaf_t* leaf)
{
    microtel_leaf_span_t s = 0;
    microtel_leaf_kv_t attrs[2];
    microtel_leaf_status_t st = microtel_leaf_span_start(
        leaf, &s, "with.events", 11, MICROTEL_LEAF_SPAN_KIND_PRODUCER, NULL);
    attrs[0] = int_kv("attempt", 2);
    attrs[1] = str_kv("reason", "timeout", 7);
    if (st == MICROTEL_LEAF_OK)
    {
        st = microtel_leaf_span_add_event(leaf, s, "retry", 5, attrs, 2);
    }
    if (st == MICROTEL_LEAF_OK)
    {
        st = microtel_leaf_span_add_event(leaf, s, "done", 4, NULL, 0);
    }
    if (st == MICROTEL_LEAF_OK)
    {
        st = microtel_leaf_span_end(leaf, s);
    }
    return st;
}

static microtel_leaf_status_t status_span(microtel_leaf_t* leaf,
                                          const char* name,
                                          microtel_leaf_status_code_t code,
                                          const char* message)
{
    microtel_leaf_span_t s = 0;
    microtel_leaf_status_t st = microtel_leaf_span_start(
        leaf, &s, name, strlen(name), MICROTEL_LEAF_SPAN_KIND_INTERNAL, NULL);
    if (st == MICROTEL_LEAF_OK)
    {
        st = microtel_leaf_span_set_status(leaf, s, code, message, strlen(message));
    }
    if (st == MICROTEL_LEAF_OK)
    {
        st = microtel_leaf_span_end(leaf, s);
    }
    return st;
}

static microtel_leaf_status_t build_status(microtel_leaf_t* leaf)
{
    microtel_leaf_status_t st = status_span(leaf, "failed", MICROTEL_LEAF_STATUS_ERROR, "boom");
    if (st == MICROTEL_LEAF_OK)
    {
        st = status_span(leaf, "ok", MICROTEL_LEAF_STATUS_OK, "");
    }
    if (st == MICROTEL_LEAF_OK)
    {
        st = status_span(leaf, "unset.msg", MICROTEL_LEAF_STATUS_UNSET, "note");
    }
    return st;
}

static microtel_leaf_status_t build_remote_parent(microtel_leaf_t* leaf)
{
    uint8_t trace_id[TRACE_ID_SIZE];
    uint8_t parent_id[SPAN_ID_SIZE];
    microtel_leaf_span_t remote = 0;
    microtel_leaf_span_t child = 0;
    size_t i;
    microtel_leaf_status_t st;
    for (i = 0; i < sizeof(trace_id); ++i)
    {
        trace_id[i] = (uint8_t)(0xa0u + i);
    }
    for (i = 0; i < sizeof(parent_id); ++i)
    {
        parent_id[i] = (uint8_t)(0xb0u + i);
    }
    st = microtel_leaf_span_start_remote(
        leaf, &remote, "handle.cmd", 10, MICROTEL_LEAF_SPAN_KIND_SERVER, trace_id, parent_id);
    if (st == MICROTEL_LEAF_OK)
    {
        st = microtel_leaf_span_start(
            leaf, &child, "actuate", 7, MICROTEL_LEAF_SPAN_KIND_INTERNAL, &remote);
    }
    if (st == MICROTEL_LEAF_OK)
    {
        st = microtel_leaf_span_end(leaf, child);
    }
    if (st == MICROTEL_LEAF_OK)
    {
        st = microtel_leaf_span_end(leaf, remote);
    }
    return st;
}

static microtel_leaf_status_t build_sync_relative(microtel_leaf_t* leaf)
{
    microtel_leaf_span_t s = 0;
    microtel_leaf_status_t st = microtel_leaf_clock_sync(leaf, SYNC_UNIX_NS, CLOCK_START);
    if (st == MICROTEL_LEAF_OK)
    {
        st = simple_span(leaf, "synced", &s);
    }
    return st;
}

static microtel_leaf_status_t build_dropped_counters(microtel_leaf_t* leaf)
{
    microtel_leaf_span_t s = 0;
    microtel_leaf_kv_t kv = int_kv("a", 1);
    microtel_leaf_kv_t kv2 = int_kv("b", 2);
    microtel_leaf_status_t st =
        microtel_leaf_span_start(leaf, &s, "capped", 6, MICROTEL_LEAF_SPAN_KIND_INTERNAL, NULL);
    if (st == MICROTEL_LEAF_OK)
    {
        st = microtel_leaf_span_set_attribute(leaf, s, &kv);
    }
    /* The cap is 1: these two are dropped and counted. */
    if (st == MICROTEL_LEAF_OK &&
        microtel_leaf_span_set_attribute(leaf, s, &kv2) == MICROTEL_LEAF_ERR_LIMIT &&
        microtel_leaf_span_add_event(leaf, s, "e", 1, NULL, 0) == MICROTEL_LEAF_OK &&
        microtel_leaf_span_add_event(leaf, s, "f", 1, NULL, 0) == MICROTEL_LEAF_ERR_LIMIT)
    {
        st = microtel_leaf_span_end(leaf, s);
    }
    return st;
}

static char g_long_a[LONG_STRING_SIZE];
static char g_long_b[LONG_STRING_SIZE];

static microtel_leaf_status_t build_max_strings(microtel_leaf_t* leaf)
{
    static const char* const keys[MAX_STRINGS_ATTRS] = {"k00",
                                                        "k01",
                                                        "k02",
                                                        "k03",
                                                        "k04",
                                                        "k05",
                                                        "k06",
                                                        "k07",
                                                        "k08",
                                                        "k09",
                                                        "k10",
                                                        "k11",
                                                        "k12",
                                                        "k13",
                                                        "k14",
                                                        "k15"};
    microtel_leaf_span_t s = 0;
    size_t i;
    microtel_leaf_status_t st;
    memset(g_long_a, 'a', sizeof(g_long_a));
    memset(g_long_b, 'b', sizeof(g_long_b));
    st = microtel_leaf_span_start(
        leaf, &s, g_long_a, sizeof(g_long_a), MICROTEL_LEAF_SPAN_KIND_INTERNAL, NULL);
    for (i = 0; i < MAX_STRINGS_ATTRS && st == MICROTEL_LEAF_OK; ++i)
    {
        const microtel_leaf_kv_t kv = str_kv(keys[i], g_long_b, i == 0u ? sizeof(g_long_b) : 1u);
        st = microtel_leaf_span_set_attribute(leaf, s, &kv);
    }
    if (st == MICROTEL_LEAF_OK)
    {
        st = microtel_leaf_span_set_status(
            leaf, s, MICROTEL_LEAF_STATUS_ERROR, g_long_b, sizeof(g_long_b));
    }
    if (st == MICROTEL_LEAF_OK)
    {
        st = microtel_leaf_span_end(leaf, s);
    }
    return st;
}

/* UTF-8 edge cases: 2-, 3- and 4-byte sequences, U+FFFD and U+10FFFF. */
static const char k_utf8_name[] = "caf\xc3\xa9 \xe6\x97\xa5\xe6\x9c\xac \xf0\x9f\x98\x80";
static const char k_utf8_value[] = "\xef\xbf\xbd\xf4\x8f\xbf\xbf";

static microtel_leaf_status_t build_utf8(microtel_leaf_t* leaf)
{
    microtel_leaf_span_t s = 0;
    const microtel_leaf_kv_t kv = str_kv("\xc3\xbc", k_utf8_value, sizeof(k_utf8_value) - 1u);
    microtel_leaf_status_t st = microtel_leaf_span_start(
        leaf, &s, k_utf8_name, sizeof(k_utf8_name) - 1u, MICROTEL_LEAF_SPAN_KIND_CONSUMER, NULL);
    if (st == MICROTEL_LEAF_OK)
    {
        st = microtel_leaf_span_set_attribute(leaf, s, &kv);
    }
    if (st == MICROTEL_LEAF_OK)
    {
        st = microtel_leaf_span_end(leaf, s);
    }
    return st;
}

typedef microtel_leaf_status_t (*build_fn)(microtel_leaf_t* leaf);

typedef struct vector
{
    const char* name;
    microtel_leaf_time_mode_t time_mode;
    int has_clock;
    uint16_t cap; /* 0: defaults; otherwise every per-span cap */
    build_fn build;
} vector;

static const vector k_vectors[] = {
    {"empty_batch", MICROTEL_LEAF_TIME_CONCENTRATOR_STAMPED, 1, 0, build_empty},
    {"one_span", MICROTEL_LEAF_TIME_CONCENTRATOR_STAMPED, 1, 0, build_one_span},
    {"attribute_types", MICROTEL_LEAF_TIME_CONCENTRATOR_STAMPED, 1, 0, build_attribute_types},
    {"events", MICROTEL_LEAF_TIME_CONCENTRATOR_STAMPED, 1, 0, build_events},
    {"status", MICROTEL_LEAF_TIME_CONCENTRATOR_STAMPED, 1, 0, build_status},
    {"remote_parent", MICROTEL_LEAF_TIME_CONCENTRATOR_STAMPED, 1, 0, build_remote_parent},
    {"time_no_clock", MICROTEL_LEAF_TIME_CONCENTRATOR_STAMPED, 0, 0, build_one_span},
    {"time_sync_relative", MICROTEL_LEAF_TIME_SYNC_RELATIVE, 1, 0, build_sync_relative},
    {"time_sync_unsynced", MICROTEL_LEAF_TIME_SYNC_RELATIVE, 1, 0, build_one_span},
    {"time_boot_relative", MICROTEL_LEAF_TIME_BOOT_RELATIVE, 1, 0, build_one_span},
    {"dropped_counters", MICROTEL_LEAF_TIME_CONCENTRATOR_STAMPED, 1, 1, build_dropped_counters},
    {"max_strings", MICROTEL_LEAF_TIME_CONCENTRATOR_STAMPED, 1, 0, build_max_strings},
    {"utf8", MICROTEL_LEAF_TIME_CONCENTRATOR_STAMPED, 1, 0, build_utf8},
};

enum
{
    VECTOR_COUNT = sizeof(k_vectors) / sizeof(k_vectors[0])
};

size_t microtel_leaf_test_vector_count(void)
{
    return VECTOR_COUNT;
}

const char* microtel_leaf_test_vector_name(size_t index)
{
    return index < VECTOR_COUNT ? k_vectors[index].name : NULL;
}

static uint64_t g_record[RECORD_BUFFER_SIZE / sizeof(uint64_t)];

static void make_config(const vector* v, microtel_leaf_config_t* config, microtel_leaf_kv_t* res)
{
    memset(config, 0, sizeof(*config));
    res[0] = str_kv("device.id", "leaf-0042", 9);
    res[1] = str_kv("service.name", "thermostat", 10);
    config->struct_size = (uint32_t)sizeof(*config);
    config->time_mode = v->time_mode;
    config->now_ns = v->has_clock != 0 ? read_clock : NULL;
    config->random_bytes = fill_random;
    config->resource = res;
    config->resource_count = 2;
    config->scope_name = "firmware";
    config->scope_name_len = 8;
    config->scope_version = "2.3.1";
    config->scope_version_len = 5;
    config->max_attributes_per_span = v->cap;
    config->max_events_per_span = v->cap;
    config->max_attributes_per_event = v->cap;
    config->boot_id = VECTOR_BOOT_ID;
}

microtel_leaf_status_t microtel_leaf_test_vector_encode(size_t index,
                                                        uint8_t* out,
                                                        size_t out_size,
                                                        size_t* written)
{
    microtel_leaf_config_t config;
    microtel_leaf_kv_t resource[2];
    microtel_leaf_t leaf;
    microtel_leaf_status_t st;
    if (index >= VECTOR_COUNT)
    {
        return MICROTEL_LEAF_ERR_ARG;
    }
    g_clock_now = CLOCK_START;
    g_random_state = RANDOM_SEED;
    make_config(&k_vectors[index], &config, resource);
    st = microtel_leaf_init(&leaf, sizeof(leaf), &config, g_record, sizeof(g_record));
    if (st == MICROTEL_LEAF_OK)
    {
        st = k_vectors[index].build(&leaf);
    }
    if (st == MICROTEL_LEAF_OK)
    {
        st = microtel_leaf_encode(&leaf, out, out_size, written);
    }
    microtel_leaf_free(&leaf);
    return st;
}
