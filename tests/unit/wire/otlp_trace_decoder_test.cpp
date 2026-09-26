// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// OtlpTraceDecoder (docs/leaf-concentrator-design.md §3.4, §3.7): the upb
// decoder behind the leaf receiver. Well-formed payloads come from the real
// OtlpEncoder, so a round trip checks the two agree; shapes the encoder cannot
// produce (kvlist values, wrong id lengths, deep nesting) are built with upb
// directly, which tests may include.

#include "wire/encoder/otlp_trace_decoder.hpp"

#include "wire/encoder/otlp_encoder.hpp"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#include "opentelemetry/proto/collector/trace/v1/trace_service.upb.h"
#include "opentelemetry/proto/common/v1/common.upb.h"
#include "opentelemetry/proto/resource/v1/resource.upb.h"
#include "opentelemetry/proto/trace/v1/trace.upb.h"
#include "upb/mem/arena.h"
#pragma GCC diagnostic pop

#include "microtel/attribute.hpp"
#include "microtel/internal/batch.hpp"
#include "microtel/internal/otlp_trace_decoder.hpp"
#include "microtel/resource.hpp"
#include "microtel/trace.hpp"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace mt = microtel;
namespace mti = microtel::internal;
namespace mtw = microtel::wire;

namespace
{

#if defined(__SANITIZE_ADDRESS__)
constexpr bool kAddressSanitizer = true;
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
constexpr bool kAddressSanitizer = true;
#else
constexpr bool kAddressSanitizer = false;
#endif
#else
constexpr bool kAddressSanitizer = false;
#endif

constexpr mti::DecodeLimits kGenerous{
    .max_spans = 1000, .max_depth = 16, .max_arena_bytes = 1U << 20U};

mt::TraceId TraceIdOf(std::uint8_t fill)
{
    mt::TraceId::Bytes b{};
    b.fill(fill);
    return mt::TraceId{b};
}

mt::SpanId SpanIdOf(std::uint8_t fill)
{
    mt::SpanId::Bytes b{};
    b.fill(fill);
    return mt::SpanId{b};
}

std::chrono::system_clock::time_point Ns(std::int64_t ns)
{
    return std::chrono::system_clock::time_point{std::chrono::nanoseconds{ns}};
}

std::vector<std::byte> ToBytes(const mti::EncodedPayload& p)
{
    return {p.Bytes().begin(), p.Bytes().end()};
}

std::vector<std::byte> Encode(std::vector<mt::KeyValue> resource,
                              std::vector<mti::SpanRecord> spans)
{
    const mti::BatchHandle batch{std::move(spans),
                                 std::make_shared<const mt::Resource>(std::move(resource)),
                                 mti::InstrumentationScope{.name = "leaf-lib", .version = "0.1"}};
    mtw::OtlpEncoder encoder;
    return ToBytes(encoder.Encode(batch));
}

mti::SpanRecord Span(std::uint8_t trace, std::uint8_t span)
{
    mti::SpanRecord r;
    r.context = mt::SpanContext{.trace_id = TraceIdOf(trace), .span_id = SpanIdOf(span)};
    r.name = "op";
    r.start_time = Ns(100);
    r.end_time = Ns(200);
    return r;
}

// ---------------------------------------------------------------------------
// upb builders for shapes the encoder never emits
// ---------------------------------------------------------------------------

using UpbSpan = opentelemetry_proto_trace_v1_Span;
using UpbResource = opentelemetry_proto_resource_v1_Resource;

/// Builds a one-span request with upb and hands the span and Resource to
/// @p mutate before serialising.
std::vector<std::byte> Build(const std::function<void(UpbSpan*, UpbResource*, upb_Arena*)>& mutate)
{
    upb_Arena* const arena = upb_Arena_New();
    auto* const req = opentelemetry_proto_collector_trace_v1_ExportTraceServiceRequest_new(arena);
    auto* const rs =
        opentelemetry_proto_collector_trace_v1_ExportTraceServiceRequest_add_resource_spans(req,
                                                                                            arena);
    auto* const res = opentelemetry_proto_trace_v1_ResourceSpans_mutable_resource(rs, arena);
    auto* const ss = opentelemetry_proto_trace_v1_ResourceSpans_add_scope_spans(rs, arena);
    auto* const span = opentelemetry_proto_trace_v1_ScopeSpans_add_spans(ss, arena);
    static const std::array<char, 16> kTrace{1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};
    static const std::array<char, 8> kSpan{2, 2, 2, 2, 2, 2, 2, 2};
    opentelemetry_proto_trace_v1_Span_set_trace_id(
        span, upb_StringView_FromDataAndSize(kTrace.data(), kTrace.size()));
    opentelemetry_proto_trace_v1_Span_set_span_id(
        span, upb_StringView_FromDataAndSize(kSpan.data(), kSpan.size()));
    opentelemetry_proto_trace_v1_Span_set_start_time_unix_nano(span, 1);
    opentelemetry_proto_trace_v1_Span_set_end_time_unix_nano(span, 2);
    mutate(span, res, arena);
    std::size_t len = 0;
    const char* const buf =
        opentelemetry_proto_collector_trace_v1_ExportTraceServiceRequest_serialize(
            req, arena, &len);
    std::vector<std::byte> out(len);
    if (len > 0)
    {
        std::memcpy(out.data(), buf, len);
    }
    upb_Arena_Free(arena);
    return out;
}

upb_StringView Sv(const char* s)
{
    return upb_StringView_FromString(s);
}

opentelemetry_proto_common_v1_AnyValue* AddSpanAttr(UpbSpan* span,
                                                    const char* key,
                                                    upb_Arena* arena)
{
    auto* const kv = opentelemetry_proto_trace_v1_Span_add_attributes(span, arena);
    opentelemetry_proto_common_v1_KeyValue_set_key(kv, Sv(key));
    return opentelemetry_proto_common_v1_KeyValue_mutable_value(kv, arena);
}

opentelemetry_proto_common_v1_AnyValue* AddResourceAttr(UpbResource* res,
                                                        const char* key,
                                                        upb_Arena* arena)
{
    auto* const kv = opentelemetry_proto_resource_v1_Resource_add_attributes(res, arena);
    opentelemetry_proto_common_v1_KeyValue_set_key(kv, Sv(key));
    return opentelemetry_proto_common_v1_KeyValue_mutable_value(kv, arena);
}

mti::DecodeFailure FailureOf(const std::vector<std::byte>& payload,
                             const mti::DecodeLimits& limits = kGenerous)
{
    const mtw::OtlpTraceDecoder decoder;
    const auto r = decoder.Decode(payload, limits);
    EXPECT_FALSE(r.has_value());
    return r.has_value() ? mti::DecodeFailure{255} : r.error();
}

}  // namespace

