// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for the log4cxx bridge appender — issue #304
// (docs/logs-design.md §10). Level mapping, message / location / timestamp /
// MDC conversion, trace correlation through the current context,
// post-shutdown drop accounting, and the addAppender / removeAppender / close
// lifecycle.

#include "microtel/adapters/log4cxx_appender.hpp"

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

#include <log4cxx/level.h>
#include <log4cxx/logger.h>
#include <log4cxx/mdc.h>

namespace mt = microtel;
namespace mta = microtel::adapters;
namespace mtk = microtel::testing;

using namespace std::chrono_literals;

// A named namespace on purpose: log4cxx cannot parse a function name out of
// __PRETTY_FUNCTION__ inside an anonymous namespace, so a call site there
// would carry no code.function.name at all.
namespace log4cxx_bridge_probe
{

/// @brief Log one INFO line and return the line number it was logged on.
static int LogFromNamedFunction(const log4cxx::LoggerPtr& logger)
{
    const int line = __LINE__ + 1;
    LOG4CXX_INFO(logger, "located");
    return line;
}

}  // namespace log4cxx_bridge_probe

namespace
{

constexpr std::uint8_t kSpanSeed = 0x4C;
constexpr int kCustomLevelBetweenInfoAndWarn = 25000;

std::string BodyOf(const mt::LogRecord& rec)
{
    return std::get<std::string>(rec.body);
}

/// @brief A log4cxx logger that reaches only the appenders a test attaches.
///
/// Additivity off keeps the root logger's appenders (if a configuration file
/// happens to supply any) out of the test; TRACE lets every level through.
/// Each test uses its own name, and removes what it attached, so tests do not
/// see each other's appenders through log4cxx's process-wide hierarchy.
class AttachedAppender
{
public:
    AttachedAppender(const std::string& logger_name, const std::shared_ptr<mt::Logger>& target)
        : m_logger(log4cxx::Logger::getLogger(logger_name)),
          m_appender(std::make_shared<mta::Log4cxxAppender>(target))
    {
        m_logger->setAdditivity(false);
        m_logger->setLevel(log4cxx::Level::getTrace());
        m_logger->addAppender(m_appender);
    }

    ~AttachedAppender()
    {
        m_logger->removeAppender(m_appender);
    }

    AttachedAppender(const AttachedAppender&) = delete;
    AttachedAppender& operator=(const AttachedAppender&) = delete;
    AttachedAppender(AttachedAppender&&) = delete;
    AttachedAppender& operator=(AttachedAppender&&) = delete;

    [[nodiscard]] const log4cxx::LoggerPtr& Logger() const
    {
        return m_logger;
    }

