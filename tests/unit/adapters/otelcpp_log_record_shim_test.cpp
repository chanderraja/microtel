// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Covers LogRecordShim: every otel-cpp logs::LogRecord pure virtual (ABI v1)
// converting into a microtel::LogRecord. Body and attribute values route
// through ConvertAttributeValue (ICP 0015); ids through context_conversion.
// The one field with no microtel slot — SetEventId's integer id — is
// asserted dropped while the name half survives.

#include "adapters/otelcpp/log_record_shim.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <string>
#include <variant>

namespace
{

using microtel::adapters::otelcpp::LogRecordShim;
namespace otel_logs = opentelemetry::logs;

TEST(OtelCppLogRecordShim, SetTimestampConverts)
{
    LogRecordShim shim;
    const auto when = std::chrono::system_clock::time_point{std::chrono::seconds{1700000000}};
    shim.SetTimestamp(opentelemetry::common::SystemTimestamp{when});

    const auto record = shim.ReleaseRecord();
    EXPECT_EQ(record.time, when);
}

TEST(OtelCppLogRecordShim, SetObservedTimestampConverts)
{
    LogRecordShim shim;
    const auto when = std::chrono::system_clock::time_point{std::chrono::seconds{1700000001}};
    shim.SetObservedTimestamp(opentelemetry::common::SystemTimestamp{when});

    const auto record = shim.ReleaseRecord();
    EXPECT_EQ(record.observed_time, when);
}

TEST(OtelCppLogRecordShim, SeverityNumericValuesAreIdentical)
{
    // otel-cpp's Severity and microtel::SeverityNumber share the same 0–24
    // numbering (OTLP SeverityNumber). If either enum ever changes shape,
    // this catches it instead of silently mis-mapping.
    LogRecordShim unset_shim;
    unset_shim.SetSeverity(otel_logs::Severity::kInvalid);
    EXPECT_EQ(unset_shim.ReleaseRecord().severity_number, microtel::SeverityNumber::Unspecified);

    LogRecordShim fatal_shim;
    fatal_shim.SetSeverity(otel_logs::Severity::kFatal4);
    EXPECT_EQ(fatal_shim.ReleaseRecord().severity_number, microtel::SeverityNumber::Fatal4);

    LogRecordShim warn_shim;
    warn_shim.SetSeverity(otel_logs::Severity::kWarn);
    EXPECT_EQ(warn_shim.ReleaseRecord().severity_number, microtel::SeverityNumber::Warn);
}

TEST(OtelCppLogRecordShim, SetBodyConvertsViaAttributeConversion)
{
    LogRecordShim shim;
    shim.SetBody(opentelemetry::common::AttributeValue{"connection reset"});

    const auto record = shim.ReleaseRecord();
    EXPECT_EQ(std::get<std::string>(record.body), "connection reset");
}

TEST(OtelCppLogRecordShim, SetBodyDegradesOverflowingUint64PerIcp0015)
{
    LogRecordShim shim;
    shim.SetBody(opentelemetry::common::AttributeValue{std::numeric_limits<std::uint64_t>::max()});

    const auto record = shim.ReleaseRecord();
    EXPECT_EQ(std::get<std::string>(record.body), "18446744073709551615");
}

TEST(OtelCppLogRecordShim, SetAttributeAppendsConvertedKeyValues)
{
    LogRecordShim shim;
    shim.SetAttribute("retry_count", opentelemetry::common::AttributeValue{std::int64_t{3}});
    shim.SetAttribute("host", opentelemetry::common::AttributeValue{"db-primary"});

    const auto record = shim.ReleaseRecord();
    ASSERT_EQ(record.attributes.size(), 2U);
    EXPECT_EQ(record.attributes[0].key, "retry_count");
    EXPECT_EQ(std::get<std::int64_t>(record.attributes[0].value), 3);
    EXPECT_EQ(record.attributes[1].key, "host");
    EXPECT_EQ(std::get<std::string>(record.attributes[1].value), "db-primary");
}

TEST(OtelCppLogRecordShim, SetEventIdKeepsNameDropsNumericId)
{
    // microtel::LogRecord has no integer event-id field, only event_name.
    // Preserve what can be preserved, omit what cannot (ICP 0015's
    // principle, applied here without a new interface change).
    LogRecordShim shim;
    shim.SetEventId(42, "checkout.completed");

    const auto record = shim.ReleaseRecord();
    EXPECT_EQ(record.event_name, "checkout.completed");
}

TEST(OtelCppLogRecordShim, SetEventIdWithNoNameLeavesEventNameEmpty)
{
    LogRecordShim shim;
    shim.SetEventId(7);

    const auto record = shim.ReleaseRecord();
    EXPECT_TRUE(record.event_name.empty());
}

TEST(OtelCppLogRecordShim, SetTraceIdAndSpanIdConvertViaContextConversion)
{
    LogRecordShim shim;
    const std::uint8_t trace_bytes[opentelemetry::trace::TraceId::kSize] = {
        1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    const std::uint8_t span_bytes[opentelemetry::trace::SpanId::kSize] = {8, 7, 6, 5, 4, 3, 2, 1};
    shim.SetTraceId(opentelemetry::trace::TraceId{trace_bytes});
    shim.SetSpanId(opentelemetry::trace::SpanId{span_bytes});
    shim.SetTraceFlags(
        opentelemetry::trace::TraceFlags{opentelemetry::trace::TraceFlags::kIsSampled});

    const auto record = shim.ReleaseRecord();
    EXPECT_EQ(record.trace_id.AsBytes().at(0), 1);
    EXPECT_EQ(record.trace_id.AsBytes().at(15), 16);
    EXPECT_EQ(record.span_id.AsBytes().at(0), 8);
    EXPECT_TRUE(record.trace_flags.IsSampled());
}

}  // namespace