// ---------------------------------------------------------------------------
// Round trip against the encoder
// ---------------------------------------------------------------------------

TEST(OtlpTraceDecoderTest, RoundTripsEverythingTheEncoderWrites)
{
    auto root = Span(7, 1);
    root.name = "root";
    root.kind = mt::SpanKind::Server;
    root.status_code = mt::StatusCode::Error;
    root.status_description = "boom";
    root.attributes = {
        {.key = "s", .value = std::string{"text"}},
        {.key = "b", .value = true},
        {.key = "i", .value = std::int64_t{-3}},
        {.key = "d", .value = 2.5},
        {.key = "sa", .value = std::vector<std::string>{"x", "y"}},
        {.key = "ba", .value = std::vector<bool>{true, false}},
        {.key = "ia", .value = std::vector<std::int64_t>{1, 2}},
        {.key = "da", .value = std::vector<double>{0.5}},
    };
    root.events.push_back(mti::SpanEvent{.name = "ev",
                                         .timestamp = Ns(150),
                                         .attributes = {{.key = "k", .value = std::int64_t{1}}}});
    root.links.push_back(mti::SpanLink{
        .linked_context = mt::SpanContext{.trace_id = TraceIdOf(8), .span_id = SpanIdOf(9)},
        .attributes = {{.key = "l", .value = std::string{"v"}}}});
    auto child = Span(7, 2);
    child.parent_context = mt::SpanContext{.trace_id = TraceIdOf(7), .span_id = SpanIdOf(1)};
    const auto bytes = Encode({{.key = "microtel.leaf.proto", .value = std::int64_t{1}},
                               {.key = "host.name", .value = std::string{"h"}}},
                              {root, child});

    const mtw::OtlpTraceDecoder decoder;
    const auto r = decoder.Decode(bytes, kGenerous);

    ASSERT_TRUE(r.has_value());
    ASSERT_EQ(r->size(), 1U);
    const auto& rs = r->at(0);
    ASSERT_EQ(rs.resource.size(), 2U);
    EXPECT_EQ(rs.resource[0].key, "microtel.leaf.proto");
    EXPECT_EQ(std::get<std::int64_t>(rs.resource[0].value), 1);
    ASSERT_EQ(rs.scopes.size(), 1U);
    EXPECT_EQ(rs.scopes[0].scope.name, "leaf-lib");
    EXPECT_EQ(rs.scopes[0].scope.version, "0.1");
    ASSERT_EQ(rs.scopes[0].spans.size(), 2U);
    const auto& d = rs.scopes[0].spans[0];
    EXPECT_EQ(d.context.trace_id.AsBytes(), TraceIdOf(7).AsBytes());
    EXPECT_EQ(d.context.span_id.AsBytes(), SpanIdOf(1).AsBytes());
    EXPECT_FALSE(d.parent_context.IsValid());
    EXPECT_EQ(d.name, "root");
    EXPECT_EQ(d.kind, mt::SpanKind::Server);
    EXPECT_EQ(d.status_code, mt::StatusCode::Error);
    EXPECT_EQ(d.status_description, "boom");
    EXPECT_EQ(d.start_time, Ns(100));
    EXPECT_EQ(d.end_time, Ns(200));
    ASSERT_EQ(d.attributes.size(), root.attributes.size());
    for (std::size_t i = 0; i < d.attributes.size(); ++i)
    {
        EXPECT_EQ(d.attributes[i].key, root.attributes[i].key);
        EXPECT_EQ(d.attributes[i].value, root.attributes[i].value) << root.attributes[i].key;
    }
    ASSERT_EQ(d.events.size(), 1U);
    EXPECT_EQ(d.events[0].name, "ev");
    EXPECT_EQ(d.events[0].timestamp, Ns(150));
    ASSERT_EQ(d.events[0].attributes.size(), 1U);
    ASSERT_EQ(d.links.size(), 1U);
    EXPECT_EQ(d.links[0].linked_context.trace_id.AsBytes(), TraceIdOf(8).AsBytes());
    EXPECT_EQ(d.links[0].linked_context.span_id.AsBytes(), SpanIdOf(9).AsBytes());
    EXPECT_EQ(d.links[0].attributes.size(), 1U);
    EXPECT_EQ(d.resource, nullptr) << "the decoder does not resolve Resources";

    const auto& c = rs.scopes[0].spans[1];
    EXPECT_TRUE(c.parent_context.IsValid());
    EXPECT_EQ(c.parent_context.trace_id.AsBytes(), TraceIdOf(7).AsBytes());
    EXPECT_EQ(c.parent_context.span_id.AsBytes(), SpanIdOf(1).AsBytes());
    EXPECT_EQ(rs.dropped_span_attributes, 0U);
    EXPECT_EQ(rs.dropped_resource_attributes, 0U);
}

