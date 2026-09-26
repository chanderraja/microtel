/* Copyright (c) 2026 The microtel Authors.
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * The leaf's target test runner (docs/leaf-concentrator-design.md §7.1): the
 * golden vectors and a representative subset of tests/unit/leaf/leaf_test.cpp,
 * in C11 with no test framework, no heap and no stdio, so it runs where the
 * gtest suite cannot: bare-metal Cortex-M0 and Cortex-M4 under
 * qemu-system-arm. The same source also runs as a process (the host build,
 * 32-bit i686, qemu-aarch64); ci/scripts/leaf-target.sh drives every form.
 *
 * Every record buffer and output buffer is used at byte offsets 0 to 3 as well
 * as aligned: an ARMv6-M core (Cortex-M0 / M0+) faults on an unaligned word or
 * halfword access where x86 and ARMv7-M do not, and the leaf promises that its
 * buffers need no alignment.
 *
 * With LEAF_TARGET_MEASURE_STACK defined it also reports the stack each public
 * entry point uses, measured by painting (docs/bench-results/leaf-footprint.md).
 *
 * Output is TAP-like: `not ok - <check>` for each failure, `stack <entry>
 * <bytes>` for each measurement, then `PASS` or `FAIL`.
 */

#include "microtel/leaf.h"

#include "leaf_target_golden.h"
#include "leaf_target_platform.h"
#include "leaf_vectors.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum
{
    OUT_BYTES = 4096,
    HALF_OUT_BYTES = OUT_BYTES / 2,
    RECORD_BYTES = 1024,
    SMALL_RECORD_BYTES = 256,
    MISALIGN_LIMIT = 4, /* byte offsets 0..3 */
    RECORD_WORDS = (RECORD_BYTES + MISALIGN_LIMIT + 7) / 8,
    SCRATCH_BYTES = 4096,
    DECIMAL_BUFFER = 24,
    DECIMAL_BASE = 10,
    SPAN_FLOOD_LIMIT = 64,
    WORD_BYTES = 4
};

#define CLOCK_START 1000000u
#define CLOCK_STEP 1000u
#define RANDOM_SEED 0x2545f491u
#define XS_A 13u
#define XS_B 17u
#define XS_C 5u
#define PAINT_WORD 0x5aa5c33cu
#define NEGATIVE_VALUE (-12345)
#define DOUBLE_VALUE 2.5

static uint8_t g_out[OUT_BYTES];
static uint64_t g_record_words[RECORD_WORDS];
static microtel_leaf_t g_leaf;
#ifdef LEAF_TARGET_BACKEND_UPB
static uint64_t g_scratch[SCRATCH_BYTES / sizeof(uint64_t)];
#endif

static unsigned g_checks;
static unsigned g_failures;
static uint64_t g_clock;
static uint32_t g_random;

/* ------------------------------------------------------------------------ */
/* Log and checks                                                           */
/* ------------------------------------------------------------------------ */

static void put_unsigned(size_t value)
{
    char text[DECIMAL_BUFFER];
    size_t pos = sizeof(text) - 1u;
    text[pos] = '\0';
    do
    {
        --pos;
        text[pos] = (char)('0' + (value % DECIMAL_BASE));
        value /= DECIMAL_BASE;
    } while (value != 0u && pos > 0u);
    leaf_target_puts(&text[pos]);
}

static void check(int ok, const char* what)
{
    ++g_checks;
    if (!ok)
    {
        ++g_failures;
        leaf_target_puts("not ok - ");
        leaf_target_puts(what);
        leaf_target_puts("\n");
    }
}

/* ------------------------------------------------------------------------ */
/* The fixture: a leaf with a stepping clock and an xorshift random source  */
/* ------------------------------------------------------------------------ */

static uint64_t read_clock(void* ctx)
{
    const uint64_t now = g_clock;
    (void)ctx;
    g_clock += CLOCK_STEP;
    return now;
}