    [[nodiscard]] const std::shared_ptr<mta::Log4cxxAppender>& Appender() const
    {
        return m_appender;
    }

private:
    log4cxx::LoggerPtr m_logger;
    std::shared_ptr<mta::Log4cxxAppender> m_appender;
};

struct LevelRow
{
    int log4cxx_level;
    mt::SeverityNumber expected;
};

class Log4cxxLevelMapping : public ::testing::TestWithParam<LevelRow>
{
};

TEST_P(Log4cxxLevelMapping, MapsToTheOtelBaseSeverity)
{
    const auto row = GetParam();
    EXPECT_EQ(mta::FromLog4cxxLevel(row.log4cxx_level), row.expected);
}

INSTANTIATE_TEST_SUITE_P(
    AllLog4cxxLevels,
    Log4cxxLevelMapping,
    ::testing::Values(
        LevelRow{.log4cxx_level = log4cxx::Level::ALL_INT, .expected = mt::SeverityNumber::Trace},
        LevelRow{.log4cxx_level = log4cxx::Level::TRACE_INT, .expected = mt::SeverityNumber::Trace},
        LevelRow{.log4cxx_level = log4cxx::Level::DEBUG_INT, .expected = mt::SeverityNumber::Debug},
        LevelRow{.log4cxx_level = log4cxx::Level::INFO_INT, .expected = mt::SeverityNumber::Info},
        LevelRow{.log4cxx_level = kCustomLevelBetweenInfoAndWarn,
                 .expected = mt::SeverityNumber::Info},
        LevelRow{.log4cxx_level = log4cxx::Level::WARN_INT, .expected = mt::SeverityNumber::Warn},
        LevelRow{.log4cxx_level = log4cxx::Level::ERROR_INT, .expected = mt::SeverityNumber::Error},
        LevelRow{.log4cxx_level = log4cxx::Level::FATAL_INT, .expected = mt::SeverityNumber::Fatal},
        LevelRow{.log4cxx_level = log4cxx::Level::OFF_INT,
                 .expected = mt::SeverityNumber::Unspecified}));

TEST(Log4cxxAppenderTest, ForwardsMessageSeverityAndText)
{
    auto fake = std::make_shared<mtk::FakeLogger>();
    const AttachedAppender a{"microtel.test.message", fake};

    LOG4CXX_WARN(a.Logger(), "disk " << 93 << "% full");

    ASSERT_EQ(fake->emitted.size(), 1U);
    const auto& rec = fake->emitted[0];
    EXPECT_EQ(BodyOf(rec), "disk 93% full");
    EXPECT_EQ(rec.severity_number, mt::SeverityNumber::Warn);
    EXPECT_EQ(rec.severity_text, "WARN");
}

TEST(Log4cxxAppenderTest, ForwardsEveryLevel)
{
    auto fake = std::make_shared<mtk::FakeLogger>();
    const AttachedAppender a{"microtel.test.levels", fake};

    LOG4CXX_TRACE(a.Logger(), "t");
    LOG4CXX_DEBUG(a.Logger(), "d");
    LOG4CXX_INFO(a.Logger(), "i");
    LOG4CXX_ERROR(a.Logger(), "e");
    LOG4CXX_FATAL(a.Logger(), "f");

    ASSERT_EQ(fake->emitted.size(), 5U);
    EXPECT_EQ(fake->emitted[0].severity_number, mt::SeverityNumber::Trace);
    EXPECT_EQ(fake->emitted[1].severity_number, mt::SeverityNumber::Debug);
    EXPECT_EQ(fake->emitted[2].severity_number, mt::SeverityNumber::Info);
    EXPECT_EQ(fake->emitted[3].severity_number, mt::SeverityNumber::Error);
    EXPECT_EQ(fake->emitted[4].severity_number, mt::SeverityNumber::Fatal);
}

TEST(Log4cxxAppenderTest, RecordsCodeLocationUnderSemconvNames)
{
    auto fake = std::make_shared<mtk::FakeLogger>();
    const AttachedAppender a{"microtel.test.location", fake};

    const int line = log4cxx_bridge_probe::LogFromNamedFunction(a.Logger());

    ASSERT_EQ(fake->emitted.size(), 1U);
    const auto& rec = fake->emitted[0];
    EXPECT_EQ(mtk::StringAttribute(rec, mta::kCodeFilePath), __FILE__);
    EXPECT_EQ(mtk::IntAttribute(rec, mta::kCodeLineNumber), line);
    EXPECT_EQ(mtk::StringAttribute(rec, mta::kCodeFunctionName),
              "log4cxx_bridge_probe::LogFromNamedFunction");
}

TEST(Log4cxxAppenderTest, OmitsCodeLocationWhenLog4cxxHasNone)
{
    auto fake = std::make_shared<mtk::FakeLogger>();
    const AttachedAppender a{"microtel.test.no_location", fake};

    a.Logger()->forcedLog(log4cxx::Level::getInfo(),
                          "no location",
                          log4cxx::spi::LocationInfo::getLocationUnavailable());

    ASSERT_EQ(fake->emitted.size(), 1U);
    EXPECT_TRUE(fake->emitted[0].attributes.empty());
}

TEST(Log4cxxAppenderTest, StampsTheEventTime)
{
    auto fake = std::make_shared<mtk::FakeLogger>();
    const AttachedAppender a{"microtel.test.time", fake};

    // log4cxx keeps microseconds; compare at that resolution.
    const auto before =
        std::chrono::floor<std::chrono::microseconds>(std::chrono::system_clock::now());
    LOG4CXX_INFO(a.Logger(), "timed");
    const auto after = std::chrono::system_clock::now();

    ASSERT_EQ(fake->emitted.size(), 1U);
    EXPECT_GE(fake->emitted[0].time, before);
    EXPECT_LE(fake->emitted[0].time, after);
}

TEST(Log4cxxAppenderTest, CopiesMdcEntriesAsAttributes)
{
    auto fake = std::make_shared<mtk::FakeLogger>();
    const AttachedAppender a{"microtel.test.mdc", fake};

    {
        const log4cxx::MDC entry{"request.id", "r-17"};
        LOG4CXX_INFO(a.Logger(), "with mdc");
    }

    ASSERT_EQ(fake->emitted.size(), 1U);
    EXPECT_EQ(mtk::StringAttribute(fake->emitted[0], "request.id"), "r-17");
}

TEST(Log4cxxAppenderTest, DoesNotRequireALayout)
{
    const mta::Log4cxxAppender appender{nullptr};
    EXPECT_FALSE(appender.requiresLayout());
}

TEST(Log4cxxAppenderTest, NullLoggerDropsSilently)
{
    const AttachedAppender a{"microtel.test.null", nullptr};
    LOG4CXX_INFO(a.Logger(), "nowhere");
    SUCCEED();
}

TEST(Log4cxxAppenderTest, StopsForwardingOnceRemoved)
{
    auto fake = std::make_shared<mtk::FakeLogger>();
    const auto logger = log4cxx::Logger::getLogger("microtel.test.lifecycle");
    {
        const AttachedAppender a{"microtel.test.lifecycle", fake};
        LOG4CXX_INFO(a.Logger(), "while attached");
    }
    LOG4CXX_INFO(logger, "after removal");

    ASSERT_EQ(fake->emitted.size(), 1U);
    EXPECT_EQ(BodyOf(fake->emitted[0]), "while attached");
}

TEST(Log4cxxAppenderTest, StopsForwardingOnceClosed)
{
    auto fake = std::make_shared<mtk::FakeLogger>();
    const AttachedAppender a{"microtel.test.close", fake};

    LOG4CXX_INFO(a.Logger(), "before close");
    a.Appender()->close();
    LOG4CXX_INFO(a.Logger(), "after close");

    ASSERT_EQ(fake->emitted.size(), 1U);
    EXPECT_EQ(BodyOf(fake->emitted[0]), "before close");
}

TEST(Log4cxxAppenderTest, CorrelatesWithTheActiveSpan)
{
    const mtk::LogBridgeHarness h;
    const mt::SpanContext span = mtk::MakeSampledSpanContext(kSpanSeed);
    {
        const AttachedAppender a{"microtel.test.correlation", h.provider->GetLogger("log4cxx")};
        const mt::ScopedContext scope{mt::Context{span}};
        LOG4CXX_INFO(a.Logger(), "inside a span");
    }
    ASSERT_EQ(h.provider->ForceFlush(2000ms), mt::Status::Completed);

    const auto records = h.Exported();
    ASSERT_EQ(records.size(), 1U);
    EXPECT_EQ(records[0].trace_id.AsBytes(), span.trace_id.AsBytes());
    EXPECT_EQ(records[0].span_id.AsBytes(), span.span_id.AsBytes());
    EXPECT_TRUE(records[0].trace_flags.IsSampled());
}

TEST(Log4cxxAppenderTest, RecordAfterProviderShutdownIsDroppedAndCounted)
{
    const mtk::LogBridgeHarness h;
    const AttachedAppender a{"microtel.test.shutdown", h.provider->GetLogger("log4cxx")};
    ASSERT_EQ(h.provider->Shutdown(2000ms), mt::Status::Completed);
    const auto drops_before = h.PostShutdownDrops();

    LOG4CXX_INFO(a.Logger(), "too late");

    EXPECT_EQ(h.PostShutdownDrops(), drops_before + 1);
    EXPECT_TRUE(h.Exported().empty());
}

}  // namespace
