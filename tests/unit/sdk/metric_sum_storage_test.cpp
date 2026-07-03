// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Behavioural tests for SumStorage<T> — the live aggregation state for Sum
// instruments (Counter / UpDownCounter), M12 (docs/metrics-design.md §1/§9).
//
// Contract under test:
//  - Add accumulates per attribute set (order-insensitive); distinct sets are
//    distinct points.
//  - Collect snapshots running totals as cumulative SumData carrying the
//    monotonic flag and one NumberPoint (value + attributes) per set.
//  - Cumulative Collect retains state (a second Collect sees the same totals).
//  - Concurrent Add from many threads is safe and conserves the total.

#include "sdk/metric_sum_storage.hpp"

#include "microtel/attribute.hpp"
#include "microtel/internal/icurrent_span_source.hpp"
#include "microtel/internal/metric_batch.hpp"
#include "microtel/trace.hpp"

#include "fakes/fake_diagnostics_sink.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <string>
#include <thread>
#include <variant>
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

std::uint64_t CardinalityDrops(const mt::testing::FakeDiagnosticsSink& sink)
{
    return sink.drop_counters[static_cast<std::size_t>(mt::DropReason::CardinalityOverflow)];
}

// Find the point whose first attribute key/value matches, returning its value
// as T. Tests use single-attribute sets so the first pair identifies the set.
template <typename T>
T PointValue(const mti::SumData& data, std::size_t index)
{
    return std::get<T>(data.points.at(index).value);
}

void AddOnes(mts::SumStorage<std::int64_t>& storage, mt::AttributeSpan attrs, int count)
{
    for (int i = 0; i < count; ++i)
    {
        storage.Add(1, attrs);
    }
}

class FakeSpanSource : public mti::ICurrentSpanSource
{
public:
    void SetSpan(mt::SpanContext ctx) noexcept
    {
        m_span = ctx;
    }
    [[nodiscard]] mt::SpanContext GetCurrentSpan() const override
    {
        return m_span;
    }

private:
    mt::SpanContext m_span;
};

mt::SpanContext MakeSampledContext()
{
    mt::TraceId::Bytes tb{};
    tb[0] = 0x01;
    mt::SpanId::Bytes sb{};
    sb[0] = 0x01;
    return mt::SpanContext{
        .trace_id = mt::TraceId{tb},
        .span_id = mt::SpanId{sb},
        .trace_flags = mt::TraceFlags{mt::TraceFlags::kSampled},
        .trace_state = {},
    };
}

bool IsOverflowPoint(const mti::NumberPoint& pt)
{
    return std::ranges::any_of(pt.attributes,
                               [](const mt::KeyValue& kv)
                               {
                                   return kv.key == "otel.metric.overflow" &&
                                          std::holds_alternative<bool>(kv.value) &&
                                          std::get<bool>(kv.value);
                               });
}

std::vector<std::int64_t> SortedRealSumValues(const mti::SumData& data)
{
    std::vector<std::int64_t> vals;
    for (const auto& pt : data.points)
    {
        if (!IsOverflowPoint(pt))
        {
            vals.push_back(std::get<std::int64_t>(pt.value));
        }
    }
    std::ranges::sort(vals);
    return vals;
}

void AddUniqueKeys(mts::SumStorage<std::int64_t>& storage, int thread_id, int per_thread)
{
    for (int i = 0; i < per_thread; ++i)
    {
        const std::string key_str = std::to_string(thread_id * per_thread + i);
        const std::vector<mt::KeyValue> attrs{Kv("k", key_str)};
        storage.Add(1, mt::AttributeSpan{attrs});
    }
}

}  // namespace

TEST(SumStorageTest, AddAccumulatesSameAttributeSet)
{
    mts::SumStorage<std::int64_t> storage{/*monotonic=*/true};
    const std::vector<mt::KeyValue> attrs{Kv("route", std::string{"/a"})};

    storage.Add(1, mt::AttributeSpan{attrs});
    storage.Add(2, mt::AttributeSpan{attrs});
    storage.Add(4, mt::AttributeSpan{attrs});

    const mti::SumData data = storage.Collect();
    ASSERT_EQ(data.points.size(), 1U);
    EXPECT_EQ(PointValue<std::int64_t>(data, 0), 7);
}

