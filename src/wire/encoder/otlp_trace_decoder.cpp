// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// LOCKED: upb headers are confined to src/wire/encoder/ (CLAUDE.md rule 13,
// docs/memory-model.md §3.1). This is the decoding half of that directory
// (ICP 0031 Decision 5): nothing upb-typed leaves this file.

#include "otlp_trace_decoder.hpp"

// upb C headers use flexible array members (C99/C11, not ISO C++).
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#include "opentelemetry/proto/collector/trace/v1/trace_service.upb.h"
#include "opentelemetry/proto/collector/trace/v1/trace_service.upb_minitable.h"
#include "opentelemetry/proto/common/v1/common.upb.h"
#include "opentelemetry/proto/resource/v1/resource.upb.h"
#include "opentelemetry/proto/trace/v1/trace.upb.h"
#include "upb/base/upcast.h"
#include "upb/mem/alloc.h"
#include "upb/mem/arena.h"
#include "upb/wire/decode.h"
#pragma GCC diagnostic pop

#include "microtel/attribute.hpp"
#include "microtel/expected.hpp"
#include "microtel/internal/batch.hpp"
#include "microtel/internal/otlp_trace_decoder.hpp"
#include "microtel/trace.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace microtel::wire
{
namespace
{

using UpbKV = opentelemetry_proto_common_v1_KeyValue;
using UpbAnyValue = opentelemetry_proto_common_v1_AnyValue;
using UpbResSp = opentelemetry_proto_trace_v1_ResourceSpans;
using UpbScopeSp = opentelemetry_proto_trace_v1_ScopeSpans;
using UpbSpan = opentelemetry_proto_trace_v1_Span;
using UpbEvent = opentelemetry_proto_trace_v1_Span_Event;
using UpbLink = opentelemetry_proto_trace_v1_Span_Link;
using UpbRequest = opentelemetry_proto_collector_trace_v1_ExportTraceServiceRequest;

// OTLP SpanKind is UNSPECIFIED = 0 .. CONSUMER = 5; microtel's is Internal = 0
// .. Consumer = 4 (the encoder adds 1). UNSPECIFIED maps to Internal.
constexpr std::int32_t kMaxOtlpKind = 5;
// OTLP StatusCode is UNSET = 0, OK = 1, ERROR = 2, as microtel's is.
constexpr std::int32_t kMaxOtlpStatus = 2;

// ---------------------------------------------------------------------------
// The per-call arena, over an allocator that counts and caps
// ---------------------------------------------------------------------------

/// A `upb_alloc` that refuses to hand out more than `cap` bytes in total, so a
/// payload that would blow up in memory fails as `kUpb_DecodeStatus_OutOfMemory`
/// (design §3.4). The `upb_alloc` must be the first member: upb hands the
/// function back a pointer to it, and the function reaches the counters
/// through that pointer.
struct CountingAlloc
{
    upb_alloc base;
    std::size_t used = 0;
    std::size_t cap = 0;
};

void* CountingAllocFunc(upb_alloc* alloc, void* ptr, std::size_t oldsize, std::size_t size)
{
    // Pointer-interconvertible: `base` is the first member of a
    // standard-layout struct.
    auto* const self = reinterpret_cast<CountingAlloc*>(alloc);
    if (size == 0)
    {
        // upb frees through the same function it allocates through.
        // NOLINTNEXTLINE(cppcoreguidelines-no-malloc,cppcoreguidelines-owning-memory)
        std::free(ptr);
        return nullptr;
    }
    const std::size_t growth = size > oldsize ? size - oldsize : 0;
    if (growth > self->cap - self->used)
    {
        return nullptr;
    }
    // NOLINTNEXTLINE(cppcoreguidelines-no-malloc,cppcoreguidelines-owning-memory)
    void* const grown = std::realloc(ptr, size);
    if (grown != nullptr)
    {
        self->used += growth;
    }
    return grown;
}

/// RAII owner of one decode arena and its allocator. Neither movable nor
/// copyable: upb holds the allocator's address for the arena's life.
class CountingArena
{
public:
    explicit CountingArena(std::size_t cap) noexcept
        : m_alloc{.base = {.func = &CountingAllocFunc}, .used = 0, .cap = cap},
          m_arena(upb_Arena_Init(nullptr, 0, &m_alloc.base))
    {
    }

    ~CountingArena() noexcept
    {
        if (m_arena != nullptr)
        {
            upb_Arena_Free(m_arena);
        }
    }

    CountingArena(const CountingArena&) = delete;
    CountingArena& operator=(const CountingArena&) = delete;
    CountingArena(CountingArena&&) = delete;
    CountingArena& operator=(CountingArena&&) = delete;

    /// Borrowed; null when even the first block was over the cap.
    [[nodiscard]] upb_Arena* Get() const noexcept
    {
        return m_arena;
    }

private:
    CountingAlloc m_alloc;
    upb_Arena* m_arena;
};

// ---------------------------------------------------------------------------
// Scalars
// ---------------------------------------------------------------------------

std::string ToString(upb_StringView v)
{
    return std::string{v.data, v.size};
}

std::string ToHex(upb_StringView v)
{
    static constexpr std::array<char, 16> kDigits{
        '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'};
    constexpr unsigned kNibbleBits = 4U;
    constexpr unsigned kNibbleMask = 0x0FU;
    std::string out;
    out.reserve(v.size * 2);
    for (std::size_t i = 0; i < v.size; ++i)
    {
        const auto byte = static_cast<unsigned char>(v.data[i]);
        out.push_back(kDigits.at(byte >> kNibbleBits));
        out.push_back(kDigits.at(byte & kNibbleMask));
    }
    return out;
}

/// A leaf timestamp as a time point; nullopt past the range of `system_clock`.
std::optional<std::chrono::system_clock::time_point> ToTime(std::uint64_t ns) noexcept
{
    if (ns > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
    {
        return std::nullopt;
    }
    return std::chrono::system_clock::time_point{
        std::chrono::duration_cast<std::chrono::system_clock::duration>(
            std::chrono::nanoseconds{static_cast<std::int64_t>(ns)})};
}

template <typename Id>
std::optional<Id> ToId(upb_StringView v) noexcept
{
    typename Id::Bytes bytes{};
    if (v.size != bytes.size())
    {
        return std::nullopt;
    }
    std::memcpy(bytes.data(), v.data, bytes.size());
    return Id{bytes};
}

// ---------------------------------------------------------------------------
// Attribute values (design §3.4, ICP 0015 Option B)
// ---------------------------------------------------------------------------

/// The value of a scalar AnyValue, or nullopt for one `AttributeValue` has no
/// lossless home for.
std::optional<AttributeValue> ConvertScalar(const UpbAnyValue* v)
{
    switch (opentelemetry_proto_common_v1_AnyValue_value_case(v))
    {
        case opentelemetry_proto_common_v1_AnyValue_value_string_value:
            return AttributeValue{ToString(opentelemetry_proto_common_v1_AnyValue_string_value(v))};
        case opentelemetry_proto_common_v1_AnyValue_value_bool_value:
            return AttributeValue{opentelemetry_proto_common_v1_AnyValue_bool_value(v)};
        case opentelemetry_proto_common_v1_AnyValue_value_int_value:
            return AttributeValue{opentelemetry_proto_common_v1_AnyValue_int_value(v)};
        case opentelemetry_proto_common_v1_AnyValue_value_double_value:
            return AttributeValue{opentelemetry_proto_common_v1_AnyValue_double_value(v)};
        case opentelemetry_proto_common_v1_AnyValue_value_bytes_value:
            return AttributeValue{ToHex(opentelemetry_proto_common_v1_AnyValue_bytes_value(v))};
        default:
            return std::nullopt;
    }
}

/// Collect a homogeneous array of @p T; nullopt if any element is another kind.
template <typename T>
std::optional<AttributeValue> CollectArray(const UpbAnyValue* const* values, std::size_t n)
{
    std::vector<T> out;
    out.reserve(n);
    for (std::size_t i = 0; i < n; ++i)
    {
        auto scalar = ConvertScalar(values[i]);
        auto* const typed = scalar.has_value() ? std::get_if<T>(&*scalar) : nullptr;
        if (typed == nullptr)
        {
            return std::nullopt;
        }
        out.push_back(std::move(*typed));
    }
    return AttributeValue{std::move(out)};
}

std::optional<AttributeValue> ConvertArray(const UpbAnyValue* v)
{
    const auto* const arr = opentelemetry_proto_common_v1_AnyValue_array_value(v);
    std::size_t n = 0;
    const UpbAnyValue* const* values = opentelemetry_proto_common_v1_ArrayValue_values(arr, &n);
    if (n == 0)
    {
        return AttributeValue{std::vector<std::string>{}};
    }
    // The first element decides the type; a nested array, a kvlist, a
    // bytes-as-hex element, or a mixed array has no lossless mapping.
    switch (opentelemetry_proto_common_v1_AnyValue_value_case(values[0]))
    {
        case opentelemetry_proto_common_v1_AnyValue_value_string_value:
            return CollectArray<std::string>(values, n);
        case opentelemetry_proto_common_v1_AnyValue_value_bool_value:
            return CollectArray<bool>(values, n);
        case opentelemetry_proto_common_v1_AnyValue_value_int_value:
            return CollectArray<std::int64_t>(values, n);
        case opentelemetry_proto_common_v1_AnyValue_value_double_value:
            return CollectArray<double>(values, n);
        default:
            return std::nullopt;
    }
}

std::optional<AttributeValue> ConvertValue(const UpbAnyValue* v)
{
    if (v == nullptr)
    {
        return std::nullopt;
    }
    if (opentelemetry_proto_common_v1_AnyValue_value_case(v) ==
        opentelemetry_proto_common_v1_AnyValue_value_array_value)
    {
        return ConvertArray(v);
    }
    return ConvertScalar(v);
}

/// Append the representable attributes of @p kvs to @p out.
/// @return how many were dropped as unrepresentable.
std::uint64_t ConvertAttributes(const UpbKV* const* kvs, std::size_t n, std::vector<KeyValue>& out)
{
    std::uint64_t dropped = 0;
    out.reserve(out.size() + n);
    for (std::size_t i = 0; i < n; ++i)
    {
        auto value = ConvertValue(opentelemetry_proto_common_v1_KeyValue_value(kvs[i]));
        if (!value.has_value())
        {
            ++dropped;
            continue;
        }
        out.push_back(KeyValue{.key = ToString(opentelemetry_proto_common_v1_KeyValue_key(kvs[i])),
                               .value = std::move(*value)});
    }
    return dropped;
}

// ---------------------------------------------------------------------------
// Spans. Each returns false for what a SpanRecord cannot represent.
// ---------------------------------------------------------------------------

bool ConvertEvents(const UpbSpan* span, internal::SpanRecord& rec, std::uint64_t& dropped)
{
    std::size_t n = 0;
    const UpbEvent* const* events = opentelemetry_proto_trace_v1_Span_events(span, &n);
    rec.events.reserve(n);
    for (std::size_t i = 0; i < n; ++i)
    {
        const auto when = ToTime(opentelemetry_proto_trace_v1_Span_Event_time_unix_nano(events[i]));
        if (!when.has_value())
        {
            return false;
        }
        internal::SpanEvent ev;
        ev.name = ToString(opentelemetry_proto_trace_v1_Span_Event_name(events[i]));
        ev.timestamp = *when;
        std::size_t n_attrs = 0;
        const UpbKV* const* attrs =
            opentelemetry_proto_trace_v1_Span_Event_attributes(events[i], &n_attrs);
        dropped += ConvertAttributes(attrs, n_attrs, ev.attributes);
        rec.events.push_back(std::move(ev));
    }
    return true;
}

bool ConvertLinks(const UpbSpan* span, internal::SpanRecord& rec, std::uint64_t& dropped)
{
    std::size_t n = 0;
    const UpbLink* const* links = opentelemetry_proto_trace_v1_Span_links(span, &n);
    rec.links.reserve(n);
    for (std::size_t i = 0; i < n; ++i)
    {
        const auto trace_id =
            ToId<TraceId>(opentelemetry_proto_trace_v1_Span_Link_trace_id(links[i]));
        const auto span_id = ToId<SpanId>(opentelemetry_proto_trace_v1_Span_Link_span_id(links[i]));
        if (!trace_id.has_value() || !span_id.has_value())
        {
            return false;
        }
        internal::SpanLink link;
        link.linked_context.trace_id = *trace_id;
        link.linked_context.span_id = *span_id;
        std::size_t n_attrs = 0;
        const UpbKV* const* attrs =
            opentelemetry_proto_trace_v1_Span_Link_attributes(links[i], &n_attrs);
        dropped += ConvertAttributes(attrs, n_attrs, link.attributes);
        rec.links.push_back(std::move(link));
    }
    return true;
}

/// Ids, parent, kind, status and times: everything but the repeated fields.
bool ConvertHeader(const UpbSpan* span, internal::SpanRecord& rec)
{
    const auto trace_id = ToId<TraceId>(opentelemetry_proto_trace_v1_Span_trace_id(span));
    const auto span_id = ToId<SpanId>(opentelemetry_proto_trace_v1_Span_span_id(span));
    const upb_StringView parent = opentelemetry_proto_trace_v1_Span_parent_span_id(span);
    const auto parent_id = ToId<SpanId>(parent);
    const std::int32_t kind = opentelemetry_proto_trace_v1_Span_kind(span);
    const auto start = ToTime(opentelemetry_proto_trace_v1_Span_start_time_unix_nano(span));
    const auto end = ToTime(opentelemetry_proto_trace_v1_Span_end_time_unix_nano(span));
    if (!trace_id || !span_id || (parent.size != 0 && !parent_id) || kind < 0 ||
        kind > kMaxOtlpKind || !start || !end)
    {
        return false;
    }
    rec.context.trace_id = *trace_id;
    rec.context.span_id = *span_id;
    if (parent_id.has_value())
    {
        rec.parent_context.trace_id = *trace_id;
        rec.parent_context.span_id = *parent_id;
    }
    rec.kind = kind == 0 ? SpanKind::Internal : static_cast<SpanKind>(kind - 1);
    rec.start_time = *start;
    rec.end_time = *end;
    rec.name = ToString(opentelemetry_proto_trace_v1_Span_name(span));
    const auto* const status = opentelemetry_proto_trace_v1_Span_status(span);
    if (status == nullptr)
    {
        return true;
    }
    const std::int32_t code = opentelemetry_proto_trace_v1_Status_code(status);
    if (code < 0 || code > kMaxOtlpStatus)
    {
        return false;
    }
    rec.status_code = static_cast<StatusCode>(code);
    rec.status_description = ToString(opentelemetry_proto_trace_v1_Status_message(status));
    return true;
}

bool ConvertSpan(const UpbSpan* span, internal::SpanRecord& rec, std::uint64_t& dropped)
{
    if (!ConvertHeader(span, rec))
    {
        return false;
    }
    std::size_t n_attrs = 0;
    const UpbKV* const* attrs = opentelemetry_proto_trace_v1_Span_attributes(span, &n_attrs);
    dropped += ConvertAttributes(attrs, n_attrs, rec.attributes);
    return ConvertEvents(span, rec, dropped) && ConvertLinks(span, rec, dropped);
}

bool ConvertScopeSpans(const UpbScopeSp* ss, internal::DecodedResourceSpans& out)
{
    internal::DecodedScopeSpans scope_spans;
    if (const auto* const scope = opentelemetry_proto_trace_v1_ScopeSpans_scope(ss);
        scope != nullptr)
    {
        scope_spans.scope.name =
            ToString(opentelemetry_proto_common_v1_InstrumentationScope_name(scope));
        scope_spans.scope.version =
            ToString(opentelemetry_proto_common_v1_InstrumentationScope_version(scope));
    }
    std::size_t n = 0;
    const UpbSpan* const* spans = opentelemetry_proto_trace_v1_ScopeSpans_spans(ss, &n);
    scope_spans.spans.resize(n);
    for (std::size_t i = 0; i < n; ++i)
    {
        if (!ConvertSpan(spans[i], scope_spans.spans[i], out.dropped_span_attributes))
        {
            return false;
        }
    }
    out.scopes.push_back(std::move(scope_spans));
    return true;
}

bool ConvertResourceSpans(const UpbResSp* rs, internal::DecodedResourceSpans& out)
{
    if (const auto* const res = opentelemetry_proto_trace_v1_ResourceSpans_resource(rs);
        res != nullptr)
    {
        std::size_t n_attrs = 0;
        const UpbKV* const* attrs =
            opentelemetry_proto_resource_v1_Resource_attributes(res, &n_attrs);
        out.dropped_resource_attributes += ConvertAttributes(attrs, n_attrs, out.resource);
    }
    std::size_t n = 0;
    const UpbScopeSp* const* scopes =
        opentelemetry_proto_trace_v1_ResourceSpans_scope_spans(rs, &n);
    out.scopes.reserve(n);
    for (std::size_t i = 0; i < n; ++i)
    {
        if (!ConvertScopeSpans(scopes[i], out))
        {
            return false;
        }
    }
    return true;
}

/// Spans across the whole request, counted before anything is copied out.
std::size_t CountSpans(const UpbResSp* const* rss, std::size_t n_rs) noexcept
{
    std::size_t total = 0;
    for (std::size_t i = 0; i < n_rs; ++i)
    {
        std::size_t n_ss = 0;
        const UpbScopeSp* const* scopes =
            opentelemetry_proto_trace_v1_ResourceSpans_scope_spans(rss[i], &n_ss);
        for (std::size_t j = 0; j < n_ss; ++j)
        {
            std::size_t n_spans = 0;
            (void)opentelemetry_proto_trace_v1_ScopeSpans_spans(scopes[j], &n_spans);
            total += n_spans;
        }
    }
    return total;
}

internal::DecodeFailure FailureFor(upb_DecodeStatus status) noexcept
{
    // The arena cap surfaces as OutOfMemory; both it and the depth limit are
    // bounds this call was given, so both are TooLarge (design §3.7).
    if (status == kUpb_DecodeStatus_OutOfMemory || status == kUpb_DecodeStatus_MaxDepthExceeded)
    {
        return internal::DecodeFailure::TooLarge;
    }
    return internal::DecodeFailure::Malformed;
}

}  // namespace

Expected<std::vector<internal::DecodedResourceSpans>, internal::DecodeFailure>
OtlpTraceDecoder::Decode(std::span<const std::byte> payload,
                         const internal::DecodeLimits& limits) const
{
    const CountingArena arena{limits.max_arena_bytes};
    UpbRequest* const req =
        arena.Get() == nullptr
            ? nullptr
            : opentelemetry_proto_collector_trace_v1_ExportTraceServiceRequest_new(arena.Get());
    if (req == nullptr)
    {
        return make_unexpected(internal::DecodeFailure::TooLarge);
    }
    const upb_DecodeStatus status =
        upb_Decode(reinterpret_cast<const char*>(payload.data()),
                   payload.size(),
                   UPB_UPCAST(req),
                   &opentelemetry__proto__collector__trace__v1__ExportTraceServiceRequest_msg_init,
                   nullptr,
                   static_cast<int>(upb_DecodeOptions_MaxDepth(limits.max_depth)),
                   arena.Get());
    if (status != kUpb_DecodeStatus_Ok)
    {
        return make_unexpected(FailureFor(status));
    }

    std::size_t n_rs = 0;
    const UpbResSp* const* rss =
        opentelemetry_proto_collector_trace_v1_ExportTraceServiceRequest_resource_spans(req, &n_rs);
    if (CountSpans(rss, n_rs) > limits.max_spans)
    {
        return make_unexpected(internal::DecodeFailure::TooLarge);
    }
    std::vector<internal::DecodedResourceSpans> out(n_rs);
    for (std::size_t i = 0; i < n_rs; ++i)
    {
        if (!ConvertResourceSpans(rss[i], out[i]))
        {
            return make_unexpected(internal::DecodeFailure::Malformed);
        }
    }
    return out;
}

}  // namespace microtel::wire
