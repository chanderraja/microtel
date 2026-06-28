// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Behavioural tests for the observable instrument stream classes and for
// Meter::CreateObservable*<T>() factory methods.
//
// Contract under test:
//  - MetricStreamObservableSum::Collect() invokes the callback, converts
//    observations to SumData, and resets between calls.
//  - MetricStreamObservableGauge::Collect() does the same, yielding GaugeData.
//  - Monotonic / non-monotonic flag propagates via is_monotonic.
//  - Temporality is forwarded into SumData.temporality.
//  - Multiple Observe() calls within one cycle produce multiple NumberPoints.
//  - A second Collect() sees only the second callback's observations.
//  - Meter::CreateObservableCounter/UpDownCounter/Gauge register streams and
//    have them appear in MetricProducer::Collect().

#include "sdk/metric_observable_instruments.hpp"

#include "microtel/attribute.hpp"
#include "microtel/internal/metric_batch.hpp"
#include "microtel/resource.hpp"

#include "sdk/meter.hpp"

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

mt::KeyValue Kv(std::string key, mt::AttributeValue value)
{
    return mt::KeyValue{.key = std::move(key), .value = std::move(value)};
}

std::vector<mt::KeyValue> Attrs(std::string key, mt::AttributeValue value)
{
    return {Kv(std::move(key), std::move(value))};
}

std::shared_ptr<mts::MetricProducer> MakeProducer()
{
    return std::make_shared<mts::MetricProducer>(std::make_shared<const mt::Resource>());
}

mti::InstrumentationScope MakeScope(std::string name = "lib")
{
    return mti::InstrumentationScope{.name = std::move(name), .version = "1.0"};
}

}  // namespace

// ── MetricStreamObservableSum ─────────────────────────────────────────────────

TEST(MetricStreamObservableSumTest, CallbackInvokedOnCollect)
{
    bool called = false;
    mts::MetricStreamObservableSum<std::int64_t> stream{
        "req.total",
        "",
        "1",
        /*monotonic=*/true,
        [&called](mts::ObservableResult<std::int64_t>& result)
        {
            called = true;
            const auto attrs = Attrs("k", std::string{"v"});
            result.Observe(42, mt::AttributeSpan{attrs});
        }};

    (void)stream.Collect(mti::AggregationTemporality::Cumulative);
    EXPECT_TRUE(called);
}

TEST(MetricStreamObservableSumTest, SumDataIsMonotonicForCounter)
{
    mts::MetricStreamObservableSum<std::int64_t> stream{
        "c",
        "",
        "1",
        /*monotonic=*/true,
        [](mts::ObservableResult<std::int64_t>&) {}};

    const auto rec = stream.Collect(mti::AggregationTemporality::Cumulative);
    EXPECT_TRUE(std::get<mti::SumData>(rec.data).is_monotonic);
}

TEST(MetricStreamObservableSumTest, SumDataIsNonMonotonicForUpDownCounter)
{
    mts::MetricStreamObservableSum<std::int64_t> stream{
        "c",
        "",
        "1",
        /*monotonic=*/false,
        [](mts::ObservableResult<std::int64_t>&) {}};

    const auto rec = stream.Collect(mti::AggregationTemporality::Cumulative);
    EXPECT_FALSE(std::get<mti::SumData>(rec.data).is_monotonic);
}

TEST(MetricStreamObservableSumTest, ObservationAppearsAsNumberPoint)
{
    const auto attrs = Attrs("host", std::string{"web-01"});
    mts::MetricStreamObservableSum<std::int64_t> stream{
        "req.total",
        "Total requests",
        "1",
        /*monotonic=*/true,
        [&attrs](mts::ObservableResult<std::int64_t>& result)
        { result.Observe(7, mt::AttributeSpan{attrs}); }};

    const auto rec = stream.Collect(mti::AggregationTemporality::Cumulative);
    const auto& sum = std::get<mti::SumData>(rec.data);
    ASSERT_EQ(sum.points.size(), 1U);
    EXPECT_EQ(std::get<std::int64_t>(sum.points[0].value), 7);
}

TEST(MetricStreamObservableSumTest, MultipleObservationsYieldMultiplePoints)
{
    const auto a1 = Attrs("host", std::string{"web-01"});
    const auto a2 = Attrs("host", std::string{"web-02"});
    mts::MetricStreamObservableSum<std::int64_t> stream{
        "c",
        "",
        "1",
        /*monotonic=*/true,
        [&a1, &a2](mts::ObservableResult<std::int64_t>& result)
        {
            result.Observe(1, mt::AttributeSpan{a1});
            result.Observe(2, mt::AttributeSpan{a2});
        }};

    const auto rec = stream.Collect(mti::AggregationTemporality::Cumulative);
    EXPECT_EQ(std::get<mti::SumData>(rec.data).points.size(), 2U);
}

TEST(MetricStreamObservableSumTest, CallbackResultResetBetweenCollects)
{
    int calls = 0;
    const auto attrs = Attrs("k", std::string{"v"});
    mts::MetricStreamObservableSum<std::int64_t> stream{
        "c",
        "",
        "1",
        /*monotonic=*/true,
        [&calls, &attrs](mts::ObservableResult<std::int64_t>& result)
        {
            ++calls;
            // First call: observe 10; second call: observe 20.
            result.Observe(calls == 1 ? 10 : 20, mt::AttributeSpan{attrs});
        }};

    (void)stream.Collect(mti::AggregationTemporality::Cumulative);
    const auto rec = stream.Collect(mti::AggregationTemporality::Cumulative);
    const auto& sum = std::get<mti::SumData>(rec.data);
    ASSERT_EQ(sum.points.size(), 1U);
    EXPECT_EQ(std::get<std::int64_t>(sum.points[0].value), 20);
}