TEST(SumStorageTest, DistinctAttributeSetsAreDistinctPoints)
{
    mts::SumStorage<std::int64_t> storage{true};
    const std::vector<mt::KeyValue> a{Kv("route", std::string{"/a"})};
    const std::vector<mt::KeyValue> b{Kv("route", std::string{"/b"})};

    storage.Add(3, mt::AttributeSpan{a});
    storage.Add(5, mt::AttributeSpan{b});

    EXPECT_EQ(storage.Collect().points.size(), 2U);
}

TEST(SumStorageTest, AttributeOrderDoesNotMatter)
{
    mts::SumStorage<std::int64_t> storage{true};
    const std::vector<mt::KeyValue> ab{Kv("a", std::int64_t{1}), Kv("b", std::int64_t{2})};
    const std::vector<mt::KeyValue> ba{Kv("b", std::int64_t{2}), Kv("a", std::int64_t{1})};

    storage.Add(10, mt::AttributeSpan{ab});
    storage.Add(5, mt::AttributeSpan{ba});

    const mti::SumData data = storage.Collect();
    ASSERT_EQ(data.points.size(), 1U);
    EXPECT_EQ(PointValue<std::int64_t>(data, 0), 15);
}

TEST(SumStorageTest, MonotonicFlagPropagates)
{
    EXPECT_TRUE(mts::SumStorage<std::int64_t>{true}.Collect().is_monotonic);
    EXPECT_FALSE(mts::SumStorage<std::int64_t>{false}.Collect().is_monotonic);
}

TEST(SumStorageTest, CollectIsCumulativeTemporality)
{
    mts::SumStorage<std::int64_t> storage{true};
    EXPECT_EQ(storage.Collect().temporality, mti::AggregationTemporality::Cumulative);
}

TEST(SumStorageTest, DeltaReportsSinceLastCollectAndClears)
{
    mts::SumStorage<std::int64_t> storage{true};
    const std::vector<mt::KeyValue> attrs{Kv("k", std::string{"v"})};

    storage.Add(5, mt::AttributeSpan{attrs});
    storage.Add(3, mt::AttributeSpan{attrs});

    const mti::SumData first = storage.Collect(mti::AggregationTemporality::Delta);
    ASSERT_EQ(first.points.size(), 1U);
    EXPECT_EQ(first.temporality, mti::AggregationTemporality::Delta);
    EXPECT_EQ(PointValue<std::int64_t>(first, 0), 8);

    // Nothing recorded since the previous delta collect → empty.
    EXPECT_TRUE(storage.Collect(mti::AggregationTemporality::Delta).points.empty());

    // New activity reports only the new increment.
    storage.Add(4, mt::AttributeSpan{attrs});
    const mti::SumData third = storage.Collect(mti::AggregationTemporality::Delta);
    ASSERT_EQ(third.points.size(), 1U);
    EXPECT_EQ(PointValue<std::int64_t>(third, 0), 4);
}

TEST(SumStorageTest, DoubleValuesAccumulate)
{
    mts::SumStorage<double> storage{false};
    const std::vector<mt::KeyValue> attrs{Kv("k", std::string{"v"})};

    storage.Add(1.5, mt::AttributeSpan{attrs});
    storage.Add(2.5, mt::AttributeSpan{attrs});

    const mti::SumData data = storage.Collect();
    ASSERT_EQ(data.points.size(), 1U);
    EXPECT_DOUBLE_EQ(PointValue<double>(data, 0), 4.0);
}

TEST(SumStorageTest, CumulativeCollectRetainsState)
{
    mts::SumStorage<std::int64_t> storage{true};
    const std::vector<mt::KeyValue> attrs{Kv("k", std::string{"v"})};
    storage.Add(2, mt::AttributeSpan{attrs});

    EXPECT_EQ(PointValue<std::int64_t>(storage.Collect(), 0), 2);
    // Cumulative: the second collect still reports the retained total.
    EXPECT_EQ(PointValue<std::int64_t>(storage.Collect(), 0), 2);
}

TEST(SumStorageTest, EmptyStorageCollectsNoPoints)
{
    const mti::SumData data = mts::SumStorage<double>{false}.Collect();
    EXPECT_TRUE(data.points.empty());
    EXPECT_FALSE(data.is_monotonic);
}