static void fill_random(void* ctx, uint8_t* out, size_t len)
{
    size_t i;
    (void)ctx;
    for (i = 0; i < len; ++i)
    {
        g_random ^= g_random << XS_A;
        g_random ^= g_random >> XS_B;
        g_random ^= g_random << XS_C;
        out[i] = (uint8_t)g_random;
    }
}

static microtel_leaf_kv_t make_kv(const char* key, size_t key_len, microtel_leaf_value_type_t type)
{
    microtel_leaf_kv_t kv;
    memset(&kv, 0, sizeof(kv));
    kv.key = key;
    kv.key_len = key_len;
    kv.type = type;
    return kv;
}

/* The record buffer at `offset` bytes past an 8-byte boundary. */
static uint8_t* records_at(size_t offset)
{
    return (uint8_t*)g_record_words + offset;
}

/* The leaf's configuration, static so a measured call's frame holds none of it. */
static microtel_leaf_config_t g_config;
static microtel_leaf_kv_t g_resource;

/* Sets g_config, and resets the clock and the random source so every run is
 * the same. */
static void fixture_config(void)
{
    static const char k_service[] = "target";
    g_resource = make_kv("service.name", 12u, MICROTEL_LEAF_VALUE_STRING);
    g_resource.value.s.ptr = k_service;
    g_resource.value.s.len = sizeof(k_service) - 1u;
    memset(&g_config, 0, sizeof(g_config));
    g_config.struct_size = (uint32_t)sizeof(g_config);
    g_config.time_mode = MICROTEL_LEAF_TIME_CONCENTRATOR_STAMPED;
    g_config.now_ns = read_clock;
    g_config.random_bytes = fill_random;
    g_config.resource = &g_resource;
    g_config.resource_count = 1u;
    g_config.scope_name = "target";
    g_config.scope_name_len = 6u;
#ifdef LEAF_TARGET_BACKEND_UPB
    g_config.scratch = g_scratch;
    g_config.scratch_size = sizeof(g_scratch);
#endif
    g_clock = CLOCK_START;
    g_random = RANDOM_SEED;
}

/* Initialises g_leaf over `size` record bytes at `offset`. */
static microtel_leaf_status_t fixture_init(size_t offset, size_t size)
{
    fixture_config();
    return microtel_leaf_init(&g_leaf, sizeof(g_leaf), &g_config, records_at(offset), size);
}

/* The four attribute types on `span`, a negative int64 and a double among them. */
static microtel_leaf_status_t set_every_attribute_type(microtel_leaf_span_t span)
{
    microtel_leaf_kv_t kvs[4];
    microtel_leaf_status_t st = MICROTEL_LEAF_OK;
    size_t i;
    kvs[0] = make_kv("s", 1u, MICROTEL_LEAF_VALUE_STRING);
    kvs[0].value.s.ptr = "text";
    kvs[0].value.s.len = 4u;
    kvs[1] = make_kv("i", 1u, MICROTEL_LEAF_VALUE_INT64);
    kvs[1].value.i = NEGATIVE_VALUE;
    kvs[2] = make_kv("d", 1u, MICROTEL_LEAF_VALUE_DOUBLE);
    kvs[2].value.d = DOUBLE_VALUE;
    kvs[3] = make_kv("b", 1u, MICROTEL_LEAF_VALUE_BOOL);
    kvs[3].value.b = 1;
    for (i = 0; i < 4u && st == MICROTEL_LEAF_OK; ++i)
    {
        st = microtel_leaf_span_set_attribute(&g_leaf, span, &kvs[i]);
    }
    return st;
}

/* An event with attributes and a status: the deepest nesting the payload has
 * (Request > ResourceSpans > ScopeSpans > Span > Event > KeyValue > AnyValue). */
