// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// TDD tests for per-view attribute_allowlist filtering — M13 increment 3.
//
// Contract under test:
//  - No allowlist: all attribute keys pass through; distinct attr-sets stay distinct.
//  - allowlist={"k1"}: only the k1 key is kept; two records that differ only on k2
//    are merged into one storage point (same filtered bucket → same accumulator).
//  - allowlist=[]: all keys stripped; every record collapses into the empty-attrs bucket.
//  - allowlist={"k1","k3"}: non-listed keys dropped; records that agree on k1 and k3
//    but differ on k2 still merge.
//  - Fan-out: two views with different allowlists produce two independently-filtered
//    streams; each stream merges or distinguishes according to its own allowlist.
//  - Rename + allowlist: both transforms apply to the same view.
//  - Gauge and Histogram also filter (smoke tests).
//
// Test strategy: rather than inspecting AttributeSet internals, tests verify the
// observable consequence — two records with the same filtered attrs collapse into
// one data point whose value is the sum (Counter) or last-written (Gauge).

#include "microtel/attribute.hpp"
#include "microtel/internal/metric_batch.hpp"
#include "microtel/resource.hpp"
#include "microtel/view.hpp"

#include "sdk/metric_attribute_set.hpp"
#include "sdk/metric_producer.hpp"
#include "sdk/sdk_meter.hpp"
#include "sdk/view_registry.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
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
                                           mts::kDefaultMaxCardinality,
                                           nullptr,
                                           std::move(registry));
}

mt::KeyValue Kv(std::string key, std::int64_t val)
{
    return {.key = std::move(key), .value = mt::AttributeValue{val}};
}

}  // namespace

// ── No allowlist: all attrs pass through ──────────────────────────────────────

TEST(AttributeAllowlistTest, NoAllowlist_DistinctAttrSetsStayDistinct)
{
    auto reg = std::make_shared<mts::ViewRegistry>();
    reg->Add(mt::ViewConfig{});  // no transform — default passthrough
    auto producer = MakeProducer();
    auto meter = MakeMeter(producer, reg);

    auto counter = meter->CreateCounter<std::int64_t>("req", "", "");
    const std::vector<mt::KeyValue> a1{Kv("k1", 1), Kv("k2", 10)};
    const std::vector<mt::KeyValue> a2{Kv("k1", 1), Kv("k2", 20)};
    counter->Add(5, mt::AttributeSpan{a1});
    counter->Add(3, mt::AttributeSpan{a2});

    const auto handles = producer->Collect();
    ASSERT_EQ(handles.size(), 1u);
    ASSERT_EQ(handles[0].Metrics().size(), 1u);
    // Different k2 values → two distinct points.
    const auto& sum = std::get<mti::SumData>(handles[0].Metrics()[0].data);
    EXPECT_EQ(sum.points.size(), 2u);
}

// ── allowlist={"k1"}: non-listed keys stripped ────────────────────────────────

TEST(AttributeAllowlistTest, Allowlist_StripsUnlistedKeys_CollapsesSameBucket)
{
    auto reg = std::make_shared<mts::ViewRegistry>();
    reg->Add(mt::ViewConfig{
        .transform = {.attribute_allowlist = std::vector<std::string>{"k1"}},
    });
    auto producer = MakeProducer();
    auto meter = MakeMeter(producer, reg);

    auto counter = meter->CreateCounter<std::int64_t>("req", "", "");
    // Both have k1=1; k2 differs but is not in the allowlist.
    const std::vector<mt::KeyValue> a1{Kv("k1", 1), Kv("k2", 10)};
    const std::vector<mt::KeyValue> a2{Kv("k1", 1), Kv("k2", 20)};
    counter->Add(5, mt::AttributeSpan{a1});
    counter->Add(3, mt::AttributeSpan{a2});

    const auto handles = producer->Collect();
    ASSERT_EQ(handles.size(), 1u);
    ASSERT_EQ(handles[0].Metrics().size(), 1u);
    // Filtered to {k1:1} for both → one combined point with value 5+3=8.
    const auto& sum = std::get<mti::SumData>(handles[0].Metrics()[0].data);
    ASSERT_EQ(sum.points.size(), 1u);
    EXPECT_EQ(std::get<std::int64_t>(sum.points[0].value), 8);
}