TEST(SumStorageTest, PointCarriesItsAttributes)
{
    mts::SumStorage<std::int64_t> storage{true};
    const std::vector<mt::KeyValue> attrs{Kv("route", std::string{"/x"})};
    storage.Add(1, mt::AttributeSpan{attrs});

    const mti::SumData data = storage.Collect();
    ASSERT_EQ(data.points.size(), 1U);
    ASSERT_EQ(data.points[0].attributes.size(), 1U);
    EXPECT_EQ(data.points[0].attributes[0].key, "route");
    EXPECT_EQ(std::get<std::string>(data.points[0].attributes[0].value), "/x");
}

TEST(SumStorageTest, ExemplarCapturedWhenSampledSpanActive)
{
    FakeSpanSource source;
    source.SetSpan(MakeSampledContext());
    mts::SumStorage<std::int64_t> storage{true, mts::kDefaultMaxCardinality, &source};
    const std::vector<mt::KeyValue> attrs{Kv("k", std::string{"v"})};

    storage.Add(5, mt::AttributeSpan{attrs});

    const mti::SumData data = storage.Collect();
    ASSERT_EQ(data.points.size(), 1U);
    ASSERT_EQ(data.points[0].exemplars.size(), 1U);
    EXPECT_TRUE(data.points[0].exemplars[0].span_context.IsValid());
    EXPECT_TRUE(data.points[0].exemplars[0].span_context.trace_flags.IsSampled());
    EXPECT_EQ(std::get<std::int64_t>(data.points[0].exemplars[0].value), 5);
}

TEST(SumStorageTest, NoExemplarWhenNoSpanActive)
{
    FakeSpanSource source;  // default-constructed span is invalid
    mts::SumStorage<std::int64_t> storage{true, mts::kDefaultMaxCardinality, &source};
    const std::vector<mt::KeyValue> attrs{Kv("k", std::string{"v"})};

    storage.Add(5, mt::AttributeSpan{attrs});

    const mti::SumData data = storage.Collect();
    ASSERT_EQ(data.points.size(), 1U);
    EXPECT_TRUE(data.points[0].exemplars.empty());
}

TEST(SumStorageTest, ExemplarIsResetAfterCollect)
{
    FakeSpanSource source;
    source.SetSpan(MakeSampledContext());
    mts::SumStorage<std::int64_t> storage{true, mts::kDefaultMaxCardinality, &source};
    const std::vector<mt::KeyValue> attrs{Kv("k", std::string{"v"})};

    storage.Add(1, mt::AttributeSpan{attrs});
    (void)storage.Collect();  // drains exemplar window

    source.SetSpan({});  // clear active span
    storage.Add(2, mt::AttributeSpan{attrs});
    const mti::SumData data = storage.Collect();
    ASSERT_EQ(data.points.size(), 1U);
    EXPECT_TRUE(data.points[0].exemplars.empty());  // window was reset
}

TEST(SumStorageTest, CardinalityOverflowRoutesToOverflowSeries)
{
    // limit=2: first two distinct attr sets get their own series; the third
    // and beyond accumulate into the overflow series.
    mts::SumStorage<std::int64_t> storage{true, /*max_cardinality=*/2};
    const std::vector<mt::KeyValue> a{Kv("k", std::string{"a"})};
    const std::vector<mt::KeyValue> b{Kv("k", std::string{"b"})};
    const std::vector<mt::KeyValue> c{Kv("k", std::string{"c"})};

    storage.Add(1, mt::AttributeSpan{a});
    storage.Add(2, mt::AttributeSpan{b});
    storage.Add(3, mt::AttributeSpan{c});  // overflows → overflow series
    storage.Add(4, mt::AttributeSpan{c});  // also overflows → overflow series += 4

    const mti::SumData data = storage.Collect();
    ASSERT_EQ(data.points.size(), 3U);  // a, b, overflow

    const auto ov_it = std::ranges::find_if(data.points, IsOverflowPoint);
    ASSERT_NE(ov_it, data.points.end());
    EXPECT_EQ(std::get<std::int64_t>(ov_it->value), 7);  // 3 + 4
}

// ── Overflow drop accounting (increment 26) ─────────────────────────────────

