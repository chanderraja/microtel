// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Behavioural tests for ExponentialHistogramStorage<T> — base-2 exponential
// histogram aggregation, M12 (docs/metrics-design.md §3).
//
// Mapping (OTel base-2): for scale s and value v > 0, the bucket index is
// ceil(log2(v) * 2^s) - 1, so bucket i covers (2^(i/2^s), 2^((i+1)/2^s)].
// At scale 0 that is index(1)=-1, index(2)=0, index(3)=1, index(8)=2.
// Downscaling by k floor-divides indices by 2^k (merging buckets), bounded by
// max_buckets. There is a known floating-point boundary subtlety at exact
// powers of two (shared with the OTel reference); tests avoid asserting on it.

#include "sdk/metric_exp_histogram_storage.hpp"

#include "microtel/attribute.hpp"
#include "microtel/internal/metric_batch.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <limits>
#include <numeric>
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

std::uint64_t Sum(const std::vector<std::uint64_t>& counts)
{
    return std::accumulate(counts.begin(), counts.end(), std::uint64_t{0});
}

void RecordN(mts::ExponentialHistogramStorage<std::int64_t>& storage,
             mt::AttributeSpan attrs,
             std::int64_t value,
             int count)
{
    for (int i = 0; i < count; ++i)
    {
        storage.Record(value, attrs);
    }
}

bool IsOverflowPoint(const mti::ExponentialHistogramPoint& pt)
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

TEST(ExpHistogramStorageTest, ZeroGoesToZeroCount)
{
    mts::ExponentialHistogramStorage<std::int64_t> storage{/*max_scale=*/0, /*max_buckets=*/160};
    const std::vector<mt::KeyValue> attrs{Kv("k", std::string{"v"})};

    storage.Record(0, mt::AttributeSpan{attrs});

    const mti::ExponentialHistogramData data = storage.Collect();
    ASSERT_EQ(data.points.size(), 1U);
    EXPECT_EQ(data.points[0].zero_count, 1U);
    EXPECT_EQ(data.points[0].count, 1U);
    EXPECT_TRUE(data.points[0].positive.bucket_counts.empty());
}

TEST(ExpHistogramStorageTest, PositiveMappingAtScaleZero)
{
    // scale 0: index(1)=-1, index(2)=0, index(3)=1 → offset -1, counts [1,1,1].
    mts::ExponentialHistogramStorage<std::int64_t> storage{0, 160};
    const std::vector<mt::KeyValue> attrs{Kv("k", std::string{"v"})};

    storage.Record(2, mt::AttributeSpan{attrs});
    storage.Record(3, mt::AttributeSpan{attrs});
    storage.Record(1, mt::AttributeSpan{attrs});

    const mti::ExponentialHistogramData data = storage.Collect();
    ASSERT_EQ(data.points.size(), 1U);
    const mti::ExponentialHistogramPoint& point = data.points[0];
    EXPECT_EQ(point.scale, 0);
    EXPECT_EQ(point.positive.offset, -1);
    const std::vector<std::uint64_t> expected{1, 1, 1};
    EXPECT_EQ(point.positive.bucket_counts, expected);
    EXPECT_EQ(point.zero_count, 0U);
}

TEST(ExpHistogramStorageTest, NegativeValuesUseNegativeBuckets)
{
    mts::ExponentialHistogramStorage<std::int64_t> storage{0, 160};
    const std::vector<mt::KeyValue> attrs{Kv("k", std::string{"v"})};

    storage.Record(-2, mt::AttributeSpan{attrs});  // |v|=2 → index 0

    const mti::ExponentialHistogramData data = storage.Collect();
    ASSERT_EQ(data.points.size(), 1U);
    const mti::ExponentialHistogramPoint& point = data.points[0];
    EXPECT_TRUE(point.positive.bucket_counts.empty());
    EXPECT_EQ(point.negative.offset, 0);
    EXPECT_EQ(Sum(point.negative.bucket_counts), 1U);
}

TEST(ExpHistogramStorageTest, DownscaleWhenSpanExceedsMaxBuckets)
{
    // scale 0, max_buckets 2. index(1)=-1, index(8)=2 → span 4 > 2 → downscale.
    mts::ExponentialHistogramStorage<std::int64_t> storage{0, 2};
    const std::vector<mt::KeyValue> attrs{Kv("k", std::string{"v"})};

    storage.Record(1, mt::AttributeSpan{attrs});
    storage.Record(8, mt::AttributeSpan{attrs});

    const mti::ExponentialHistogramData data = storage.Collect();
    ASSERT_EQ(data.points.size(), 1U);
    const mti::ExponentialHistogramPoint& point = data.points[0];
    EXPECT_LT(point.scale, 0);  // scale was reduced
    EXPECT_EQ(point.count, 2U);
    EXPECT_EQ(Sum(point.positive.bucket_counts), 2U);    // both values conserved
    EXPECT_LE(point.positive.bucket_counts.size(), 2U);  // within max_buckets
}

