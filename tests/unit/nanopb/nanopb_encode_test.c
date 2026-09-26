/* Copyright (c) 2026 The microtel Authors.
 * SPDX-License-Identifier: Apache-2.0
 *
 * Exercises the vendored nanopb runtime and the generated OTLP trace
 * descriptors (third_party/nanopb/, gen/nanopb/) by encoding a small
 * ExportTraceServiceRequest and comparing it with golden bytes.
 *
 * Plain C11, not GoogleTest: the leaf's nanopb backend is C, so this is the
 * language the descriptors and the rename header have to work in. Built with
 * -std=c11 -pedantic-errors -Wall -Wextra -Werror.
 *
 * The golden bytes were produced by an independent encoder (python-protobuf
 * 5.29.4, SerializeToString(deterministic=True), from proto/) and checked with
 * `protoc --decode`. Matching them pins down the assumptions of
 * docs/leaf-concentrator-design.md §2.3 on the nanopb side:
 *   - fields in ascending field-number order, including Span.flags (16) after
 *     Span.status (15);
 *   - FT_CALLBACK members inside the AnyValue oneof (string_value);
 *   - proto3 presence: a root span's empty parent_span_id is omitted, a
 *     child's is written; Status is written only when has_status is set;
 *   - int64 -1 as a ten-byte varint, double from its bit pattern, fixed64
 *     timestamps.
 * It also checks the streaming form (a callback pb_ostream_t) produces the
 * same bytes, and that callbacks run more than once per encode (§2.2: once to
 * size a submessage, once to write it), so they must be pure. */

#include "opentelemetry/proto/collector/trace/v1/trace_service.pb.h"

#include <pb_encode.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum
{
    TRACE_ID_SIZE = 16,
    SPAN_ID_SIZE = 8,
    MAX_ATTRS = 2,
    SPAN_COUNT = 2,
    GOLDEN_SIZE = 222,
    SPAN_FLAGS_REMOTE_KNOWN = 0x100,
    SPAN_NAME_DEPTH = 3,
    STREAM_CAPACITY = 512
};

static const uint8_t k_golden[GOLDEN_SIZE] = {
    0x0a, 0xdb, 0x01, 0x0a, 0x18, 0x0a, 0x16, 0x0a, 0x0c, 0x73, 0x65, 0x72, 0x76, 0x69, 0x63, 0x65,
    0x2e, 0x6e, 0x61, 0x6d, 0x65, 0x12, 0x06, 0x0a, 0x04, 0x6c, 0x65, 0x61, 0x66, 0x12, 0xbe, 0x01,
    0x0a, 0x0f, 0x0a, 0x0d, 0x6d, 0x69, 0x63, 0x72, 0x6f, 0x74, 0x65, 0x6c, 0x2e, 0x6c, 0x65, 0x61,
    0x66, 0x12, 0x50, 0x0a, 0x10, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b,
    0x0c, 0x0d, 0x0e, 0x0f, 0x10, 0x12, 0x08, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x2a,
    0x02, 0x6f, 0x70, 0x30, 0x03, 0x39, 0xe8, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x41, 0xd0,
    0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x4a, 0x10, 0x0a, 0x01, 0x6e, 0x12, 0x0b, 0x18, 0xff,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x01, 0x7a, 0x02, 0x18, 0x02, 0x85, 0x01, 0x00,
    0x01, 0x00, 0x00, 0x12, 0x59, 0x0a, 0x10, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09,
    0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10, 0x12, 0x08, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27,
    0x28, 0x22, 0x08, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x2a, 0x05, 0x63, 0x68, 0x69,
    0x6c, 0x64, 0x39, 0x4c, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x41, 0x6c, 0x07, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x4a, 0x08, 0x0a, 0x02, 0x6f, 0x6b, 0x12, 0x02, 0x10, 0x01, 0x4a, 0x0e,
    0x0a, 0x01, 0x64, 0x12, 0x09, 0x21, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xe0, 0x3f,
};

/* ---- the input: one resource, one scope, a root span and its child ---- */

typedef struct test_attr
{
    char* key;
    pb_size_t which; /* an AnyValue oneof tag */
    char* str;
    bool b;
    int64_t i;
    double d;
} test_attr;

typedef struct test_attrs
{
    test_attr* items;
    size_t count;
} test_attrs;

