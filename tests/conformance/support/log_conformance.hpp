// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// The logs conformance scenarios, shared by tests/conformance/http/logs_test.cpp
// and tests/conformance/grpc/logs_test.cpp.
//
// Each scenario emits log records through a caller-built Provider, flushes,
// and asserts on what the collector wrote to /out/logs.jsonl. The two protocol
// suites differ only in how the Provider reaches the collector; the payload
// assertions are shared because the file exporter serialises the decoded
// ResourceLogs, by which point the receiving protocol has been discarded. That
// is the same reasoning (and the same measured result) as the trace suites'
// shared fragments: a fragment that differed by protocol would mean microtel's
// gRPC encoding disagreed with its HTTP encoding, which is a finding, not a
// reason for per-protocol expectations.
//
// Every record carries a per-run unique `event_name`, and each assertion is
// narrowed to that one record's JSON object with `EnclosingObject`. A line
// alone is not enough: the collector's batch processor may merge records from
// several exports into one line, and the correlation scenario in particular
// has to prove that one record *lacks* a field another record in the same line
// has.
//
// The expected fragments are the pinned collector's protojson rendering, read
// off a real run: camelCase keys, int64 (including the nanosecond timestamps)
// as quoted strings, `severityNumber` and `flags` as bare integers, zero-valued
// fields omitted.

#pragma once

#include "microtel/attribute.hpp"
#include "microtel/log_record.hpp"
#include "microtel/logger.hpp"
#include "microtel/provider.hpp"
#include "microtel/span.hpp"
#include "microtel/status.hpp"
#include "microtel/trace.hpp"
#include "microtel/tracer.hpp"

