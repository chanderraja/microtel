/* Copyright (c) 2026 The microtel Authors.
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * The nanopb encoder backend (docs/leaf-concentrator-design.md §2.2). The only
 * leaf file that includes nanopb headers.
 *
 * Encodes the ExportTraceServiceRequest with pb_encode over the generated
 * descriptors under gen/nanopb/. Every string and repeated field is
 * FT_CALLBACK (leaf/nanopb/otlp_trace.options), so the message structs hold
 * no arrays: each callback builds the next level's struct on the stack from a
 * cursor into the record buffer and encodes it. Nothing is allocated; the
 * only libc calls are memcpy and memset.
 *
 * nanopb sizes a submessage by running its callbacks once with a sizing
 * stream and then again to write it, so a callback nested d submessages deep
 * runs d + 1 times per encode. Every callback therefore starts its cursor
 * from zero and reads only the batch: it is a pure function of it.
 *
 * Byte identity with the upb backend (§2.3): the generator sorts each
 * descriptor by tag, so fields come out in field-number order; nanopb omits
 * proto3 scalars equal to 0, and the string callbacks write nothing for an
 * empty string, except the AnyValue member, which is a oneof and is always
 * written; submessages are present through their `has_` flag exactly where
 * upb creates them.
 *
 * Buffer mode encodes straight into the caller's buffer. When it does not
 * fit, the encode stops at the first write that would overflow, the size is
 * measured with a sizing pass, and MICROTEL_LEAF_ERR_BUFFER_SMALL reports it;
 * the buffer may then hold a partial payload. Streaming mode hands every
 * piece nanopb writes to the sink as it is produced, so the payload is never
 * held in memory as a whole (§1.8).
 */

#include "leaf_internal.h"
#include "opentelemetry/proto/collector/trace/v1/trace_service.pb.h"
#include "opentelemetry/proto/common/v1/common.pb.h"
#include "opentelemetry/proto/resource/v1/resource.pb.h"
#include "opentelemetry/proto/trace/v1/trace.pb.h"

#include <pb.h>
#include <pb_encode.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define TRACE_ID_BYTES 16u
#define SPAN_ID_BYTES 8u

typedef opentelemetry_proto_common_v1_KeyValue key_value;
typedef opentelemetry_proto_trace_v1_Span span_msg;
typedef opentelemetry_proto_trace_v1_Span_Event event_msg;

/* A borrowed string, the argument of the string callbacks. */
typedef struct str_view
{
    const char* ptr;
    size_t len;
} str_view;

/* Which list of attributes an attribute callback walks. */
typedef enum attr_list
{
    ATTRS_RESOURCE,
    ATTRS_SPAN,
    ATTRS_EVENT
} attr_list;

/* The argument of the attribute callbacks. */
typedef struct attr_source
{
    attr_list list;
    const microtel_leaf_internal_batch* batch;
    const microtel_leaf_internal_span* span;
    const microtel_leaf_internal_event* event;
} attr_source;

/* The argument of the event callback. */
typedef struct span_source
{
    const microtel_leaf_internal_batch* batch;
    const microtel_leaf_internal_span* span;
} span_source;

/* ------------------------------------------------------------------------ */
/* Strings                                                                  */
/* ------------------------------------------------------------------------ */

static bool write_string(pb_ostream_t* stream, const pb_field_t* field, const char* ptr, size_t len)
{
    return pb_encode_tag_for_field(stream, field) &&
           pb_encode_string(stream, (const pb_byte_t*)ptr, len);
}

/* A proto3 string: omitted when empty, as upb omits it. */
static bool encode_str(pb_ostream_t* stream, const pb_field_t* field, void* const* arg)
{
    const str_view* s = (const str_view*)*arg;
    return s->len == 0u || write_string(stream, field, s->ptr, s->len);
}

/* KeyValue.key; the argument is the microtel_leaf_kv_t. Never empty: the core
 * rejects an empty key. */
static bool encode_kv_key(pb_ostream_t* stream, const pb_field_t* field, void* const* arg)
{
    const microtel_leaf_kv_t* kv = (const microtel_leaf_kv_t*)*arg;
    return write_string(stream, field, kv->key, kv->key_len);
}

/* AnyValue.string_value: a oneof member, so written even when empty. */
static bool encode_kv_string(pb_ostream_t* stream, const pb_field_t* field, void* const* arg)
{
    const microtel_leaf_kv_t* kv = (const microtel_leaf_kv_t*)*arg;
    return write_string(stream, field, kv->value.s.ptr, kv->value.s.len);
}

static void set_str(pb_callback_t* cb, str_view* view, const char* ptr, size_t len)
{
    view->ptr = ptr;
    view->len = len;
    cb->funcs.encode = &encode_str;
    cb->arg = view;
}

/* ------------------------------------------------------------------------ */
/* Attributes                                                               */
/* ------------------------------------------------------------------------ */