static microtel_leaf_status_t add_event_and_status(microtel_leaf_span_t span)
{
    microtel_leaf_kv_t attrs[2];
    microtel_leaf_status_t st;
    attrs[0] = make_kv("attempt", 7u, MICROTEL_LEAF_VALUE_INT64);
    attrs[0].value.i = 2;
    attrs[1] = make_kv("reason", 6u, MICROTEL_LEAF_VALUE_STRING);
    attrs[1].value.s.ptr = "timeout";
    attrs[1].value.s.len = 7u;
    st = microtel_leaf_span_add_event(&g_leaf, span, "retry", 5u, attrs, 2u);
    if (st == MICROTEL_LEAF_OK)
    {
        st = microtel_leaf_span_set_status(&g_leaf, span, MICROTEL_LEAF_STATUS_ERROR, "bad", 3u);
    }
    return st;
}

/* A root span with every attribute type, an event and a status, and one
 * child: every field the backends write. */
static microtel_leaf_status_t build_rich(void)
{
    microtel_leaf_span_t root = 0;
    microtel_leaf_span_t child = 0;
    microtel_leaf_status_t st =
        microtel_leaf_span_start(&g_leaf, &root, "root", 4u, MICROTEL_LEAF_SPAN_KIND_SERVER, NULL);
    if (st == MICROTEL_LEAF_OK)
    {
        st = set_every_attribute_type(root);
    }
    if (st == MICROTEL_LEAF_OK)
    {
        st = add_event_and_status(root);
    }
    if (st == MICROTEL_LEAF_OK)
    {
        st = microtel_leaf_span_start(
            &g_leaf, &child, "child", 5u, MICROTEL_LEAF_SPAN_KIND_CLIENT, &root);
    }
    if (st == MICROTEL_LEAF_OK)
    {
        st = microtel_leaf_span_end(&g_leaf, child);
    }
    if (st == MICROTEL_LEAF_OK)
    {
        st = microtel_leaf_span_end(&g_leaf, root);
    }
    return st;
}

/* Initialises at `offset`, builds the rich batch and encodes it into `out`. */
static microtel_leaf_status_t encode_rich(size_t offset, uint8_t* out, size_t cap, size_t* written)
{
    microtel_leaf_status_t st = fixture_init(offset, RECORD_BYTES);
    if (st == MICROTEL_LEAF_OK)
    {
        st = build_rich();
    }
    if (st == MICROTEL_LEAF_OK)
    {
        st = microtel_leaf_encode(&g_leaf, out, cap, written);
    }
    return st;
}

/* ------------------------------------------------------------------------ */
/* Tests                                                                    */
/* ------------------------------------------------------------------------ */

static const leaf_target_golden* find_golden(const char* name)
{
    size_t i;
    for (i = 0; i < leaf_target_golden_count; ++i)
    {
        if (strcmp(leaf_target_goldens[i].name, name) == 0)
        {
            return &leaf_target_goldens[i];
        }
    }
    return NULL;
}

/* Encodes vector `index` at `offset` bytes into g_out and compares it with
 * its committed .bin. */
static int vector_matches(size_t index, const leaf_target_golden* golden, size_t offset)
{
    size_t len = 0;
    const microtel_leaf_status_t st =
        microtel_leaf_test_vector_encode(index, g_out + offset, OUT_BYTES - offset, &len);
    return st == MICROTEL_LEAF_OK && golden != NULL && len == golden->size &&
           memcmp(g_out + offset, golden->bytes, len) == 0;
}

/* Every golden vector, byte for byte, twice (deterministic), each time at a
 * different output offset. */
static void test_golden_vectors(void)
{
    size_t i;
    check(microtel_leaf_test_vector_count() == leaf_target_golden_count,
          "golden: one .bin per vector");
    for (i = 0; i < microtel_leaf_test_vector_count(); ++i)
    {
        const char* name = microtel_leaf_test_vector_name(i);
        const leaf_target_golden* golden = find_golden(name);
        leaf_target_puts("# vector ");
        leaf_target_puts(name);
        leaf_target_puts("\n");
        check(vector_matches(i, golden, i % MISALIGN_LIMIT),
              "golden: bytes match the committed .bin");
        check(vector_matches(i, golden, (i + 1u) % MISALIGN_LIMIT),
              "golden: a second encode gives the same bytes");
    }
}

