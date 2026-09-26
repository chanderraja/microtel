/* Copyright (c) 2026 The microtel Authors.
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * The upb encoder backend (docs/leaf-concentrator-design.md §2.2). The only
 * leaf file that includes upb headers.
 *
 * Builds the ExportTraceServiceRequest with the generated accessors under
 * gen/, in one arena per call, and serialises it with upb_Encode. upb writes
 * each message's fields in its MiniTable order, which the generator sorts by
 * field number, so the output is in ascending field-number order (§2.3 rule 1).
 * Strings are not copied into the arena: they alias the record buffer, which
 * outlives the call.
 *
 * With a caller-supplied scratch buffer the arena gets an allocator that
 * refuses every request, so running out of scratch fails the encode instead
 * of touching the heap. That allocator must not be NULL: the vendored upb
 * v29.4 declares a NULL allocator "fixed-size", but on exhausting a
 * caller-supplied initial block it calls through the NULL allocator anyway
 * (third_party/upb/upb/mem/arena.c, _upb_Arena_AllocBlock).
 */

#include "leaf_internal.h"
#include "opentelemetry/proto/collector/trace/v1/trace_service.upb.h"
#include "opentelemetry/proto/common/v1/common.upb.h"
#include "opentelemetry/proto/resource/v1/resource.upb.h"
#include "opentelemetry/proto/trace/v1/trace.upb.h"
#include "upb/mem/alloc.h"
#include "upb/mem/arena.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define TRACE_ID_BYTES 16u
#define SPAN_ID_BYTES 8u

typedef opentelemetry_proto_common_v1_KeyValue key_value;
typedef opentelemetry_proto_trace_v1_Span span_msg;

/* The fixed-scratch allocator: never allocates, frees nothing. */
static void* refuse_alloc(upb_alloc* alloc, void* ptr, size_t oldsize, size_t size)
{
    (void)alloc;
    (void)ptr;
    (void)oldsize;
    (void)size;
    return NULL;
}

static upb_alloc s_refuse = {&refuse_alloc};

static upb_StringView view(const char* data, size_t len)
{
    return upb_StringView_FromDataAndSize(data, len);
}

static upb_StringView bytes_view(const uint8_t* data, size_t len)
{
    return upb_StringView_FromDataAndSize((const char*)data, len);
}

/* Fills an allocated KeyValue. Returns 0 on arena exhaustion. */
static int set_kv(key_value* out, const microtel_leaf_kv_t* kv, upb_Arena* arena)
{
    opentelemetry_proto_common_v1_AnyValue* value;
    if (out == NULL)
    {
        return 0;
    }
    opentelemetry_proto_common_v1_KeyValue_set_key(out, view(kv->key, kv->key_len));
    value = opentelemetry_proto_common_v1_KeyValue_mutable_value(out, arena);
    if (value == NULL)
    {
        return 0;
    }
    switch (kv->type)
    {
        case MICROTEL_LEAF_VALUE_BOOL:
            opentelemetry_proto_common_v1_AnyValue_set_bool_value(value, kv->value.b != 0);
            break;
        case MICROTEL_LEAF_VALUE_INT64:
            opentelemetry_proto_common_v1_AnyValue_set_int_value(value, kv->value.i);
            break;
        case MICROTEL_LEAF_VALUE_DOUBLE:
            opentelemetry_proto_common_v1_AnyValue_set_double_value(value, kv->value.d);
            break;
        default:
            opentelemetry_proto_common_v1_AnyValue_set_string_value(
                value, view(kv->value.s.ptr, kv->value.s.len));
            break;
    }
    return 1;
}

static int build_resource(const microtel_leaf_internal_batch* batch,
                          opentelemetry_proto_trace_v1_ResourceSpans* rs,
                          upb_Arena* arena)
{
    microtel_leaf_internal_cursor c = {0u, 0u};
    microtel_leaf_kv_t kv;
    opentelemetry_proto_resource_v1_Resource* res =
        opentelemetry_proto_trace_v1_ResourceSpans_mutable_resource(rs, arena);
    int ok = res != NULL;
    while (ok && microtel_leaf_internal_next_resource_attr(batch, &c, &kv))
    {
        ok =
            set_kv(opentelemetry_proto_resource_v1_Resource_add_attributes(res, arena), &kv, arena);
    }
    return ok;
}