TEST(MetricStreamObservableSumTest, TemporalityForwardedIntoSumData)
{
    mts::MetricStreamObservableSum<std::int64_t> stream{
        "c", "", "1", true, [](mts::ObservableResult<std::int64_t>&) {}};

    const auto rec = stream.Collect(mti::AggregationTemporality::Delta);
    EXPECT_EQ(std::get<mti::SumData>(rec.data).temporality, mti::AggregationTemporality::Delta);
}

// ── MetricStreamObservableGauge ───────────────────────────────────────────────

TEST(MetricStreamObservableGaugeTest, ObservationAppearsAsGaugePoint)
{
    const auto attrs = Attrs("sensor", std::string{"therm-1"});
    mts::MetricStreamObservableGauge<double> stream{
        "room.temp", "Room temperature", "Cel", [&attrs](auto& result) {
            result.Observe(21.5, mt::AttributeSpan{attrs});
        }};

    const auto rec = stream.Collect(mti::AggregationTemporality::Cumulative);
    const auto& gd = std::get<mti::GaugeData>(rec.data);
    ASSERT_EQ(gd.points.size(), 1U);
    EXPECT_DOUBLE_EQ(std::get<double>(gd.points[0].value), 21.5);
}

TEST(MetricStreamObservableGaugeTest, MultipleObservationsYieldMultiplePoints)
{
    const auto a1 = Attrs("sensor", std::string{"s1"});
    const auto a2 = Attrs("sensor", std::string{"s2"});
    mts::MetricStreamObservableGauge<double> stream{
        "temp",
        "",
        "Cel",
        [&a1, &a2](mts::ObservableResult<double>& result)
        {
            result.Observe(20.0, mt::AttributeSpan{a1});
            result.Observe(25.0, mt::AttributeSpan{a2});
        }};

    const auto rec = stream.Collect(mti::AggregationTemporality::Cumulative);
    EXPECT_EQ(std::get<mti::GaugeData>(rec.data).points.size(), 2U);
}

TEST(MetricStreamObservableGaugeTest, CallbackResultResetBetweenCollects)
{
    int calls = 0;
    const auto attrs = Attrs("k", std::string{"v"});
    mts::MetricStreamObservableGauge<double> stream{
        "g",
        "",
        "1",
        [&calls, &attrs](mts::ObservableResult<double>& result)
        {
            ++calls;
            result.Observe(calls == 1 ? 1.0 : 2.0, mt::AttributeSpan{attrs});
        }};

    (void)stream.Collect(mti::AggregationTemporality::Cumulative);
    const auto rec = stream.Collect(mti::AggregationTemporality::Cumulative);
    const auto& gd = std::get<mti::GaugeData>(rec.data);
    ASSERT_EQ(gd.points.size(), 1U);
    EXPECT_DOUBLE_EQ(std::get<double>(gd.points[0].value), 2.0);
}

// ── Meter integration ─────────────────────────────────────────────────────────

TEST(MeterObservableTest, CreateObservableCounterRegistersWithProducer)
{
    auto producer = MakeProducer();
    mts::Meter meter{MakeScope(), producer};
    const auto attrs = Attrs("k", std::string{"v"});
    (void)meter.CreateObservableCounter<std::int64_t>(
        "req.total",
        "",
        "1",
        [&attrs](mts::ObservableResult<std::int64_t>& result)
        { result.Observe(5, mt::AttributeSpan{attrs}); });

    const auto handles = producer->Collect();
    ASSERT_EQ(handles.size(), 1U);
    ASSERT_EQ(handles[0].Metrics().size(), 1U);
    const auto& sum = std::get<mti::SumData>(handles[0].Metrics()[0].data);
    EXPECT_TRUE(sum.is_monotonic);
    ASSERT_EQ(sum.points.size(), 1U);
    EXPECT_EQ(std::get<std::int64_t>(sum.points[0].value), 5);
}

TEST(MeterObservableTest, CreateObservableUpDownCounterNonMonotonic)
{
    auto producer = MakeProducer();
    mts::Meter meter{MakeScope(), producer};
    (void)meter.CreateObservableUpDownCounter<std::int64_t>(
        "q.depth", "", "1", [](mts::ObservableResult<std::int64_t>&) {});

    const auto handles = producer->Collect();
    const auto& sum = std::get<mti::SumData>(handles[0].Metrics()[0].data);
    EXPECT_FALSE(sum.is_monotonic);
}

TEST(MeterObservableTest, CreateObservableGaugeYieldsGaugeData)
{
    auto producer = MakeProducer();
    mts::Meter meter{MakeScope(), producer};
    const auto attrs = Attrs("sensor", std::string{"s1"});
    (void)meter.CreateObservableGauge<double>("room.temp",
                                              "",
                                              "Cel",
                                              [&attrs](mts::ObservableResult<double>& result)
                                              { result.Observe(22.0, mt::AttributeSpan{attrs}); });

    const auto handles = producer->Collect();
    const auto& gd = std::get<mti::GaugeData>(handles[0].Metrics()[0].data);
    ASSERT_EQ(gd.points.size(), 1U);
    EXPECT_DOUBLE_EQ(std::get<double>(gd.points[0].value), 22.0);
}
