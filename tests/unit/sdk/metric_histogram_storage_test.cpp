// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Behavioural tests for HistogramStorage<T> — explicit-bucket histogram
// aggregation state, M12 (docs/metrics-design.md §3).
//
// Bucket semantics (OTLP explicit bucket): for boundaries b[0..n-1] there are
// n+1 buckets; a value v falls in bucket i = the smallest i with v <= b[i]
// (upper-inclusive), or bucket n if v > b[n-1]. So bucket i counts
// (b[i-1], b[i]].

#include "sdk/metric_histogram_storage.hpp"

#include "microtel/attribute.hpp"
#include "microtel/internal/metric_batch.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace mt = microtel;
namespace mts = microtel::sdk;
namespace mti = microtel::internal;

namespace
{

mt::KeyValue Kv(std::string key, mt::AttributeValue value)
{
    return mt::KeyValue{.key = std::move(key), .value = std::move(value)};
}

void RecordN(mts::HistogramStorage<std::int64_t>& storage,
             mt::AttributeSpan attrs,
             std::int64_t value,
             int count)
{
    for (int i = 0; i < count; ++i)
    {
        storage.Record(value, attrs);
    }
}

bool IsOverflowPoint(const mti::HistogramPoint& pt)
{
    return std::ranges::any_of(pt.attributes,
                               [](const mt::KeyValue& kv)
                               {
                                   return kv.key == "otel.metric.overflow" &&
                                          std::holds_alternative<bool>(kv.value) &&
                                          std::get<bool>(kv.value);
                               });
}

}  // namespace

TEST(HistogramStorageTest, TracksCountSumMinMax)
{
    mts::HistogramStorage<std::int64_t> storage{std::vector<double>{10, 20}};
    const std::vector<mt::KeyValue> attrs{Kv("k", std::string{"v"})};

    storage.Record(5, mt::AttributeSpan{attrs});
    storage.Record(15, mt::AttributeSpan{attrs});
    storage.Record(25, mt::AttributeSpan{attrs});

    const mti::HistogramData data = storage.Collect();
    ASSERT_EQ(data.points.size(), 1U);
    const mti::HistogramPoint& point = data.points[0];
    EXPECT_EQ(point.count, 3U);
    EXPECT_DOUBLE_EQ(point.sum, 45.0);
    // Compare the optionals directly (5.0/25.0 are exact) — avoids unchecked
    // optional access, which the analyzer flags even after ASSERT_TRUE.
    EXPECT_EQ(point.min, std::optional<double>{5.0});
    EXPECT_EQ(point.max, std::optional<double>{25.0});
}

TEST(HistogramStorageTest, BucketsAreUpperInclusive)
{
    mts::HistogramStorage<std::int64_t> storage{std::vector<double>{10, 20}};
    const std::vector<mt::KeyValue> attrs{Kv("k", std::string{"v"})};

    for (const std::int64_t v : {5, 10, 15, 20, 25})  // b0:{5,10} b1:{15,20} b2:{25}
    {
        storage.Record(v, mt::AttributeSpan{attrs});
    }

    const mti::HistogramData data = storage.Collect();
    ASSERT_EQ(data.points.size(), 1U);
    const std::vector<std::uint64_t> expected{2, 2, 1};
    EXPECT_EQ(data.points[0].bucket_counts, expected);
}

TEST(HistogramStorageTest, BucketCountsSizeIsBoundariesPlusOne)
{
    mts::HistogramStorage<std::int64_t> storage{std::vector<double>{1, 2, 3}};
    const std::vector<mt::KeyValue> attrs{Kv("k", std::string{"v"})};
    storage.Record(2, mt::AttributeSpan{attrs});

    EXPECT_EQ(storage.Collect().points[0].bucket_counts.size(), 4U);
}

TEST(HistogramStorageTest, ExplicitBoundsEchoedInOutput)
{
    const std::vector<double> bounds{1, 5, 10};
    mts::HistogramStorage<double> storage{bounds};
    const std::vector<mt::KeyValue> attrs{Kv("k", std::string{"v"})};
    storage.Record(3.0, mt::AttributeSpan{attrs});

    EXPECT_EQ(storage.Collect().points[0].explicit_bounds, bounds);
}

TEST(HistogramStorageTest, DoubleValues)
{
    mts::HistogramStorage<double> storage{std::vector<double>{1.0}};
    const std::vector<mt::KeyValue> attrs{Kv("k", std::string{"v"})};

    storage.Record(0.5, mt::AttributeSpan{attrs});  // b0
    storage.Record(1.0, mt::AttributeSpan{attrs});  // b0 (<=1.0)
    storage.Record(1.5, mt::AttributeSpan{attrs});  // b1

    const mti::HistogramData data = storage.Collect();
    ASSERT_EQ(data.points.size(), 1U);
    const std::vector<std::uint64_t> expected{2, 1};
    EXPECT_EQ(data.points[0].bucket_counts, expected);
    EXPECT_DOUBLE_EQ(data.points[0].sum, 3.0);
}

TEST(HistogramStorageTest, DistinctAttributeSetsAreDistinctPoints)
{
    mts::HistogramStorage<std::int64_t> storage{std::vector<double>{10}};
    const std::vector<mt::KeyValue> a{Kv("r", std::string{"a"})};
    const std::vector<mt::KeyValue> b{Kv("r", std::string{"b"})};

    storage.Record(1, mt::AttributeSpan{a});
    storage.Record(2, mt::AttributeSpan{b});

    EXPECT_EQ(storage.Collect().points.size(), 2U);
}

