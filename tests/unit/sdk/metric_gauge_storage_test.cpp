// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Behavioural tests for GaugeStorage<T> — last-value aggregation state for
// Gauge instruments (synchronous Gauge / ObservableGauge), M12.
//
// Contract under test:
//  - Record is last-write-wins per attribute set (a gauge is the latest value).
//  - Distinct sets are distinct points; attribute order is insignificant.
//  - Collect snapshots one NumberPoint (value + attributes) per set into
//    GaugeData (no temporality — a gauge is always the last value).
//  - Concurrent Record is safe (per-instrument mutex).

#include "sdk/metric_gauge_storage.hpp"

#include "microtel/attribute.hpp"
#include "microtel/internal/icurrent_span_source.hpp"
#include "microtel/internal/metric_batch.hpp"
#include "microtel/trace.hpp"

#include <gtest/gtest.h>

#include <algorithm>
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

template <typename T>
T PointValue(const mti::GaugeData& data, std::size_t index)
{
    return std::get<T>(data.points.at(index).value);
}

void RecordSame(mts::GaugeStorage<std::int64_t>& storage,
                mt::AttributeSpan attrs,
                std::int64_t value,
                int count)
{
    for (int i = 0; i < count; ++i)
    {
        storage.Record(value, attrs);
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

}  // namespace

TEST(GaugeStorageTest, RecordIsLastWriteWins)
{
    mts::GaugeStorage<std::int64_t> storage;
    const std::vector<mt::KeyValue> attrs{Kv("k", std::string{"v"})};

    storage.Record(5, mt::AttributeSpan{attrs});
    storage.Record(8, mt::AttributeSpan{attrs});
    storage.Record(3, mt::AttributeSpan{attrs});

    const mti::GaugeData data = storage.Collect();
    ASSERT_EQ(data.points.size(), 1U);
    EXPECT_EQ(PointValue<std::int64_t>(data, 0), 3);
}

TEST(GaugeStorageTest, DistinctAttributeSetsAreDistinctPoints)
{
    mts::GaugeStorage<std::int64_t> storage;
    const std::vector<mt::KeyValue> a{Kv("host", std::string{"a"})};
    const std::vector<mt::KeyValue> b{Kv("host", std::string{"b"})};

    storage.Record(1, mt::AttributeSpan{a});
    storage.Record(2, mt::AttributeSpan{b});

    EXPECT_EQ(storage.Collect().points.size(), 2U);
}

TEST(GaugeStorageTest, AttributeOrderDoesNotMatter)
{
    mts::GaugeStorage<std::int64_t> storage;
    const std::vector<mt::KeyValue> ab{Kv("a", std::int64_t{1}), Kv("b", std::int64_t{2})};
    const std::vector<mt::KeyValue> ba{Kv("b", std::int64_t{2}), Kv("a", std::int64_t{1})};

    storage.Record(5, mt::AttributeSpan{ab});
    storage.Record(9, mt::AttributeSpan{ba});  // same set → overwrites

    const mti::GaugeData data = storage.Collect();
    ASSERT_EQ(data.points.size(), 1U);
    EXPECT_EQ(PointValue<std::int64_t>(data, 0), 9);
}

TEST(GaugeStorageTest, DoubleValues)
{
    mts::GaugeStorage<double> storage;
    const std::vector<mt::KeyValue> attrs{Kv("k", std::string{"v"})};

    storage.Record(1.5, mt::AttributeSpan{attrs});
    storage.Record(2.25, mt::AttributeSpan{attrs});

    const mti::GaugeData data = storage.Collect();
    ASSERT_EQ(data.points.size(), 1U);
    EXPECT_DOUBLE_EQ(PointValue<double>(data, 0), 2.25);
}

TEST(GaugeStorageTest, EmptyStorageCollectsNoPoints)
{
    EXPECT_TRUE(mts::GaugeStorage<double>{}.Collect().points.empty());
}

TEST(GaugeStorageTest, PointCarriesItsAttributes)
{
    mts::GaugeStorage<std::int64_t> storage;
    const std::vector<mt::KeyValue> attrs{Kv("host", std::string{"h1"})};
    storage.Record(1, mt::AttributeSpan{attrs});

    const mti::GaugeData data = storage.Collect();
    ASSERT_EQ(data.points.size(), 1U);
    ASSERT_EQ(data.points[0].attributes.size(), 1U);
    EXPECT_EQ(data.points[0].attributes[0].key, "host");
    EXPECT_EQ(std::get<std::string>(data.points[0].attributes[0].value), "h1");
}

TEST(GaugeStorageTest, ExemplarCapturedWhenSampledSpanActive)
{
    FakeSpanSource source;
    source.SetSpan(MakeSampledContext());
    mts::GaugeStorage<std::int64_t> storage{mts::kDefaultMaxCardinality, &source};
    const std::vector<mt::KeyValue> attrs{Kv("k", std::string{"v"})};

    storage.Record(7, mt::AttributeSpan{attrs});

    const mti::GaugeData data = storage.Collect();
    ASSERT_EQ(data.points.size(), 1U);
    ASSERT_EQ(data.points[0].exemplars.size(), 1U);
    EXPECT_TRUE(data.points[0].exemplars[0].span_context.IsValid());
    EXPECT_EQ(std::get<std::int64_t>(data.points[0].exemplars[0].value), 7);
}

TEST(GaugeStorageTest, ExemplarIsResetAfterCollect)
{
    FakeSpanSource source;
    source.SetSpan(MakeSampledContext());
    mts::GaugeStorage<std::int64_t> storage{mts::kDefaultMaxCardinality, &source};
    const std::vector<mt::KeyValue> attrs{Kv("k", std::string{"v"})};

    storage.Record(1, mt::AttributeSpan{attrs});
    (void)storage.Collect();  // drains exemplar window

    source.SetSpan({});
    storage.Record(2, mt::AttributeSpan{attrs});
    const mti::GaugeData data = storage.Collect();
    ASSERT_EQ(data.points.size(), 1U);
    EXPECT_TRUE(data.points[0].exemplars.empty());
}

TEST(GaugeStorageTest, CardinalityOverflowRoutesToOverflowSeries)
{
    mts::GaugeStorage<std::int64_t> storage{/*max_cardinality=*/2};
    const std::vector<mt::KeyValue> a{Kv("k", std::string{"a"})};
    const std::vector<mt::KeyValue> b{Kv("k", std::string{"b"})};
    const std::vector<mt::KeyValue> c{Kv("k", std::string{"c"})};

    storage.Record(1, mt::AttributeSpan{a});
    storage.Record(2, mt::AttributeSpan{b});
    storage.Record(3, mt::AttributeSpan{c});  // overflows → overflow series
    storage.Record(9, mt::AttributeSpan{c});  // also overflows → last write = 9

    const mti::GaugeData data = storage.Collect();
    ASSERT_EQ(data.points.size(), 3U);

    const auto ov_it = std::ranges::find_if(data.points, IsOverflowPoint);
    ASSERT_NE(ov_it, data.points.end());
    EXPECT_EQ(std::get<std::int64_t>(ov_it->value), 9);  // last write wins
}

TEST(GaugeStorageTest, NonFiniteValueIsDropped)
{
    mts::GaugeStorage<double> storage;
    const std::vector<mt::KeyValue> attrs{Kv("k", std::string{"v"})};
    storage.Record(5.0, mt::AttributeSpan{attrs});
    storage.Record(std::numeric_limits<double>::quiet_NaN(), mt::AttributeSpan{attrs});
    const mti::GaugeData data = storage.Collect();
    ASSERT_EQ(data.points.size(), 1U);
    EXPECT_DOUBLE_EQ(std::get<double>(data.points[0].value), 5.0);
}

TEST(GaugeStorageTest, ConcurrentRecordsAreSafe)
{
    mts::GaugeStorage<std::int64_t> storage;
    const std::vector<mt::KeyValue> attrs{Kv("k", std::string{"v"})};

    constexpr int kThreads = 8;
    constexpr int kPerThread = 1000;
    constexpr std::int64_t kValue = 7;
    std::vector<std::thread> workers;
    workers.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t)
    {
        workers.emplace_back(
            RecordSame, std::ref(storage), mt::AttributeSpan{attrs}, kValue, kPerThread);
    }
    for (auto& worker : workers)
    {
        worker.join();
    }

    // All writers store the same value, so the last value is deterministic.
    const mti::GaugeData data = storage.Collect();
    ASSERT_EQ(data.points.size(), 1U);
    EXPECT_EQ(PointValue<std::int64_t>(data, 0), kValue);
}
