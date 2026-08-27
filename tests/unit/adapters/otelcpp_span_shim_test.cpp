// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Covers SpanShim: every otel-cpp trace::Span pure virtual (ABI v1) forwarded
// onto a recording FakeSpan. Attribute values route through
// ConvertAttributeValue, so one degraded-type case is asserted here to prove
// the wiring; exhaustive conversion coverage lives in
// otelcpp_attribute_conversion_test.cpp.

#include "adapters/otelcpp/span_shim.hpp"
#include "fakes/fake_span.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace
{

using microtel::adapters::otelcpp::SpanShim;
namespace otel_trace = opentelemetry::trace;
namespace otel_common = opentelemetry::common;

/// Wraps a caller-owned FakeSpan in a SpanHandle whose deleter is a no-op,
/// mirroring the unsampled-singleton pattern the handle documents.
[[nodiscard]] microtel::SpanHandle BorrowHandle(microtel::testing::FakeSpan& fake)
{
    return microtel::SpanHandle{&fake, microtel::internal::SpanDeleter{.deleter = nullptr}};
}

TEST(OtelCppSpanShim, SetAttributeForwardsConvertedValue)
{
    microtel::testing::FakeSpan fake;
    SpanShim shim{BorrowHandle(fake)};

    shim.SetAttribute("count", std::int64_t{42});

    ASSERT_EQ(fake.attributes.size(), 1U);
    EXPECT_EQ(fake.attributes[0].key, "count");
    EXPECT_EQ(std::get<std::int64_t>(fake.attributes[0].value), 42);
}

TEST(OtelCppSpanShim, SetAttributeDegradesOverflowingUint64PerIcp0015)
{
    microtel::testing::FakeSpan fake;
    SpanShim shim{BorrowHandle(fake)};

    shim.SetAttribute("big", std::numeric_limits<std::uint64_t>::max());

    ASSERT_EQ(fake.attributes.size(), 1U);
    EXPECT_EQ(std::get<std::string>(fake.attributes[0].value), "18446744073709551615");
}

TEST(OtelCppSpanShim, AddEventNameOnly)
{
    microtel::testing::FakeSpan fake;
    SpanShim shim{BorrowHandle(fake)};

    shim.AddEvent("checkpoint");

    ASSERT_EQ(fake.events.size(), 1U);
    EXPECT_EQ(fake.events[0].name, "checkpoint");
    EXPECT_TRUE(fake.events[0].attributes.empty());
    // Zero timestamp means "now" per microtel's AddEvent contract.
    EXPECT_EQ(fake.events[0].timestamp, std::chrono::system_clock::time_point{});
}

TEST(OtelCppSpanShim, AddEventWithTimestamp)
{
    microtel::testing::FakeSpan fake;
    SpanShim shim{BorrowHandle(fake)};

    const auto when = std::chrono::system_clock::time_point{std::chrono::seconds{1700000000}};
    shim.AddEvent("checkpoint", otel_common::SystemTimestamp{when});

    ASSERT_EQ(fake.events.size(), 1U);
    EXPECT_EQ(fake.events[0].timestamp, when);
}

TEST(OtelCppSpanShim, AddEventWithTimestampAndAttributesConvertsEachValue)
{
    microtel::testing::FakeSpan fake;
    SpanShim shim{BorrowHandle(fake)};

    const auto when = std::chrono::system_clock::time_point{std::chrono::seconds{1700000000}};
    shim.AddEvent("checkpoint",
                  otel_common::SystemTimestamp{when},
                  {{"code", std::int64_t{7}}, {"detail", "retry"}});

    ASSERT_EQ(fake.events.size(), 1U);
    const auto& event = fake.events[0];
    ASSERT_EQ(event.attributes.size(), 2U);
    EXPECT_EQ(event.attributes[0].key, "code");
    EXPECT_EQ(std::get<std::int64_t>(event.attributes[0].value), 7);
    EXPECT_EQ(event.attributes[1].key, "detail");
    EXPECT_EQ(std::get<std::string>(event.attributes[1].value), "retry");
}

TEST(OtelCppSpanShim, SetStatusMapsEveryCode)
{
    microtel::testing::FakeSpan fake;
    SpanShim shim{BorrowHandle(fake)};

    shim.SetStatus(otel_trace::StatusCode::kUnset, "u");
    shim.SetStatus(otel_trace::StatusCode::kOk, "o");
    shim.SetStatus(otel_trace::StatusCode::kError, "boom");

    ASSERT_EQ(fake.statuses.size(), 3U);
    EXPECT_EQ(fake.statuses[0].code, microtel::StatusCode::Unset);
    EXPECT_EQ(fake.statuses[1].code, microtel::StatusCode::Ok);
    EXPECT_EQ(fake.statuses[2].code, microtel::StatusCode::Error);
    EXPECT_EQ(fake.statuses[2].description, "boom");
}

TEST(OtelCppSpanShim, UpdateNameForwards)
{
    microtel::testing::FakeSpan fake;
    SpanShim shim{BorrowHandle(fake)};

    shim.UpdateName("renamed");

    ASSERT_EQ(fake.names.size(), 1U);
    EXPECT_EQ(fake.names[0], "renamed");
}

TEST(OtelCppSpanShim, EndWithDefaultOptionsForwardsZeroTime)
{
    microtel::testing::FakeSpan fake;
    SpanShim shim{BorrowHandle(fake)};

    shim.End();

    ASSERT_EQ(fake.end_calls.size(), 1U);
    // Zero means "now" per microtel's End contract; the shim must not invent
    // a timestamp the SDK would otherwise stamp more precisely.
    EXPECT_EQ(fake.end_calls[0], std::chrono::system_clock::time_point{});
}

TEST(OtelCppSpanShim, EndWithSteadyTimeMapsOntoSystemClock)
{
    microtel::testing::FakeSpan fake;
    SpanShim shim{BorrowHandle(fake)};

    const auto before = std::chrono::system_clock::now();
    shim.End(otel_trace::EndSpanOptions{
        .end_steady_time = otel_common::SteadyTimestamp{std::chrono::steady_clock::now()}});
    const auto after = std::chrono::system_clock::now();

    ASSERT_EQ(fake.end_calls.size(), 1U);
    // A steady "now" must land on the system clock's "now", within the window
    // the test itself observed.
    EXPECT_GE(fake.end_calls[0], before - std::chrono::seconds{1});
    EXPECT_LE(fake.end_calls[0], after + std::chrono::seconds{1});
}

TEST(OtelCppSpanShim, GetContextConvertsIds)
{
    microtel::testing::FakeSpan fake;
    fake.context.trace_id = microtel::TraceId{
        microtel::TraceId::Bytes{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16}};
    fake.context.span_id = microtel::SpanId{microtel::SpanId::Bytes{8, 7, 6, 5, 4, 3, 2, 1}};
    fake.context.trace_flags = microtel::TraceFlags{microtel::TraceFlags::kSampled};
    const SpanShim shim{BorrowHandle(fake)};

    const auto otel_context = shim.GetContext();

    ASSERT_TRUE(otel_context.IsValid());
    EXPECT_EQ(otel_context.trace_id().Id()[0], 1);
    EXPECT_EQ(otel_context.trace_id().Id()[15], 16);
    EXPECT_EQ(otel_context.span_id().Id()[0], 8);
    EXPECT_TRUE(otel_context.trace_flags().IsSampled());
}

TEST(OtelCppSpanShim, IsRecordingReflectsSampling)
{
    microtel::testing::FakeSpan sampled_fake;
    sampled_fake.sampled = true;
    EXPECT_TRUE(SpanShim{BorrowHandle(sampled_fake)}.IsRecording());

    microtel::testing::FakeSpan unsampled_fake;
    unsampled_fake.sampled = false;
    EXPECT_FALSE(SpanShim{BorrowHandle(unsampled_fake)}.IsRecording());
}

}  // namespace
