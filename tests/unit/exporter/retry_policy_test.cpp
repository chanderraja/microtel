// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for ComputeBackoff (M5-A).

#include "exporter/retry_policy.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <optional>

namespace mte = microtel::exporter;

using Ms = std::chrono::milliseconds;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Convenience: zero-jitter config for deterministic tests.
static mte::RetryPolicyConfig NoJitterConfig(Ms initial, Ms max_backoff, double multiplier = 1.5)
{
    return mte::RetryPolicyConfig{
        .initial_backoff = initial,
        .max_backoff = max_backoff,
        .backoff_multiplier = multiplier,
        .jitter_fraction = 0.0,
    };
}

// ---------------------------------------------------------------------------
// Exponential growth
// ---------------------------------------------------------------------------

TEST(ComputeBackoffTest, AttemptZero_ReturnsInitialBackoff)
{
    const auto cfg = NoJitterConfig(Ms{1000}, Ms{32000}, 1.5);
    EXPECT_EQ(ComputeBackoff(0, cfg, std::nullopt, 0.5), Ms{1000});
}

TEST(ComputeBackoffTest, AttemptOne_AppliesMultiplier)
{
    const auto cfg = NoJitterConfig(Ms{1000}, Ms{32000}, 2.0);
    EXPECT_EQ(ComputeBackoff(1, cfg, std::nullopt, 0.5), Ms{2000});
}

TEST(ComputeBackoffTest, AttemptTwo_MultiplierAppliedTwice)
{
    const auto cfg = NoJitterConfig(Ms{1000}, Ms{32000}, 2.0);
    EXPECT_EQ(ComputeBackoff(2, cfg, std::nullopt, 0.5), Ms{4000});
}

// ---------------------------------------------------------------------------
// Max-backoff clamping
// ---------------------------------------------------------------------------

TEST(ComputeBackoffTest, ExponentialGrowthClampsToMaxBackoff)
{
    const auto cfg = NoJitterConfig(Ms{1000}, Ms{5000}, 2.0);
    // 1000 * 2^5 = 32000 → clamped to 5000
    EXPECT_EQ(ComputeBackoff(5, cfg, std::nullopt, 0.5), Ms{5000});
}

TEST(ComputeBackoffTest, ZeroInitialBackoff_AlwaysZero)
{
    const auto cfg = NoJitterConfig(Ms{0}, Ms{32000}, 1.5);
    EXPECT_EQ(ComputeBackoff(3, cfg, std::nullopt, 0.5), Ms{0});
}

// ---------------------------------------------------------------------------
// Jitter
// ---------------------------------------------------------------------------

TEST(ComputeBackoffTest, JitterZero_ReducesBackoffByFraction)
{
    // jitter_01=0: jitter = base * fraction * (2*0 - 1) = -fraction*base
    // 50% fraction → result = base * (1 - 0.5) = 500ms
    mte::RetryPolicyConfig cfg = NoJitterConfig(Ms{1000}, Ms{32000}, 1.0);
    cfg.jitter_fraction = 0.5;
    EXPECT_EQ(ComputeBackoff(0, cfg, std::nullopt, 0.0), Ms{500});
}

TEST(ComputeBackoffTest, JitterOne_IncreasesBackoffByFraction)
{
    // jitter_01=1: jitter = base * fraction * (2*1 - 1) = +fraction*base
    // 50% fraction → result = base * (1 + 0.5) = 1500ms
    mte::RetryPolicyConfig cfg = NoJitterConfig(Ms{1000}, Ms{32000}, 1.0);
    cfg.jitter_fraction = 0.5;
    EXPECT_EQ(ComputeBackoff(0, cfg, std::nullopt, 1.0), Ms{1500});
}

TEST(ComputeBackoffTest, JitterHalf_ZeroNetEffect)
{
    mte::RetryPolicyConfig cfg = NoJitterConfig(Ms{1000}, Ms{32000}, 1.0);
    cfg.jitter_fraction = 0.5;
    // jitter_01=0.5: jitter = base * 0.5 * 0 = 0 → 1000ms
    EXPECT_EQ(ComputeBackoff(0, cfg, std::nullopt, 0.5), Ms{1000});
}

TEST(ComputeBackoffTest, ExtremeNegativeJitter_ClampsToZero)
{
    mte::RetryPolicyConfig cfg = NoJitterConfig(Ms{100}, Ms{100}, 1.0);
    cfg.jitter_fraction = 2.0;  // ±200%: minimum = 100 - 200 = -100ms → 0ms
    EXPECT_EQ(ComputeBackoff(0, cfg, std::nullopt, 0.0), Ms{0});
}

// ---------------------------------------------------------------------------
// Retry-After override
// ---------------------------------------------------------------------------

TEST(ComputeBackoffTest, RetryAfterLargerThanBackoff_Overrides)
{
    const auto cfg = NoJitterConfig(Ms{1000}, Ms{32000}, 1.0);
    EXPECT_EQ(ComputeBackoff(0, cfg, Ms{5000}, 0.5), Ms{5000});
}

TEST(ComputeBackoffTest, RetryAfterSmallerThanBackoff_Ignored)
{
    const auto cfg = NoJitterConfig(Ms{1000}, Ms{32000}, 1.0);
    EXPECT_EQ(ComputeBackoff(0, cfg, Ms{100}, 0.5), Ms{1000});
}

TEST(ComputeBackoffTest, RetryAfterNullopt_DoesNotAffectResult)
{
    const auto cfg = NoJitterConfig(Ms{1000}, Ms{32000}, 1.0);
    EXPECT_EQ(ComputeBackoff(0, cfg, std::nullopt, 0.5), Ms{1000});
}
