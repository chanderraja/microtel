// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Decodes a leaf payload with upb into plain C++ values, so the leaf tests can
// assert on what went over the wire field by field. Test-only; the leaf itself
// never decodes.

#pragma once

#include "microtel/leaf.h"

// upb C headers use flexible array members — suppress the pedantic warning.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#include "opentelemetry/proto/collector/trace/v1/trace_service.upb.h"
#include "opentelemetry/proto/common/v1/common.upb.h"
#include "opentelemetry/proto/resource/v1/resource.upb.h"
#include "opentelemetry/proto/trace/v1/trace.upb.h"
#include "upb/mem/arena.h"
#pragma GCC diagnostic pop

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace microtel::leaf_test
{

/// One decoded KeyValue. `type` is which AnyValue member was present.
struct DecodedKv
{
    std::string key;
    int type = -1;
    bool b = false;
    std::int64_t i = 0;
    double d = 0.0;
    std::string s;
};

struct DecodedEvent
{
    std::uint64_t time = 0;
    std::string name;
    std::vector<DecodedKv> attrs;
};

struct DecodedSpan
{
    std::string trace_id;
    std::string span_id;
    std::string parent_span_id;
    std::string name;
    int kind = 0;
    std::uint64_t start = 0;
    std::uint64_t end = 0;
    bool has_status = false;
    int status_code = 0;
    std::string status_message;
    std::vector<DecodedKv> attrs;
    std::vector<DecodedEvent> events;
};

struct DecodedPayload
{
    std::size_t resource_spans_count = 0;
    bool has_resource = false;
    std::vector<DecodedKv> resource;
    std::size_t scope_spans_count = 0;
    bool has_scope = false;
    std::string scope_name;
    std::string scope_version;
    std::vector<DecodedSpan> spans;

    /// The resource attribute named `key`, if present.
    [[nodiscard]] std::optional<DecodedKv> ResourceAttr(const std::string& key) const
    {
        for (const auto& kv : resource)
        {
            if (kv.key == key)
            {
                return kv;
            }
        }
        return std::nullopt;
    }
};

inline std::string ToString(upb_StringView v)
{
    return {v.data, v.size};
}

inline DecodedKv DecodeKv(const opentelemetry_proto_common_v1_KeyValue* kv)
{
    DecodedKv out;
    out.key = ToString(opentelemetry_proto_common_v1_KeyValue_key(kv));
    const auto* v = opentelemetry_proto_common_v1_KeyValue_value(kv);
    if (v == nullptr)
    {
        return out;
    }
    if (opentelemetry_proto_common_v1_AnyValue_has_bool_value(v))
    {
        out.type = MICROTEL_LEAF_VALUE_BOOL;
        out.b = opentelemetry_proto_common_v1_AnyValue_bool_value(v);
    }
    else if (opentelemetry_proto_common_v1_AnyValue_has_int_value(v))
    {
        out.type = MICROTEL_LEAF_VALUE_INT64;
        out.i = opentelemetry_proto_common_v1_AnyValue_int_value(v);
    }
    else if (opentelemetry_proto_common_v1_AnyValue_has_double_value(v))
    {
        out.type = MICROTEL_LEAF_VALUE_DOUBLE;
        out.d = opentelemetry_proto_common_v1_AnyValue_double_value(v);
    }
    else if (opentelemetry_proto_common_v1_AnyValue_has_string_value(v))
    {
        out.type = MICROTEL_LEAF_VALUE_STRING;
        out.s = ToString(opentelemetry_proto_common_v1_AnyValue_string_value(v));
    }
    return out;
}

inline std::vector<DecodedKv> DecodeKvs(const opentelemetry_proto_common_v1_KeyValue* const* kvs,
                                        std::size_t n)
{
    std::vector<DecodedKv> out;
    for (std::size_t i = 0; i < n; ++i)
    {
        out.push_back(DecodeKv(kvs[i]));
    }
    return out;
}

inline DecodedSpan DecodeSpan(const opentelemetry_proto_trace_v1_Span* s)
{
    DecodedSpan out;
    out.trace_id = ToString(opentelemetry_proto_trace_v1_Span_trace_id(s));
    out.span_id = ToString(opentelemetry_proto_trace_v1_Span_span_id(s));
    out.parent_span_id = ToString(opentelemetry_proto_trace_v1_Span_parent_span_id(s));
    out.name = ToString(opentelemetry_proto_trace_v1_Span_name(s));
    out.kind = opentelemetry_proto_trace_v1_Span_kind(s);
    out.start = opentelemetry_proto_trace_v1_Span_start_time_unix_nano(s);
    out.end = opentelemetry_proto_trace_v1_Span_end_time_unix_nano(s);
    std::size_t n = 0;
    const auto* attrs = opentelemetry_proto_trace_v1_Span_attributes(s, &n);
    out.attrs = DecodeKvs(attrs, n);
    const auto* events = opentelemetry_proto_trace_v1_Span_events(s, &n);
    for (std::size_t i = 0; i < n; ++i)
    {
        DecodedEvent ev;
        ev.time = opentelemetry_proto_trace_v1_Span_Event_time_unix_nano(events[i]);
        ev.name = ToString(opentelemetry_proto_trace_v1_Span_Event_name(events[i]));
        std::size_t an = 0;
        const auto* ea = opentelemetry_proto_trace_v1_Span_Event_attributes(events[i], &an);
        ev.attrs = DecodeKvs(ea, an);
        out.events.push_back(ev);
    }
    out.has_status = opentelemetry_proto_trace_v1_Span_has_status(s);
    if (out.has_status)
    {
        const auto* st = opentelemetry_proto_trace_v1_Span_status(s);
        out.status_code = opentelemetry_proto_trace_v1_Status_code(st);
        out.status_message = ToString(opentelemetry_proto_trace_v1_Status_message(st));
    }
    return out;
}

/// Decodes `bytes`. Returns nullopt if upb rejects them.
inline std::optional<DecodedPayload> Decode(const std::uint8_t* bytes, std::size_t len)
{
    upb_Arena* arena = upb_Arena_New();
    const auto* req = opentelemetry_proto_collector_trace_v1_ExportTraceServiceRequest_parse(
        reinterpret_cast<const char*>(bytes), len, arena);
    if (req == nullptr)
    {
        upb_Arena_Free(arena);
        return std::nullopt;
    }
    DecodedPayload out;
    std::size_t n = 0;
    const auto* rss =
        opentelemetry_proto_collector_trace_v1_ExportTraceServiceRequest_resource_spans(req, &n);
    out.resource_spans_count = n;
    if (n > 0)
    {
        const auto* rs = rss[0];
        out.has_resource = opentelemetry_proto_trace_v1_ResourceSpans_has_resource(rs);
        if (out.has_resource)
        {
            std::size_t rn = 0;
            const auto* res = opentelemetry_proto_trace_v1_ResourceSpans_resource(rs);
            const auto* kvs = opentelemetry_proto_resource_v1_Resource_attributes(res, &rn);
            out.resource = DecodeKvs(kvs, rn);
        }
        std::size_t sn = 0;
        const auto* sss = opentelemetry_proto_trace_v1_ResourceSpans_scope_spans(rs, &sn);
        out.scope_spans_count = sn;
        if (sn > 0)
        {
            out.has_scope = opentelemetry_proto_trace_v1_ScopeSpans_has_scope(sss[0]);
            if (out.has_scope)
            {
                const auto* sc = opentelemetry_proto_trace_v1_ScopeSpans_scope(sss[0]);
                out.scope_name =
                    ToString(opentelemetry_proto_common_v1_InstrumentationScope_name(sc));
                out.scope_version =
                    ToString(opentelemetry_proto_common_v1_InstrumentationScope_version(sc));
            }
            std::size_t pn = 0;
            const auto* spans = opentelemetry_proto_trace_v1_ScopeSpans_spans(sss[0], &pn);
            for (std::size_t i = 0; i < pn; ++i)
            {
                out.spans.push_back(DecodeSpan(spans[i]));
            }
        }
    }
    upb_Arena_Free(arena);
    return out;
}

}  // namespace microtel::leaf_test
