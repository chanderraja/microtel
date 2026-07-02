// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for DiagnosticsCounters — the production IDiagnosticsSink
// backing Provider::GetExporterHealth() (docs/interfaces.md §4.11).

#include "sdk/diagnostics_counters.hpp"

#include "microtel/error.hpp"
#include "microtel/provider.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace mt = microtel;
namespace mts = microtel::sdk;

namespace
{

[[nodiscard]] constexpr std::size_t Index(mt::DropReason reason) noexcept
{
    return static_cast<std::size_t>(reason);
}

// Worker for the concurrency test: records `count` single drops spread
// across all reasons, starting at `offset` so threads interleave reasons.
void RecordMixedDrops(mts::DiagnosticsCounters& counters, std::size_t offset, std::size_t count)
{
    for (std::size_t i = 0; i < count; ++i)
    {
        const auto reason = static_cast<mt::DropReason>((offset + i) % mt::kDropReasonCount);
        counters.RecordDrop(reason);
    }
}

}  // namespace

// A freshly constructed sink reports an all-zero, error-free snapshot.
TEST(DiagnosticsCountersTest, SnapshotIsZeroInitialized)
{
    const mts::DiagnosticsCounters counters;

    const mt::HealthSnapshot snap = counters.Snapshot();

    for (std::size_t i = 0; i < mt::kDropReasonCount; ++i)
    {
        EXPECT_EQ(snap.drop_counters.at(i), 0U) << "reason index " << i;
    }
    EXPECT_EQ(snap.batches_sent, 0U);
    EXPECT_EQ(snap.batches_failed, 0U);
    EXPECT_EQ(snap.queue_depth_now, 0U);
    EXPECT_FALSE(snap.last_error_time.has_value());
    EXPECT_TRUE(snap.last_error_message.empty());
    EXPECT_EQ(snap.connection_state, mt::ConnectionState::Disconnected);
}

// A single RecordDrop bumps exactly one slot in the counter array.
TEST(DiagnosticsCountersTest, RecordDropIncrementsOnlyThatReason)
{
    mts::DiagnosticsCounters counters;

    counters.RecordDrop(mt::DropReason::QueueFull);

    const mt::HealthSnapshot snap = counters.Snapshot();
    for (std::size_t i = 0; i < mt::kDropReasonCount; ++i)
    {
        const std::uint64_t expected = (i == Index(mt::DropReason::QueueFull)) ? 1U : 0U;
        EXPECT_EQ(snap.drop_counters.at(i), expected) << "reason index " << i;
    }
}

// The count parameter adds N in one call; the default adds 1.
TEST(DiagnosticsCountersTest, RecordDropWithCountAddsN)
{
    constexpr std::uint64_t kBatchDrop = 17;
    mts::DiagnosticsCounters counters;

    counters.RecordDrop(mt::DropReason::RecordTooLarge, kBatchDrop);
    counters.RecordDrop(mt::DropReason::RecordTooLarge);

    const mt::HealthSnapshot snap = counters.Snapshot();
    EXPECT_EQ(snap.drop_counters[Index(mt::DropReason::RecordTooLarge)], kBatchDrop + 1U);
}

// N threads × M drops each across mixed reasons: no increment may be lost.
TEST(DiagnosticsCountersTest, ConcurrentRecordDropIsLossless)
{
    constexpr std::size_t kThreads = 8;
    constexpr std::size_t kDropsPerThread = 20'000;
    mts::DiagnosticsCounters counters;

    std::vector<std::thread> workers;
    workers.reserve(kThreads);
    for (std::size_t t = 0; t < kThreads; ++t)
    {
        workers.emplace_back(RecordMixedDrops, std::ref(counters), t, kDropsPerThread);
    }
    for (auto& worker : workers)
    {
        worker.join();
    }

    const mt::HealthSnapshot snap = counters.Snapshot();
    std::uint64_t total = 0;
    for (std::size_t i = 0; i < mt::kDropReasonCount; ++i)
    {
        total += snap.drop_counters.at(i);
    }
    EXPECT_EQ(total, static_cast<std::uint64_t>(kThreads) * kDropsPerThread);
}

// Batch-sent counter is monotonically incremented.
TEST(DiagnosticsCountersTest, RecordBatchSentIncrementsCounter)
{
    mts::DiagnosticsCounters counters;

    counters.RecordBatchSent();
    counters.RecordBatchSent();

    EXPECT_EQ(counters.Snapshot().batches_sent, 2U);
}

// A failed batch bumps the counter and fills the last-error slot.
TEST(DiagnosticsCountersTest, RecordBatchFailedStoresLastError)
{
    mts::DiagnosticsCounters counters;

    counters.RecordBatchFailed(
        mt::Error{.kind = mt::Error::Kind::Network, .message = "connect refused", .os_errno = 0});

    const mt::HealthSnapshot snap = counters.Snapshot();
    EXPECT_EQ(snap.batches_failed, 1U);
    EXPECT_TRUE(snap.last_error_time.has_value());
    EXPECT_EQ(snap.last_error_message, "connect refused");
}

// The stored last-error message is capped (HealthSnapshot doc: "capped").
TEST(DiagnosticsCountersTest, RecordBatchFailedCapsMessageLength)
{
    constexpr std::size_t kOverhang = 100;
    mts::DiagnosticsCounters counters;
    const std::string long_message(mts::DiagnosticsCounters::kMaxErrorMessageLength + kOverhang,
                                   'x');

    counters.RecordBatchFailed(
        mt::Error{.kind = mt::Error::Kind::Protocol, .message = long_message, .os_errno = 0});

    EXPECT_EQ(counters.Snapshot().last_error_message.size(),
              mts::DiagnosticsCounters::kMaxErrorMessageLength);
}

// Queue depth is a gauge: the snapshot reflects the latest published value.
TEST(DiagnosticsCountersTest, SetQueueDepthReflectsLatestValue)
{
    constexpr std::uint64_t kFirstDepth = 42;
    constexpr std::uint64_t kSecondDepth = 7;
    mts::DiagnosticsCounters counters;

    counters.SetQueueDepth(kFirstDepth);
    counters.SetQueueDepth(kSecondDepth);

    EXPECT_EQ(counters.Snapshot().queue_depth_now, kSecondDepth);
}

// Connection state is a gauge: the snapshot reflects the latest value.
TEST(DiagnosticsCountersTest, SetConnectionStateReflectsLatestValue)
{
    mts::DiagnosticsCounters counters;

    counters.SetConnectionState(mt::ConnectionState::Connected);

    EXPECT_EQ(counters.Snapshot().connection_state, mt::ConnectionState::Connected);
}