#include "conformance/support/collector_output.hpp"
#include "conformance/support/conformance_env.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace microtel::testing::logs
{

/// @brief Environment variable naming the collector's logs output file.
inline constexpr const char* kLogsOutputFileEnv = "MICROTEL_CONFORMANCE_LOGS_OUTPUT_FILE";

inline constexpr const char* kServiceName = "microtel-conformance";
inline constexpr const char* kScopeVersion = "1.0";

inline constexpr auto kFlushTimeout = std::chrono::seconds(30);
inline constexpr auto kCollectorPollTimeout = std::chrono::seconds(15);
inline constexpr auto kPollInterval = std::chrono::milliseconds(100);

/// Nesting from a log record's `eventName` outward: the record itself, then
/// its `ScopeLogs`, then its `ResourceLogs`.
inline constexpr std::size_t kRecordLevel = 1;
inline constexpr std::size_t kScopeLogsLevel = 2;
inline constexpr std::size_t kResourceLogsLevel = 3;

/// Fixed, microsecond-aligned instants for the explicit-timestamp record, so
/// the expected nanosecond strings are exact whatever the clock's period.
inline constexpr std::int64_t kEventTimeNanos = 1'700'000'000'123'456'000;
inline constexpr std::int64_t kObservedTimeNanos = 1'700'000'000'223'456'000;

inline constexpr std::uint32_t kDroppedAttributes = 3;
inline constexpr std::int64_t kIntAttribute = 42;
inline constexpr double kDoubleAttribute = 2.5;
inline constexpr std::int64_t kIntBody = -7;
inline constexpr double kDoubleBody = 0.25;

/// SeverityNumber::Unspecified (0) through SeverityNumber::Fatal4 (24).
inline constexpr unsigned kSeverityCount = 25;

inline constexpr std::size_t kGzipRecordCount = 50;

// Record-level fragments, confirmed against the pinned collector.
inline constexpr const char* kServiceNameJson =
    R"({"key":"service.name","value":{"stringValue":"microtel-conformance"}})";
inline constexpr const char* kTimeJson = R"("timeUnixNano":"1700000000123456000")";
inline constexpr const char* kObservedTimeJson = R"("observedTimeUnixNano":"1700000000223456000")";
inline constexpr const char* kSeverityNumberJson = R"("severityNumber":13)";
inline constexpr const char* kSeverityTextJson = R"("severityText":"WARN")";
inline constexpr const char* kStringBodyJson = R"("body":{"stringValue":"conformance log body"})";
inline constexpr const char* kStringAttrJson =
    R"({"key":"conformance.attr.string","value":{"stringValue":"string-value"}})";
inline constexpr const char* kInt64AttrJson =
    R"({"key":"conformance.attr.int64","value":{"intValue":"42"}})";
inline constexpr const char* kDoubleAttrJson =
    R"({"key":"conformance.attr.double","value":{"doubleValue":2.5}})";
inline constexpr const char* kBoolAttrJson =
    R"({"key":"conformance.attr.bool","value":{"boolValue":true}})";
inline constexpr const char* kDroppedAttrsJson = R"("droppedAttributesCount":3)";
inline constexpr const char* kSampledFlagsJson = R"("flags":1)";

/// @brief Asserts @p haystack contains @p fragment, reporting @p haystack if not.
inline void ExpectContains(const std::string& haystack, const std::string& fragment)
{
    EXPECT_NE(haystack.find(fragment), std::string::npos)
        << "collector output is missing " << fragment << "\n  in: " << haystack;
}

/// @brief Asserts @p haystack does not contain @p fragment.
inline void ExpectLacks(const std::string& haystack, const std::string& fragment)
{
    EXPECT_EQ(haystack.find(fragment), std::string::npos)
        << "collector output unexpectedly has " << fragment << "\n  in: " << haystack;
}

/// @brief The exact `eventName` member for @p marker — quoted on both sides,
///        so `<m>.sev.1` cannot match inside `<m>.sev.10`.
inline std::string EventNameJson(const std::string& marker)
{
    return R"("eventName":")" + marker + R"(")";
}

/// @brief A time point @p nanos after the epoch.
inline std::chrono::system_clock::time_point FromNanos(const std::int64_t nanos)
{
    return std::chrono::system_clock::time_point{
        std::chrono::duration_cast<std::chrono::system_clock::duration>(
            std::chrono::nanoseconds{nanos})};
}

/// @brief Nanoseconds since the epoch, now.
inline std::int64_t NowNanos()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

/// @brief Flushes @p provider and asserts every export was accepted.
inline void FlushAndExpectDelivered(Provider& provider)
{
    ASSERT_EQ(provider.ForceFlush(kFlushTimeout), Status::Completed);
    const HealthSnapshot health = provider.GetExporterHealth();
    EXPECT_GE(health.batches_sent, 1U);
    EXPECT_EQ(health.batches_failed, 0U) << health.last_error_message;
}

/// @brief Waits for the record whose event name is @p marker and returns the
///        object @p levels out from it (see `EnclosingObject`).
///
/// @return the object, or empty if it never arrived; a matched object is never
///         empty, so the caller asserts non-empty rather than dereferencing an
///         optional (gtest's early return is invisible to clang-tidy).
inline std::string AwaitRecordObject(const std::string& output_file,
                                     const std::string& marker,
                                     const std::size_t levels = kRecordLevel)
{
    const std::string needle = EventNameJson(marker);
    const auto line = PollForLineContaining(output_file, needle, kCollectorPollTimeout);
    if (!line.has_value())
    {
        return {};
    }
    return EnclosingObject(*line, needle, levels).value_or(std::string{});
}

/// @brief Polls until @p needle occurs at least @p expected times.
/// @return the last observed count, so over-delivery stays visible.
inline std::size_t PollForOccurrences(const std::string& path,
                                      const std::string& needle,
                                      const std::size_t expected)
{
    const auto deadline = std::chrono::steady_clock::now() + kCollectorPollTimeout;
    std::size_t count = CountOccurrences(path, needle);
    while (count < expected && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(kPollInterval);
        count = CountOccurrences(path, needle);
    }
    return count;
}

/// @brief One record with every scalar field set: severity, text body, one
///        attribute of each scalar type, both timestamps, a dropped-attribute
///        count and an event name. Asserts it arrived exactly once, with its
///        resource and instrumentation scope.
inline void RunRecordRoundTrip(Provider& provider,
                               const std::string& output_file,
                               const std::string& scope_name)
{
    const std::string marker = UniqueMarker();
    {
        LogRecord record;
        record.time = FromNanos(kEventTimeNanos);
        record.observed_time = FromNanos(kObservedTimeNanos);
        record.severity_number = SeverityNumber::Warn;
        record.severity_text = "WARN";
        record.body = std::string{"conformance log body"};
        record.attributes = {
            KeyValue{.key = "conformance.attr.string", .value = std::string{"string-value"}},
            KeyValue{.key = "conformance.attr.int64", .value = kIntAttribute},
            KeyValue{.key = "conformance.attr.double", .value = kDoubleAttribute},
            KeyValue{.key = "conformance.attr.bool", .value = true},
        };
        record.dropped_attributes_count = kDroppedAttributes;
        record.event_name = marker;
        provider.GetLogger(scope_name, kScopeVersion)->Emit(std::move(record));
    }
    FlushAndExpectDelivered(provider);

    const std::string resource_logs = AwaitRecordObject(output_file, marker, kResourceLogsLevel);
    ASSERT_FALSE(resource_logs.empty())
        << "collector never wrote log record '" << marker << "' to " << output_file;
    const std::string scope_logs = AwaitRecordObject(output_file, marker, kScopeLogsLevel);
    const std::string record = AwaitRecordObject(output_file, marker);

    ExpectContains(resource_logs, kServiceNameJson);
    ExpectContains(scope_logs, R"("scope":{"name":")" + scope_name + R"(","version":"1.0"})");
    ExpectContains(record, kTimeJson);
    ExpectContains(record, kObservedTimeJson);
    ExpectContains(record, kSeverityNumberJson);
    ExpectContains(record, kSeverityTextJson);
    ExpectContains(record, kStringBodyJson);
    ExpectContains(record, kStringAttrJson);
    ExpectContains(record, kInt64AttrJson);
    ExpectContains(record, kDoubleAttrJson);
    ExpectContains(record, kBoolAttrJson);
    ExpectContains(record, kDroppedAttrsJson);
    // No active span, and the caller set no ids: the record is uncorrelated.
    ExpectLacks(record, R"("traceId")");
    ExpectLacks(record, R"("spanId")");

    EXPECT_EQ(CountOccurrences(output_file, EventNameJson(marker)), 1U)
        << "record delivered more than once";
}

/// @brief Every `SeverityNumber` from Unspecified (0) to Fatal4 (24), in one
///        flush. The collector must decode each to the same number; 0 is the
///        proto default and is therefore absent rather than rendered.
inline void RunEverySeverity(Provider& provider,
                             const std::string& output_file,
                             const std::string& scope_name)
{
    const std::string marker = UniqueMarker();
    const auto logger = provider.GetLogger(scope_name, kScopeVersion);
    for (unsigned sev = 0; sev < kSeverityCount; ++sev)
    {
        LogRecord record;
        record.severity_number = static_cast<SeverityNumber>(sev);
        record.severity_text = "sev-" + std::to_string(sev);
        record.body = std::string{"severity probe"};
        record.event_name = marker + ".sev." + std::to_string(sev);
        logger->Emit(std::move(record));
    }
    FlushAndExpectDelivered(provider);

    for (unsigned sev = 0; sev < kSeverityCount; ++sev)
    {
        const std::string record =
            AwaitRecordObject(output_file, marker + ".sev." + std::to_string(sev));
        ASSERT_FALSE(record.empty()) << "severity " << sev << " never reached the collector";
        ExpectContains(record, R"("severityText":"sev-)" + std::to_string(sev) + R"(")");
        if (sev == 0)
        {
            ExpectLacks(record, R"("severityNumber")");
            continue;
        }
        ExpectContains(record, R"("severityNumber":)" + std::to_string(sev) + ",");
    }
}

/// @brief A body of each type `AttributeValue` can hold: the four scalars and
///        the four homogeneous arrays. (A key/value-list body is not
///        expressible in the public API, so there is nothing to send.)
inline void RunBodyTypes(Provider& provider,
                         const std::string& output_file,
                         const std::string& scope_name)
{
    struct BodyCase
    {
        const char* suffix;
        AttributeValue body;
        const char* expected;
    };
    const std::vector<BodyCase> cases = {
        {.suffix = "string",
         .body = std::string{"text"},
         .expected = R"("body":{"stringValue":"text"})"},
        {.suffix = "bool", .body = true, .expected = R"("body":{"boolValue":true})"},
        {.suffix = "int64", .body = kIntBody, .expected = R"("body":{"intValue":"-7"})"},
        {.suffix = "double", .body = kDoubleBody, .expected = R"("body":{"doubleValue":0.25})"},
        {.suffix = "string-array",
         .body = std::vector<std::string>{"a", "b"},
         .expected =
             R"("body":{"arrayValue":{"values":[{"stringValue":"a"},{"stringValue":"b"}]}})"},
        {.suffix = "bool-array",
         .body = std::vector<bool>{true, false},
         .expected =
             R"("body":{"arrayValue":{"values":[{"boolValue":true},{"boolValue":false}]}})"},
        {.suffix = "int64-array",
         .body = std::vector<std::int64_t>{1, -2},
         .expected = R"("body":{"arrayValue":{"values":[{"intValue":"1"},{"intValue":"-2"}]}})"},
        {.suffix = "double-array",
         .body = std::vector<double>{0.5, -1.5},
         .expected =
             R"("body":{"arrayValue":{"values":[{"doubleValue":0.5},{"doubleValue":-1.5}]}})"},
    };

    const std::string marker = UniqueMarker();
    const auto logger = provider.GetLogger(scope_name, kScopeVersion);
    for (const BodyCase& c : cases)
    {
        LogRecord record;
        record.severity_number = SeverityNumber::Info;
        record.body = c.body;
        record.event_name = marker + "." + c.suffix;
        logger->Emit(std::move(record));
    }
    FlushAndExpectDelivered(provider);

    for (const BodyCase& c : cases)
    {
        const std::string record = AwaitRecordObject(output_file, marker + "." + c.suffix);
        ASSERT_FALSE(record.empty()) << c.suffix << " body never reached the collector";
        ExpectContains(record, c.expected);
    }
}

/// @brief A record with neither timestamp set: `time` stays unknown (absent
///        on the wire) and the SDK backfills `observed_time` at `Emit()` with
///        a wall-clock instant the collector decodes intact.
inline void RunObservedTimeBackfill(Provider& provider,
                                    const std::string& output_file,
                                    const std::string& scope_name)
{
    const std::string marker = UniqueMarker();
    const std::int64_t before = NowNanos();
    {
        LogRecord record;
        record.severity_number = SeverityNumber::Info;
        record.body = std::string{"no timestamps"};
        record.event_name = marker;
        provider.GetLogger(scope_name, kScopeVersion)->Emit(std::move(record));
    }
    const std::int64_t after = NowNanos();
    FlushAndExpectDelivered(provider);

    const std::string record = AwaitRecordObject(output_file, marker);
    ASSERT_FALSE(record.empty()) << "collector never wrote log record '" << marker << "'";
    ExpectLacks(record, R"("timeUnixNano")");

    const std::string key = R"("observedTimeUnixNano":")";
    const std::size_t at = record.find(key);
    ASSERT_NE(at, std::string::npos) << "observed time was not backfilled: " << record;
    const std::size_t digits = at + key.size();
    const std::int64_t observed =
        std::stoll(record.substr(digits, record.find('"', digits) - digits));
    EXPECT_GE(observed, before);
    EXPECT_LE(observed, after);
}