TEST(OtlpTraceDecoderTest, EmptyPayloadIsAnEmptyRequest)
{
    const mtw::OtlpTraceDecoder decoder;
    const auto r = decoder.Decode({}, kGenerous);
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(r->empty());
}

TEST(OtlpTraceDecoderTest, GarbageIsMalformed)
{
    const std::vector<std::byte> garbage{std::byte{0xff}, std::byte{0xff}, std::byte{0xff}};
    EXPECT_EQ(FailureOf(garbage), mti::DecodeFailure::Malformed);
}

TEST(OtlpTraceDecoderTest, TruncatedPayloadIsMalformed)
{
    auto bytes = Encode({}, {Span(1, 1)});
    bytes.resize(bytes.size() - 3);
    EXPECT_EQ(FailureOf(bytes), mti::DecodeFailure::Malformed);
}

TEST(OtlpTraceDecoderTest, InvalidUtf8IsMalformed)
{
    const auto bytes = Build(
        [](UpbSpan* span, UpbResource*, upb_Arena*)
        {
            static const char kBad[] = {'a', static_cast<char>(0xff), 'b'};
            opentelemetry_proto_trace_v1_Span_set_name(
                span, upb_StringView_FromDataAndSize(kBad, sizeof(kBad)));
        });
    EXPECT_EQ(FailureOf(bytes), mti::DecodeFailure::Malformed);
}

// ---------------------------------------------------------------------------
// Limits (§3.7)
// ---------------------------------------------------------------------------

