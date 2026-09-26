// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// The leaf receiver's time arithmetic (docs/leaf-concentrator-design.md §5):
// saturation, the three corrections, and the boot-relative anchor on its own.
// The receiver-level tests in leaf_receiver_test.cpp drive the same code
// through Ingest.

#include "sdk/leaf_time.hpp"

#include "microtel/internal/batch.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <limits>
#include <optional>

namespace mti = microtel::internal;
namespace mts = microtel::sdk;

namespace
{

constexpr std::int64_t kMax = std::numeric_limits<std::int64_t>::max();
constexpr std::int64_t kMin = std::numeric_limits<std::int64_t>::min();

std::chrono::system_clock::time_point Ns(std::int64_t ns)
{
    return std::chrono::system_clock::time_point{std::chrono::nanoseconds{ns}};
}

std::int64_t NsOf(std::chrono::system_clock::time_point tp)
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(tp.time_since_epoch()).count();
}

mts::BootSample Sample(std::int64_t offset, std::int64_t at = 0, std::int64_t boot_id = 1)
{
    return mts::BootSample{.boot_id = boot_id, .offset = offset, .at = at};
}

}  // namespace

TEST(LeafTimeArithmeticTest, SaturatingAddAndSubClampToTheRange)
{
    EXPECT_EQ(mts::SaturatingAdd(1, 2), 3);
    EXPECT_EQ(mts::SaturatingAdd(kMax, 1), kMax);
    EXPECT_EQ(mts::SaturatingAdd(kMin, -1), kMin);
    EXPECT_EQ(mts::SaturatingSub(5, 7), -2);
    EXPECT_EQ(mts::SaturatingSub(kMax, -1), kMax);
    EXPECT_EQ(mts::SaturatingSub(kMin, 1), kMin);
    EXPECT_EQ(mts::SaturatingSub(0, kMin), kMax);
}

TEST(LeafTimeArithmeticTest, ConcentratorStampedIsReceiveMinusEncodeOrFixedAtReceive)
{
    const auto with_e = mts::ConcentratorStamped(1000, 300);
    EXPECT_EQ(with_e.offset, 700);
    EXPECT_FALSE(with_e.fixed.has_value());

    const auto without_e = mts::ConcentratorStamped(1000, std::nullopt);
    EXPECT_EQ(without_e.fixed, 1000);
}

TEST(LeafTimeArithmeticTest, SyncRelativeTrustsOnlyWithinBothLimits)
{
    const mts::SyncLimits limits{.max_sync_age = 10, .max_clock_skew = 5};
    EXPECT_TRUE(mts::SyncRelative(100, 95, 10, limits).has_value());
    EXPECT_TRUE(mts::SyncRelative(100, 105, 0, limits).has_value());
    EXPECT_FALSE(mts::SyncRelative(100, 94, 0, limits).has_value()) << "skew 6";
    EXPECT_FALSE(mts::SyncRelative(100, 106, 0, limits).has_value()) << "skew -6";
    EXPECT_FALSE(mts::SyncRelative(100, 100, 11, limits).has_value()) << "stale";
    EXPECT_FALSE(mts::SyncRelative(100, 100, -1, limits).has_value()) << "negative age";
    EXPECT_FALSE(mts::SyncRelative(kMax, kMin, 0, limits).has_value()) << "no overflow";
    const mts::TimeCorrection untrusted{.offset = -1, .fixed = std::nullopt};
    EXPECT_EQ(mts::SyncRelative(100, 100, 0, limits).value_or(untrusted).offset, 0)
        << "trusted: t' = t";
}

TEST(LeafTimeArithmeticTest, ApplyTimeCorrectionRewritesStartEndAndEvents)
{
    mti::SpanRecord span;
    span.start_time = Ns(10);
    span.end_time = Ns(20);
    mti::SpanEvent event;
    event.timestamp = Ns(15);
    span.events.push_back(event);

    mts::ApplyTimeCorrection(span, mts::TimeCorrection{.offset = 100, .fixed = std::nullopt});

    EXPECT_EQ(NsOf(span.start_time), 110);
    EXPECT_EQ(NsOf(span.end_time), 120);
    EXPECT_EQ(NsOf(span.events.at(0).timestamp), 115);

    mts::ApplyTimeCorrection(span, mts::TimeCorrection{.offset = -1000, .fixed = std::nullopt});
    EXPECT_EQ(NsOf(span.start_time), 0) << "clamped at the epoch";

    mts::ApplyTimeCorrection(span, mts::TimeCorrection{.offset = 0, .fixed = 42});
    EXPECT_EQ(NsOf(span.end_time), 42);
    EXPECT_EQ(NsOf(span.events.at(0).timestamp), 42);
}

TEST(BootAnchorTest, OneSampleIsTheProvisionalAnchor)
{
    mts::BootAnchor anchor;
    EXPECT_EQ(anchor.Update(Sample(500), 10), 500);
    EXPECT_EQ(anchor.Size(), 1U);
}

TEST(BootAnchorTest, TheAnchorIsTheSecondSmallestSample)
{
    mts::BootAnchor anchor;
    (void)anchor.Update(Sample(300), 10);
    EXPECT_EQ(anchor.Update(Sample(100), 10), 300);
    EXPECT_EQ(anchor.Update(Sample(200), 10), 200);
    EXPECT_EQ(anchor.Update(Sample(100), 10), 100) << "a tie at the minimum is corroboration";
}

TEST(BootAnchorTest, ASingleLowOutlierIsIgnored)
{
    mts::BootAnchor anchor;
    for (int i = 0; i < 8; ++i)
    {
        (void)anchor.Update(Sample(1000), 10);
    }
    EXPECT_EQ(anchor.Update(Sample(-5000), 10), 1000);
    EXPECT_EQ(anchor.Update(Sample(1001), 10), 1000);
}

TEST(BootAnchorTest, SamplesAgeOutOfTheWindow)
{
    mts::BootAnchor anchor;
    (void)anchor.Update(Sample(100, 0), 10);
    (void)anchor.Update(Sample(100, 0), 10);
    EXPECT_EQ(anchor.Update(Sample(300, 10), 10), 100) << "age exactly the window: kept";
    EXPECT_EQ(anchor.Update(Sample(300, 11), 10), 300);
    EXPECT_EQ(anchor.Size(), 2U);
}

TEST(BootAnchorTest, TheRingHoldsSixteen)
{
    mts::BootAnchor anchor;
    for (std::size_t i = 0; i < mts::BootAnchor::kMaxSamples + 4; ++i)
    {
        (void)anchor.Update(Sample(static_cast<std::int64_t>(i)), 1000);
    }
    EXPECT_EQ(anchor.Size(), mts::BootAnchor::kMaxSamples);
    // Samples 4..19 remain: the anchor is 5.
    EXPECT_EQ(anchor.Update(Sample(100), 1000), 6) << "5..19 and 100";
}

TEST(BootAnchorTest, ANewBootIdDiscardsTheOldSamples)
{
    mts::BootAnchor anchor;
    (void)anchor.Update(Sample(1, 0, 1), 10);
    (void)anchor.Update(Sample(1, 0, 1), 10);
    EXPECT_EQ(anchor.Update(Sample(900, 0, 2), 10), 900);
    EXPECT_EQ(anchor.Size(), 1U);
}

TEST(BootAnchorTest, AHugeWindowDoesNotOverflow)
{
    mts::BootAnchor anchor;
    (void)anchor.Update(Sample(7, kMin), kMax);
    EXPECT_EQ(anchor.Update(Sample(7, kMin), kMax), 7);
}
