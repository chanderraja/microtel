// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for RetryEngine's backoff schedule (issue #311): the first retry
// backs off like every later one, using the fan-out result's `retry_after`
// and `initial_backoff`.

#include "exporter/retry_engine.hpp"

#include "microtel/internal/wire_result.hpp"
#include "microtel/provider.hpp"

#include "exporter/retry_policy.hpp"
#include "fakes/fake_diagnostics_sink.hpp"
#include "fakes/fake_steady_clock.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <thread>

namespace mt = microtel;
namespace mti = microtel::internal;
namespace mtmk = microtel::testing;
namespace mte = microtel::exporter;

namespace
{

constexpr auto kInitialBackoff = std::chrono::milliseconds{40};
constexpr double kJitterFraction = 0.2;
constexpr double kDoublingMultiplier = 2.0;
constexpr auto kFanOutRetryAfter = std::chrono::milliseconds{60};
constexpr auto kLongBackoff = std::chrono::seconds{10};
constexpr auto kNoSleepBound = std::chrono::milliseconds{400};
constexpr auto kAbortDelay = std::chrono::milliseconds{50};
constexpr auto kAbortReturnBound = std::chrono::seconds{3};
constexpr std::string_view kStage = "test-stage";

mte::RetryPolicyConfig Policy(std::uint32_t max_attempts,
                              std::chrono::milliseconds initial_backoff,
                              std::chrono::milliseconds retry_budget)
{
    return mte::RetryPolicyConfig{
        .max_attempts = max_attempts,
        .initial_backoff = initial_backoff,
        .max_backoff = kLongBackoff,
        .backoff_multiplier = 1.0,
        .jitter_fraction = 0.0,
        .retry_budget = retry_budget,
    };
}

mti::WireResult Retryable(std::optional<std::chrono::milliseconds> retry_after = std::nullopt)
{
    return mti::WireResult{.success = false, .retryable = true, .retry_after = retry_after};
}

std::uint64_t DropCount(const mtmk::FakeDiagnosticsSink& sink, mt::DropReason reason)
{
    return sink.drop_counters.at(static_cast<std::size_t>(reason));
}

// Time from `Settle` to the first retry, and how many retries were made.
struct FirstRetryTiming
{
    std::chrono::steady_clock::duration until_first_retry{};
    int retries = 0;
};

FirstRetryTiming SettleAndTimeFirstRetry(mte::RetryEngine& engine, const mti::WireResult& fan_out)
{
    FirstRetryTiming timing;
    const auto started = std::chrono::steady_clock::now();
    engine.Settle(
        fan_out,
        [&]
        {
            if (timing.retries++ == 0)
            {
                timing.until_first_retry = std::chrono::steady_clock::now() - started;
            }
            return mti::WireResult{.success = true};
        },
        kStage);
    return timing;
}

}  // namespace

TEST(RetryEngineTest, FirstRetry_WaitsInitialBackoffWithinJitter)
{
    auto policy = Policy(2, kInitialBackoff, std::chrono::minutes(5));
    policy.jitter_fraction = kJitterFraction;
    mte::RetryEngine engine{policy, nullptr, nullptr};

    const auto timing = SettleAndTimeFirstRetry(engine, Retryable());

    ASSERT_EQ(timing.retries, 1);
    const auto lower_bound = std::chrono::duration_cast<std::chrono::milliseconds>(
        kInitialBackoff * (1.0 - kJitterFraction));
    EXPECT_GE(timing.until_first_retry, lower_bound);
}

// The first backoff is step 0 of the schedule (`initial_backoff`), not step 1
// (`initial_backoff * multiplier`). The clock never advances, so the budget
// look-ahead sees each backoff alone: a budget that fits step 0 but not step 1
// lets exactly one retry through.
TEST(RetryEngineTest, FirstRetry_UsesInitialBackoffNotTheSecondStep)
{
    mtmk::FakeSteadyClock clock;
    auto policy = Policy(3, kInitialBackoff, kInitialBackoff + std::chrono::milliseconds{1});
    policy.backoff_multiplier = kDoublingMultiplier;
    mte::RetryEngine engine{policy, nullptr, &clock};

    int retries = 0;
    engine.Settle(
        Retryable(),
        [&]
        {
            ++retries;
            return Retryable();
        },
        kStage);

    EXPECT_EQ(retries, 1) << "step 0 fits the budget; step 1 (2x) does not";
}

TEST(RetryEngineTest, FirstRetry_HonoursFanOutRetryAfter)
{
    mte::RetryEngine engine{
        Policy(2, std::chrono::milliseconds{0}, std::chrono::minutes(5)), nullptr, nullptr};

    const auto timing = SettleAndTimeFirstRetry(engine, Retryable(kFanOutRetryAfter));

    ASSERT_EQ(timing.retries, 1);
    EXPECT_GE(timing.until_first_retry, kFanOutRetryAfter);
}

TEST(RetryEngineTest, FirstBackoffPastBudget_NoRetryAndFanOutResultStands)
{
    mtmk::FakeSteadyClock clock;
    mtmk::FakeDiagnosticsSink sink;
    mte::RetryEngine engine{
        Policy(5, std::chrono::seconds{1}, std::chrono::milliseconds{10}), &sink, &clock};

    const auto started = std::chrono::steady_clock::now();
    const auto timing = SettleAndTimeFirstRetry(engine, Retryable());
    const auto elapsed = std::chrono::steady_clock::now() - started;

    EXPECT_EQ(timing.retries, 0);
    EXPECT_LT(elapsed, kNoSleepBound) << "exits before the sleep, not after it";
    EXPECT_EQ(DropCount(sink, mt::DropReason::RetryBudgetExhausted), 1U);
    EXPECT_EQ(sink.batches_failed, 1U);
}

TEST(RetryEngineTest, FanOutRetryAfterPastBudget_NoRetry)
{
    mtmk::FakeSteadyClock clock;
    mte::RetryEngine engine{
        Policy(5, std::chrono::milliseconds{0}, std::chrono::seconds{1}), nullptr, &clock};

    const auto timing = SettleAndTimeFirstRetry(engine, Retryable(kLongBackoff));

    EXPECT_EQ(timing.retries, 0);
}

TEST(RetryEngineTest, AbortDuringFirstBackoff_EndsWithoutRetrying)
{
    mtmk::FakeDiagnosticsSink sink;
    mte::RetryEngine engine{Policy(3, kLongBackoff, std::chrono::minutes(5)), &sink, nullptr};

    int retries = 0;
    const auto started = std::chrono::steady_clock::now();
    std::thread worker{[&]
                       {
                           engine.Settle(
                               Retryable(),
                               [&]
                               {
                                   ++retries;
                                   return Retryable();
                               },
                               kStage);
                       }};
    std::this_thread::sleep_for(kAbortDelay);
    engine.Abort();
    worker.join();
    const auto elapsed = std::chrono::steady_clock::now() - started;

    EXPECT_LT(elapsed, kAbortReturnBound);
    EXPECT_EQ(retries, 0) << "no retry after Abort";
    EXPECT_EQ(DropCount(sink, mt::DropReason::RetryBudgetExhausted), 1U);
}

TEST(RetryEngineTest, MaxAttemptsCountsTheFanOut)
{
    mte::RetryEngine engine{
        Policy(3, std::chrono::milliseconds{0}, std::chrono::minutes(5)), nullptr, nullptr};

    int retries = 0;
    engine.Settle(
        Retryable(),
        [&]
        {
            ++retries;
            return Retryable();
        },
        kStage);

    EXPECT_EQ(retries, 2) << "3 attempts = the fan-out + 2 retries";
}
