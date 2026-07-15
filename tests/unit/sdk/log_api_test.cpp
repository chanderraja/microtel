// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// M14-L2: unit tests for the public OTel Logs API surface.
// Covers: SeverityNumber enum values, LogRecord construction, Logger
// interface, and LogBatchHandle (internal).

#include "microtel/internal/log_batch.hpp"
#include "microtel/log_record.hpp"
#include "microtel/logger.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <string>

namespace mt = microtel;
namespace mti = microtel::internal;

// ---------------------------------------------------------------------------
// SeverityNumber — verify OTel-spec numeric values
// ---------------------------------------------------------------------------

TEST(SeverityNumberTest, UnspecifiedIsZero)
{
    EXPECT_EQ(static_cast<int>(mt::SeverityNumber::Unspecified), 0);
}

TEST(SeverityNumberTest, TraceIsOne)
{
    EXPECT_EQ(static_cast<int>(mt::SeverityNumber::Trace), 1);
}

TEST(SeverityNumberTest, Trace4IsFour)
{
    EXPECT_EQ(static_cast<int>(mt::SeverityNumber::Trace4), 4);
}

TEST(SeverityNumberTest, DebugIsFive)
{
    EXPECT_EQ(static_cast<int>(mt::SeverityNumber::Debug), 5);
}

TEST(SeverityNumberTest, Debug4IsEight)
{
    EXPECT_EQ(static_cast<int>(mt::SeverityNumber::Debug4), 8);
}

TEST(SeverityNumberTest, InfoIsNine)
{
    EXPECT_EQ(static_cast<int>(mt::SeverityNumber::Info), 9);
}

TEST(SeverityNumberTest, Info4IsTwelve)
{
    EXPECT_EQ(static_cast<int>(mt::SeverityNumber::Info4), 12);
}

TEST(SeverityNumberTest, WarnIsThirteen)
{
    EXPECT_EQ(static_cast<int>(mt::SeverityNumber::Warn), 13);
}

TEST(SeverityNumberTest, Warn4IsSixteen)
{
    EXPECT_EQ(static_cast<int>(mt::SeverityNumber::Warn4), 16);
}

TEST(SeverityNumberTest, ErrorIsSeventeen)
{
    EXPECT_EQ(static_cast<int>(mt::SeverityNumber::Error), 17);
}

TEST(SeverityNumberTest, Error4IsTwenty)
{
    EXPECT_EQ(static_cast<int>(mt::SeverityNumber::Error4), 20);
}

TEST(SeverityNumberTest, FatalIsTwentyOne)
{
    EXPECT_EQ(static_cast<int>(mt::SeverityNumber::Fatal), 21);
}

TEST(SeverityNumberTest, Fatal4IsTwentyFour)
{
    EXPECT_EQ(static_cast<int>(mt::SeverityNumber::Fatal4), 24);
}

// ---------------------------------------------------------------------------
// LogRecord — construction and field coverage
// ---------------------------------------------------------------------------

TEST(LogRecordTest, DefaultConstruct)
{
    const mt::LogRecord rec;
    EXPECT_EQ(rec.severity_number, mt::SeverityNumber::Unspecified);
    EXPECT_TRUE(rec.severity_text.empty());
    EXPECT_TRUE(rec.attributes.empty());
    EXPECT_TRUE(rec.event_name.empty());
    EXPECT_FALSE(rec.trace_id.IsValid());
    EXPECT_FALSE(rec.span_id.IsValid());
}

TEST(LogRecordTest, DesignatedInitSeverityAndBody)
{
    const mt::LogRecord rec = {
        .severity_number = mt::SeverityNumber::Info,
        .severity_text = "INFO",
        .body = std::string{"hello world"},
    };
    EXPECT_EQ(rec.severity_number, mt::SeverityNumber::Info);
    EXPECT_EQ(rec.severity_text, "INFO");
    ASSERT_TRUE(std::holds_alternative<std::string>(rec.body));
    EXPECT_EQ(std::get<std::string>(rec.body), "hello world");
}