/* The same batch from a record buffer and into an output buffer at every
 * byte offset gives the same bytes. */
static void test_unaligned_buffers(void)
{
    uint8_t* reference = g_out + HALF_OUT_BYTES;
    size_t reference_len = 0;
    size_t offset;
    check(encode_rich(0u, reference, HALF_OUT_BYTES, &reference_len) == MICROTEL_LEAF_OK,
          "unaligned: aligned reference encodes");
    microtel_leaf_free(&g_leaf);
    for (offset = 1u; offset < MISALIGN_LIMIT; ++offset)
    {
        size_t len = 0;
        const microtel_leaf_status_t st =
            encode_rich(offset, g_out + offset, HALF_OUT_BYTES - MISALIGN_LIMIT, &len);
        check(st == MICROTEL_LEAF_OK, "unaligned: encodes at an odd offset");
        check(len == reference_len && memcmp(g_out + offset, reference, len) == 0,
              "unaligned: same bytes at every offset");
        microtel_leaf_free(&g_leaf);
    }
}

/* A short buffer reports the exact size and consumes nothing. */
static void test_buffer_too_small(void)
{
    size_t needed = 0;
    size_t written = 0;
    microtel_leaf_status_t st = fixture_init(0u, RECORD_BYTES);
    if (st == MICROTEL_LEAF_OK)
    {
        st = build_rich();
    }
    check(st == MICROTEL_LEAF_OK, "short buffer: batch builds");
    check(microtel_leaf_encode(&g_leaf, g_out, 0u, &needed) == MICROTEL_LEAF_ERR_BUFFER_SMALL,
          "short buffer: empty buffer is ERR_BUFFER_SMALL");
    check(needed > 0u && needed <= HALF_OUT_BYTES, "short buffer: reports a size");
    check(microtel_leaf_encode(&g_leaf, g_out, needed - 1u, &written) ==
                  MICROTEL_LEAF_ERR_BUFFER_SMALL &&
              written == needed,
          "short buffer: one byte short reports the exact size");
    check(microtel_leaf_encode(&g_leaf, g_out, needed, &written) == MICROTEL_LEAF_OK &&
              written == needed,
          "short buffer: nothing was consumed");
    microtel_leaf_free(&g_leaf);
}

typedef struct sink
{
    uint8_t* out;
    size_t len;
    size_t cap;
    int fail;
} sink;

static int sink_write(void* ctx, const uint8_t* bytes, size_t len)
{
    sink* s = (sink*)ctx;
    if (s->fail != 0 || len > s->cap - s->len)
    {
        return 1;
    }
    memcpy(s->out + s->len, bytes, len);
    s->len += len;
    return 0;
}

/* Streaming gives the buffer encode's bytes; a failing write consumes nothing. */
static void test_encode_to(void)
{
    uint8_t* reference = g_out + HALF_OUT_BYTES;
    size_t reference_len = 0;
    size_t written = 0;
    uint64_t clock_before;
    sink s = {g_out + 1, 0u, HALF_OUT_BYTES - 1u, 1};
    microtel_leaf_status_t st = encode_rich(0u, reference, HALF_OUT_BYTES, &reference_len);
    microtel_leaf_free(&g_leaf);
    if (st == MICROTEL_LEAF_OK)
    {
        st = fixture_init(0u, RECORD_BYTES);
    }
    if (st == MICROTEL_LEAF_OK)
    {
        st = build_rich();
    }
    check(st == MICROTEL_LEAF_OK, "encode_to: batch builds");
    /* The encode reads the clock once (the encode time): rewind it after the
     * failed attempt so the retry stamps what the reference did. */
    clock_before = g_clock;
    check(microtel_leaf_encode_to(&g_leaf, sink_write, &s, &written) == MICROTEL_LEAF_ERR_ENCODE,
          "encode_to: a failing write is ERR_ENCODE");
    g_clock = clock_before;
    s.fail = 0;
    s.len = 0;
    check(microtel_leaf_encode_to(&g_leaf, sink_write, &s, &written) == MICROTEL_LEAF_OK,
          "encode_to: succeeds after the failed attempt");
    check(written == reference_len && s.len == reference_len &&
              memcmp(s.out, reference, reference_len) == 0,
          "encode_to: same bytes as the buffer encode, nothing consumed by the failure");
    microtel_leaf_free(&g_leaf);
}

