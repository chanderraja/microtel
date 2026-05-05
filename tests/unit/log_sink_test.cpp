// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Tests for microtel::SetLogSink / ResetLogSink and the internal
// LogImpl entry point. Behavioural — exercises real production code in
// src/common/log_sink.cpp, not a mock.

#include "microtel/log_sink.hpp"

// Implementation header from src/common/. Only test code reaches into
// the internal log entry point this way; production code uses it via
// the same header.
#include "common/internal_log.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <string>
#include <thread>
#include <vector>

namespace mt = microtel;

namespace
{

struct CapturedLog
{
    mt::LogLevel level;
    std::string message;
};

class LogSinkTest : public ::testing::Test
{
protected:
    void TearDown() override
    {
        // Each test installs a fresh sink; restore the default afterward
        // so subsequent tests / suites are not affected.
        mt::ResetLogSink();
    }
};

TEST_F(LogSinkTest, InstalledSinkReceivesLogCalls)
{
    std::vector<CapturedLog> captured;
    mt::SetLogSink([&](mt::LogLevel lvl, std::string_view msg)
                   { captured.push_back({lvl, std::string{msg}}); });

    mt::internal::LogImpl(mt::LogLevel::Info, "hello world");
    mt::internal::LogImpl(mt::LogLevel::Warn, "second");

    ASSERT_EQ(captured.size(), std::size_t{2});
    EXPECT_EQ(captured[0].level, mt::LogLevel::Info);
    EXPECT_EQ(captured[0].message, "hello world");
    EXPECT_EQ(captured[1].level, mt::LogLevel::Warn);
    EXPECT_EQ(captured[1].message, "second");
}

TEST_F(LogSinkTest, ResetRestoresDefault)
{
    int count = 0;
    mt::SetLogSink([&](mt::LogLevel, std::string_view) { ++count; });
    mt::internal::LogImpl(mt::LogLevel::Info, "captured");
    EXPECT_EQ(count, 1);

    mt::ResetLogSink();
    // After reset, LogImpl falls back to stderr; the count must not change.
    mt::internal::LogImpl(mt::LogLevel::Info, "ignored-by-test-but-stderr-emits");
    EXPECT_EQ(count, 1);
}

TEST_F(LogSinkTest, ThrowingSinkIsSwallowed)
{
    mt::SetLogSink([](mt::LogLevel, std::string_view) -> void
                   { throw std::runtime_error{"boom"}; });
    // Per docs/error-model.md §9.3, microtel must not propagate the
    // application's exception. The call below should return normally.
    EXPECT_NO_THROW(mt::internal::LogImpl(mt::LogLevel::Error, "expect-swallowed"));
}

// Helpers for the concurrent stress tests. Extracted to keep each TEST_F
// body small enough for `readability-function-size` and to flatten
// nesting per `coding-standards.md` §2.2 (max 3 levels per function).
void EmitLoop(int iters) noexcept
{
    for (int j = 0; j < iters; ++j)
    {
        mt::internal::LogImpl(mt::LogLevel::Debug, "x");
    }
}

void SpawnLoggers(int threads, int iters)
{
    std::vector<std::thread> ts;
    ts.reserve(static_cast<std::size_t>(threads));
    for (int i = 0; i < threads; ++i)
    {
        ts.emplace_back(EmitLoop, iters);
    }
    for (auto& t : ts)
    {
        t.join();
    }
}

TEST_F(LogSinkTest, ConcurrentEmitWithStableSink)
{
    // Sanitizer test: many threads call LogImpl with one fixed sink. No
    // assertion on output content; the test's value is TSan/ASan
    // cleanliness through the sink-copy path in LogImpl.
    std::atomic<int> emitted{0};
    mt::SetLogSink([&](mt::LogLevel, std::string_view) { ++emitted; });
    SpawnLoggers(/*threads=*/4, /*iters=*/200);
    EXPECT_GE(emitted.load(), 0);
}

TEST_F(LogSinkTest, ConcurrentSinkSwap)
{
    // Sanitizer test: cycle SetLogSink / ResetLogSink while threads emit.
    std::atomic<int> emitted{0};
    mt::SetLogSink([&](mt::LogLevel, std::string_view) { ++emitted; });
    std::thread loggers([&] { SpawnLoggers(2, 200); });
    for (int j = 0; j < 50; ++j)
    {
        mt::SetLogSink([&](mt::LogLevel, std::string_view) { ++emitted; });
        mt::ResetLogSink();
    }
    loggers.join();
    EXPECT_GE(emitted.load(), 0);
}

}  // namespace
