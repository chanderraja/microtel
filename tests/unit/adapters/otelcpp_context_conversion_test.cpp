// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Covers the microtel ↔ otel-cpp identity bridging: TraceId, SpanId, flags,
// the remote bit, and — since issue #208 gave microtel's TraceState storage —
// tracestate in both directions.

#include "adapters/otelcpp/context_conversion.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <string_view>

namespace
{

using microtel::adapters::otelcpp::ToMicrotelSpanContext;
using microtel::adapters::otelcpp::ToOtelSpanContext;

/// @brief The W3C Trace Context specification's own example `tracestate`.
constexpr std::string_view kSpecTracestate = "rojo=00f067aa0ba902b7,congo=t61rcWkgMzE";

[[nodiscard]] microtel::SpanContext MakeMicrotelContext(bool sampled, bool remote)
{
    microtel::TraceId::Bytes trace_bytes{};
    for (std::size_t i = 0; i < trace_bytes.size(); ++i)
    {
        trace_bytes.at(i) = static_cast<std::uint8_t>(i + 1);
    }
    microtel::SpanId::Bytes span_bytes{};
    for (std::size_t i = 0; i < span_bytes.size(); ++i)
    {
        span_bytes.at(i) = static_cast<std::uint8_t>(0xA0U + i);
    }
    return microtel::SpanContext{
        .trace_id = microtel::TraceId{trace_bytes},
        .span_id = microtel::SpanId{span_bytes},
        .trace_flags =
            microtel::TraceFlags{sampled ? microtel::TraceFlags::kSampled : std::uint8_t{0}},
        .trace_state = {},
        .remote = remote,
    };
}

TEST(OtelCppContextConversion, TraceAndSpanIdBytesSurviveToOtel)
{
    const auto source = MakeMicrotelContext(/*sampled=*/true, /*remote=*/false);
    const auto otel = ToOtelSpanContext(source);

    ASSERT_TRUE(otel.IsValid());
    for (std::size_t i = 0; i < microtel::TraceId::kSizeBytes; ++i)
    {
        EXPECT_EQ(otel.trace_id().Id()[i], source.trace_id.AsBytes().at(i)) << "trace byte " << i;
    }
    for (std::size_t i = 0; i < microtel::SpanId::kSizeBytes; ++i)
    {
        EXPECT_EQ(otel.span_id().Id()[i], source.span_id.AsBytes().at(i)) << "span byte " << i;
    }
}

TEST(OtelCppContextConversion, RoundTripPreservesIdsFlagsAndRemote)
{
    const auto source = MakeMicrotelContext(/*sampled=*/true, /*remote=*/true);
    const auto round_tripped = ToMicrotelSpanContext(ToOtelSpanContext(source));

    EXPECT_EQ(round_tripped.trace_id.AsBytes(), source.trace_id.AsBytes());
    EXPECT_EQ(round_tripped.span_id.AsBytes(), source.span_id.AsBytes());
    EXPECT_TRUE(round_tripped.trace_flags.IsSampled());
    EXPECT_TRUE(round_tripped.remote);
}

TEST(OtelCppContextConversion, UnsampledLocalContextStaysUnsampledLocal)
{
    const auto source = MakeMicrotelContext(/*sampled=*/false, /*remote=*/false);
    const auto otel = ToOtelSpanContext(source);

    EXPECT_FALSE(otel.trace_flags().IsSampled());
    EXPECT_FALSE(otel.IsRemote());

    const auto back = ToMicrotelSpanContext(otel);
    EXPECT_FALSE(back.trace_flags.IsSampled());
    EXPECT_FALSE(back.remote);
}

TEST(OtelCppContextConversion, InvalidContextConvertsToInvalid)
{
    const auto otel = ToOtelSpanContext(microtel::SpanContext{});
    EXPECT_FALSE(otel.IsValid());

    const auto back = ToMicrotelSpanContext(otel);
    EXPECT_FALSE(back.IsValid());
}

TEST(OtelCppContextConversion, AnEmptyTraceStateStaysEmptyBothWays)
{
    const auto otel = ToOtelSpanContext(MakeMicrotelContext(/*sampled=*/true, /*remote=*/false));
    EXPECT_TRUE(otel.trace_state()->Empty());

    EXPECT_TRUE(ToMicrotelSpanContext(otel).trace_state.Empty());
}

TEST(OtelCppContextConversion, TraceStateCrossesToOtelCpp)
{
    // Issue #208 / ICP 0025 packet 2.3a: before microtel's TraceState had
    // storage this was asserted to be empty in both directions. Both
    // implementations speak the same W3C §3.3 grammar, so the header string is
    // the bridge.
    auto source = MakeMicrotelContext(/*sampled=*/true, /*remote=*/false);
    source.trace_state = microtel::TraceState::FromHeader(kSpecTracestate);

    const auto otel = ToOtelSpanContext(source);
    EXPECT_FALSE(otel.trace_state()->Empty());
    EXPECT_EQ(otel.trace_state()->ToHeader(), std::string(kSpecTracestate));

    std::string value;
    EXPECT_TRUE(otel.trace_state()->Get("rojo", value));
    EXPECT_EQ(value, "00f067aa0ba902b7");
}

TEST(OtelCppContextConversion, TraceStateCrossesBackFromOtelCpp)
{
    const opentelemetry::trace::SpanContext otel{
        opentelemetry::trace::TraceId{},
        opentelemetry::trace::SpanId{},
        opentelemetry::trace::TraceFlags{},
        /*is_remote=*/true,
        opentelemetry::trace::TraceState::FromHeader(std::string(kSpecTracestate))};

    const auto back = ToMicrotelSpanContext(otel);
    EXPECT_EQ(back.trace_state.Size(), 2U);
    EXPECT_EQ(back.trace_state.ToHeader(), kSpecTracestate);
}

TEST(OtelCppContextConversion, TraceStateSurvivesAFullRoundTrip)
{
    auto source = MakeMicrotelContext(/*sampled=*/true, /*remote=*/true);
    source.trace_state = microtel::TraceState::FromHeader(kSpecTracestate);

    const auto round_tripped = ToMicrotelSpanContext(ToOtelSpanContext(source));
    EXPECT_EQ(round_tripped.trace_state.ToHeader(), kSpecTracestate);
    EXPECT_EQ(round_tripped.trace_id.AsBytes(), source.trace_id.AsBytes());
}

}  // namespace
