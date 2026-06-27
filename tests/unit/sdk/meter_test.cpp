// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Behavioural tests for the Meter class — the M12 instrument factory.
//
// Contract under test:
//  - Counter<T>::Add() accumulates into SumStorage (monotonic=true).
//  - UpDownCounter<T>::Add() accumulates into SumStorage (monotonic=false).
//  - Gauge<T>::Record() uses last-write-wins GaugeStorage.
//  - Histogram<T>::Record() uses HistogramStorage.
//  - All instruments created on one Meter appear under one scope entry.
//  - Instrument name/description/unit propagate into MetricRecord.
//  - Histogram with default boundaries yields the OTel 15-boundary ladder
//    (16 buckets per attribute set).
//  - Histogram with custom boundaries is accepted.

#include "sdk/meter.hpp"

#include "microtel/attribute.hpp"
#include "microtel/internal/metric_batch.hpp"
#include "microtel/resource.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <string>
#include <variant>
#include <vector>

namespace mts = microtel::sdk;
namespace mti = microtel::internal;
namespace mt = microtel;

namespace
{

std::shared_ptr<mts::MetricProducer> MakeProducer()
{
    return std::make_shared<mts::MetricProducer>(std::make_shared<const mt::Resource>());
}

mti::InstrumentationScope MakeScope(std::string name = "lib", std::string version = "1.0")
{
    return mti::InstrumentationScope{.name = std::move(name), .version = std::move(version)};
}

mt::KeyValue Kv(std::string key, mt::AttributeValue value)
{
    return mt::KeyValue{.key = std::move(key), .value = std::move(value)};
}

}  // namespace

// ── Counter ───────────────────────────────────────────────────────────────────

TEST(MeterTest, CounterAddAccumulatesValue)
{
    auto producer = MakeProducer();
    mts::Meter meter{MakeScope(), producer};
    auto counter = meter.CreateCounter<std::int64_t>("req.count", "Total requests", "1");

    const std::vector<mt::KeyValue> attrs{Kv("k", std::string{"v"})};
    counter.Add(3, mt::AttributeSpan{attrs});
    counter.Add(5, mt::AttributeSpan{attrs});

    const auto handles = producer->Collect();
    ASSERT_EQ(handles.size(), 1U);
    ASSERT_EQ(handles[0].Metrics().size(), 1U);
    const auto& sum = std::get<mti::SumData>(handles[0].Metrics()[0].data);
    ASSERT_EQ(sum.points.size(), 1U);
    EXPECT_EQ(std::get<std::int64_t>(sum.points[0].value), 8);
    EXPECT_TRUE(sum.is_monotonic);
}

// ── UpDownCounter ─────────────────────────────────────────────────────────────

TEST(MeterTest, UpDownCounterAllowsNegativeAdd)
{
    auto producer = MakeProducer();
    mts::Meter meter{MakeScope(), producer};
    auto c = meter.CreateUpDownCounter<std::int64_t>("q.depth", "", "1");

    const std::vector<mt::KeyValue> attrs{Kv("k", std::string{"v"})};
    c.Add(10, mt::AttributeSpan{attrs});
    c.Add(-3, mt::AttributeSpan{attrs});

    const auto handles = producer->Collect();
    const auto& sum = std::get<mti::SumData>(handles[0].Metrics()[0].data);
    EXPECT_EQ(std::get<std::int64_t>(sum.points[0].value), 7);
    EXPECT_FALSE(sum.is_monotonic);
}

// ── Gauge ─────────────────────────────────────────────────────────────────────

TEST(MeterTest, GaugeRecordLastWriteWins)
{
    auto producer = MakeProducer();
    mts::Meter meter{MakeScope(), producer};
    auto g = meter.CreateGauge<double>("cpu.util", "", "1");

    const std::vector<mt::KeyValue> attrs{Kv("k", std::string{"v"})};
    g.Record(0.3, mt::AttributeSpan{attrs});
    g.Record(0.7, mt::AttributeSpan{attrs});

    const auto handles = producer->Collect();
    const auto& gd = std::get<mti::GaugeData>(handles[0].Metrics()[0].data);
    ASSERT_EQ(gd.points.size(), 1U);
    EXPECT_DOUBLE_EQ(std::get<double>(gd.points[0].value), 0.7);
}

// ── Histogram ─────────────────────────────────────────────────────────────────

