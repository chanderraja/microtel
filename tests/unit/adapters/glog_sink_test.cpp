// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for the glog bridge sink — issue #304 (docs/logs-design.md §10).
// Severity mapping, message / location / timestamp conversion, trace
// correlation through the current context, post-shutdown drop accounting, and
// the sink's register-on-construct / unregister-on-destruct lifecycle.

#include "microtel/adapters/glog_sink.hpp"

#include "microtel/adapters/code_attributes.hpp"
#include "microtel/context.hpp"
#include "microtel/log_record.hpp"
#include "microtel/logger.hpp"
#include "microtel/status.hpp"

#include "fakes/fake_logger.hpp"
#include "helpers/log_bridge_harness.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <variant>

#include <glog/logging.h>

namespace mt = microtel;
namespace mta = microtel::adapters;
namespace mtk = microtel::testing;

using namespace std::chrono_literals;

namespace
{

constexpr int kGlogInfo = 0;
constexpr int kGlogWarning = 1;
constexpr int kGlogError = 2;
constexpr int kGlogFatal = 3;
constexpr std::uint8_t kSpanSeed = 0x5A;
constexpr int kVerbosity = 2;
constexpr int kFakeLine = 42;

std::string BodyOf(const mt::LogRecord& rec)
{
    return std::get<std::string>(rec.body);
}

struct SeverityRow
{
    int glog_severity;
    mt::SeverityNumber expected;
};

class GlogSeverityMapping : public ::testing::TestWithParam<SeverityRow>
{
};

TEST_P(GlogSeverityMapping, MapsToTheOtelBaseSeverity)
{
    const auto row = GetParam();
    EXPECT_EQ(mta::FromGlogSeverity(static_cast<mta::GlogSeverity>(row.glog_severity)),
              row.expected);
}

INSTANTIATE_TEST_SUITE_P(
    AllGlogSeverities,
    GlogSeverityMapping,
    ::testing::Values(
        SeverityRow{.glog_severity = kGlogInfo, .expected = mt::SeverityNumber::Info},
        SeverityRow{.glog_severity = kGlogWarning, .expected = mt::SeverityNumber::Warn},
        SeverityRow{.glog_severity = kGlogError, .expected = mt::SeverityNumber::Error},
        SeverityRow{.glog_severity = kGlogFatal, .expected = mt::SeverityNumber::Fatal}));

TEST(GlogSinkTest, ForwardsMessageSeverityAndText)
{
    auto fake = std::make_shared<mtk::FakeLogger>();
    const mta::GlogSink sink{fake};

    LOG(WARNING) << "disk " << 93 << "% full";

    ASSERT_EQ(fake->emitted.size(), 1U);
    const auto& rec = fake->emitted[0];
    EXPECT_EQ(BodyOf(rec), "disk 93% full");
    EXPECT_EQ(rec.severity_number, mt::SeverityNumber::Warn);
    EXPECT_EQ(rec.severity_text, "WARNING");
}

TEST(GlogSinkTest, RecordsCodeLocationUnderSemconvNames)
{
    auto fake = std::make_shared<mtk::FakeLogger>();
    const mta::GlogSink sink{fake};

    const int line = __LINE__ + 1;
    LOG(INFO) << "located";

    ASSERT_EQ(fake->emitted.size(), 1U);
    const auto& rec = fake->emitted[0];
    EXPECT_EQ(mtk::StringAttribute(rec, mta::kCodeFilePath), __FILE__);
    EXPECT_EQ(mtk::IntAttribute(rec, mta::kCodeLineNumber), line);
}

TEST(GlogSinkTest, StampsTheGlogEventTime)
{
    auto fake = std::make_shared<mtk::FakeLogger>();
    const mta::GlogSink sink{fake};

    // glog keeps microseconds; compare at that resolution.
    const auto before =
        std::chrono::floor<std::chrono::microseconds>(std::chrono::system_clock::now());
    LOG(INFO) << "timed";
    const auto after = std::chrono::system_clock::now();

    ASSERT_EQ(fake->emitted.size(), 1U);
    EXPECT_GE(fake->emitted[0].time, before);
    EXPECT_LE(fake->emitted[0].time, after);
}

TEST(GlogSinkTest, VlogArrivesAsInfo)
{
    auto fake = std::make_shared<mtk::FakeLogger>();
    const mta::GlogSink sink{fake};
    const int saved = FLAGS_v;
    FLAGS_v = kVerbosity;

    VLOG(kVerbosity) << "verbose";

    FLAGS_v = saved;
    ASSERT_EQ(fake->emitted.size(), 1U);
    EXPECT_EQ(fake->emitted[0].severity_number, mt::SeverityNumber::Info);
}

// FATAL aborts the process once the sinks have run, so it is driven through
// send() directly rather than through LOG(FATAL).
TEST(GlogSinkTest, ForwardsFatalWhenCalledDirectly)
{
    auto fake = std::make_shared<mtk::FakeLogger>();
    mta::GlogSink sink{fake};
    const std::string_view msg = "unrecoverable";

    sink.send(static_cast<mta::GlogSeverity>(kGlogFatal),
              "/src/app.cc",
              "app.cc",
              kFakeLine,
              google::LogMessageTime{},
              msg.data(),
              msg.size());

    ASSERT_EQ(fake->emitted.size(), 1U);
    EXPECT_EQ(fake->emitted[0].severity_number, mt::SeverityNumber::Fatal);
    EXPECT_EQ(fake->emitted[0].severity_text, "FATAL");
    EXPECT_EQ(BodyOf(fake->emitted[0]), "unrecoverable");
}

TEST(GlogSinkTest, NullLoggerDropsSilently)
{
    const mta::GlogSink sink{nullptr};
    LOG(INFO) << "nowhere";
    SUCCEED();
}

TEST(GlogSinkTest, UnregistersOnDestruction)
{
    auto fake = std::make_shared<mtk::FakeLogger>();
    {
        const mta::GlogSink sink{fake};
        LOG(INFO) << "while registered";
    }
    LOG(INFO) << "after destruction";

    ASSERT_EQ(fake->emitted.size(), 1U);
    EXPECT_EQ(BodyOf(fake->emitted[0]), "while registered");
}

TEST(GlogSinkTest, CorrelatesWithTheActiveSpan)
{
    const mtk::LogBridgeHarness h;
    const mt::SpanContext span = mtk::MakeSampledSpanContext(kSpanSeed);
    {
        const mta::GlogSink sink{h.provider->GetLogger("glog")};
        const mt::ScopedContext scope{mt::Context{span}};
        LOG(INFO) << "inside a span";
    }
    ASSERT_EQ(h.provider->ForceFlush(2000ms), mt::Status::Completed);

    const auto records = h.Exported();
    ASSERT_EQ(records.size(), 1U);
    EXPECT_EQ(records[0].trace_id.AsBytes(), span.trace_id.AsBytes());
    EXPECT_EQ(records[0].span_id.AsBytes(), span.span_id.AsBytes());
    EXPECT_TRUE(records[0].trace_flags.IsSampled());
}

TEST(GlogSinkTest, RecordAfterProviderShutdownIsDroppedAndCounted)
{
    const mtk::LogBridgeHarness h;
    const mta::GlogSink sink{h.provider->GetLogger("glog")};
    ASSERT_EQ(h.provider->Shutdown(2000ms), mt::Status::Completed);
    const auto drops_before = h.PostShutdownDrops();

    LOG(INFO) << "too late";

    EXPECT_EQ(h.PostShutdownDrops(), drops_before + 1);
    EXPECT_TRUE(h.Exported().empty());
}

}  // namespace