static int build_event(const microtel_leaf_internal_event* ev, span_msg* span, upb_Arena* arena)
{
    microtel_leaf_internal_cursor c = {0u, 0u};
    microtel_leaf_kv_t kv;
    opentelemetry_proto_trace_v1_Span_Event* msg =
        opentelemetry_proto_trace_v1_Span_add_events(span, arena);
    int ok = msg != NULL;
    if (ok)
    {
        opentelemetry_proto_trace_v1_Span_Event_set_time_unix_nano(msg, ev->time_unix_nano);
        opentelemetry_proto_trace_v1_Span_Event_set_name(msg, view(ev->name, ev->name_len));
    }
    while (ok && microtel_leaf_internal_next_event_attr(ev, &c, &kv))
    {
        ok = set_kv(opentelemetry_proto_trace_v1_Span_Event_add_attributes(msg, arena), &kv, arena);
    }
    return ok;
}

static int build_status(const microtel_leaf_internal_span* s, span_msg* span, upb_Arena* arena)
{
    opentelemetry_proto_trace_v1_Status* status;
    if (!s->has_status)
    {
        return 1;
    }
    status = opentelemetry_proto_trace_v1_Span_mutable_status(span, arena);
    if (status == NULL)
    {
        return 0;
    }
    opentelemetry_proto_trace_v1_Status_set_message(status,
                                                    view(s->status_message, s->status_message_len));
    opentelemetry_proto_trace_v1_Status_set_code(status, (int32_t)s->status_code);
    return 1;
}

static void set_span_scalars(const microtel_leaf_internal_span* s, span_msg* span)
{
    opentelemetry_proto_trace_v1_Span_set_trace_id(span, bytes_view(s->trace_id, TRACE_ID_BYTES));
    opentelemetry_proto_trace_v1_Span_set_span_id(span, bytes_view(s->span_id, SPAN_ID_BYTES));
    if (s->parent_span_id != NULL)
    {
        opentelemetry_proto_trace_v1_Span_set_parent_span_id(
            span, bytes_view(s->parent_span_id, SPAN_ID_BYTES));
    }
    opentelemetry_proto_trace_v1_Span_set_name(span, view(s->name, s->name_len));
    opentelemetry_proto_trace_v1_Span_set_kind(span, (int32_t)s->kind);
    opentelemetry_proto_trace_v1_Span_set_start_time_unix_nano(span, s->start_time_unix_nano);
    opentelemetry_proto_trace_v1_Span_set_end_time_unix_nano(span, s->end_time_unix_nano);
}

static int build_span(const microtel_leaf_internal_batch* batch,
                      const microtel_leaf_internal_span* s,
                      opentelemetry_proto_trace_v1_ScopeSpans* ss,
                      upb_Arena* arena)
{
    microtel_leaf_internal_cursor ac = {0u, 0u};
    microtel_leaf_internal_cursor ec = {0u, 0u};
    microtel_leaf_kv_t kv;
    microtel_leaf_internal_event ev;
    span_msg* span = opentelemetry_proto_trace_v1_ScopeSpans_add_spans(ss, arena);
    int ok = span != NULL;
    if (ok)
    {
        set_span_scalars(s, span);
    }
    while (ok && microtel_leaf_internal_next_attr(batch, s, &ac, &kv))
    {
        ok = set_kv(opentelemetry_proto_trace_v1_Span_add_attributes(span, arena), &kv, arena);
    }
    while (ok && microtel_leaf_internal_next_event(batch, s, &ec, &ev))
    {
        ok = build_event(&ev, span, arena);
    }
    return ok && build_status(s, span, arena);
}

