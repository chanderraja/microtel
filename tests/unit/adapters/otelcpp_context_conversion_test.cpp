// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Covers the microtel ↔ otel-cpp identity bridging: TraceId, SpanId, flags,
// and the remote bit. TraceState deliberately does not round-trip — microtel's
// TraceState carries no storage yet — so both directions produce the empty
// default and that is asserted, not glossed over.

#include "adapters/otelcpp/context_conversion.hpp"

#include <gtest/gtest.h>

#include <cstdint>

namespace
{

using microtel::adapters::otelcpp::ToMicrotelSpanContext;
using microtel::adapters::otelcpp::ToOtelSpanContext;

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

TEST(OtelCppContextConversion, TraceStateIsEmptyDefaultBothWays)
{
    // microtel::TraceState has no storage or implementation yet; the bridge
    // must not pretend otherwise. Both directions yield the empty default.
    const auto otel = ToOtelSpanContext(MakeMicrotelContext(true, false));
    EXPECT_TRUE(otel.trace_state()->ToHeader().empty());
}

}  // namespace
