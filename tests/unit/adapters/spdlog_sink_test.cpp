// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for the spdlog bridge sink — M14 L6 (docs/logs-design.md §10).
// Verifies level → SeverityNumber mapping, payload → body, time stamping, and
// that spdlog's own level filtering is respected before the sink is reached.

#include "microtel/adapters/spdlog_sink.hpp"

#include "microtel/log_record.hpp"
#include "microtel/logger.hpp"

#include "fakes/fake_logger.hpp"

#include <gtest/gtest.h>
#include <spdlog/common.h>
#include <spdlog/logger.h>

#include <memory>
#include <string>
#include <variant>

namespace mt = microtel;
namespace mta = microtel::adapters;
namespace mtk = microtel::testing;

namespace
{

std::shared_ptr<spdlog::logger> MakeSpdlogLogger(const std::shared_ptr<mt::Logger>& target)
{
    auto sink = std::make_shared<mta::SpdlogSinkSt>(target);
    auto logger = std::make_shared<spdlog::logger>("test", sink);
    logger->set_level(spdlog::level::trace);  // let every level reach the sink
    return logger;
}

TEST(SpdlogSinkTest, ToSeverityNumberMapsEachLevel)
{
    EXPECT_EQ(mta::ToSeverityNumber(spdlog::level::trace), mt::SeverityNumber::Trace);
    EXPECT_EQ(mta::ToSeverityNumber(spdlog::level::debug), mt::SeverityNumber::Debug);
    EXPECT_EQ(mta::ToSeverityNumber(spdlog::level::info), mt::SeverityNumber::Info);
    EXPECT_EQ(mta::ToSeverityNumber(spdlog::level::warn), mt::SeverityNumber::Warn);
    EXPECT_EQ(mta::ToSeverityNumber(spdlog::level::err), mt::SeverityNumber::Error);
    EXPECT_EQ(mta::ToSeverityNumber(spdlog::level::critical), mt::SeverityNumber::Fatal);
    EXPECT_EQ(mta::ToSeverityNumber(spdlog::level::off), mt::SeverityNumber::Unspecified);
}

TEST(SpdlogSinkTest, ForwardsMessageAsLogRecord)
{
    auto fake = std::make_shared<mtk::FakeLogger>();
    auto logger = MakeSpdlogLogger(fake);

    logger->info("hello world");

    ASSERT_EQ(fake->emitted.size(), 1U);
    const auto& rec = fake->emitted[0];
    EXPECT_EQ(rec.severity_number, mt::SeverityNumber::Info);
    EXPECT_EQ(std::get<std::string>(rec.body), "hello world");
    EXPECT_EQ(rec.severity_text, "info");
}

TEST(SpdlogSinkTest, ForwardsSeverityPerLevel)
{
    auto fake = std::make_shared<mtk::FakeLogger>();
    auto logger = MakeSpdlogLogger(fake);

    logger->warn("w");
    logger->error("e");
    logger->critical("c");

    ASSERT_EQ(fake->emitted.size(), 3U);
    EXPECT_EQ(fake->emitted[0].severity_number, mt::SeverityNumber::Warn);
    EXPECT_EQ(fake->emitted[1].severity_number, mt::SeverityNumber::Error);
    EXPECT_EQ(fake->emitted[2].severity_number, mt::SeverityNumber::Fatal);
}

TEST(SpdlogSinkTest, StampsEventTime)
{
    auto fake = std::make_shared<mtk::FakeLogger>();
    auto logger = MakeSpdlogLogger(fake);

    logger->info("x");

    ASSERT_EQ(fake->emitted.size(), 1U);
    EXPECT_GT(fake->emitted[0].time.time_since_epoch().count(), 0);
}

TEST(SpdlogSinkTest, RespectsSpdlogLevelFilter)
{
    auto fake = std::make_shared<mtk::FakeLogger>();
    auto sink = std::make_shared<mta::SpdlogSinkSt>(fake);
    auto logger = std::make_shared<spdlog::logger>("filtered", sink);
    logger->set_level(spdlog::level::warn);  // info/debug dropped before the sink

    logger->info("dropped");
    logger->warn("kept");

    ASSERT_EQ(fake->emitted.size(), 1U);
    EXPECT_EQ(std::get<std::string>(fake->emitted[0].body), "kept");
}

}  // namespace