TEST(OtlpTraceDecoderTest, SpanCountAtTheLimitDecodesAndOnePastIsTooLarge)
{
    const auto bytes = Encode({}, {Span(1, 1), Span(1, 2), Span(1, 3)});
    const mtw::OtlpTraceDecoder decoder;

    EXPECT_TRUE(
        decoder.Decode(bytes, {.max_spans = 3, .max_depth = 16, .max_arena_bytes = 1U << 20U})
            .has_value());
    EXPECT_EQ(FailureOf(bytes, {.max_spans = 2, .max_depth = 16, .max_arena_bytes = 1U << 20U}),
              mti::DecodeFailure::TooLarge);
}

TEST(OtlpTraceDecoderTest, ArenaCapIsTooLarge)
{
    const auto bytes = Encode({}, {Span(1, 1), Span(1, 2)});
    EXPECT_EQ(FailureOf(bytes, {.max_spans = 10, .max_depth = 16, .max_arena_bytes = 64}),
              mti::DecodeFailure::TooLarge);
}

// The receiver caps the arena at 16 x the payload plus 16 KiB (design §3.7;
// the factor was measured when the decoder landed). Dense numeric arrays are
// the most arena-hungry legitimate shape measured; this pins that the cap
// still fits them, small and large.
TEST(OtlpTraceDecoderTest, TheReceiversArenaCapFitsDenseLegitimatePayloads)
{
    if (kAddressSanitizer)
    {
        GTEST_SKIP() << "upb pads every arena allocation with an ASan guard, so the "
                        "measured ratio does not hold in an ASan build";
    }
    constexpr std::size_t kFactor = 16;
    constexpr std::size_t kFloor = std::size_t{16} * 1024U;
    for (const int n : {1, 50, 400})
    {
        std::vector<mti::SpanRecord> spans;
        for (int i = 0; i < n; ++i)
        {
            auto s = Span(1, static_cast<std::uint8_t>((i % 250) + 1));
            s.attributes.push_back({.key = "k", .value = std::vector<std::int64_t>(20, 1)});
            spans.push_back(std::move(s));
        }
        const auto bytes = Encode({}, std::move(spans));
        const mtw::OtlpTraceDecoder decoder;
        EXPECT_TRUE(decoder
                        .Decode(bytes,
                                {.max_spans = 1000,
                                 .max_depth = 16,
                                 .max_arena_bytes = (kFactor * bytes.size()) + kFloor})
                        .has_value())
            << n << " spans, " << bytes.size() << " bytes";
    }
}

TEST(OtlpTraceDecoderTest, AnArenaThatRunsOutPartWayIsTooLarge)
{
    // Room for the arena's first block but not for 200 spans.
    constexpr int kSpans = 200;
    std::vector<mti::SpanRecord> spans;
    spans.reserve(kSpans);
    for (int i = 0; i < kSpans; ++i)
    {
        spans.push_back(Span(1, static_cast<std::uint8_t>(i + 1)));
    }
    const auto bytes = Encode({}, std::move(spans));
    EXPECT_EQ(FailureOf(bytes, {.max_spans = 1000, .max_depth = 16, .max_arena_bytes = 4096}),
              mti::DecodeFailure::TooLarge);
}

TEST(OtlpTraceDecoderTest, AKeyValueWithNoValueIsDroppedAndCounted)
{
    const auto bytes = Build(
        [](UpbSpan* span, UpbResource*, upb_Arena* arena)
        {
            auto* const kv = opentelemetry_proto_trace_v1_Span_add_attributes(span, arena);
            opentelemetry_proto_common_v1_KeyValue_set_key(kv, Sv("no-value"));
        });
    const mtw::OtlpTraceDecoder decoder;
    const auto r = decoder.Decode(bytes, kGenerous);
    ASSERT_TRUE(r.has_value());
    EXPECT_TRUE(r->at(0).scopes.at(0).spans.at(0).attributes.empty());
    EXPECT_EQ(r->at(0).dropped_span_attributes, 1U);
}