typedef struct test_span
{
    uint8_t span_id[SPAN_ID_SIZE];
    uint8_t parent_span_id[SPAN_ID_SIZE];
    bool has_parent;
    char* name;
    int kind;
    uint64_t start;
    uint64_t end;
    test_attrs attrs;
    bool has_status;
    int status_code;
    uint32_t flags;
} test_span;

static test_attr g_resource_attr[] = {
    {.key = "service.name",
     .which = opentelemetry_proto_common_v1_AnyValue_string_value_tag,
     .str = "leaf"},
};
static test_attr g_root_attrs[] = {
    {.key = "n", .which = opentelemetry_proto_common_v1_AnyValue_int_value_tag, .i = -1},
};
static test_attr g_child_attrs[MAX_ATTRS] = {
    {.key = "ok", .which = opentelemetry_proto_common_v1_AnyValue_bool_value_tag, .b = true},
    {.key = "d", .which = opentelemetry_proto_common_v1_AnyValue_double_value_tag, .d = 0.5},
};
static test_attrs g_resource_attrs = {.items = g_resource_attr, .count = 1};
static char g_scope_name[] = "microtel.leaf";

static test_span g_spans[SPAN_COUNT] = {
    {
        .span_id = {0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18},
        .has_parent = false,
        .name = "op",
        .kind = opentelemetry_proto_trace_v1_Span_SpanKind_SPAN_KIND_CLIENT,
        .start = 1000,
        .end = 2000,
        .attrs = {.items = g_root_attrs, .count = 1},
        .has_status = true,
        .status_code = opentelemetry_proto_trace_v1_Status_StatusCode_STATUS_CODE_ERROR,
        .flags = SPAN_FLAGS_REMOTE_KNOWN,
    },
    {
        .span_id = {0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28},
        .parent_span_id = {0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18},
        .has_parent = true,
        .name = "child",
        .start = 1100,
        .end = 1900,
        .attrs = {.items = g_child_attrs, .count = MAX_ATTRS},
    },
};

static const uint8_t k_trace_id[TRACE_ID_SIZE] = {
    0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10};

/* How often the root span's name callback ran; see test_callbacks_run_once_per_level. */
static int g_root_name_calls = 0;

/* ---- encode callbacks: each writes its own tag, as nanopb requires ---- */

static bool encode_str(pb_ostream_t* stream, const pb_field_t* field, void* const* arg)
{
    const char* s = (const char*)*arg;
    return pb_encode_tag_for_field(stream, field) &&
           pb_encode_string(stream, (const pb_byte_t*)s, strlen(s));
}

static bool encode_root_name(pb_ostream_t* stream, const pb_field_t* field, void* const* arg)
{
    ++g_root_name_calls;
    return encode_str(stream, field, arg);
}

static void fill_any_value(opentelemetry_proto_common_v1_AnyValue* v, test_attr* a)
{
    v->which_value = a->which;
    switch (a->which)
    {
        case opentelemetry_proto_common_v1_AnyValue_string_value_tag:
            v->value.string_value.funcs.encode = encode_str;
            v->value.string_value.arg = a->str;
            break;
        case opentelemetry_proto_common_v1_AnyValue_bool_value_tag:
            v->value.bool_value = a->b;
            break;
        case opentelemetry_proto_common_v1_AnyValue_int_value_tag:
            v->value.int_value = a->i;
            break;
        default:
            v->value.double_value = a->d;
            break;
    }
}

static bool encode_attrs(pb_ostream_t* stream, const pb_field_t* field, void* const* arg)
{
    const test_attrs* attrs = (const test_attrs*)*arg;
    for (size_t i = 0; i < attrs->count; ++i)
    {
        opentelemetry_proto_common_v1_KeyValue kv =
            opentelemetry_proto_common_v1_KeyValue_init_zero;
        kv.key.funcs.encode = encode_str;
        kv.key.arg = attrs->items[i].key;
        kv.has_value = true;
        fill_any_value(&kv.value, &attrs->items[i]);
        if (!pb_encode_tag_for_field(stream, field) ||
            !pb_encode_submessage(stream, opentelemetry_proto_common_v1_KeyValue_fields, &kv))
        {
            return false;
        }
    }
    return true;
}