TEST(MeterTest, HistogramRecordsMeasurement)
{
    auto producer = MakeProducer();
    mts::Meter meter{MakeScope(), producer};
    auto h = meter.CreateHistogram<double>("latency", "", "ms");

    const std::vector<mt::KeyValue> attrs{Kv("k", std::string{"v"})};
    h.Record(42.0, mt::AttributeSpan{attrs});

    const auto handles = producer->Collect();
    const auto& hd = std::get<mti::HistogramData>(handles[0].Metrics()[0].data);
    ASSERT_EQ(hd.points.size(), 1U);
    EXPECT_EQ(hd.points[0].count, 1U);
    EXPECT_DOUBLE_EQ(hd.points[0].sum, 42.0);
}

TEST(MeterTest, HistogramDefaultBoundariesYieldSixteenBuckets)
{
    auto producer = MakeProducer();
    mts::Meter meter{MakeScope(), producer};
    auto h = meter.CreateHistogram<double>("latency", "", "ms");

    const std::vector<mt::KeyValue> attrs{Kv("k", std::string{"v"})};
    h.Record(1.0, mt::AttributeSpan{attrs});

    const auto handles = producer->Collect();
    const auto& hd = std::get<mti::HistogramData>(handles[0].Metrics()[0].data);
    // OTel default: 15 boundaries → 16 buckets.
    ASSERT_EQ(hd.points.size(), 1U);
    EXPECT_EQ(hd.points[0].bucket_counts.size(), 16U);
}

TEST(MeterTest, HistogramWithCustomBoundariesRoutesCorrectly)
{
    auto producer = MakeProducer();
    mts::Meter meter{MakeScope(), producer};
    auto h =
        meter.CreateHistogram<std::int64_t>("latency", "", "ms", std::vector<double>{10.0, 100.0});

    const std::vector<mt::KeyValue> attrs{Kv("k", std::string{"v"})};
    h.Record(5, mt::AttributeSpan{attrs});  // falls in first bucket [0, 10)

    const auto handles = producer->Collect();
    const auto& hd = std::get<mti::HistogramData>(handles[0].Metrics()[0].data);
    // 2 boundaries → 3 buckets; value=5 is below 10, so bucket_counts[0] == 1.
    ASSERT_GE(hd.points[0].bucket_counts.size(), 1U);
    EXPECT_EQ(hd.points[0].bucket_counts[0], 1U);
}

// ── Scope grouping and metadata ───────────────────────────────────────────────

TEST(MeterTest, AllInstrumentsAppearUnderOneScopeEntry)
{
    auto producer = MakeProducer();
    mts::Meter meter{MakeScope("my.lib", "2.0"), producer};
    (void)meter.CreateCounter<std::int64_t>("a", "", "1");
    (void)meter.CreateGauge<double>("b", "", "1");

    const auto handles = producer->Collect();
    ASSERT_EQ(handles.size(), 1U);
    EXPECT_EQ(handles[0].Metrics().size(), 2U);
    EXPECT_EQ(handles[0].Scope().name, "my.lib");
    EXPECT_EQ(handles[0].Scope().version, "2.0");
}

TEST(MeterTest, InstrumentNameDescriptionUnitPropagateIntoMetricRecord)
{
    auto producer = MakeProducer();
    mts::Meter meter{MakeScope(), producer};
    (void)meter.CreateCounter<std::int64_t>("my.counter", "My counter", "reqs");

    const auto handles = producer->Collect();
    EXPECT_EQ(handles[0].Metrics()[0].name, "my.counter");
    EXPECT_EQ(handles[0].Metrics()[0].description, "My counter");
    EXPECT_EQ(handles[0].Metrics()[0].unit, "reqs");
}

TEST(MeterTest, TwoMetersWithDifferentScopesYieldTwoHandles)
{
    auto producer = MakeProducer();
    mts::Meter meter_a{MakeScope("lib-a"), producer};
    mts::Meter meter_b{MakeScope("lib-b"), producer};
    (void)meter_a.CreateCounter<std::int64_t>("x", "", "1");
    (void)meter_b.CreateCounter<std::int64_t>("y", "", "1");

    EXPECT_EQ(producer->Collect().size(), 2U);
}

TEST(MeterTest, DoubleCounterAccumulatesFloatingPoint)
{
    auto producer = MakeProducer();
    mts::Meter meter{MakeScope(), producer};
    auto counter = meter.CreateCounter<double>("bytes", "", "By");

    const std::vector<mt::KeyValue> attrs{Kv("k", std::string{"v"})};
    counter.Add(1.5, mt::AttributeSpan{attrs});
    counter.Add(2.5, mt::AttributeSpan{attrs});

    const auto handles = producer->Collect();
    const auto& sum = std::get<mti::SumData>(handles[0].Metrics()[0].data);
    EXPECT_DOUBLE_EQ(std::get<double>(sum.points[0].value), 4.0);
}
