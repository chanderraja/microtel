// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Behavioural tests for SdkLogger::Emit — M14 L4.1 (docs/logs-design.md §1/§4/§5).
//
// Contract under test:
//  - Emit routes the record to the processor, tagged with the logger's scope.
//  - observed_time is stamped when unset and preserved when the caller set it.
//  - Trace context is filled from the active span when trace_id is invalid and
//    a source is present; never overwritten, never fabricated without a span.
//  - The per-record attribute limit truncates surplus attributes, bumps
//    dropped_attributes_count, and records a LogAttributeLimit drop.

#include "sdk/sdk_logger.hpp"

#include "microtel/attribute.hpp"
#include "microtel/internal/icurrent_span_source.hpp"
#include "microtel/log_record.hpp"
#include "microtel/provider.hpp"
#include "microtel/trace.hpp"

#include "fakes/fake_diagnostics_sink.hpp"
#include "fakes/fake_log_record_processor.hpp"
#include "mocks/mock_log_record_processor.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <variant>

namespace mt = microtel;
namespace mts = microtel::sdk;
namespace mti = microtel::internal;

namespace
{

mti::InstrumentationScope MakeScope()
{
    return mti::InstrumentationScope{.name = "test.logger", .version = "1.0"};
}

mt::KeyValue Kv(std::string key)
{
    return mt::KeyValue{.key = std::move(key), .value = mt::AttributeValue{std::int64_t{1}}};
}

std::vector<mt::KeyValue> NAttributes(std::size_t n)
{
    std::vector<mt::KeyValue> attrs;
    attrs.reserve(n);
    for (std::size_t i = 0; i < n; ++i)
    {
        attrs.push_back(Kv("k" + std::to_string(i)));
    }
    return attrs;
}

class FakeSpanSource : public mti::ICurrentSpanSource
{
public:
    void SetSpan(mt::SpanContext ctx) noexcept
    {
        m_span = ctx;
    }
    [[nodiscard]] mt::SpanContext GetCurrentSpan() const override
    {
        return m_span;
    }

private:
    mt::SpanContext m_span;
};

mt::SpanContext MakeSampledContext()
{
    mt::TraceId::Bytes tb{};
    tb[0] = 0x0A;
    mt::SpanId::Bytes sb{};
    sb[0] = 0x0B;
    return mt::SpanContext{
        .trace_id = mt::TraceId{tb},
        .span_id = mt::SpanId{sb},
        .trace_flags = mt::TraceFlags{mt::TraceFlags::kSampled},
        .trace_state = {},
        .remote = false,
    };
}

std::uint64_t AttrLimitDrops(const mt::testing::FakeDiagnosticsSink& sink)
{
    return sink.drop_counters[static_cast<std::size_t>(mt::DropReason::LogAttributeLimit)];
}

TEST(SdkLoggerTest, EmitRoutesRecordTaggedWithScope)
{
    mt::testing::FakeLogRecordProcessor proc;
    mts::SdkLogger logger{&proc, MakeScope(), nullptr, nullptr, {}};

    mt::LogRecord rec;
    rec.body = mt::AttributeValue{std::string{"hello"}};
    logger.Emit(std::move(rec));

    ASSERT_EQ(proc.emitted.size(), 1U);
    EXPECT_EQ(std::get<std::string>(proc.emitted[0].record.body), "hello");
    EXPECT_EQ(proc.emitted[0].scope.name, "test.logger");
    EXPECT_EQ(proc.emitted[0].scope.version, "1.0");
}

TEST(SdkLoggerTest, EmitInvokesProcessorOncePerCall)
{
    mt::testing::MockLogRecordProcessor mock;
    mts::SdkLogger logger{&mock, MakeScope(), nullptr, nullptr, {}};

    logger.Emit(mt::LogRecord{});
    logger.Emit(mt::LogRecord{});

    EXPECT_EQ(mock.on_emit_call_count, 2);
}

TEST(SdkLoggerTest, StampsObservedTimeWhenUnset)
{
    mt::testing::FakeLogRecordProcessor proc;
    mts::SdkLogger logger{&proc, MakeScope(), nullptr, nullptr, {}};

    logger.Emit(mt::LogRecord{});  // observed_time defaults to the epoch

    ASSERT_EQ(proc.emitted.size(), 1U);
    EXPECT_GT(proc.emitted[0].record.observed_time.time_since_epoch().count(), 0);
}

TEST(SdkLoggerTest, PreservesCallerObservedTime)
{
    mt::testing::FakeLogRecordProcessor proc;
    mts::SdkLogger logger{&proc, MakeScope(), nullptr, nullptr, {}};

    const auto ts = std::chrono::system_clock::now() - std::chrono::hours(1);
    mt::LogRecord rec;
    rec.observed_time = ts;
    logger.Emit(std::move(rec));

    ASSERT_EQ(proc.emitted.size(), 1U);
    EXPECT_EQ(proc.emitted[0].record.observed_time, ts);
}

TEST(SdkLoggerTest, FillsTraceContextFromActiveSpan)
{
    FakeSpanSource source;
    source.SetSpan(MakeSampledContext());
    mt::testing::FakeLogRecordProcessor proc;
    mts::SdkLogger logger{&proc, MakeScope(), &source, nullptr, {}};

    logger.Emit(mt::LogRecord{});  // trace_id left invalid

    ASSERT_EQ(proc.emitted.size(), 1U);
    const auto& out = proc.emitted[0].record;
    EXPECT_TRUE(out.trace_id.IsValid());
    EXPECT_EQ(out.trace_id.AsBytes(), MakeSampledContext().trace_id.AsBytes());
    EXPECT_EQ(out.span_id.AsBytes(), MakeSampledContext().span_id.AsBytes());
    EXPECT_TRUE(out.trace_flags.IsSampled());
}

TEST(SdkLoggerTest, DoesNotOverwriteExplicitTraceId)
{
    FakeSpanSource source;
    source.SetSpan(MakeSampledContext());
    mt::testing::FakeLogRecordProcessor proc;
    mts::SdkLogger logger{&proc, MakeScope(), &source, nullptr, {}};

    mt::TraceId::Bytes explicit_bytes{};
    explicit_bytes[0] = 0x99;
    mt::LogRecord rec;
    rec.trace_id = mt::TraceId{explicit_bytes};
    logger.Emit(std::move(rec));

    ASSERT_EQ(proc.emitted.size(), 1U);
    EXPECT_EQ(proc.emitted[0].record.trace_id.AsBytes(), explicit_bytes);
}

TEST(SdkLoggerTest, NoCorrelationWhenSourceNull)
{
    mt::testing::FakeLogRecordProcessor proc;
    mts::SdkLogger logger{&proc, MakeScope(), nullptr, nullptr, {}};

    logger.Emit(mt::LogRecord{});

    ASSERT_EQ(proc.emitted.size(), 1U);
    EXPECT_FALSE(proc.emitted[0].record.trace_id.IsValid());
}

TEST(SdkLoggerTest, NoCorrelationWhenNoActiveSpan)
{
    FakeSpanSource source;  // default-constructed → invalid context
    mt::testing::FakeLogRecordProcessor proc;
    mts::SdkLogger logger{&proc, MakeScope(), &source, nullptr, {}};

    logger.Emit(mt::LogRecord{});

    ASSERT_EQ(proc.emitted.size(), 1U);
    EXPECT_FALSE(proc.emitted[0].record.trace_id.IsValid());
}

TEST(SdkLoggerTest, EnforcesAttributeLimitAndCountsDrop)
{
    mt::testing::FakeLogRecordProcessor proc;
    mt::testing::FakeDiagnosticsSink sink;
    const mts::LogLimitOptions limits{.max_attributes = 2};
    mts::SdkLogger logger{&proc, MakeScope(), nullptr, &sink, limits};

    mt::LogRecord rec;
    rec.attributes = NAttributes(5);
    logger.Emit(std::move(rec));

    ASSERT_EQ(proc.emitted.size(), 1U);
    const auto& out = proc.emitted[0].record;
    EXPECT_EQ(out.attributes.size(), 2U);
    EXPECT_EQ(out.dropped_attributes_count, 3U);
    EXPECT_EQ(AttrLimitDrops(sink), 3U);
}

TEST(SdkLoggerTest, UnderAttributeLimitNoDrop)
{
    mt::testing::FakeLogRecordProcessor proc;
    mt::testing::FakeDiagnosticsSink sink;
    const mts::LogLimitOptions limits{.max_attributes = 8};
    mts::SdkLogger logger{&proc, MakeScope(), nullptr, &sink, limits};

    mt::LogRecord rec;
    rec.attributes = NAttributes(3);
    logger.Emit(std::move(rec));

    ASSERT_EQ(proc.emitted.size(), 1U);
    EXPECT_EQ(proc.emitted[0].record.attributes.size(), 3U);
    EXPECT_EQ(proc.emitted[0].record.dropped_attributes_count, 0U);
    EXPECT_EQ(AttrLimitDrops(sink), 0U);
}

}  // namespace