/* A full record buffer drops and counts, and recovers after an encode. */
static void test_exhaustion_recovers(void)
{
    microtel_leaf_counters_t counters;
    microtel_leaf_span_t span = 0;
    microtel_leaf_status_t st = MICROTEL_LEAF_OK;
    size_t written = 0;
    int started = 0;
    check(fixture_init(1u, SMALL_RECORD_BYTES) == MICROTEL_LEAF_OK, "exhaustion: init");
    while (st == MICROTEL_LEAF_OK && started < SPAN_FLOOD_LIMIT)
    {
        st = microtel_leaf_span_start(
            &g_leaf, &span, "flood", 5u, MICROTEL_LEAF_SPAN_KIND_INTERNAL, NULL);
        if (st == MICROTEL_LEAF_OK)
        {
            st = microtel_leaf_span_end(&g_leaf, span);
            ++started;
        }
    }
    check(st == MICROTEL_LEAF_ERR_NO_SPACE && started > 0, "exhaustion: fills, then NO_SPACE");
    microtel_leaf_get_counters(&g_leaf, &counters, sizeof(counters));
    check(counters.dropped_spans == 1u, "exhaustion: the drop is counted");
    check(microtel_leaf_encode(&g_leaf, g_out, HALF_OUT_BYTES, &written) == MICROTEL_LEAF_OK,
          "exhaustion: encodes what fitted");
    microtel_leaf_get_counters(&g_leaf, &counters, sizeof(counters));
    check(counters.dropped_spans == 0u, "exhaustion: counters reset by the encode");
    check(microtel_leaf_span_start(
              &g_leaf, &span, "again", 5u, MICROTEL_LEAF_SPAN_KIND_INTERNAL, NULL) ==
              MICROTEL_LEAF_OK,
          "exhaustion: space is back");
    microtel_leaf_free(&g_leaf);
}

/* Handles: ended and encoded spans refuse changes, even when the slot is reused. */
static void test_handles(void)
{
    microtel_leaf_span_t old_span = 0;
    microtel_leaf_span_t open_span = 0;
    microtel_leaf_span_t reused = 0;
    microtel_leaf_kv_t kv = make_kv("k", 1u, MICROTEL_LEAF_VALUE_BOOL);
    size_t written = 0;
    check(fixture_init(2u, RECORD_BYTES) == MICROTEL_LEAF_OK, "handles: init");
    check(microtel_leaf_span_start(
              &g_leaf, &open_span, "open", 4u, MICROTEL_LEAF_SPAN_KIND_INTERNAL, NULL) ==
                  MICROTEL_LEAF_OK &&
              microtel_leaf_span_start(
                  &g_leaf, &old_span, "old", 3u, MICROTEL_LEAF_SPAN_KIND_INTERNAL, NULL) ==
                  MICROTEL_LEAF_OK &&
              microtel_leaf_span_end(&g_leaf, old_span) == MICROTEL_LEAF_OK,
          "handles: spans start and end");
    check(microtel_leaf_span_set_attribute(&g_leaf, old_span, &kv) == MICROTEL_LEAF_ERR_STATE,
          "handles: an ended span refuses attributes");
    check(microtel_leaf_encode(&g_leaf, g_out, HALF_OUT_BYTES, &written) == MICROTEL_LEAF_OK,
          "handles: encode");
    check(microtel_leaf_span_start(
              &g_leaf, &reused, "new", 3u, MICROTEL_LEAF_SPAN_KIND_INTERNAL, NULL) ==
                  MICROTEL_LEAF_OK &&
              reused != old_span,
          "handles: a reused slot gets a new handle");
    check(microtel_leaf_span_end(&g_leaf, old_span) == MICROTEL_LEAF_ERR_STATE,
          "handles: an encoded span's handle is stale");
    check(microtel_leaf_span_set_attribute(&g_leaf, open_span, &kv) == MICROTEL_LEAF_OK &&
              microtel_leaf_span_end(&g_leaf, open_span) == MICROTEL_LEAF_OK,
          "handles: the open span survived the encode");
    check(microtel_leaf_encode(&g_leaf, g_out, HALF_OUT_BYTES, &written) == MICROTEL_LEAF_OK,
          "handles: second encode");
    microtel_leaf_free(&g_leaf);
    check(microtel_leaf_span_start(
              &g_leaf, &reused, "x", 1u, MICROTEL_LEAF_SPAN_KIND_INTERNAL, NULL) ==
              MICROTEL_LEAF_ERR_STATE,
          "handles: a freed leaf is ERR_STATE");
}