TEST(SumStorageTest, OverflowIncrementsCardinalityDropCounterPerMeasurement)
{
    mt::testing::FakeDiagnosticsSink sink;
    mts::SumStorage<std::int64_t> storage{true, /*max_cardinality=*/2, nullptr, &sink};
    for (int i = 0; i < 5; ++i)  // 2 real series + 3 folded measurements
    {
        const std::vector<mt::KeyValue> attrs{Kv("k", std::to_string(i))};
        storage.Add(1, mt::AttributeSpan{attrs});
    }

    EXPECT_EQ(CardinalityDrops(sink), 3U);
}

TEST(SumStorageTest, ExistingSeriesUpdatesAfterOverflowDoNotCount)
{
    mt::testing::FakeDiagnosticsSink sink;
    mts::SumStorage<std::int64_t> storage{true, /*max_cardinality=*/2, nullptr, &sink};
    const std::vector<mt::KeyValue> a{Kv("k", std::string{"a"})};
    const std::vector<mt::KeyValue> b{Kv("k", std::string{"b"})};
    const std::vector<mt::KeyValue> c{Kv("k", std::string{"c"})};
    storage.Add(1, mt::AttributeSpan{a});
    storage.Add(2, mt::AttributeSpan{b});
    storage.Add(3, mt::AttributeSpan{c});  // folds → 1 drop

    storage.Add(4, mt::AttributeSpan{a});  // pre-existing series — no drop
    storage.Add(5, mt::AttributeSpan{b});  // pre-existing series — no drop

    EXPECT_EQ(CardinalityDrops(sink), 1U);
}

TEST(SumStorageTest, NullSinkIsSafe)
{
    mts::SumStorage<std::int64_t> storage{true, /*max_cardinality=*/1};  // no sink
    const std::vector<mt::KeyValue> a{Kv("k", std::string{"a"})};
    const std::vector<mt::KeyValue> b{Kv("k", std::string{"b"})};
    storage.Add(1, mt::AttributeSpan{a});
    storage.Add(2, mt::AttributeSpan{b});  // folds — must not crash

    const mti::SumData data = storage.Collect();
    ASSERT_EQ(data.points.size(), 2U);
    EXPECT_EQ(std::ranges::count_if(data.points, IsOverflowPoint), 1);
}

// ── Spec-lock characterisation: cardinality-overflow semantics ─────────────
// These lock the overflow contract in place ahead of drop accounting
// (increment 26); they assert current behaviour and act as a regression net.

TEST(SumStorageTest, NoOverflowAtExactlyLimitDistinctSets)
{
    mts::SumStorage<std::int64_t> storage{true, /*max_cardinality=*/3};
    for (int i = 0; i < 3; ++i)
    {
        const std::vector<mt::KeyValue> attrs{Kv("k", std::to_string(i))};
        storage.Add(1, mt::AttributeSpan{attrs});
    }

    const mti::SumData data = storage.Collect();
    EXPECT_EQ(data.points.size(), 3U);
    EXPECT_EQ(std::ranges::count_if(data.points, IsOverflowPoint), 0);
}

TEST(SumStorageTest, CumulativePreOverflowSeriesKeepExportingAfterOverflow)
{
    mts::SumStorage<std::int64_t> storage{true, /*max_cardinality=*/2};
    const std::vector<mt::KeyValue> a{Kv("k", std::string{"a"})};
    const std::vector<mt::KeyValue> b{Kv("k", std::string{"b"})};
    const std::vector<mt::KeyValue> c{Kv("k", std::string{"c"})};
    storage.Add(1, mt::AttributeSpan{a});
    storage.Add(2, mt::AttributeSpan{b});
    storage.Add(5, mt::AttributeSpan{c});  // folds into the overflow series

    for (int round = 0; round < 2; ++round)
    {
        const mti::SumData data = storage.Collect();
        ASSERT_EQ(data.points.size(), 3U);
        EXPECT_EQ(SortedRealSumValues(data), (std::vector<std::int64_t>{1, 2}));
    }
}

TEST(SumStorageTest, EveryMeasurementReflectedInExactlyOneSeries)
{
    mts::SumStorage<std::int64_t> storage{true, /*max_cardinality=*/2};
    std::int64_t recorded_total = 0;
    for (int i = 0; i < 5; ++i)
    {
        const std::vector<mt::KeyValue> attrs{Kv("k", std::to_string(i))};
        storage.Add(static_cast<std::int64_t>(i) + 1, mt::AttributeSpan{attrs});
        recorded_total += static_cast<std::int64_t>(i) + 1;
    }

    const mti::SumData data = storage.Collect();
    ASSERT_EQ(data.points.size(), 3U);  // two real series + overflow
    std::int64_t exported_total = 0;
    for (const auto& pt : data.points)
    {
        exported_total += std::get<std::int64_t>(pt.value);
    }
    EXPECT_EQ(exported_total, recorded_total);
}

