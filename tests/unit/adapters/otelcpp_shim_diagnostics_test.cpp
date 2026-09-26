// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Covers shim_diagnostics.hpp's own mechanics in isolation: the accessor
// reflects recorded events, each counter is independent, and concurrent
// recording is race-free (TSAN-relevant). ICP 0016.
//
// The counters are process-global statics, and gtest runs every TEST in
// this binary in one process — so tests here take a baseline snapshot
// before recording and assert on the delta, never on an absolute value.
// (metrics_instruments_shim.hpp's own call sites are covered by
// otelcpp_meter_shim_test.cpp's Uint64AboveInt64MaxIsOmittedNotWrapped and
// ThrowingCallbackCostsNeitherTheProcessNorTheOtherCallbacks, which assert
// the same delta discipline against a *different* process.)

#include "adapters/otelcpp/shim_diagnostics.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <thread>
#include <vector>

namespace
{

using microtel::adapters::otelcpp::GetShimDiagnostics;
using microtel::adapters::otelcpp::detail::RecordObserverCallbackFailure;
using microtel::adapters::otelcpp::detail::RecordOversizedByteAttributeOmitted;
using microtel::adapters::otelcpp::detail::RecordUnrepresentableMeasurementOmitted;

TEST(OtelCppShimDiagnostics, RecordingIncrementsOnlyItsOwnCounter)
{
    const auto baseline = GetShimDiagnostics();

    RecordUnrepresentableMeasurementOmitted();

    const auto after = GetShimDiagnostics();
    EXPECT_EQ(after.unrepresentable_measurements_omitted,
              baseline.unrepresentable_measurements_omitted + 1U);
    EXPECT_EQ(after.observer_callback_failures, baseline.observer_callback_failures);
}

TEST(OtelCppShimDiagnostics, BothCountersAreIndependentlyRecordable)
{
    const auto baseline = GetShimDiagnostics();

    RecordUnrepresentableMeasurementOmitted();
    RecordUnrepresentableMeasurementOmitted();
    RecordObserverCallbackFailure();

    const auto after = GetShimDiagnostics();
    EXPECT_EQ(after.unrepresentable_measurements_omitted,
              baseline.unrepresentable_measurements_omitted + 2U);
    EXPECT_EQ(after.observer_callback_failures, baseline.observer_callback_failures + 1U);
}

TEST(OtelCppShimDiagnostics, OversizedByteAttributeCountsAndRaisesTheHighWaterMark)
{
    // The high-water mark is a maximum, so it cannot be diffed: assert
    // after == max(before, n) instead (ICP 0033 §7).
    constexpr std::uint64_t kBytes = 12345;
    const auto baseline = GetShimDiagnostics();

    RecordOversizedByteAttributeOmitted(kBytes);

    const auto after = GetShimDiagnostics();
    EXPECT_EQ(after.oversized_byte_attributes_omitted,
              baseline.oversized_byte_attributes_omitted + 1U);
    EXPECT_EQ(after.largest_omitted_byte_attribute,
              std::max(baseline.largest_omitted_byte_attribute, kBytes));
    EXPECT_EQ(after.unrepresentable_measurements_omitted,
              baseline.unrepresentable_measurements_omitted);
    EXPECT_EQ(after.observer_callback_failures, baseline.observer_callback_failures);
}

TEST(OtelCppShimDiagnostics, SmallerOmissionDoesNotLowerTheHighWaterMark)
{
    constexpr std::uint64_t kLarge = 54321;
    RecordOversizedByteAttributeOmitted(kLarge);
    const auto baseline = GetShimDiagnostics();

    RecordOversizedByteAttributeOmitted(1U);

    const auto after = GetShimDiagnostics();
    EXPECT_EQ(after.oversized_byte_attributes_omitted,
              baseline.oversized_byte_attributes_omitted + 1U);
    EXPECT_EQ(after.largest_omitted_byte_attribute, baseline.largest_omitted_byte_attribute);
    EXPECT_GE(after.largest_omitted_byte_attribute, kLarge);
}

constexpr int kConcurrentIncrementsPerThread = 1000;

void RecordManyTimes()
{
    for (int i = 0; i < kConcurrentIncrementsPerThread; ++i)
    {
        RecordUnrepresentableMeasurementOmitted();
    }
}

TEST(OtelCppShimDiagnostics, ConcurrentRecordingLosesNoIncrements)
{
    constexpr int kThreads = 8;
    const auto baseline = GetShimDiagnostics();

    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t)
    {
        threads.emplace_back(&RecordManyTimes);
    }
    for (auto& thread : threads)
    {
        thread.join();
    }

    EXPECT_EQ(GetShimDiagnostics().unrepresentable_measurements_omitted,
              baseline.unrepresentable_measurements_omitted +
                  static_cast<std::uint64_t>(kThreads * kConcurrentIncrementsPerThread));
}

constexpr std::uint64_t kHighWaterBase = 1000000;
constexpr std::uint64_t kHighWaterThreadStride = 10000;

void RecordOversizedManyTimes(std::uint64_t thread_index)
{
    for (int i = 0; i < kConcurrentIncrementsPerThread; ++i)
    {
        RecordOversizedByteAttributeOmitted(kHighWaterBase +
                                            (thread_index * kHighWaterThreadStride) +
                                            static_cast<std::uint64_t>(i));
    }
}

TEST(OtelCppShimDiagnostics, ConcurrentOversizedOmissionsLoseNoCountAndKeepTheTrueMaximum)
{
    constexpr int kThreads = 8;
    const auto baseline = GetShimDiagnostics();

    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t)
    {
        threads.emplace_back(&RecordOversizedManyTimes, static_cast<std::uint64_t>(t));
    }
    for (auto& thread : threads)
    {
        thread.join();
    }

    // The largest value any thread recorded; the compare-exchange loop must
    // not let a smaller concurrent store overwrite it.
    constexpr std::uint64_t kLargestRecorded = kHighWaterBase +
                                               ((kThreads - 1) * kHighWaterThreadStride) +
                                               (kConcurrentIncrementsPerThread - 1);
    const auto after = GetShimDiagnostics();
    EXPECT_EQ(after.oversized_byte_attributes_omitted,
              baseline.oversized_byte_attributes_omitted +
                  static_cast<std::uint64_t>(kThreads * kConcurrentIncrementsPerThread));
    EXPECT_EQ(after.largest_omitted_byte_attribute,
              std::max(baseline.largest_omitted_byte_attribute, kLargestRecorded));
}

}  // namespace