TEST(AttributeAllowlistTest, Allowlist_DifferentListedKey_RemainsDistinct)
{
    auto reg = std::make_shared<mts::ViewRegistry>();
    reg->Add(mt::ViewConfig{
        .transform = {.attribute_allowlist = std::vector<std::string>{"k1"}},
    });
    auto producer = MakeProducer();
    auto meter = MakeMeter(producer, reg);

    auto counter = meter->CreateCounter<std::int64_t>("req", "", "");
    // k1 differs → filtered sets are different → two distinct points.
    const std::vector<mt::KeyValue> a1{Kv("k1", 1), Kv("k2", 99)};
    const std::vector<mt::KeyValue> a2{Kv("k1", 2), Kv("k2", 99)};
    counter->Add(5, mt::AttributeSpan{a1});
    counter->Add(3, mt::AttributeSpan{a2});

    const auto handles = producer->Collect();
    ASSERT_EQ(handles.size(), 1u);
    const auto& sum = std::get<mti::SumData>(handles[0].Metrics()[0].data);
    EXPECT_EQ(sum.points.size(), 2u);
}

// ── Empty allowlist: all attrs stripped ──────────────────────────────────────

TEST(AttributeAllowlistTest, EmptyAllowlist_CollapseAllIntoOnePoint)
{
    auto reg = std::make_shared<mts::ViewRegistry>();
    reg->Add(mt::ViewConfig{
        .transform = {.attribute_allowlist = std::vector<std::string>{}},
    });
    auto producer = MakeProducer();
    auto meter = MakeMeter(producer, reg);

    auto counter = meter->CreateCounter<std::int64_t>("req", "", "");
    const std::vector<mt::KeyValue> a1{Kv("k1", 1)};
    const std::vector<mt::KeyValue> a2{Kv("k1", 2)};
    counter->Add(5, mt::AttributeSpan{a1});
    counter->Add(3, mt::AttributeSpan{a2});

    const auto handles = producer->Collect();
    ASSERT_EQ(handles.size(), 1u);
    const auto& sum = std::get<mti::SumData>(handles[0].Metrics()[0].data);
    // Both map to empty attrs → one point, value 5+3=8.
    ASSERT_EQ(sum.points.size(), 1u);
    EXPECT_EQ(std::get<std::int64_t>(sum.points[0].value), 8);
}

// ── Multi-key allowlist ───────────────────────────────────────────────────────

TEST(AttributeAllowlistTest, MultiKeyAllowlist_StripsThirdKey)
{
    auto reg = std::make_shared<mts::ViewRegistry>();
    reg->Add(mt::ViewConfig{
        .transform = {.attribute_allowlist = std::vector<std::string>{"k1", "k3"}},
    });
    auto producer = MakeProducer();
    auto meter = MakeMeter(producer, reg);

    auto counter = meter->CreateCounter<std::int64_t>("req", "", "");
    // k2 is not in allowlist; k1 and k3 are the same → same filtered bucket.
    const std::vector<mt::KeyValue> a1{Kv("k1", 1), Kv("k2", 10), Kv("k3", 5)};
    const std::vector<mt::KeyValue> a2{Kv("k1", 1), Kv("k2", 20), Kv("k3", 5)};
    counter->Add(4, mt::AttributeSpan{a1});
    counter->Add(6, mt::AttributeSpan{a2});

    const auto handles = producer->Collect();
    ASSERT_EQ(handles.size(), 1u);
    const auto& sum = std::get<mti::SumData>(handles[0].Metrics()[0].data);
    ASSERT_EQ(sum.points.size(), 1u);
    EXPECT_EQ(std::get<std::int64_t>(sum.points[0].value), 10);
}

// ── Fan-out: two views with different allowlists ──────────────────────────────

TEST(AttributeAllowlistTest, FanOut_EachViewHasOwnAllowlist)
{
    auto reg = std::make_shared<mts::ViewRegistry>();
    // Stream "by_k1": keeps only k1 → two records with same k1 collapse.
    reg->Add(mt::ViewConfig{
        .transform = {.name = "by_k1", .attribute_allowlist = std::vector<std::string>{"k1"}},
    });
    // Stream "by_k2": keeps only k2 → two records with different k2 stay distinct.
    reg->Add(mt::ViewConfig{
        .transform = {.name = "by_k2", .attribute_allowlist = std::vector<std::string>{"k2"}},
    });
    auto producer = MakeProducer();
    auto meter = MakeMeter(producer, reg);

    auto counter = meter->CreateCounter<std::int64_t>("req", "", "");
    const std::vector<mt::KeyValue> a1{Kv("k1", 1), Kv("k2", 10)};
    const std::vector<mt::KeyValue> a2{Kv("k1", 1), Kv("k2", 20)};
    counter->Add(5, mt::AttributeSpan{a1});
    counter->Add(3, mt::AttributeSpan{a2});

    const auto handles = producer->Collect();
    ASSERT_EQ(handles.size(), 1u);
    ASSERT_EQ(handles[0].Metrics().size(), 2u);

    // Locate streams by name (order matches registration order).
    const auto& m0 = handles[0].Metrics()[0];
    const auto& m1 = handles[0].Metrics()[1];
    EXPECT_EQ(m0.name, "by_k1");
    EXPECT_EQ(m1.name, "by_k2");

    // by_k1: both records → {k1:1} → 1 point, value=8.
    const auto& sum0 = std::get<mti::SumData>(m0.data);
    ASSERT_EQ(sum0.points.size(), 1u);
    EXPECT_EQ(std::get<std::int64_t>(sum0.points[0].value), 8);

    // by_k2: {k2:10} ≠ {k2:20} → 2 distinct points.
    const auto& sum1 = std::get<mti::SumData>(m1.data);
    EXPECT_EQ(sum1.points.size(), 2u);
}

