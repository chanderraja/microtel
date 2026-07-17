// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// TDD tests for per-view attribute_allowlist filtering on observable instruments
// — M13 increment 4.
//
// Contract under test:
//  - No allowlist: observable callbacks receive an adapter that forwards all
//    attribute keys unchanged to the underlying ObservableResult.
//  - allowlist={"k1"}: the adapter strips unlisted keys before forwarding.
//    Two Observe() calls that differ only on k2 produce one data point (same
//    AttributeSet after filtering → second value overwrites first in the map).
//  - allowlist=[]: all keys stripped; every Observe() collapses to the empty
//    attribute set, last value wins.
//  - Fan-out: two views with different allowlists produce two independently-
//    filtered streams under the same scope.
//  - ObservableGauge and ObservableUpDownCounter are smoke-tested.
//  - double-type templates are exercised.
//
// Test strategy: invoke producer->Collect() to trigger callbacks, then inspect
// the resulting NumberPoints. Collapsing is verified by point-count (two
// observations that share the same filtered AttributeSet → 1 point); filtering
// is verified by checking the attributes present on the surviving point.

#include "microtel/attribute.hpp"
#include "microtel/internal/metric_batch.hpp"
#include "microtel/resource.hpp"
#include "microtel/view.hpp"

#include "sdk/metric_producer.hpp"
#include "sdk/sdk_meter.hpp"
#include "sdk/view_registry.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <variant>
#include <vector>

namespace mt = microtel;
namespace mts = microtel::sdk;
namespace mti = microtel::internal;

namespace
{

std::shared_ptr<mts::MetricProducer> MakeProducer()
{
    return std::make_shared<mts::MetricProducer>(std::make_shared<const mt::Resource>());
}

mti::InstrumentationScope MakeScope()
{
    return {.name = "test.lib", .version = "1.0"};
}

std::shared_ptr<mts::SdkMeter> MakeMeter(std::shared_ptr<mts::MetricProducer> producer,
                                         std::shared_ptr<const mts::ViewRegistry> registry)
{
    return std::make_shared<mts::SdkMeter>(MakeScope(),
                                           std::move(producer),
                                           /*max_cardinality=*/128,
                                           /*diag=*/nullptr,
                                           std::move(registry));
}

mt::KeyValue Kv(std::string key, std::string value)
{
    return mt::KeyValue{.key = std::move(key), .value = mt::AttributeValue{std::move(value)}};
}

// Returns all NumberPoints from the first (and typically only) metric record
// in the first scope handle returned by producer->Collect().
std::vector<mti::NumberPoint> CollectPoints(mts::MetricProducer& producer)
{
    const auto handles = producer.Collect();
    if (handles.empty())
    {
        return {};
    }
    const auto& metrics = handles[0].Metrics();
    if (metrics.empty())
    {
        return {};
    }
    return std::visit(
        [](const auto& data) -> std::vector<mti::NumberPoint>
        {
            using D = std::decay_t<decltype(data)>;
            if constexpr (std::is_same_v<D, mti::SumData> || std::is_same_v<D, mti::GaugeData>)
            {
                return data.points;
            }
            return {};
        },
        metrics[0].data);
}

}  // namespace

// ── No allowlist ──────────────────────────────────────────────────────────────

TEST(ObservableAllowlistTest, NoAllowlist_AllAttrsPassThrough)
{
    auto producer = MakeProducer();
    auto reg = std::make_shared<mts::ViewRegistry>();
    reg->Add(mt::ViewConfig{
        .selector = {.name = "req.total"},
        .transform = {.name = "req.total"},  // rename only, no allowlist
    });
    auto meter = MakeMeter(producer, reg);

    const auto a1 = std::vector{Kv("k1", "a"), Kv("k2", "x")};
    const auto a2 = std::vector{Kv("k1", "b"), Kv("k2", "y")};
    (void)meter->CreateObservableCounter<std::int64_t>(
        "req.total",
        "",
        "1",
        [&a1, &a2](mt::ObservableResult<std::int64_t>& r)
        {
            r.Observe(10, mt::AttributeSpan{a1});
            r.Observe(20, mt::AttributeSpan{a2});
        });

    const auto pts = CollectPoints(*producer);
    ASSERT_EQ(pts.size(), 2U);
}