static void fill_span(opentelemetry_proto_trace_v1_Span* out, test_span* in, bool count_name)
{
    memcpy(out->trace_id, k_trace_id, TRACE_ID_SIZE);
    memcpy(out->span_id, in->span_id, SPAN_ID_SIZE);
    if (in->has_parent)
    {
        out->parent_span_id.size = SPAN_ID_SIZE;
        memcpy(out->parent_span_id.bytes, in->parent_span_id, SPAN_ID_SIZE);
    }
    out->name.funcs.encode = count_name ? encode_root_name : encode_str;
    out->name.arg = in->name;
    out->kind = (opentelemetry_proto_trace_v1_Span_SpanKind)in->kind;
    out->start_time_unix_nano = in->start;
    out->end_time_unix_nano = in->end;
    out->attributes.funcs.encode = encode_attrs;
    out->attributes.arg = &in->attrs;
    out->has_status = in->has_status;
    out->status.code = (opentelemetry_proto_trace_v1_Status_StatusCode)in->status_code;
    out->flags = in->flags;
}

static bool encode_spans(pb_ostream_t* stream, const pb_field_t* field, void* const* arg)
{
    (void)arg;
    for (size_t i = 0; i < SPAN_COUNT; ++i)
    {
        opentelemetry_proto_trace_v1_Span span = opentelemetry_proto_trace_v1_Span_init_zero;
        fill_span(&span, &g_spans[i], i == 0);
        if (!pb_encode_tag_for_field(stream, field) ||
            !pb_encode_submessage(stream, opentelemetry_proto_trace_v1_Span_fields, &span))
        {
            return false;
        }
    }
    return true;
}

static bool encode_scope_spans(pb_ostream_t* stream, const pb_field_t* field, void* const* arg)
{
    (void)arg;
    opentelemetry_proto_trace_v1_ScopeSpans ss = opentelemetry_proto_trace_v1_ScopeSpans_init_zero;
    ss.has_scope = true;
    ss.scope.name.funcs.encode = encode_str;
    ss.scope.name.arg = g_scope_name;
    ss.spans.funcs.encode = encode_spans;
    return pb_encode_tag_for_field(stream, field) &&
           pb_encode_submessage(stream, opentelemetry_proto_trace_v1_ScopeSpans_fields, &ss);
}

static bool encode_resource_spans(pb_ostream_t* stream, const pb_field_t* field, void* const* arg)
{
    (void)arg;
    opentelemetry_proto_trace_v1_ResourceSpans rs =
        opentelemetry_proto_trace_v1_ResourceSpans_init_zero;
    rs.has_resource = true;
    rs.resource.attributes.funcs.encode = encode_attrs;
    rs.resource.attributes.arg = &g_resource_attrs;
    rs.scope_spans.funcs.encode = encode_scope_spans;
    return pb_encode_tag_for_field(stream, field) &&
           pb_encode_submessage(stream, opentelemetry_proto_trace_v1_ResourceSpans_fields, &rs);
}

static opentelemetry_proto_collector_trace_v1_ExportTraceServiceRequest make_request(void)
{
    opentelemetry_proto_collector_trace_v1_ExportTraceServiceRequest req =
        opentelemetry_proto_collector_trace_v1_ExportTraceServiceRequest_init_zero;
    req.resource_spans.funcs.encode = encode_resource_spans;
    return req;
}

/* ---- a streaming sink, as microtel_leaf_encode_to will use (§1.8) ---- */

typedef struct stream_sink
{
    uint8_t bytes[STREAM_CAPACITY];
    size_t len;
    int writes;
} stream_sink;

static bool sink_write(pb_ostream_t* stream, const pb_byte_t* buf, size_t count)
{
    stream_sink* sink = (stream_sink*)stream->state;
    if (sink->len + count > sizeof sink->bytes)
    {
        return false;
    }
    memcpy(sink->bytes + sink->len, buf, count);
    sink->len += count;
    ++sink->writes;
    return true;
}

/* ---- tests ---- */

static int g_failures = 0;

static void check(bool ok, const char* test, const char* what)
{
    if (!ok)
    {
        ++g_failures;
        fprintf(stderr, "FAIL %s: %s\n", test, what);
    }
}

static void dump_mismatch(const uint8_t* got, size_t len)
{
    for (size_t i = 0; i < len && i < GOLDEN_SIZE; ++i)
    {
        if (got[i] != k_golden[i])
        {
            fprintf(stderr,
                    "  first difference at byte %zu: got 0x%02x, want 0x%02x\n",
                    i,
                    got[i],
                    k_golden[i]);
            return;
        }
    }
}