TEST(OtlpTraceDecoderTest, NestingPastMaxDepthIsTooLarge)
{
    constexpr int kLevels = 20;
    const auto bytes = Build(
        [](UpbSpan* span, UpbResource*, upb_Arena* arena)
        {
            auto* value = AddSpanAttr(span, "deep", arena);
            for (int i = 0; i < kLevels; ++i)
            {
                auto* const arr =
                    opentelemetry_proto_common_v1_AnyValue_mutable_array_value(value, arena);
                value = opentelemetry_proto_common_v1_ArrayValue_add_values(arr, arena);
            }
            opentelemetry_proto_common_v1_AnyValue_set_int_value(value, 1);
        });
    EXPECT_EQ(FailureOf(bytes), mti::DecodeFailure::TooLarge);
}

// ---------------------------------------------------------------------------
// Values AttributeValue cannot hold (§3.4, ICP 0015 Option B)
// ---------------------------------------------------------------------------

TEST(OtlpTraceDecoderTest, BytesBecomeLowercaseHex)
{
    const auto bytes = Build(
        [](UpbSpan* span, UpbResource*, upb_Arena* arena)
        {
            static const char kRaw[] = {static_cast<char>(0xAB), 0x01};
            opentelemetry_proto_common_v1_AnyValue_set_bytes_value(
                AddSpanAttr(span, "raw", arena),
                upb_StringView_FromDataAndSize(kRaw, sizeof(kRaw)));
        });
    const mtw::OtlpTraceDecoder decoder;
    const auto r = decoder.Decode(bytes, kGenerous);
    ASSERT_TRUE(r.has_value());
    const auto& attrs = r->at(0).scopes.at(0).spans.at(0).attributes;
    ASSERT_EQ(attrs.size(), 1U);
    EXPECT_EQ(std::get<std::string>(attrs[0].value), "ab01");
}

TEST(OtlpTraceDecoderTest, KvlistNestedMixedAndEmptyValuesAreDroppedAndCounted)
{
    const auto bytes = Build(
        [](UpbSpan* span, UpbResource* res, upb_Arena* arena)
        {
            (void)opentelemetry_proto_common_v1_AnyValue_mutable_kvlist_value(
                AddSpanAttr(span, "kv", arena), arena);
            auto* const nested = opentelemetry_proto_common_v1_AnyValue_mutable_array_value(
                AddSpanAttr(span, "nested", arena), arena);
            (void)opentelemetry_proto_common_v1_AnyValue_mutable_array_value(
                opentelemetry_proto_common_v1_ArrayValue_add_values(nested, arena), arena);
            auto* const mixed = opentelemetry_proto_common_v1_AnyValue_mutable_array_value(
                AddSpanAttr(span, "mixed", arena), arena);
            opentelemetry_proto_common_v1_AnyValue_set_int_value(
                opentelemetry_proto_common_v1_ArrayValue_add_values(mixed, arena), 1);
            opentelemetry_proto_common_v1_AnyValue_set_bool_value(
                opentelemetry_proto_common_v1_ArrayValue_add_values(mixed, arena), true);
            (void)AddSpanAttr(span, "unset", arena);
            opentelemetry_proto_common_v1_AnyValue_set_int_value(AddSpanAttr(span, "ok", arena), 5);
            (void)opentelemetry_proto_common_v1_AnyValue_mutable_kvlist_value(
                AddResourceAttr(res, "rkv", arena), arena);
        });
    const mtw::OtlpTraceDecoder decoder;
    const auto r = decoder.Decode(bytes, kGenerous);

    ASSERT_TRUE(r.has_value());
    const auto& rs = r->at(0);
    const auto& attrs = rs.scopes.at(0).spans.at(0).attributes;
    ASSERT_EQ(attrs.size(), 1U);
    EXPECT_EQ(attrs[0].key, "ok");
    EXPECT_EQ(rs.dropped_span_attributes, 4U);
    EXPECT_TRUE(rs.resource.empty());
    EXPECT_EQ(rs.dropped_resource_attributes, 1U);
}

TEST(OtlpTraceDecoderTest, EmptyArrayIsAnEmptyStringArray)
{
    const auto bytes = Build(
        [](UpbSpan* span, UpbResource*, upb_Arena* arena)
        {
            (void)opentelemetry_proto_common_v1_AnyValue_mutable_array_value(
                AddSpanAttr(span, "empty", arena), arena);
        });
    const mtw::OtlpTraceDecoder decoder;
    const auto r = decoder.Decode(bytes, kGenerous);
    ASSERT_TRUE(r.has_value());
    const auto& attrs = r->at(0).scopes.at(0).spans.at(0).attributes;
    ASSERT_EQ(attrs.size(), 1U);
    EXPECT_TRUE(std::get<std::vector<std::string>>(attrs[0].value).empty());
}