/* Fills a KeyValue that reads its strings from `kv`, which must outlive it. */
static void fill_kv(key_value* msg, microtel_leaf_kv_t* kv)
{
    memset(msg, 0, sizeof(*msg));
    msg->key.funcs.encode = &encode_kv_key;
    msg->key.arg = kv;
    msg->has_value = true;
    switch (kv->type)
    {
        case MICROTEL_LEAF_VALUE_BOOL:
            msg->value.which_value = opentelemetry_proto_common_v1_AnyValue_bool_value_tag;
            msg->value.value.bool_value = kv->value.b != 0;
            break;
        case MICROTEL_LEAF_VALUE_INT64:
            msg->value.which_value = opentelemetry_proto_common_v1_AnyValue_int_value_tag;
            msg->value.value.int_value = kv->value.i;
            break;
        case MICROTEL_LEAF_VALUE_DOUBLE:
            msg->value.which_value = opentelemetry_proto_common_v1_AnyValue_double_value_tag;
            msg->value.value.double_value = kv->value.d;
            break;
        default:
            msg->value.which_value = opentelemetry_proto_common_v1_AnyValue_string_value_tag;
            msg->value.value.string_value.funcs.encode = &encode_kv_string;
            msg->value.value.string_value.arg = kv;
            break;
    }
}

static int next_attr(const attr_source* src,
                     microtel_leaf_internal_cursor* cursor,
                     microtel_leaf_kv_t* out)
{
    switch (src->list)
    {
        case ATTRS_RESOURCE:
            return microtel_leaf_internal_next_resource_attr(src->batch, cursor, out);
        case ATTRS_SPAN:
            return microtel_leaf_internal_next_attr(src->batch, src->span, cursor, out);
        default:
            return microtel_leaf_internal_next_event_attr(src->event, cursor, out);
    }
}

/* A repeated KeyValue field, in the order the core's cursor returns. */
static bool encode_attrs(pb_ostream_t* stream, const pb_field_t* field, void* const* arg)
{
    const attr_source* src = (const attr_source*)*arg;
    microtel_leaf_internal_cursor c = {0u, 0u};
    microtel_leaf_kv_t kv;
    key_value msg;
    bool ok = true;
    while (ok && next_attr(src, &c, &kv))
    {
        fill_kv(&msg, &kv);
        ok = pb_encode_tag_for_field(stream, field) &&
             pb_encode_submessage(stream, opentelemetry_proto_common_v1_KeyValue_fields, &msg);
    }
    return ok;
}

static void set_attrs(pb_callback_t* cb, attr_source* src)
{
    cb->funcs.encode = &encode_attrs;
    cb->arg = src;
}

/* ------------------------------------------------------------------------ */
/* Events and spans                                                         */
/* ------------------------------------------------------------------------ */

static bool encode_events(pb_ostream_t* stream, const pb_field_t* field, void* const* arg)
{
    const span_source* src = (const span_source*)*arg;
    microtel_leaf_internal_cursor c = {0u, 0u};
    microtel_leaf_internal_event ev;
    str_view name;
    attr_source attrs;
    event_msg msg;
    bool ok = true;
    while (ok && microtel_leaf_internal_next_event(src->batch, src->span, &c, &ev))
    {
        memset(&msg, 0, sizeof(msg));
        memset(&attrs, 0, sizeof(attrs));
        msg.time_unix_nano = ev.time_unix_nano;
        set_str(&msg.name, &name, ev.name, ev.name_len);
        attrs.list = ATTRS_EVENT;
        attrs.event = &ev;
        set_attrs(&msg.attributes, &attrs);
        ok = pb_encode_tag_for_field(stream, field) &&
             pb_encode_submessage(stream, opentelemetry_proto_trace_v1_Span_Event_fields, &msg);
    }
    return ok;
}

/* The strings and lists of one span message; lives while it is encoded. */
typedef struct span_args
{
    str_view name;
    str_view status_message;
    attr_source attrs;
    span_source events;
} span_args;

/* Fills a Span that reads its strings and lists through `args`. */
static void fill_span(span_msg* msg,
                      span_args* args,
                      const microtel_leaf_internal_batch* batch,
                      const microtel_leaf_internal_span* s)
{
    memset(msg, 0, sizeof(*msg));
    memset(args, 0, sizeof(*args));
    memcpy(msg->trace_id, s->trace_id, TRACE_ID_BYTES);
    memcpy(msg->span_id, s->span_id, SPAN_ID_BYTES);
    if (s->parent_span_id != NULL)
    {
        memcpy(msg->parent_span_id.bytes, s->parent_span_id, SPAN_ID_BYTES);
        msg->parent_span_id.size = SPAN_ID_BYTES;
    }
    set_str(&msg->name, &args->name, s->name, s->name_len);
    msg->kind = (opentelemetry_proto_trace_v1_Span_SpanKind)s->kind;
    msg->start_time_unix_nano = s->start_time_unix_nano;
    msg->end_time_unix_nano = s->end_time_unix_nano;
    args->attrs.list = ATTRS_SPAN;
    args->attrs.batch = batch;
    args->attrs.span = s;
    set_attrs(&msg->attributes, &args->attrs);
    args->events.batch = batch;
    args->events.span = s;
    msg->events.funcs.encode = &encode_events;
    msg->events.arg = &args->events;
    msg->has_status = s->has_status != 0;
    set_str(&msg->status.message, &args->status_message, s->status_message, s->status_message_len);
    msg->status.code = (opentelemetry_proto_trace_v1_Status_StatusCode)s->status_code;
}