// ── Allowlist collapses distinct sets ────────────────────────────────────────

TEST(ObservableAllowlistTest, Allowlist_CollapsesTwoObservationsIntoOnePoint)
{
    auto producer = MakeProducer();
    auto reg = std::make_shared<mts::ViewRegistry>();
    reg->Add(mt::ViewConfig{
        .selector = {.name = "req.total"},
        .transform = {.attribute_allowlist = std::vector<std::string>{"k1"}},
    });
    auto meter = MakeMeter(producer, reg);

    // Two observations differ only on k2; after filtering to {k1}, same set.
    // Second Observe() overwrites the first in the ObservableResult map.
    const auto a1 = std::vector{Kv("k1", "a"), Kv("k2", "x")};
    const auto a2 = std::vector{Kv("k1", "a"), Kv("k2", "y")};
    (void)meter->CreateObservableCounter<std::int64_t>(
        "req.total",
        "",
        "1",
        [&a1, &a2](mt::ObservableResult<std::int64_t>& r)
        {
            r.Observe(10, mt::AttributeSpan{a1});
            r.Observe(20, mt::AttributeSpan{a2});
        });

    const auto pts = CollectPoints(*producer);
    ASSERT_EQ(pts.size(), 1U);
    EXPECT_EQ(std::get<std::int64_t>(pts[0].value), 20);
    ASSERT_EQ(pts[0].attributes.size(), 1U);
    EXPECT_EQ(pts[0].attributes[0].key, "k1");
}

// ── Allowlist keeps distinct sets that differ on listed key ──────────────────

TEST(ObservableAllowlistTest, Allowlist_KeepsDistinctSetsWhenListedKeysDiffer)
{
    auto producer = MakeProducer();
    auto reg = std::make_shared<mts::ViewRegistry>();
    reg->Add(mt::ViewConfig{
        .selector = {.name = "req.total"},
        .transform = {.attribute_allowlist = std::vector<std::string>{"k1"}},
    });
    auto meter = MakeMeter(producer, reg);

    // k1 values differ → two distinct filtered sets → two points.
    const auto a1 = std::vector{Kv("k1", "a"), Kv("k2", "same")};
    const auto a2 = std::vector{Kv("k1", "b"), Kv("k2", "same")};
    (void)meter->CreateObservableCounter<std::int64_t>(
        "req.total",
        "",
        "1",
        [&a1, &a2](mt::ObservableResult<std::int64_t>& r)
        {
            r.Observe(10, mt::AttributeSpan{a1});
            r.Observe(20, mt::AttributeSpan{a2});
        });

    const auto pts = CollectPoints(*producer);
    ASSERT_EQ(pts.size(), 2U);
}

// ── Empty allowlist strips all keys ──────────────────────────────────────────

TEST(ObservableAllowlistTest, EmptyAllowlist_AllAttrsStripped)
{
    auto producer = MakeProducer();
    auto reg = std::make_shared<mts::ViewRegistry>();
    reg->Add(mt::ViewConfig{
        .selector = {.name = "g"},
        .transform = {.attribute_allowlist = std::vector<std::string>{}},
    });
    auto meter = MakeMeter(producer, reg);

    const auto a1 = std::vector{Kv("k1", "a")};
    const auto a2 = std::vector{Kv("k1", "b")};
    const auto a3 = std::vector{Kv("k1", "c")};
    (void)meter->CreateObservableCounter<std::int64_t>(
        "g",
        "",
        "1",
        [&a1, &a2, &a3](mt::ObservableResult<std::int64_t>& r)
        {
            r.Observe(1, mt::AttributeSpan{a1});
            r.Observe(2, mt::AttributeSpan{a2});
            r.Observe(3, mt::AttributeSpan{a3});
        });

    const auto pts = CollectPoints(*producer);
    ASSERT_EQ(pts.size(), 1U);
    EXPECT_TRUE(pts[0].attributes.empty());
    EXPECT_EQ(std::get<std::int64_t>(pts[0].value), 3);
}