// ---------------------------------------------------------------------------
// What a SpanRecord cannot represent is Malformed
// ---------------------------------------------------------------------------

TEST(OtlpTraceDecoderTest, TraceIdNotSixteenBytesIsMalformed)
{
    const auto bytes =
        Build([](UpbSpan* span, UpbResource*, upb_Arena*)
              { opentelemetry_proto_trace_v1_Span_set_trace_id(span, Sv("short")); });
    EXPECT_EQ(FailureOf(bytes), mti::DecodeFailure::Malformed);
}

TEST(OtlpTraceDecoderTest, SpanIdNotEightBytesIsMalformed)
{
    const auto bytes =
        Build([](UpbSpan* span, UpbResource*, upb_Arena*)
              { opentelemetry_proto_trace_v1_Span_set_span_id(span, Sv("1234567")); });
    EXPECT_EQ(FailureOf(bytes), mti::DecodeFailure::Malformed);
}

TEST(OtlpTraceDecoderTest, ParentSpanIdNeitherEmptyNorEightBytesIsMalformed)
{
    const auto bytes =
        Build([](UpbSpan* span, UpbResource*, upb_Arena*)
              { opentelemetry_proto_trace_v1_Span_set_parent_span_id(span, Sv("1234")); });
    EXPECT_EQ(FailureOf(bytes), mti::DecodeFailure::Malformed);
}

TEST(OtlpTraceDecoderTest, LinkIdsOfTheWrongLengthAreMalformed)
{
    const auto bytes = Build(
        [](UpbSpan* span, UpbResource*, upb_Arena* arena)
        {
            auto* const link = opentelemetry_proto_trace_v1_Span_add_links(span, arena);
            opentelemetry_proto_trace_v1_Span_Link_set_trace_id(link, Sv("short"));
        });
    EXPECT_EQ(FailureOf(bytes), mti::DecodeFailure::Malformed);
}

TEST(OtlpTraceDecoderTest, KindOutOfRangeIsMalformed)
{
    const auto bytes = Build([](UpbSpan* span, UpbResource*, upb_Arena*)
                             { opentelemetry_proto_trace_v1_Span_set_kind(span, 6); });
    EXPECT_EQ(FailureOf(bytes), mti::DecodeFailure::Malformed);
}

TEST(OtlpTraceDecoderTest, UnspecifiedKindIsInternal)
{
    const auto bytes = Build([](UpbSpan* span, UpbResource*, upb_Arena*)
                             { opentelemetry_proto_trace_v1_Span_set_kind(span, 0); });
    const mtw::OtlpTraceDecoder decoder;
    const auto r = decoder.Decode(bytes, kGenerous);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->at(0).scopes.at(0).spans.at(0).kind, mt::SpanKind::Internal);
}

TEST(OtlpTraceDecoderTest, StatusCodeOutOfRangeIsMalformed)
{
    const auto bytes = Build(
        [](UpbSpan* span, UpbResource*, upb_Arena* arena)
        {
            opentelemetry_proto_trace_v1_Status_set_code(
                opentelemetry_proto_trace_v1_Span_mutable_status(span, arena), 3);
        });
    EXPECT_EQ(FailureOf(bytes), mti::DecodeFailure::Malformed);
}

TEST(OtlpTraceDecoderTest, TimestampPastTheClockRangeIsMalformed)
{
    const auto bytes = Build(
        [](UpbSpan* span, UpbResource*, upb_Arena*)
        {
            opentelemetry_proto_trace_v1_Span_set_end_time_unix_nano(
                span, std::numeric_limits<std::uint64_t>::max());
        });
    EXPECT_EQ(FailureOf(bytes), mti::DecodeFailure::Malformed);
}

TEST(OtlpTraceDecoderTest, EventTimestampPastTheClockRangeIsMalformed)
{
    const auto bytes = Build(
        [](UpbSpan* span, UpbResource*, upb_Arena* arena)
        {
            opentelemetry_proto_trace_v1_Span_Event_set_time_unix_nano(
                opentelemetry_proto_trace_v1_Span_add_events(span, arena),
                std::numeric_limits<std::uint64_t>::max());
        });
    EXPECT_EQ(FailureOf(bytes), mti::DecodeFailure::Malformed);
}