static bool encode_spans(pb_ostream_t* stream, const pb_field_t* field, void* const* arg)
{
    const microtel_leaf_internal_batch* batch = (const microtel_leaf_internal_batch*)*arg;
    microtel_leaf_internal_cursor c = {0u, 0u};
    microtel_leaf_internal_span s;
    span_args args;
    span_msg msg;
    bool ok = true;
    while (ok && microtel_leaf_internal_next_span(batch, &c, &s))
    {
        fill_span(&msg, &args, batch, &s);
        ok = pb_encode_tag_for_field(stream, field) &&
             pb_encode_submessage(stream, opentelemetry_proto_trace_v1_Span_fields, &msg);
    }
    return ok;
}

/* ------------------------------------------------------------------------ */
/* The single ResourceSpans and ScopeSpans (§2.3 rule 5)                    */
/* ------------------------------------------------------------------------ */

static bool encode_scope_spans(pb_ostream_t* stream, const pb_field_t* field, void* const* arg)
{
    microtel_leaf_internal_batch* batch = (microtel_leaf_internal_batch*)*arg;
    opentelemetry_proto_trace_v1_ScopeSpans msg;
    str_view name;
    str_view version;
    memset(&msg, 0, sizeof(msg));
    msg.has_scope = true;
    set_str(&msg.scope.name, &name, batch->scope_name, batch->scope_name_len);
    set_str(&msg.scope.version, &version, batch->scope_version, batch->scope_version_len);
    msg.spans.funcs.encode = &encode_spans;
    msg.spans.arg = batch;
    return pb_encode_tag_for_field(stream, field) &&
           pb_encode_submessage(stream, opentelemetry_proto_trace_v1_ScopeSpans_fields, &msg);
}

static bool encode_resource_spans(pb_ostream_t* stream, const pb_field_t* field, void* const* arg)
{
    microtel_leaf_internal_batch* batch = (microtel_leaf_internal_batch*)*arg;
    opentelemetry_proto_trace_v1_ResourceSpans msg;
    attr_source attrs;
    memset(&msg, 0, sizeof(msg));
    memset(&attrs, 0, sizeof(attrs));
    msg.has_resource = true;
    attrs.list = ATTRS_RESOURCE;
    attrs.batch = batch;
    set_attrs(&msg.resource.attributes, &attrs);
    msg.scope_spans.funcs.encode = &encode_scope_spans;
    msg.scope_spans.arg = batch;
    return pb_encode_tag_for_field(stream, field) &&
           pb_encode_submessage(stream, opentelemetry_proto_trace_v1_ResourceSpans_fields, &msg);
}

/* ------------------------------------------------------------------------ */
/* Entry point                                                              */
/* ------------------------------------------------------------------------ */

/* Streaming mode: every piece goes straight to the application's sink. */
static bool sink_write(pb_ostream_t* stream, const pb_byte_t* buf, size_t count)
{
    const microtel_leaf_internal_sink* sink = (const microtel_leaf_internal_sink*)stream->state;
    return sink->write(sink->write_ctx, buf, count) == 0;
}

microtel_leaf_status_t microtel_leaf_internal_encode_nanopb(
    const microtel_leaf_internal_batch* batch,
    const microtel_leaf_internal_sink* sink,
    void* scratch,
    size_t scratch_size,
    size_t* written)
{
    /* The callbacks take a non-const argument; they only read the batch and
     * the sink through it. */
    microtel_leaf_internal_batch view = *batch;
    microtel_leaf_internal_sink out = *sink;
    opentelemetry_proto_collector_trace_v1_ExportTraceServiceRequest req;
    pb_ostream_t stream;
    size_t size = 0;
    (void)scratch;
    (void)scratch_size;
    *written = 0;
    memset(&req, 0, sizeof(req));
    req.resource_spans.funcs.encode = &encode_resource_spans;
    req.resource_spans.arg = &view;
    if (out.write != NULL)
    {
        memset(&stream, 0, sizeof(stream));
        stream.callback = &sink_write;
        stream.state = &out;
        stream.max_size = SIZE_MAX;
    }
    else
    {
        stream = pb_ostream_from_buffer(out.buf, out.cap);
    }
    if (pb_encode(
            &stream, opentelemetry_proto_collector_trace_v1_ExportTraceServiceRequest_fields, &req))
    {
        *written = stream.bytes_written;
        return MICROTEL_LEAF_OK;
    }
    /* A buffer stream fails only when the payload does not fit. */
    if (out.write == NULL &&
        pb_get_encoded_size(
            &size, opentelemetry_proto_collector_trace_v1_ExportTraceServiceRequest_fields, &req))
    {
        *written = size;
        return MICROTEL_LEAF_ERR_BUFFER_SMALL;
    }
    return MICROTEL_LEAF_ERR_ENCODE;
}