static void test_buffer_encode_matches_golden(void)
{
    const char* t = "buffer_encode_matches_golden";
    uint8_t buf[STREAM_CAPACITY];
    opentelemetry_proto_collector_trace_v1_ExportTraceServiceRequest req = make_request();
    pb_ostream_t os = pb_ostream_from_buffer(buf, sizeof buf);

    const bool ok = pb_encode(
        &os, opentelemetry_proto_collector_trace_v1_ExportTraceServiceRequest_fields, &req);
    check(ok, t, "pb_encode returned false");
    check(os.bytes_written == GOLDEN_SIZE, t, "encoded size differs from golden");
    const bool same = os.bytes_written == GOLDEN_SIZE && memcmp(buf, k_golden, GOLDEN_SIZE) == 0;
    check(same, t, "encoded bytes differ from golden");
    if (!same)
    {
        dump_mismatch(buf, os.bytes_written);
    }
}

static void test_encoded_size_matches_golden(void)
{
    const char* t = "encoded_size_matches_golden";
    size_t size = 0;
    opentelemetry_proto_collector_trace_v1_ExportTraceServiceRequest req = make_request();

    check(pb_get_encoded_size(
              &size, opentelemetry_proto_collector_trace_v1_ExportTraceServiceRequest_fields, &req),
          t,
          "pb_get_encoded_size returned false");
    check(size == GOLDEN_SIZE, t, "sized length differs from golden");
}

static void test_streaming_encode_matches_golden(void)
{
    const char* t = "streaming_encode_matches_golden";
    stream_sink sink = {.len = 0, .writes = 0};
    opentelemetry_proto_collector_trace_v1_ExportTraceServiceRequest req = make_request();
    pb_ostream_t os = {
        .callback = sink_write, .state = &sink, .max_size = SIZE_MAX, .bytes_written = 0};

    check(pb_encode(
              &os, opentelemetry_proto_collector_trace_v1_ExportTraceServiceRequest_fields, &req),
          t,
          "pb_encode returned false");
    check(sink.len == GOLDEN_SIZE && memcmp(sink.bytes, k_golden, GOLDEN_SIZE) == 0,
          t,
          "streamed bytes differ from golden");
    check(
        sink.writes > 1, t, "expected the payload in several writes, with no whole-payload buffer");
}

static void test_short_buffer_fails(void)
{
    const char* t = "short_buffer_fails";
    uint8_t buf[GOLDEN_SIZE - 1];
    opentelemetry_proto_collector_trace_v1_ExportTraceServiceRequest req = make_request();
    pb_ostream_t os = pb_ostream_from_buffer(buf, sizeof buf);

    check(!pb_encode(
              &os, opentelemetry_proto_collector_trace_v1_ExportTraceServiceRequest_fields, &req),
          t,
          "pb_encode succeeded into a buffer one byte short");
}

static void test_callbacks_run_once_per_level(void)
{
    const char* t = "callbacks_run_once_per_level";
    uint8_t buf[STREAM_CAPACITY];
    opentelemetry_proto_collector_trace_v1_ExportTraceServiceRequest req = make_request();
    pb_ostream_t os = pb_ostream_from_buffer(buf, sizeof buf);

    g_root_name_calls = 0;
    check(pb_encode(
              &os, opentelemetry_proto_collector_trace_v1_ExportTraceServiceRequest_fields, &req),
          t,
          "pb_encode returned false");
    /* Span.name sits three submessages deep (ResourceSpans, ScopeSpans, Span).
     * Each enclosing pb_encode_submessage runs it once to size, and the write
     * runs it once more: depth + 1. */
    check(g_root_name_calls == SPAN_NAME_DEPTH + 1, t, "unexpected number of callback runs");
}

int main(void)
{
    test_buffer_encode_matches_golden();
    test_encoded_size_matches_golden();
    test_streaming_encode_matches_golden();
    test_short_buffer_fails();
    test_callbacks_run_once_per_level();

    if (g_failures != 0)
    {
        fprintf(stderr, "%d check(s) failed\n", g_failures);
        return 1;
    }
    puts("nanopb_encode_test: all checks passed");
    return 0;
}
