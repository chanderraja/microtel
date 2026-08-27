// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Covers TracerShim and TracerProviderShim (otel-cpp ABI v1): StartSpan
// option mapping (kind, parent in all three variant states, start time,
// initial attributes), link forwarding via AddLink, flush/close delegation,
// and provider scope pass-through.

#include "adapters/otelcpp/tracer_shim.hpp"
#include "fakes/fake_provider.hpp"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <variant>

#include <opentelemetry/context/context.h>
#include <opentelemetry/trace/context.h>
#include <opentelemetry/trace/span_context.h>
#include <opentelemetry/trace/span_startoptions.h>

// clang-tidy does not model gtest's ASSERT_* macros, which `return` on
// failure. Every optional access below is guarded by a preceding
// `ASSERT_TRUE(parent.has_value())`, so the access is checked in fact even
// though the checker cannot see it. Suppressed file-wide, following the
// precedent set when otelcpp_attribute_conversion_test.cpp carried optionals.
// NOLINTBEGIN(bugprone-unchecked-optional-access)

namespace
{

using microtel::adapters::otelcpp::TracerProviderShim;
using microtel::adapters::otelcpp::TracerShim;
namespace otel_trace = opentelemetry::trace;

struct ShimFixture
{
    std::shared_ptr<microtel::testing::FakeProvider> provider =
        std::make_shared<microtel::testing::FakeProvider>();
    TracerShim shim{provider->tracer, provider};
};

[[nodiscard]] otel_trace::SpanContext MakeOtelContext(std::uint8_t seed)
{
    std::array<std::uint8_t, otel_trace::TraceId::kSize> trace_bytes{};
    for (std::size_t i = 0; i < trace_bytes.size(); ++i)
    {
        trace_bytes.at(i) = static_cast<std::uint8_t>(seed + i);
    }
    std::array<std::uint8_t, otel_trace::SpanId::kSize> span_bytes{};
    for (std::size_t i = 0; i < span_bytes.size(); ++i)
    {
        span_bytes.at(i) = static_cast<std::uint8_t>(seed + 1U + i);
    }
    return otel_trace::SpanContext{
        otel_trace::TraceId{
            opentelemetry::nostd::span<const std::uint8_t, otel_trace::TraceId::kSize>{
                trace_bytes.data(), trace_bytes.size()}},
        otel_trace::SpanId{
            opentelemetry::nostd::span<const std::uint8_t, otel_trace::SpanId::kSize>{
                span_bytes.data(), span_bytes.size()}},
        otel_trace::TraceFlags{otel_trace::TraceFlags::kIsSampled},
        /*is_remote=*/false};
}

// ── StartSpan: name, kind, attributes ─────────────────────────────────────────

TEST(OtelCppTracerShim, StartSpanForwardsNameKindAndAttributes)
{
    ShimFixture f;

    otel_trace::StartSpanOptions options;
    options.kind = otel_trace::SpanKind::kServer;
    auto span = f.shim.StartSpan("request", {{"code", std::int64_t{7}}}, options);

    ASSERT_EQ(f.provider->tracer->starts.size(), 1U);
    const auto& start = f.provider->tracer->starts[0];
    EXPECT_EQ(start.name, "request");
    EXPECT_EQ(start.kind, microtel::SpanKind::Server);
    ASSERT_EQ(start.attributes.size(), 1U);
    EXPECT_EQ(start.attributes[0].key, "code");
    EXPECT_EQ(std::get<std::int64_t>(start.attributes[0].value), 7);
}

TEST(OtelCppTracerShim, StartSpanMapsEveryKind)
{
    ShimFixture f;

    const std::pair<otel_trace::SpanKind, microtel::SpanKind> kinds[] = {
        {otel_trace::SpanKind::kInternal, microtel::SpanKind::Internal},
        {otel_trace::SpanKind::kServer, microtel::SpanKind::Server},
        {otel_trace::SpanKind::kClient, microtel::SpanKind::Client},
        {otel_trace::SpanKind::kProducer, microtel::SpanKind::Producer},
        {otel_trace::SpanKind::kConsumer, microtel::SpanKind::Consumer},
    };
    for (const auto& [otel_kind, microtel_kind] : kinds)
    {
        otel_trace::StartSpanOptions options;
        options.kind = otel_kind;
        auto span = f.shim.StartSpan("s", options);
        EXPECT_EQ(f.provider->tracer->starts.back().kind, microtel_kind);
    }
}

// ── StartSpan: parent, all three variant states ───────────────────────────────

TEST(OtelCppTracerShim, DefaultParentLeavesMicrotelParentUnset)
{
    ShimFixture f;

    auto span = f.shim.StartSpan("s");

    ASSERT_EQ(f.provider->tracer->starts.size(), 1U);
    // otel's default parent is the invalid SpanContext, meaning "inherit the
    // current context" — exactly microtel's unset semantics.
    EXPECT_FALSE(f.provider->tracer->starts[0].parent.has_value());
}

TEST(OtelCppTracerShim, ValidSpanContextParentConverts)
{
    ShimFixture f;

    otel_trace::StartSpanOptions options;
    options.parent = MakeOtelContext(0x10);
    auto span = f.shim.StartSpan("s", options);

    const auto& parent = f.provider->tracer->starts[0].parent;
    ASSERT_TRUE(parent.has_value());
    EXPECT_TRUE(parent.value().IsValid());
    EXPECT_EQ(parent.value().trace_id.AsBytes().at(0), 0x10);
    EXPECT_TRUE(parent.value().trace_flags.IsSampled());
}

TEST(OtelCppTracerShim, RootContextForcesRootSpan)
{
    ShimFixture f;

    opentelemetry::context::Context root;
    root = root.SetValue(otel_trace::kIsRootSpanKey, true);
    otel_trace::StartSpanOptions options;
    options.parent = root;
    auto span = f.shim.StartSpan("s", options);

    const auto& parent = f.provider->tracer->starts[0].parent;
    // A set-but-invalid parent is microtel's explicit root request
    // (sdk_tracer.cpp: invalid parent → fresh TraceId).
    ASSERT_TRUE(parent.has_value());
    EXPECT_FALSE(parent.value().IsValid());
}

TEST(OtelCppTracerShim, ContextCarryingSpanParentsToThatSpan)
{
    ShimFixture f;

    // Build an otel span whose context is known, stuff it into a Context, and
    // hand that Context as the parent.
    auto parent_span = f.shim.StartSpan("parent");
    f.provider->tracer->spans[0]->context = microtel::SpanContext{
        .trace_id = microtel::TraceId{microtel::TraceId::Bytes{
            9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9}},
        .span_id = microtel::SpanId{microtel::SpanId::Bytes{1, 2, 3, 4, 5, 6, 7, 8}},
        .trace_flags = microtel::TraceFlags{microtel::TraceFlags::kSampled},
        .trace_state = {},
        .remote = false,
    };

    opentelemetry::context::Context context;
    context = otel_trace::SetSpan(context, parent_span);
    otel_trace::StartSpanOptions options;
    options.parent = context;
    auto child = f.shim.StartSpan("child", options);

    ASSERT_EQ(f.provider->tracer->starts.size(), 2U);
    const auto& parent = f.provider->tracer->starts[1].parent;
    ASSERT_TRUE(parent.has_value());
    EXPECT_EQ(parent.value().trace_id.AsBytes().at(0), 9);
    EXPECT_EQ(parent.value().span_id.AsBytes().at(0), 1);
}

TEST(OtelCppTracerShim, ContextWithoutSpanLeavesParentUnset)
{
    ShimFixture f;

    otel_trace::StartSpanOptions options;
    options.parent = opentelemetry::context::Context{};
    auto span = f.shim.StartSpan("s", options);

    EXPECT_FALSE(f.provider->tracer->starts[0].parent.has_value());
}

// ── StartSpan: time, links, returned span ─────────────────────────────────────

TEST(OtelCppTracerShim, StartSystemTimeForwards)
{
    ShimFixture f;

    const auto when = std::chrono::system_clock::time_point{std::chrono::seconds{1700000000}};
    otel_trace::StartSpanOptions options;
    options.start_system_time = opentelemetry::common::SystemTimestamp{when};
    auto span = f.shim.StartSpan("s", options);

    EXPECT_EQ(f.provider->tracer->starts[0].start_time, when);
}

TEST(OtelCppTracerShim, LinksForwardViaAddLinkWithConvertedAttributes)
{
    ShimFixture f;

    const auto linked = MakeOtelContext(0x40);
    auto span = f.shim.StartSpan(
        "s", {}, {{linked, {{"link.kind", "follows"}}}}, otel_trace::StartSpanOptions{});

    ASSERT_EQ(f.provider->tracer->spans.size(), 1U);
    const auto& fake_span = *f.provider->tracer->spans[0];
    ASSERT_EQ(fake_span.links.size(), 1U);
    EXPECT_EQ(fake_span.links[0].context.trace_id.AsBytes().at(0), 0x40);
    ASSERT_EQ(fake_span.links[0].attributes.size(), 1U);
    EXPECT_EQ(fake_span.links[0].attributes[0].key, "link.kind");
    EXPECT_EQ(std::get<std::string>(fake_span.links[0].attributes[0].value), "follows");
}

TEST(OtelCppTracerShim, ReturnedSpanForwardsOntoMicrotel)
{
    ShimFixture f;

    auto span = f.shim.StartSpan("s");
    span->SetAttribute("k", std::int64_t{1});

    ASSERT_EQ(f.provider->tracer->spans.size(), 1U);
    ASSERT_EQ(f.provider->tracer->spans[0]->attributes.size(), 1U);
    EXPECT_EQ(f.provider->tracer->spans[0]->attributes[0].key, "k");
}

// ── Flush / close delegation ──────────────────────────────────────────────────

TEST(OtelCppTracerShim, ForceFlushDelegatesToProviderInMilliseconds)
{
    ShimFixture f;

    constexpr std::uint64_t kTwoSecondsUs = 2000000U;
    f.shim.ForceFlushWithMicroseconds(kTwoSecondsUs);

    ASSERT_EQ(f.provider->force_flush_calls.size(), 1U);
    EXPECT_EQ(f.provider->force_flush_calls[0], std::chrono::milliseconds{2000});
}

TEST(OtelCppTracerShim, CloseDelegatesToProviderShutdown)
{
    ShimFixture f;

    constexpr std::uint64_t kHalfSecondUs = 500000U;
    f.shim.CloseWithMicroseconds(kHalfSecondUs);

    ASSERT_EQ(f.provider->shutdown_calls.size(), 1U);
    EXPECT_EQ(f.provider->shutdown_calls[0], std::chrono::milliseconds{500});
}

// ── TracerProviderShim ────────────────────────────────────────────────────────

TEST(OtelCppTracerProviderShim, GetTracerForwardsNameAndVersion)
{
    auto provider = std::make_shared<microtel::testing::FakeProvider>();
    TracerProviderShim shim{provider};

    auto tracer = shim.GetTracer("my.lib", "1.2.3", "https://example.test/schema");

    ASSERT_NE(tracer, nullptr);
    ASSERT_EQ(provider->tracer_requests.size(), 1U);
    EXPECT_EQ(provider->tracer_requests[0].name, "my.lib");
    // schema_url has no microtel GetTracer surface and is dropped.
    EXPECT_EQ(provider->tracer_requests[0].version, "1.2.3");
}

TEST(OtelCppTracerProviderShim, GetTracerReturnsWorkingShim)
{
    auto provider = std::make_shared<microtel::testing::FakeProvider>();
    TracerProviderShim shim{provider};

    auto tracer = shim.GetTracer("my.lib");
    auto span = tracer->StartSpan("s");

    EXPECT_EQ(provider->tracer->starts.size(), 1U);
}

}  // namespace

// NOLINTEND(bugprone-unchecked-optional-access)