/* The version, and init's size guard. */
static void test_init_guards(void)
{
    microtel_leaf_config_t config;
    memset(&config, 0, sizeof(config));
    check(microtel_leaf_version() == MICROTEL_LEAF_VERSION, "version: library matches header");
    check(microtel_leaf_init(&g_leaf, sizeof(g_leaf) - 1u, &config, records_at(0u), RECORD_BYTES) ==
              MICROTEL_LEAF_ERR_ARG,
          "init: a short leaf size is ERR_ARG");
    check(microtel_leaf_init(&g_leaf, sizeof(g_leaf), &config, records_at(0u), RECORD_BYTES) ==
              MICROTEL_LEAF_ERR_ARG,
          "init: a zeroed config is ERR_ARG");
}

/* ------------------------------------------------------------------------ */
/* Stack measurement                                                        */
/* ------------------------------------------------------------------------ */

#ifdef LEAF_TARGET_MEASURE_STACK

typedef void (*step_fn)(void);

static microtel_leaf_status_t g_step_status;
static microtel_leaf_span_t g_step_span;
static size_t g_step_written;
static volatile uint8_t g_link_register;

/* The address of a local in a frame one call below the caller's: where the
 * measured function's frame will start. */
__attribute__((noinline)) static uintptr_t callee_frame(void)
{
    volatile uint32_t local = 0;
    return (uintptr_t)&local;
}

/* Paints the stack below the next call's frame, runs `step`, and returns how
 * far down it wrote. Under-reads by at most the few bytes of callee_frame's
 * frame above its local. */
__attribute__((noinline)) static size_t stack_used_by(step_fn step)
{
    const uintptr_t top = callee_frame() & ~(uintptr_t)(WORD_BYTES - 1);
    const uintptr_t floor =
        (leaf_target_stack_floor(top) + WORD_BYTES - 1u) & ~(uintptr_t)(WORD_BYTES - 1);
    uintptr_t p;
    for (p = floor; p < top; p += WORD_BYTES)
    {
        *(volatile uint32_t*)p = PAINT_WORD;
    }
    step();
    for (p = floor; p < top && *(volatile uint32_t*)p == PAINT_WORD; p += WORD_BYTES)
    {
    }
    return (size_t)(top - p);
}

/* The size probe (examples/leaf/size_probe.c): one span, one int64 attribute. */
static microtel_leaf_kv_t g_probe_attribute;

static void step_probe_init(void)
{
    g_step_status =
        microtel_leaf_init(&g_leaf, sizeof(g_leaf), &g_config, records_at(0u), SMALL_RECORD_BYTES);
}