// ── Rename + allowlist apply together ─────────────────────────────────────────

TEST(AttributeAllowlistTest, RenameAndAllowlist_BothApply)
{
    auto reg = std::make_shared<mts::ViewRegistry>();
    reg->Add(mt::ViewConfig{
        .transform = {.name = "filtered.req",
                      .attribute_allowlist = std::vector<std::string>{"k1"}},
    });
    auto producer = MakeProducer();
    auto meter = MakeMeter(producer, reg);

    auto counter = meter->CreateCounter<std::int64_t>("req", "", "");
    const std::vector<mt::KeyValue> a1{Kv("k1", 1), Kv("k2", 10)};
    const std::vector<mt::KeyValue> a2{Kv("k1", 1), Kv("k2", 20)};
    counter->Add(5, mt::AttributeSpan{a1});
    counter->Add(3, mt::AttributeSpan{a2});

    const auto handles = producer->Collect();
    ASSERT_EQ(handles.size(), 1u);
    ASSERT_EQ(handles[0].Metrics().size(), 1u);
    EXPECT_EQ(handles[0].Metrics()[0].name, "filtered.req");

    const auto& sum = std::get<mti::SumData>(handles[0].Metrics()[0].data);
    ASSERT_EQ(sum.points.size(), 1u);
    EXPECT_EQ(std::get<std::int64_t>(sum.points[0].value), 8);
}

// ── Gauge smoke test ──────────────────────────────────────────────────────────

TEST(AttributeAllowlistTest, Gauge_AllowlistStripsUnlistedKey)
{
    auto reg = std::make_shared<mts::ViewRegistry>();
    reg->Add(mt::ViewConfig{
        .transform = {.attribute_allowlist = std::vector<std::string>{"k1"}},
    });
    auto producer = MakeProducer();
    auto meter = MakeMeter(producer, reg);

    auto gauge = meter->CreateGauge<double>("cpu", "", "");
    // Two records with same k1 but different k2: after filtering both are {k1:1}.
    // Gauge keeps last-written value per bucket; same bucket → one point.
    const std::vector<mt::KeyValue> a1{Kv("k1", 1), Kv("k2", 10)};
    const std::vector<mt::KeyValue> a2{Kv("k1", 1), Kv("k2", 20)};
    gauge->Record(0.5, mt::AttributeSpan{a1});
    gauge->Record(0.9, mt::AttributeSpan{a2});

    const auto handles = producer->Collect();
    ASSERT_EQ(handles.size(), 1u);
    const auto& gdata = std::get<mti::GaugeData>(handles[0].Metrics()[0].data);
    // Same filtered bucket → one point (last-written wins).
    EXPECT_EQ(gdata.points.size(), 1u);
}

// ── Histogram smoke test ──────────────────────────────────────────────────────

TEST(AttributeAllowlistTest, Histogram_AllowlistStripsUnlistedKey)
{
    auto reg = std::make_shared<mts::ViewRegistry>();
    reg->Add(mt::ViewConfig{
        .transform = {.attribute_allowlist = std::vector<std::string>{"k1"}},
    });
    auto producer = MakeProducer();
    auto meter = MakeMeter(producer, reg);

    auto hist = meter->CreateHistogram<double>("latency", "", "");
    // Same k1, different k2 → filtered to {k1:1} for both → one bucket.
    const std::vector<mt::KeyValue> a1{Kv("k1", 1), Kv("k2", 10)};
    const std::vector<mt::KeyValue> a2{Kv("k1", 1), Kv("k2", 20)};
    hist->Record(1.0, mt::AttributeSpan{a1});
    hist->Record(2.0, mt::AttributeSpan{a2});

    const auto handles = producer->Collect();
    ASSERT_EQ(handles.size(), 1u);
    const auto& hdata = std::get<mti::HistogramData>(handles[0].Metrics()[0].data);
    // Both map to same filtered bucket → one histogram data point.
    EXPECT_EQ(hdata.points.size(), 1u);
}