// ── Multi-key allowlist ───────────────────────────────────────────────────────

TEST(ObservableAllowlistTest, MultiKeyAllowlist_UnlistedKeyDropped)
{
    auto producer = MakeProducer();
    auto reg = std::make_shared<mts::ViewRegistry>();
    reg->Add(mt::ViewConfig{
        .selector = {.name = "m"},
        .transform = {.attribute_allowlist = std::vector<std::string>{"k1", "k3"}},
    });
    auto meter = MakeMeter(producer, reg);

    // k2 differs between the two; k1 and k3 agree → same filtered set → 1 point.
    const auto a1 = std::vector{Kv("k1", "a"), Kv("k2", "x"), Kv("k3", "z")};
    const auto a2 = std::vector{Kv("k1", "a"), Kv("k2", "y"), Kv("k3", "z")};
    (void)meter->CreateObservableCounter<std::int64_t>(
        "m",
        "",
        "1",
        [&a1, &a2](mt::ObservableResult<std::int64_t>& r)
        {
            r.Observe(5, mt::AttributeSpan{a1});
            r.Observe(9, mt::AttributeSpan{a2});
        });

    const auto pts = CollectPoints(*producer);
    ASSERT_EQ(pts.size(), 1U);
    ASSERT_EQ(pts[0].attributes.size(), 2U);
    EXPECT_EQ(std::get<std::int64_t>(pts[0].value), 9);
}

// ── Fan-out: two views, independent allowlists ────────────────────────────────

TEST(ObservableAllowlistTest, FanOut_IndependentAllowlists)
{
    auto producer = MakeProducer();
    auto reg = std::make_shared<mts::ViewRegistry>();
    // View 1: rename to "filtered", allowlist={"k1"} → 1 point (k2 differs → collapse)
    reg->Add(mt::ViewConfig{
        .selector = {.name = "c"},
        .transform = {.name = "filtered", .attribute_allowlist = std::vector<std::string>{"k1"}},
    });
    // View 2: rename to "full", no allowlist → 2 distinct points
    reg->Add(mt::ViewConfig{
        .selector = {.name = "c"},
        .transform = {.name = "full"},
    });
    auto meter = MakeMeter(producer, reg);

    const auto a1 = std::vector{Kv("k1", "a"), Kv("k2", "x")};
    const auto a2 = std::vector{Kv("k1", "a"), Kv("k2", "y")};
    (void)meter->CreateObservableCounter<std::int64_t>(
        "c",
        "",
        "1",
        [&a1, &a2](mt::ObservableResult<std::int64_t>& r)
        {
            r.Observe(10, mt::AttributeSpan{a1});
            r.Observe(20, mt::AttributeSpan{a2});
        });

    const auto handles = producer->Collect();
    ASSERT_EQ(handles.size(), 1U);
    const auto& metrics = handles[0].Metrics();
    ASSERT_EQ(metrics.size(), 2U);

    // Find the filtered stream (name="filtered") — should have 1 point.
    const auto filtered_it = std::ranges::find_if(
        metrics, [](const mti::MetricRecord& r) { return r.name == "filtered"; });
    ASSERT_NE(filtered_it, metrics.end());
    EXPECT_EQ(std::get<mti::SumData>(filtered_it->data).points.size(), 1U);

    // Find the full stream (name="full") — should have 2 points.
    const auto full_it =
        std::ranges::find_if(metrics, [](const mti::MetricRecord& r) { return r.name == "full"; });
    ASSERT_NE(full_it, metrics.end());
    EXPECT_EQ(std::get<mti::SumData>(full_it->data).points.size(), 2U);
}

// ── ObservableGauge smoke ────────────────────────────────────────────────────