TEST(LogRecordTest, AttributesCanBeSet)
{
    mt::LogRecord rec = {
        .attributes = {mt::KeyValue{.key = "http.status_code", .value = std::int64_t{200}}},
    };
    ASSERT_EQ(rec.attributes.size(), 1u);
    EXPECT_EQ(rec.attributes[0].key, "http.status_code");
    EXPECT_EQ(std::get<std::int64_t>(rec.attributes[0].value), 200);
}

TEST(LogRecordTest, TraceCorrelationFields)
{
    const mt::TraceId::Bytes tid_bytes = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    const mt::SpanId::Bytes sid_bytes = {0xA, 0xB, 0xC, 0xD, 0xE, 0xF, 0x01, 0x02};

    const mt::LogRecord rec = {
        .trace_id = mt::TraceId{tid_bytes},
        .span_id = mt::SpanId{sid_bytes},
        .trace_flags = mt::TraceFlags{mt::TraceFlags::kSampled},
    };
    EXPECT_TRUE(rec.trace_id.IsValid());
    EXPECT_TRUE(rec.span_id.IsValid());
    EXPECT_TRUE(rec.trace_flags.IsSampled());
}

TEST(LogRecordTest, EventNameField)
{
    const mt::LogRecord rec = {
        .event_name = "exception",
    };
    EXPECT_EQ(rec.event_name, "exception");
}

// ---------------------------------------------------------------------------
// Logger — interface contract via a no-op stub
// ---------------------------------------------------------------------------

class NoOpLogger final : public mt::Logger
{
public:
    void Emit(mt::LogRecord /*record*/) noexcept override {}
};

TEST(LoggerTest, NoOpEmitCompiles)
{
    NoOpLogger logger;
    const mt::LogRecord rec = {
        .severity_number = mt::SeverityNumber::Warn,
        .body = std::string{"something happened"},
    };
    EXPECT_NO_FATAL_FAILURE(logger.Emit(rec));
}

TEST(LoggerTest, NoOpEmitIsNoexcept)
{
    NoOpLogger logger;
    mt::LogRecord rec;
    EXPECT_TRUE(noexcept(logger.Emit(std::move(rec))));
}

// ---------------------------------------------------------------------------
// LogBatchHandle — internal batch container used by L3 encoder
// ---------------------------------------------------------------------------

TEST(LogBatchHandleTest, DefaultConstructIsEmpty)
{
    const mti::LogBatchHandle batch;
    EXPECT_TRUE(batch.Records().empty());
}

TEST(LogBatchHandleTest, ConstructWithRecords)
{
    std::vector<mt::LogRecord> recs;
    recs.push_back(
        mt::LogRecord{.severity_number = mt::SeverityNumber::Info, .body = std::string{"first"}});
    recs.push_back(
        mt::LogRecord{.severity_number = mt::SeverityNumber::Error, .body = std::string{"second"}});

    auto resource = std::make_shared<mt::Resource>();
    const mti::InstrumentationScope scope = {.name = "test.lib", .version = "1.0"};
    mti::LogBatchHandle batch{std::move(recs), std::move(resource), scope};

    ASSERT_EQ(batch.Records().size(), 2u);
    EXPECT_EQ(batch.Scope().name, "test.lib");
    EXPECT_EQ(batch.Scope().version, "1.0");
}

TEST(LogBatchHandleTest, ResourceRefIsAccessible)
{
    auto resource = std::make_shared<mt::Resource>();
    mti::LogBatchHandle batch{{}, resource, {}};
    EXPECT_EQ(&batch.ResourceRef(), resource.get());
}

TEST(LogBatchHandleTest, MoveConstruct)
{
    std::vector<mt::LogRecord> recs;
    recs.push_back(mt::LogRecord{.severity_number = mt::SeverityNumber::Debug});
    auto resource = std::make_shared<mt::Resource>();

    mti::LogBatchHandle a{std::move(recs), resource, {.name = "lib"}};
    mti::LogBatchHandle b{std::move(a)};

    EXPECT_EQ(b.Records().size(), 1u);
    EXPECT_EQ(b.Scope().name, "lib");
    EXPECT_TRUE(a.Records().empty());  // NOLINT(bugprone-use-after-move)
}