TEST(HistogramStorageTest, EmptyStorageCollectsNoPoints)
{
    mts::HistogramStorage<double> storage{std::vector<double>{1, 2}};
    EXPECT_TRUE(storage.Collect().points.empty());
}

TEST(HistogramStorageTest, CollectIsCumulativeTemporality)
{
    mts::HistogramStorage<std::int64_t> storage{std::vector<double>{1}};
    EXPECT_EQ(storage.Collect().temporality, mti::AggregationTemporality::Cumulative);
}

TEST(HistogramStorageTest, DeltaReportsSinceLastCollectAndClears)
{
    mts::HistogramStorage<std::int64_t> storage{std::vector<double>{10}};
    const std::vector<mt::KeyValue> attrs{Kv("k", std::string{"v"})};

    storage.Record(5, mt::AttributeSpan{attrs});
    const mti::HistogramData first = storage.Collect(mti::AggregationTemporality::Delta);
    ASSERT_EQ(first.points.size(), 1U);
    EXPECT_EQ(first.temporality, mti::AggregationTemporality::Delta);
    EXPECT_EQ(first.points[0].count, 1U);

    // Nothing recorded since → empty delta.
    EXPECT_TRUE(storage.Collect(mti::AggregationTemporality::Delta).points.empty());

    storage.Record(15, mt::AttributeSpan{attrs});
    const mti::HistogramData third = storage.Collect(mti::AggregationTemporality::Delta);
    ASSERT_EQ(third.points.size(), 1U);
    EXPECT_EQ(third.points[0].count, 1U);  // only the value since the last collect
}

TEST(HistogramStorageTest, CardinalityOverflowRoutesToOverflowSeries)
{
    mts::HistogramStorage<std::int64_t> storage{std::vector<double>{10}, /*max_cardinality=*/2};
    const std::vector<mt::KeyValue> a{Kv("k", std::string{"a"})};
    const std::vector<mt::KeyValue> b{Kv("k", std::string{"b"})};
    const std::vector<mt::KeyValue> c{Kv("k", std::string{"c"})};

    storage.Record(1, mt::AttributeSpan{a});
    storage.Record(2, mt::AttributeSpan{b});
    storage.Record(3, mt::AttributeSpan{c});  // overflows → overflow series
    storage.Record(4, mt::AttributeSpan{c});  // also overflows

    const mti::HistogramData data = storage.Collect();
    ASSERT_EQ(data.points.size(), 3U);

    const auto ov_it = std::ranges::find_if(data.points, IsOverflowPoint);
    ASSERT_NE(ov_it, data.points.end());
    EXPECT_EQ(ov_it->count, 2U);  // 2 overflowed recordings
    EXPECT_DOUBLE_EQ(ov_it->sum, 7.0);
}

TEST(HistogramStorageTest, NonFiniteValueIsDropped)
{
    mts::HistogramStorage<double> storage{std::vector<double>{10.0}};
    const std::vector<mt::KeyValue> attrs{Kv("k", std::string{"v"})};
    storage.Record(5.0, mt::AttributeSpan{attrs});
    storage.Record(std::numeric_limits<double>::infinity(), mt::AttributeSpan{attrs});
    storage.Record(std::numeric_limits<double>::quiet_NaN(), mt::AttributeSpan{attrs});
    const mti::HistogramData data = storage.Collect();
    ASSERT_EQ(data.points.size(), 1U);
    EXPECT_EQ(data.points[0].count, 1U);
    EXPECT_DOUBLE_EQ(data.points[0].sum, 5.0);
}

TEST(HistogramStorageTest, PointCarriesItsAttributes)
{
    mts::HistogramStorage<std::int64_t> storage{std::vector<double>{10}};
    const std::vector<mt::KeyValue> attrs{Kv("route", std::string{"/x"})};
    storage.Record(1, mt::AttributeSpan{attrs});

    const mti::HistogramData data = storage.Collect();
    ASSERT_EQ(data.points[0].attributes.size(), 1U);
    EXPECT_EQ(data.points[0].attributes[0].key, "route");
}

TEST(HistogramStorageTest, ConcurrentRecordsConserveCount)
{
    mts::HistogramStorage<std::int64_t> storage{std::vector<double>{10, 20}};
    const std::vector<mt::KeyValue> attrs{Kv("k", std::string{"v"})};

    constexpr int kThreads = 8;
    constexpr int kPerThread = 1000;
    std::vector<std::thread> workers;
    workers.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t)
    {
        workers.emplace_back(
            RecordN, std::ref(storage), mt::AttributeSpan{attrs}, std::int64_t{15}, kPerThread);
    }
    for (auto& worker : workers)
    {
        worker.join();
    }

    const mti::HistogramData data = storage.Collect();
    ASSERT_EQ(data.points.size(), 1U);
    const std::uint64_t total = static_cast<std::uint64_t>(kThreads) * kPerThread;
    EXPECT_EQ(data.points[0].count, total);
    EXPECT_EQ(data.points[0].bucket_counts.at(1), total);  // 15 → bucket 1 (10,20]
}