TEST(ObservableAllowlistTest, ObservableGauge_AllowlistFiltersKeys)
{
    auto producer = MakeProducer();
    auto reg = std::make_shared<mts::ViewRegistry>();
    reg->Add(mt::ViewConfig{
        .selector = {.name = "temp"},
        .transform = {.attribute_allowlist = std::vector<std::string>{"region"}},
    });
    auto meter = MakeMeter(producer, reg);

    // Two observations agree on region, differ on host → collapse after filtering.
    const auto a1 = std::vector{Kv("region", "us-east"), Kv("host", "web-01")};
    const auto a2 = std::vector{Kv("region", "us-east"), Kv("host", "web-02")};
    (void)meter->CreateObservableGauge<double>("temp",
                                               "",
                                               "Cel",
                                               [&a1, &a2](mt::ObservableResult<double>& r)
                                               {
                                                   r.Observe(21.0, mt::AttributeSpan{a1});
                                                   r.Observe(22.0, mt::AttributeSpan{a2});
                                               });

    const auto handles = producer->Collect();
    ASSERT_EQ(handles.size(), 1U);
    const auto& gd = std::get<mti::GaugeData>(handles[0].Metrics()[0].data);
    ASSERT_EQ(gd.points.size(), 1U);
    EXPECT_DOUBLE_EQ(std::get<double>(gd.points[0].value), 22.0);
    ASSERT_EQ(gd.points[0].attributes.size(), 1U);
    EXPECT_EQ(gd.points[0].attributes[0].key, "region");
}

// ── ObservableUpDownCounter smoke ────────────────────────────────────────────

TEST(ObservableAllowlistTest, ObservableUpDownCounter_AllowlistFiltersKeys)
{
    auto producer = MakeProducer();
    auto reg = std::make_shared<mts::ViewRegistry>();
    reg->Add(mt::ViewConfig{
        .selector = {.name = "q.depth"},
        .transform = {.attribute_allowlist = std::vector<std::string>{"queue"}},
    });
    auto meter = MakeMeter(producer, reg);

    const auto a1 = std::vector{Kv("queue", "high"), Kv("worker", "w1")};
    const auto a2 = std::vector{Kv("queue", "high"), Kv("worker", "w2")};
    (void)meter->CreateObservableUpDownCounter<std::int64_t>(
        "q.depth",
        "",
        "1",
        [&a1, &a2](mt::ObservableResult<std::int64_t>& r)
        {
            r.Observe(5, mt::AttributeSpan{a1});
            r.Observe(7, mt::AttributeSpan{a2});
        });

    const auto pts = CollectPoints(*producer);
    ASSERT_EQ(pts.size(), 1U);
    EXPECT_EQ(std::get<std::int64_t>(pts[0].value), 7);
}

// ── double type ──────────────────────────────────────────────────────────────

TEST(ObservableAllowlistTest, DoubleType_AllowlistFiltersKeys)
{
    auto producer = MakeProducer();
    auto reg = std::make_shared<mts::ViewRegistry>();
    reg->Add(mt::ViewConfig{
        .selector = {.name = "cpu"},
        .transform = {.attribute_allowlist = std::vector<std::string>{"host"}},
    });
    auto meter = MakeMeter(producer, reg);

    const auto a1 = std::vector{Kv("host", "node-1"), Kv("core", "0")};
    const auto a2 = std::vector{Kv("host", "node-1"), Kv("core", "1")};
    (void)meter->CreateObservableCounter<double>("cpu",
                                                 "",
                                                 "1",
                                                 [&a1, &a2](mt::ObservableResult<double>& r)
                                                 {
                                                     r.Observe(0.3, mt::AttributeSpan{a1});
                                                     r.Observe(0.7, mt::AttributeSpan{a2});
                                                 });

    const auto pts = CollectPoints(*producer);
    ASSERT_EQ(pts.size(), 1U);
    EXPECT_DOUBLE_EQ(std::get<double>(pts[0].value), 0.7);
}