TEST(ExpHistogramStorageTest, TracksCountSumMinMax)
{
    mts::ExponentialHistogramStorage<double> storage{20, 160};
    const std::vector<mt::KeyValue> attrs{Kv("k", std::string{"v"})};

    storage.Record(2.0, mt::AttributeSpan{attrs});
    storage.Record(8.0, mt::AttributeSpan{attrs});
    storage.Record(0.0, mt::AttributeSpan{attrs});  // counts toward count, not buckets

    const mti::ExponentialHistogramData data = storage.Collect();
    ASSERT_EQ(data.points.size(), 1U);
    const mti::ExponentialHistogramPoint& point = data.points[0];
    EXPECT_EQ(point.count, 3U);
    EXPECT_DOUBLE_EQ(point.sum, 10.0);
    EXPECT_EQ(point.min, std::optional<double>{0.0});
    EXPECT_EQ(point.max, std::optional<double>{8.0});
    EXPECT_EQ(point.zero_count, 1U);
}

TEST(ExpHistogramStorageTest, DistinctAttributeSetsAreDistinctPoints)
{
    mts::ExponentialHistogramStorage<std::int64_t> storage{0, 160};
    const std::vector<mt::KeyValue> a{Kv("r", std::string{"a"})};
    const std::vector<mt::KeyValue> b{Kv("r", std::string{"b"})};

    storage.Record(2, mt::AttributeSpan{a});
    storage.Record(4, mt::AttributeSpan{b});

    EXPECT_EQ(storage.Collect().points.size(), 2U);
}

TEST(ExpHistogramStorageTest, EmptyStorageCollectsNoPoints)
{
    mts::ExponentialHistogramStorage<double> storage{20, 160};
    EXPECT_TRUE(storage.Collect().points.empty());
}

TEST(ExpHistogramStorageTest, CollectIsCumulativeTemporality)
{
    mts::ExponentialHistogramStorage<std::int64_t> storage{20, 160};
    EXPECT_EQ(storage.Collect().temporality, mti::AggregationTemporality::Cumulative);
}

TEST(ExpHistogramStorageTest, DeltaReportsSinceLastCollectAndClears)
{
    mts::ExponentialHistogramStorage<std::int64_t> storage{0, 160};
    const std::vector<mt::KeyValue> attrs{Kv("k", std::string{"v"})};

    storage.Record(2, mt::AttributeSpan{attrs});
    const mti::ExponentialHistogramData first = storage.Collect(mti::AggregationTemporality::Delta);
    ASSERT_EQ(first.points.size(), 1U);
    EXPECT_EQ(first.temporality, mti::AggregationTemporality::Delta);
    EXPECT_EQ(first.points[0].count, 1U);

    // Nothing recorded since → empty delta.
    EXPECT_TRUE(storage.Collect(mti::AggregationTemporality::Delta).points.empty());

    storage.Record(4, mt::AttributeSpan{attrs});
    const mti::ExponentialHistogramData third = storage.Collect(mti::AggregationTemporality::Delta);
    ASSERT_EQ(third.points.size(), 1U);
    EXPECT_EQ(third.points[0].count, 1U);  // only the value since the last collect
}

TEST(ExpHistogramStorageTest, CardinalityOverflowRoutesToOverflowSeries)
{
    mts::ExponentialHistogramStorage<std::int64_t> storage{0, 160, /*max_cardinality=*/2};
    const std::vector<mt::KeyValue> a{Kv("k", std::string{"a"})};
    const std::vector<mt::KeyValue> b{Kv("k", std::string{"b"})};
    const std::vector<mt::KeyValue> c{Kv("k", std::string{"c"})};

    storage.Record(2, mt::AttributeSpan{a});
    storage.Record(3, mt::AttributeSpan{b});
    storage.Record(4, mt::AttributeSpan{c});  // overflows → overflow series
    storage.Record(5, mt::AttributeSpan{c});  // also overflows

    const mti::ExponentialHistogramData data = storage.Collect();
    ASSERT_EQ(data.points.size(), 3U);

    const auto ov_it = std::ranges::find_if(data.points, IsOverflowPoint);
    ASSERT_NE(ov_it, data.points.end());
    EXPECT_EQ(ov_it->count, 2U);  // 2 overflowed recordings
}

TEST(ExpHistogramStorageTest, NonFiniteValueIsDropped)
{
    mts::ExponentialHistogramStorage<double> storage{20, 160};
    const std::vector<mt::KeyValue> attrs{Kv("k", std::string{"v"})};
    storage.Record(2.0, mt::AttributeSpan{attrs});
    storage.Record(std::numeric_limits<double>::quiet_NaN(), mt::AttributeSpan{attrs});
    const mti::ExponentialHistogramData data = storage.Collect();
    ASSERT_EQ(data.points.size(), 1U);
    EXPECT_EQ(data.points[0].count, 1U);
}

TEST(ExpHistogramStorageTest, ConcurrentRecordsConserveCount)
{
    mts::ExponentialHistogramStorage<std::int64_t> storage{20, 160};
    const std::vector<mt::KeyValue> attrs{Kv("k", std::string{"v"})};

    constexpr int kThreads = 8;
    constexpr int kPerThread = 1000;
    std::vector<std::thread> workers;
    workers.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t)
    {
        workers.emplace_back(
            RecordN, std::ref(storage), mt::AttributeSpan{attrs}, std::int64_t{5}, kPerThread);
    }
    for (auto& worker : workers)
    {
        worker.join();
    }

    const mti::ExponentialHistogramData data = storage.Collect();
    ASSERT_EQ(data.points.size(), 1U);
    const std::uint64_t total = static_cast<std::uint64_t>(kThreads) * kPerThread;
    EXPECT_EQ(data.points[0].count, total);
    EXPECT_EQ(Sum(data.points[0].positive.bucket_counts), total);
}