TEST(SumStorageTest, DeltaCollectReclaimsCardinalitySlots)
{
    mts::SumStorage<std::int64_t> storage{true, /*max_cardinality=*/2};
    const std::vector<mt::KeyValue> a{Kv("k", std::string{"a"})};
    const std::vector<mt::KeyValue> b{Kv("k", std::string{"b"})};
    const std::vector<mt::KeyValue> c{Kv("k", std::string{"c"})};
    storage.Add(1, mt::AttributeSpan{a});
    storage.Add(2, mt::AttributeSpan{b});
    storage.Add(3, mt::AttributeSpan{c});                       // folds into the overflow series
    (void)storage.Collect(mti::AggregationTemporality::Delta);  // clears live state

    const std::vector<mt::KeyValue> d{Kv("k", std::string{"d"})};
    storage.Add(4, mt::AttributeSpan{d});

    const mti::SumData data = storage.Collect(mti::AggregationTemporality::Delta);
    ASSERT_EQ(data.points.size(), 1U);
    EXPECT_FALSE(IsOverflowPoint(data.points[0]));  // d got a real series again
}

TEST(SumStorageTest, NonFiniteValueIsDropped)
{
    mts::SumStorage<double> storage{true};
    const std::vector<mt::KeyValue> attrs{Kv("k", std::string{"v"})};
    storage.Add(1.0, mt::AttributeSpan{attrs});
    storage.Add(std::numeric_limits<double>::infinity(), mt::AttributeSpan{attrs});
    storage.Add(std::numeric_limits<double>::quiet_NaN(), mt::AttributeSpan{attrs});
    const mti::SumData data = storage.Collect();
    ASSERT_EQ(data.points.size(), 1U);
    EXPECT_DOUBLE_EQ(std::get<double>(data.points[0].value), 1.0);
}

TEST(SumStorageTest, ConcurrentAddsConserveTotal)
{
    mts::SumStorage<std::int64_t> storage{true};
    const std::vector<mt::KeyValue> attrs{Kv("k", std::string{"v"})};

    constexpr int kThreads = 8;
    constexpr int kPerThread = 1000;
    std::vector<std::thread> workers;
    workers.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t)
    {
        workers.emplace_back(AddOnes, std::ref(storage), mt::AttributeSpan{attrs}, kPerThread);
    }
    for (auto& worker : workers)
    {
        worker.join();
    }

    const mti::SumData data = storage.Collect();
    ASSERT_EQ(data.points.size(), 1U);
    EXPECT_EQ(PointValue<std::int64_t>(data, 0), static_cast<std::int64_t>(kThreads) * kPerThread);
}

// TSAN: concurrent overflow drops reach the diagnostics sink without a data race.
// RecordDrop is called under m_mu so the non-atomic FakeDiagnosticsSink counter
// is sequentially consistent. TSAN verifies no race on either the map or the sink.
TEST(SumStorageTest, ConcurrentOverflowDropAccountingIsRaceFree)
{
    mt::testing::FakeDiagnosticsSink sink;
    // max_cardinality=1: only one real series slot; all others fold.
    mts::SumStorage<std::int64_t> storage{true, /*max_cardinality=*/1, nullptr, &sink};

    constexpr int kThreads = 8;
    constexpr int kPerThread = 200;
    std::vector<std::thread> workers;
    workers.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t)
    {
        workers.emplace_back(AddUniqueKeys, std::ref(storage), t, kPerThread);
    }
    for (auto& worker : workers)
    {
        worker.join();
    }

    // First distinct key claims the real slot; every other distinct new key folds.
    // Total drops == total distinct keys - 1 == (kThreads * kPerThread) - 1.
    constexpr std::uint64_t kTotalAdds = static_cast<std::uint64_t>(kThreads) * kPerThread;
    EXPECT_EQ(CardinalityDrops(sink), kTotalAdds - 1U);
}
