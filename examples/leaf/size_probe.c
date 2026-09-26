/* Copyright (c) 2026 The microtel Authors.
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * size_probe.c: the smallest useful leaf firmware, for footprint measurement
 * (docs/leaf-concentrator-design.md §7.6, ICP 0031 gate 4).
 *
 * It initialises a leaf in static memory, builds one span with one attribute,
 * and streams the payload to a sink. The CI jobs `leaf-footprint` link it for
 * Cortex-M0+ and Cortex-M4 (nanopb) and for aarch64 Linux (upb) with
 * --gc-sections, so only the code a trace-only firmware needs is counted.
 *
 * It is not a program anyone runs: the "clock", the "random source" and the
 * "link" are stand-ins that keep the compiler from discarding the work. The
 * sink writes to a volatile byte, as a UART data register would be written.
 */

#include "microtel/leaf.h"

#include <stddef.h>
#include <stdint.h>

/* The record buffer this trace needs: the Resource, the scope and one span
 * with one attribute take a little over 192 bytes. */
#define PROBE_RECORD_BUFFER_BYTES 256u

/* upb only (build with -DPROBE_WITH_SCRATCH): the encode arena for the same
 * payload, which needs a little over 1.25 KiB on a 64-bit target. nanopb
 * ignores `scratch` and allocates nothing, so its probe has none. */
#define PROBE_SCRATCH_BYTES 2048u

/* A free-running tick counter's step, in nanoseconds. */
#define PROBE_TICK_NS 1000u

/* xorshift32 constants: not cryptographic, and ids need not be (leaf.h). */
#define PROBE_XS_A 13u
#define PROBE_XS_B 17u
#define PROBE_XS_C 5u
#define PROBE_SEED 0x9e3779b9u

static microtel_leaf_t g_leaf;
static uint8_t g_records[PROBE_RECORD_BUFFER_BYTES];
#ifdef PROBE_WITH_SCRATCH
static uint64_t g_scratch[PROBE_SCRATCH_BYTES / sizeof(uint64_t)];
#endif
static uint64_t g_ticks;
static uint32_t g_rng = PROBE_SEED;

/* Stands in for a peripheral data register. */
volatile uint8_t g_link_register;

static uint64_t probe_now_ns(void* ctx)
{
    (void)ctx;
    g_ticks += PROBE_TICK_NS;
    return g_ticks;
}

static void probe_random(void* ctx, uint8_t* out, size_t len)
{
    size_t i;
    (void)ctx;
    for (i = 0; i < len; ++i)
    {
        g_rng ^= g_rng << PROBE_XS_A;
        g_rng ^= g_rng >> PROBE_XS_B;
        g_rng ^= g_rng << PROBE_XS_C;
        out[i] = (uint8_t)g_rng;
    }
}

static int probe_write(void* ctx, const uint8_t* bytes, size_t len)
{
    size_t i;
    (void)ctx;
    for (i = 0; i < len; ++i)
    {
        g_link_register = bytes[i];
    }
    return 0;
}

int main(void)
{
    static const char kService[] = "probe";
    static const char kSpan[] = "sample";
    static const char kKey[] = "sensor.value";
    microtel_leaf_kv_t resource;
    microtel_leaf_kv_t attr;
    microtel_leaf_config_t config;
    microtel_leaf_span_t span = 0;
    size_t written = 0;
    microtel_leaf_status_t st;

    resource.key = "service.name";
    resource.key_len = sizeof("service.name") - 1u;
    resource.type = MICROTEL_LEAF_VALUE_STRING;
    resource.value.s.ptr = kService;
    resource.value.s.len = sizeof(kService) - 1u;

    config = (microtel_leaf_config_t){0};
    config.struct_size = (uint32_t)sizeof(config);
    config.time_mode = MICROTEL_LEAF_TIME_CONCENTRATOR_STAMPED;
    config.now_ns = probe_now_ns;
    config.random_bytes = probe_random;
    config.resource = &resource;
    config.resource_count = 1u;
#ifdef PROBE_WITH_SCRATCH
    config.scratch = g_scratch;
    config.scratch_size = sizeof(g_scratch);
#endif

    st = microtel_leaf_init(&g_leaf, sizeof(g_leaf), &config, g_records, sizeof(g_records));
    if (st == MICROTEL_LEAF_OK)
    {
        st = microtel_leaf_span_start(
            &g_leaf, &span, kSpan, sizeof(kSpan) - 1u, MICROTEL_LEAF_SPAN_KIND_INTERNAL, NULL);
    }
    if (st == MICROTEL_LEAF_OK)
    {
        attr.key = kKey;
        attr.key_len = sizeof(kKey) - 1u;
        attr.type = MICROTEL_LEAF_VALUE_INT64;
        attr.value.i = 42;
        st = microtel_leaf_span_set_attribute(&g_leaf, span, &attr);
    }
    if (st == MICROTEL_LEAF_OK)
    {
        st = microtel_leaf_span_end(&g_leaf, span);
    }
    if (st == MICROTEL_LEAF_OK)
    {
        st = microtel_leaf_encode_to(&g_leaf, probe_write, NULL, &written);
    }
    microtel_leaf_free(&g_leaf);
    return st == MICROTEL_LEAF_OK ? 0 : 1;
}