/// @brief A record emitted inside an active span carries that span's
///        trace id, span id and sampled flag on the wire; its sibling emitted
///        after the span scope closed carries none of the three.
inline void RunTraceCorrelation(Provider& provider,
                                const std::string& output_file,
                                const std::string& scope_name)
{
    const std::string marker = UniqueMarker();
    const auto logger = provider.GetLogger(scope_name, kScopeVersion);
    SpanContext context;
    {
        const auto scoped =
            provider.GetTracer(scope_name, kScopeVersion)->StartAsCurrentSpan(marker + ".span");
        context = scoped->GetContext();
        LogRecord inside;
        inside.severity_number = SeverityNumber::Info;
        inside.body = std::string{"inside span"};
        inside.event_name = marker + ".inside";
        logger->Emit(std::move(inside));
    }
    {
        LogRecord outside;
        outside.severity_number = SeverityNumber::Info;
        outside.body = std::string{"outside span"};
        outside.event_name = marker + ".outside";
        logger->Emit(std::move(outside));
    }
    ASSERT_TRUE(context.IsValid()) << "the scoped span has no valid context";
    ASSERT_TRUE(context.trace_flags.IsSampled()) << "the default sampler should sample";
    FlushAndExpectDelivered(provider);

    const std::string inside = AwaitRecordObject(output_file, marker + ".inside");
    ASSERT_FALSE(inside.empty()) << "in-span record never reached the collector";
    ExpectContains(inside, R"("traceId":")" + context.trace_id.ToHex() + R"(")");
    ExpectContains(inside, R"("spanId":")" + context.span_id.ToHex() + R"(")");
    ExpectContains(inside, kSampledFlagsJson);

    const std::string outside = AwaitRecordObject(output_file, marker + ".outside");
    ASSERT_FALSE(outside.empty()) << "out-of-span record never reached the collector";
    ExpectLacks(outside, R"("traceId")");
    ExpectLacks(outside, R"("spanId")");
    ExpectLacks(outside, R"("flags")");
}

/// @brief @ref kGzipRecordCount records through a provider the caller built
///        with gzip compression; every one must arrive exactly once.
inline void RunGzipDelivery(Provider& provider,
                            const std::string& output_file,
                            const std::string& scope_name)
{
    const std::string marker = UniqueMarker();
    const auto logger = provider.GetLogger(scope_name, kScopeVersion);
    for (std::size_t i = 0; i < kGzipRecordCount; ++i)
    {
        LogRecord record;
        record.severity_number = SeverityNumber::Info;
        record.body = std::string{"compressed"};
        record.event_name = marker + "." + std::to_string(i);
        logger->Emit(std::move(record));
    }
    FlushAndExpectDelivered(provider);

    // The prefix is followed by the index, so it counts records, and the
    // leading quote-colon keeps it from matching anywhere but an eventName.
    const std::string prefix = R"("eventName":")" + marker + ".";
    EXPECT_EQ(PollForOccurrences(output_file, prefix, kGzipRecordCount), kGzipRecordCount);
}

}  // namespace microtel::testing::logs