static int build_scope_spans(const microtel_leaf_internal_batch* batch,
                             opentelemetry_proto_trace_v1_ResourceSpans* rs,
                             upb_Arena* arena)
{
    microtel_leaf_internal_cursor c = {0u, 0u};
    microtel_leaf_internal_span s;
    opentelemetry_proto_common_v1_InstrumentationScope* scope = NULL;
    opentelemetry_proto_trace_v1_ScopeSpans* ss =
        opentelemetry_proto_trace_v1_ResourceSpans_add_scope_spans(rs, arena);
    int ok;
    if (ss != NULL)
    {
        scope = opentelemetry_proto_trace_v1_ScopeSpans_mutable_scope(ss, arena);
    }
    ok = scope != NULL;
    if (ok)
    {
        opentelemetry_proto_common_v1_InstrumentationScope_set_name(
            scope, view(batch->scope_name, batch->scope_name_len));
        opentelemetry_proto_common_v1_InstrumentationScope_set_version(
            scope, view(batch->scope_version, batch->scope_version_len));
    }
    while (ok && microtel_leaf_internal_next_span(batch, &c, &s))
    {
        ok = build_span(batch, &s, ss, arena);
    }
    return ok;
}

/* Builds the request and encodes it into the arena. */
static microtel_leaf_status_t encode_request(const microtel_leaf_internal_batch* batch,
                                             upb_Arena* arena,
                                             char** bytes,
                                             size_t* size)
{
    opentelemetry_proto_collector_trace_v1_ExportTraceServiceRequest* req =
        opentelemetry_proto_collector_trace_v1_ExportTraceServiceRequest_new(arena);
    opentelemetry_proto_trace_v1_ResourceSpans* rs = NULL;
    if (req != NULL)
    {
        rs = opentelemetry_proto_collector_trace_v1_ExportTraceServiceRequest_add_resource_spans(
            req, arena);
    }
    if (rs == NULL || !build_resource(batch, rs, arena) || !build_scope_spans(batch, rs, arena))
    {
        return MICROTEL_LEAF_ERR_ENCODE;
    }
    *bytes = opentelemetry_proto_collector_trace_v1_ExportTraceServiceRequest_serialize(
        req, arena, size);
    return *bytes != NULL ? MICROTEL_LEAF_OK : MICROTEL_LEAF_ERR_ENCODE;
}

/* Hands the encoded bytes to the sink. upb cannot stream, so a streaming sink
 * gets the whole payload in one write (§1.8). */
static microtel_leaf_status_t deliver(const microtel_leaf_internal_sink* sink,
                                      const char* bytes,
                                      size_t size)
{
    if (sink->write != NULL)
    {
        return sink->write(sink->write_ctx, (const uint8_t*)bytes, size) == 0
                   ? MICROTEL_LEAF_OK
                   : MICROTEL_LEAF_ERR_ENCODE;
    }
    if (size > sink->cap)
    {
        return MICROTEL_LEAF_ERR_BUFFER_SMALL;
    }
    memcpy(sink->buf, bytes, size);
    return MICROTEL_LEAF_OK;
}

microtel_leaf_status_t microtel_leaf_internal_encode_upb(const microtel_leaf_internal_batch* batch,
                                                         const microtel_leaf_internal_sink* sink,
                                                         void* scratch,
                                                         size_t scratch_size,
                                                         size_t* written)
{
    char* bytes = NULL;
    size_t size = 0;
    microtel_leaf_status_t status;
    upb_Arena* arena =
        scratch != NULL ? upb_Arena_Init(scratch, scratch_size, &s_refuse) : upb_Arena_New();
    *written = 0;
    if (arena == NULL)
    {
        return MICROTEL_LEAF_ERR_ENCODE;
    }
    status = encode_request(batch, arena, &bytes, &size);
    if (status == MICROTEL_LEAF_OK)
    {
        status = deliver(sink, bytes, size);
    }
    if (status == MICROTEL_LEAF_OK || status == MICROTEL_LEAF_ERR_BUFFER_SMALL)
    {
        *written = size;
    }
    upb_Arena_Free(arena);
    return status;
}
