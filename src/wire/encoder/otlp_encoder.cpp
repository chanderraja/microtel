// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// LOCKED: this is the only file in the project that #includes upb headers.
// See docs/interfaces.md §4.2 / docs/memory-model.md §3.1.

#include "otlp_encoder.hpp"

// upb C headers use flexible array members (C99/C11, not ISO C++).
// Suppress the pedantic warning for this include block only.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#include "opentelemetry/proto/collector/trace/v1/trace_service.upb.h"
#include "opentelemetry/proto/common/v1/common.upb.h"
#include "opentelemetry/proto/resource/v1/resource.upb.h"
#include "opentelemetry/proto/trace/v1/trace.upb.h"
#include "upb/mem/arena.h"
#pragma GCC diagnostic pop

// microtel public + internal headers
#include "microtel/attribute.hpp"
#include "microtel/internal/batch.hpp"
#include "microtel/internal/encoded_payload.hpp"
#include "microtel/resource.hpp"
#include "microtel/trace.hpp"

#include <chrono>
#include <cstddef>
#include <cstring>
#include <memory>
#include <variant>

namespace microtel::wire
{

namespace
{

// ---------------------------------------------------------------------------
// Type aliases — shorten the upb generated names inside this TU only.
// ---------------------------------------------------------------------------

using UpbKV       = opentelemetry_proto_common_v1_KeyValue;
using UpbAnyValue = opentelemetry_proto_common_v1_AnyValue;
using UpbArrayVal = opentelemetry_proto_common_v1_ArrayValue;
using UpbScope    = opentelemetry_proto_common_v1_InstrumentationScope;
using UpbResource = opentelemetry_proto_resource_v1_Resource;
using UpbResSp    = opentelemetry_proto_trace_v1_ResourceSpans;
using UpbScopeSp  = opentelemetry_proto_trace_v1_ScopeSpans;
using UpbSpan     = opentelemetry_proto_trace_v1_Span;
using UpbEvent    = opentelemetry_proto_trace_v1_Span_Event;
using UpbLink     = opentelemetry_proto_trace_v1_Span_Link;
using UpbStatus   = opentelemetry_proto_trace_v1_Status;
using UpbRequest  = opentelemetry_proto_collector_trace_v1_ExportTraceServiceRequest;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

upb_StringView Sv(const std::string& s) noexcept
{
    return upb_StringView_FromDataAndSize(s.data(), s.size());
}

upb_StringView SvBytes(const std::uint8_t* data, std::size_t size) noexcept
{
    return upb_StringView_FromDataAndSize(reinterpret_cast<const char*>(data), size);
}

std::uint64_t ToNanos(std::chrono::system_clock::time_point tp) noexcept
{
    return static_cast<std::uint64_t>(
        std::chrono::time_point_cast<std::chrono::nanoseconds>(tp)
            .time_since_epoch()
            .count());
}

// SpanKind: microtel values are Internal=0..Consumer=4; OTLP proto is
// UNSPECIFIED=0, INTERNAL=1 .. CONSUMER=5 — add 1 to remap.
int32_t MapKind(microtel::SpanKind kind) noexcept
{
    return static_cast<int32_t>(kind) + 1;
}

// StatusCode: both enums use 0=Unset, 1=Ok, 2=Error — direct cast is safe.
int32_t MapStatusCode(microtel::StatusCode code) noexcept
{
    return static_cast<int32_t>(code);
}

// ---------------------------------------------------------------------------
// Attribute encoding
// ---------------------------------------------------------------------------

void EncodeAnyValueStr(UpbAnyValue* av, const std::string& s)
{
    opentelemetry_proto_common_v1_AnyValue_set_string_value(av, Sv(s));
}

void EncodeAnyValueBool(UpbAnyValue* av, bool v)
{
    opentelemetry_proto_common_v1_AnyValue_set_bool_value(av, v);
}

void EncodeAnyValueInt(UpbAnyValue* av, std::int64_t v)
{
    opentelemetry_proto_common_v1_AnyValue_set_int_value(av, v);
}

void EncodeAnyValueDouble(UpbAnyValue* av, double v)
{
    opentelemetry_proto_common_v1_AnyValue_set_double_value(av, v);
}

void EncodeArrayBool(UpbAnyValue* av, const std::vector<bool>& vec, upb_Arena* arena)
{
    UpbArrayVal* arr = opentelemetry_proto_common_v1_AnyValue_mutable_array_value(av, arena);
    for (const bool elem : vec)
    {
        UpbAnyValue* item = opentelemetry_proto_common_v1_ArrayValue_add_values(arr, arena);
        if (item != nullptr)
        {
            opentelemetry_proto_common_v1_AnyValue_set_bool_value(item, elem);
        }
    }
}

void EncodeArrayInt(UpbAnyValue* av, const std::vector<std::int64_t>& vec, upb_Arena* arena)
{
    UpbArrayVal* arr = opentelemetry_proto_common_v1_AnyValue_mutable_array_value(av, arena);
    for (const std::int64_t elem : vec)
    {
        UpbAnyValue* item = opentelemetry_proto_common_v1_ArrayValue_add_values(arr, arena);
        if (item != nullptr)
        {
            opentelemetry_proto_common_v1_AnyValue_set_int_value(item, elem);
        }
    }
}

void EncodeArrayDouble(UpbAnyValue* av, const std::vector<double>& vec, upb_Arena* arena)
{
    UpbArrayVal* arr = opentelemetry_proto_common_v1_AnyValue_mutable_array_value(av, arena);
    for (const double elem : vec)
    {
        UpbAnyValue* item = opentelemetry_proto_common_v1_ArrayValue_add_values(arr, arena);
        if (item != nullptr)
        {
            opentelemetry_proto_common_v1_AnyValue_set_double_value(item, elem);
        }
    }
}

void EncodeArrayStr(UpbAnyValue* av, const std::vector<std::string>& vec, upb_Arena* arena)
{
    UpbArrayVal* arr = opentelemetry_proto_common_v1_AnyValue_mutable_array_value(av, arena);
    for (const std::string& elem : vec)
    {
        UpbAnyValue* item = opentelemetry_proto_common_v1_ArrayValue_add_values(arr, arena);
        if (item != nullptr)
        {
            opentelemetry_proto_common_v1_AnyValue_set_string_value(item, Sv(elem));
        }
    }
}

void EncodeAttrValue(UpbAnyValue* av, const microtel::AttributeValue& val, upb_Arena* arena)
{
    std::visit(
        [av, arena](const auto& v)
        {
            using T = std::decay_t<decltype(v)>;
            if constexpr (std::is_same_v<T, bool>)
            {
                EncodeAnyValueBool(av, v);
            }
            else if constexpr (std::is_same_v<T, std::int64_t>)
            {
                EncodeAnyValueInt(av, v);
            }
            else if constexpr (std::is_same_v<T, double>)
            {
                EncodeAnyValueDouble(av, v);
            }
            else if constexpr (std::is_same_v<T, std::string>)
            {
                EncodeAnyValueStr(av, v);
            }
            else if constexpr (std::is_same_v<T, std::vector<bool>>)
            {
                EncodeArrayBool(av, v, arena);
            }
            else if constexpr (std::is_same_v<T, std::vector<std::int64_t>>)
            {
                EncodeArrayInt(av, v, arena);
            }
            else if constexpr (std::is_same_v<T, std::vector<double>>)
            {
                EncodeArrayDouble(av, v, arena);
            }
            else if constexpr (std::is_same_v<T, std::vector<std::string>>)
            {
                EncodeArrayStr(av, v, arena);
            }
        },
        val);
}

void EncodeAttributes(
    const std::vector<microtel::KeyValue>& attrs,
    upb_Arena* arena,
    UpbKV* (*add_fn)(void*, upb_Arena*),
    void* msg)
{
    for (const auto& kv : attrs)
    {
        UpbKV* ukv = add_fn(msg, arena);
        if (ukv == nullptr)
        {
            continue;
        }
        opentelemetry_proto_common_v1_KeyValue_set_key(ukv, Sv(kv.key));
        UpbAnyValue* av = opentelemetry_proto_common_v1_KeyValue_mutable_value(ukv, arena);
        if (av != nullptr)
        {
            EncodeAttrValue(av, kv.value, arena);
        }
    }
}

// ---------------------------------------------------------------------------
// Event / Link encoding
// ---------------------------------------------------------------------------

void EncodeEvent(const internal::SpanEvent& ev, UpbSpan* span, upb_Arena* arena)
{
    UpbEvent* uev = opentelemetry_proto_trace_v1_Span_add_events(span, arena);
    if (uev == nullptr)
    {
        return;
    }
    opentelemetry_proto_trace_v1_Span_Event_set_name(uev, Sv(ev.name));
    opentelemetry_proto_trace_v1_Span_Event_set_time_unix_nano(uev, ToNanos(ev.timestamp));

    EncodeAttributes(
        ev.attributes, arena,
        reinterpret_cast<UpbKV* (*)(void*, upb_Arena*)>(
            opentelemetry_proto_trace_v1_Span_Event_add_attributes),
        uev);
}

void EncodeLink(const internal::SpanLink& lnk, UpbSpan* span, upb_Arena* arena)
{
    UpbLink* ulnk = opentelemetry_proto_trace_v1_Span_add_links(span, arena);
    if (ulnk == nullptr)
    {
        return;
    }
    const auto& tc = lnk.linked_context;
    opentelemetry_proto_trace_v1_Span_Link_set_trace_id(
        ulnk, SvBytes(tc.trace_id.AsBytes().data(), tc.trace_id.AsBytes().size()));
    opentelemetry_proto_trace_v1_Span_Link_set_span_id(
        ulnk, SvBytes(tc.span_id.AsBytes().data(), tc.span_id.AsBytes().size()));

    EncodeAttributes(
        lnk.attributes, arena,
        reinterpret_cast<UpbKV* (*)(void*, upb_Arena*)>(
            opentelemetry_proto_trace_v1_Span_Link_add_attributes),
        ulnk);
}

// ---------------------------------------------------------------------------
// Status encoding
// ---------------------------------------------------------------------------

void EncodeStatus(const internal::SpanRecord& rec, UpbSpan* span, upb_Arena* arena)
{
    if (rec.status_code == microtel::StatusCode::Unset)
    {
        return;
    }
    UpbStatus* st = opentelemetry_proto_trace_v1_Span_mutable_status(span, arena);
    if (st == nullptr)
    {
        return;
    }
    opentelemetry_proto_trace_v1_Status_set_code(st, MapStatusCode(rec.status_code));
    if (!rec.status_description.empty())
    {
        opentelemetry_proto_trace_v1_Status_set_message(st, Sv(rec.status_description));
    }
}

// ---------------------------------------------------------------------------
// Span encoding
// ---------------------------------------------------------------------------

void EncodeSpan(const internal::SpanRecord& rec, UpbScopeSp* ss, upb_Arena* arena)
{
    UpbSpan* span = opentelemetry_proto_trace_v1_ScopeSpans_add_spans(ss, arena);
    if (span == nullptr)
    {
        return;
    }

    const auto& ctx = rec.context;
    opentelemetry_proto_trace_v1_Span_set_trace_id(
        span, SvBytes(ctx.trace_id.AsBytes().data(), ctx.trace_id.AsBytes().size()));
    opentelemetry_proto_trace_v1_Span_set_span_id(
        span, SvBytes(ctx.span_id.AsBytes().data(), ctx.span_id.AsBytes().size()));
    opentelemetry_proto_trace_v1_Span_set_name(span, Sv(rec.name));
    opentelemetry_proto_trace_v1_Span_set_kind(span, MapKind(rec.kind));
    opentelemetry_proto_trace_v1_Span_set_start_time_unix_nano(span, ToNanos(rec.start_time));
    opentelemetry_proto_trace_v1_Span_set_end_time_unix_nano(span, ToNanos(rec.end_time));

    if (rec.parent_context.IsValid())
    {
        const auto& pc = rec.parent_context;
        opentelemetry_proto_trace_v1_Span_set_parent_span_id(
            span, SvBytes(pc.span_id.AsBytes().data(), pc.span_id.AsBytes().size()));
    }

    EncodeAttributes(
        rec.attributes, arena,
        reinterpret_cast<UpbKV* (*)(void*, upb_Arena*)>(
            opentelemetry_proto_trace_v1_Span_add_attributes),
        span);

    for (const auto& ev : rec.events)
    {
        EncodeEvent(ev, span, arena);
    }
    for (const auto& lnk : rec.links)
    {
        EncodeLink(lnk, span, arena);
    }

    EncodeStatus(rec, span, arena);
}

// ---------------------------------------------------------------------------
// Resource encoding
// ---------------------------------------------------------------------------

void EncodeResource(const microtel::Resource& res, UpbResSp* rs, upb_Arena* arena)
{
    UpbResource* ures = opentelemetry_proto_trace_v1_ResourceSpans_mutable_resource(rs, arena);
    if (ures == nullptr)
    {
        return;
    }
    EncodeAttributes(
        res.Attributes(), arena,
        reinterpret_cast<UpbKV* (*)(void*, upb_Arena*)>(
            opentelemetry_proto_resource_v1_Resource_add_attributes),
        ures);
}

// ---------------------------------------------------------------------------
// Scope encoding
// ---------------------------------------------------------------------------

void EncodeScope(const internal::InstrumentationScope& scope, UpbScopeSp* ss, upb_Arena* arena)
{
    UpbScope* usc = opentelemetry_proto_trace_v1_ScopeSpans_mutable_scope(ss, arena);
    if (usc == nullptr)
    {
        return;
    }
    opentelemetry_proto_common_v1_InstrumentationScope_set_name(usc, Sv(scope.name));
    opentelemetry_proto_common_v1_InstrumentationScope_set_version(usc, Sv(scope.version));
}

}  // anonymous namespace

// ---------------------------------------------------------------------------
// OtlpEncoder::Encode
// ---------------------------------------------------------------------------

internal::EncodedPayload OtlpEncoder::Encode(const internal::BatchHandle& batch)
{
    if (batch.Spans().empty())
    {
        return {};
    }

    upb_Arena* arena = upb_Arena_New();

    UpbRequest* req = opentelemetry_proto_collector_trace_v1_ExportTraceServiceRequest_new(arena);
    UpbResSp* rs    = opentelemetry_proto_collector_trace_v1_ExportTraceServiceRequest_add_resource_spans(req, arena);

    EncodeResource(batch.ResourceRef(), rs, arena);

    UpbScopeSp* ss = opentelemetry_proto_trace_v1_ResourceSpans_add_scope_spans(rs, arena);
    EncodeScope(batch.Scope(), ss, arena);

    for (const auto& rec : batch.Spans())
    {
        EncodeSpan(rec, ss, arena);
    }

    std::size_t len = 0;
    const char* buf =
        opentelemetry_proto_collector_trace_v1_ExportTraceServiceRequest_serialize(
            req, arena, &len);

    internal::EncodedPayload payload;
    if (buf != nullptr && len > 0)
    {
        auto bytes = std::make_unique<std::byte[]>(len);
        std::memcpy(bytes.get(), buf, len);
        payload = internal::EncodedPayload{std::move(bytes), len};
    }

    upb_Arena_Free(arena);
    return payload;
}

}  // namespace microtel::wire