static void step_probe_start(void)
{
    g_step_status = microtel_leaf_span_start(
        &g_leaf, &g_step_span, "sample", 6u, MICROTEL_LEAF_SPAN_KIND_INTERNAL, NULL);
}

static void step_probe_attribute(void)
{
    g_step_status = microtel_leaf_span_set_attribute(&g_leaf, g_step_span, &g_probe_attribute);
}

static void step_probe_end(void)
{
    g_step_status = microtel_leaf_span_end(&g_leaf, g_step_span);
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

static void step_encode_to(void)
{
    g_step_status = microtel_leaf_encode_to(&g_leaf, probe_write, NULL, &g_step_written);
}

static void step_encode(void)
{
    g_step_status = microtel_leaf_encode(&g_leaf, g_out, HALF_OUT_BYTES, &g_step_written);
}

static void step_nothing(void)
{
    g_step_status = MICROTEL_LEAF_OK;
}

static size_t measure(const char* entry, step_fn step)
{
    const size_t used = stack_used_by(step);
    check(g_step_status == MICROTEL_LEAF_OK, entry);
    leaf_target_puts("stack ");
    leaf_target_puts(entry);
    leaf_target_puts(" ");
    put_unsigned(used);
    leaf_target_puts("\n");
    return used;
}

static void rebuild_probe_span(void)
{
    step_probe_start();
    step_probe_attribute();
    step_probe_end();
}

static void measure_stack(void)
{
    fixture_config();
    g_probe_attribute = make_kv("sensor.value", 12u, MICROTEL_LEAF_VALUE_INT64);
    g_probe_attribute.value.i = 42;
    (void)measure("baseline", step_nothing);
    (void)measure("init", step_probe_init);
    (void)measure("span_start", step_probe_start);
    (void)measure("span_set_attribute", step_probe_attribute);
    (void)measure("span_end", step_probe_end);
    (void)measure("encode_to", step_encode_to);
    rebuild_probe_span();
    (void)measure("encode", step_encode);
    microtel_leaf_free(&g_leaf);
    /* The deepest nesting: an event with attributes. */
    g_step_status = fixture_init(0u, RECORD_BYTES);
    if (g_step_status == MICROTEL_LEAF_OK)
    {
        g_step_status = build_rich();
    }
    check(g_step_status == MICROTEL_LEAF_OK, "stack: rich batch builds");
    (void)measure("encode_to_nested", step_encode_to);
    microtel_leaf_free(&g_leaf);
}

#endif /* LEAF_TARGET_MEASURE_STACK */

#ifdef LEAF_TARGET_EXPECT_ALIGNMENT_FAULT
/* The control for the offset tests: an unaligned word load, which must fault
 * on the core under test, or passing them would prove nothing. */
static void provoke_alignment_fault(void)
{
    /* Through a volatile pointer, or the compiler, seeing the misalignment,
     * would load byte by byte. */
    uint8_t* volatile address = records_at(1u);
    const volatile uint32_t* word = (const volatile uint32_t*)(void*)address;
    leaf_target_puts("# unaligned load, expecting a fault\n");
    (void)*word;
    leaf_target_puts("no alignment fault: this core allows unaligned access\n");
}
#endif

int main(void)
{
    leaf_target_puts("# microtel leaf target tests, " LEAF_TARGET_BACKEND_NAME " backend\n");
#ifdef LEAF_TARGET_EXPECT_ALIGNMENT_FAULT
    provoke_alignment_fault();
    return 1;
#endif
    test_init_guards();
    test_golden_vectors();
    test_unaligned_buffers();
    test_buffer_too_small();
    test_encode_to();
    test_exhaustion_recovers();
    test_handles();
#ifdef LEAF_TARGET_MEASURE_STACK
    measure_stack();
#endif
    put_unsigned(g_checks);
    leaf_target_puts(" checks, ");
    put_unsigned(g_failures);
    leaf_target_puts(g_failures == 0u ? " failed\nPASS\n" : " failed\nFAIL\n");
    return g_failures == 0u ? 0 : 1;
}
