// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// TDD tests for ViewRegistry wiring into SdkMeter — M13 increment 2.
//
// Contract under test:
//  - No registry (null): one default stream with the original instrument name.
//  - Registry with no match: one default stream with the original name.
//  - drop=true view: no stream registered; Add()/Record() is a no-op.
//  - transform.name: stream is registered under the renamed name.
//  - Fan-out (two non-drop views): two streams; both receive every Add() call.
//  - Mixed drop + rename: only the non-dropped view produces a stream.
//  - Kind filter mismatch: view for a different kind → no match → default stream.
//  - Smoke test: Gauge and Histogram also respect view rename.

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

mt::KeyValue Kv(std::string key, std::int64_t val)
{
    return {.key = std::move(key), .value = mt::AttributeValue{val}};
}

// Creates a meter with the given registry pointer (null = no view registry).
// Registry is held as shared_ptr so the meter's m_registry keeps it alive even
// if the caller drops its own reference — exactly the scenario meter_api_test
// exercises with MakeProvider()->GetMeter(...).
std::shared_ptr<mts::SdkMeter> MakeMeter(std::shared_ptr<mts::MetricProducer> producer,
                                         std::shared_ptr<const mts::ViewRegistry> registry)
{
    return std::make_shared<mts::SdkMeter>(MakeScope(),
                                           std::move(producer),
                                           mts::kDefaultMaxCardinality,
                                           nullptr,
                                           std::move(registry));
}

}  // namespace

// ── No registry ───────────────────────────────────────────────────────────────

TEST(ViewWiringTest, NullRegistry_DefaultStream)
{
    auto producer = MakeProducer();
    auto meter = MakeMeter(producer, nullptr);
    const std::vector<mt::KeyValue> attrs{Kv("k", 1)};

    auto counter = meter->CreateCounter<std::int64_t>("requests", "", "");
    counter->Add(5, mt::AttributeSpan{attrs});

    const auto handles = producer->Collect();
    ASSERT_EQ(handles.size(), 1u);
    ASSERT_EQ(handles[0].Metrics().size(), 1u);
    EXPECT_EQ(handles[0].Metrics()[0].name, "requests");
}

// ── Empty registry ────────────────────────────────────────────────────────────

TEST(ViewWiringTest, EmptyRegistry_DefaultStream)
{
    const auto reg = std::make_shared<mts::ViewRegistry>();  // no views added
    auto producer = MakeProducer();
    auto meter = MakeMeter(producer, reg);
    const std::vector<mt::KeyValue> attrs{Kv("k", 1)};

    auto counter = meter->CreateCounter<std::int64_t>("requests", "", "");
    counter->Add(5, mt::AttributeSpan{attrs});

    const auto handles = producer->Collect();
    ASSERT_EQ(handles.size(), 1u);
    ASSERT_EQ(handles[0].Metrics().size(), 1u);
    EXPECT_EQ(handles[0].Metrics()[0].name, "requests");
}

// ── Drop ──────────────────────────────────────────────────────────────────────

TEST(ViewWiringTest, DropView_InstrumentIsNoop)
{
    auto reg = std::make_shared<mts::ViewRegistry>();
    reg->Add(mt::ViewConfig{.transform = {.drop = true}});
    auto producer = MakeProducer();
    auto meter = MakeMeter(producer, reg);
    const std::vector<mt::KeyValue> attrs{Kv("k", 1)};

    auto counter = meter->CreateCounter<std::int64_t>("requests", "", "");
    counter->Add(5, mt::AttributeSpan{attrs});  // no-op

    EXPECT_TRUE(producer->Collect().empty());
}

TEST(ViewWiringTest, AllViewsDrop_AllInstrumentsAreNoop)
{
    auto reg = std::make_shared<mts::ViewRegistry>();
    reg->Add(mt::ViewConfig{.transform = {.drop = true}});
    reg->Add(mt::ViewConfig{.transform = {.drop = true}});
    auto producer = MakeProducer();
    auto meter = MakeMeter(producer, reg);
    const std::vector<mt::KeyValue> attrs{Kv("k", 1)};

    auto c = meter->CreateCounter<std::int64_t>("x", "", "");
    c->Add(1, mt::AttributeSpan{attrs});

    EXPECT_TRUE(producer->Collect().empty());
}

// ── Rename ────────────────────────────────────────────────────────────────────

TEST(ViewWiringTest, RenameView_StreamHasNewName)
{
    auto reg = std::make_shared<mts::ViewRegistry>();
    reg->Add(mt::ViewConfig{.transform = {.name = "http.server.requests"}});
    auto producer = MakeProducer();
    auto meter = MakeMeter(producer, reg);
    const std::vector<mt::KeyValue> attrs{Kv("k", 1)};

    auto counter = meter->CreateCounter<std::int64_t>("requests", "", "");
    counter->Add(7, mt::AttributeSpan{attrs});

    const auto handles = producer->Collect();
    ASSERT_EQ(handles.size(), 1u);
    ASSERT_EQ(handles[0].Metrics().size(), 1u);
    EXPECT_EQ(handles[0].Metrics()[0].name, "http.server.requests");

    const auto& sum = std::get<mti::SumData>(handles[0].Metrics()[0].data);
    ASSERT_EQ(sum.points.size(), 1u);
    EXPECT_EQ(std::get<std::int64_t>(sum.points[0].value), 7);
}

// ── Fan-out ───────────────────────────────────────────────────────────────────

TEST(ViewWiringTest, FanOut_TwoNonDropViews_TwoStreams)
{
    auto reg = std::make_shared<mts::ViewRegistry>();
    reg->Add(mt::ViewConfig{.transform = {.name = "alpha"}});
    reg->Add(mt::ViewConfig{.transform = {.name = "beta"}});
    auto producer = MakeProducer();
    auto meter = MakeMeter(producer, reg);
    const std::vector<mt::KeyValue> attrs{Kv("k", 1)};

    auto counter = meter->CreateCounter<std::int64_t>("x", "", "");
    counter->Add(3, mt::AttributeSpan{attrs});

    const auto handles = producer->Collect();
    ASSERT_EQ(handles.size(), 1u);
    ASSERT_EQ(handles[0].Metrics().size(), 2u);
    EXPECT_EQ(handles[0].Metrics()[0].name, "alpha");
    EXPECT_EQ(handles[0].Metrics()[1].name, "beta");

    const auto& sum_a = std::get<mti::SumData>(handles[0].Metrics()[0].data);
    ASSERT_EQ(sum_a.points.size(), 1u);
    EXPECT_EQ(std::get<std::int64_t>(sum_a.points[0].value), 3);

    const auto& sum_b = std::get<mti::SumData>(handles[0].Metrics()[1].data);
    ASSERT_EQ(sum_b.points.size(), 1u);
    EXPECT_EQ(std::get<std::int64_t>(sum_b.points[0].value), 3);
}

// ── Mixed drop and rename ─────────────────────────────────────────────────────

TEST(ViewWiringTest, MixedDropAndRename_OnlyKeptStream)
{
    auto reg = std::make_shared<mts::ViewRegistry>();
    reg->Add(mt::ViewConfig{.transform = {.drop = true}});
    reg->Add(mt::ViewConfig{.transform = {.name = "kept"}});
    auto producer = MakeProducer();
    auto meter = MakeMeter(producer, reg);
    const std::vector<mt::KeyValue> attrs{Kv("k", 1)};

    auto counter = meter->CreateCounter<std::int64_t>("x", "", "");
    counter->Add(9, mt::AttributeSpan{attrs});

    const auto handles = producer->Collect();
    ASSERT_EQ(handles.size(), 1u);
    ASSERT_EQ(handles[0].Metrics().size(), 1u);
    EXPECT_EQ(handles[0].Metrics()[0].name, "kept");
}

// ── Kind filter mismatch → default stream ─────────────────────────────────────

TEST(ViewWiringTest, KindFilter_NoMatch_DefaultStream)
{
    auto reg = std::make_shared<mts::ViewRegistry>();
    reg->Add(mt::ViewConfig{
        .selector = {.kind = mt::InstrumentKind::Histogram},
        .transform = {.name = "should_not_apply"},
    });
    auto producer = MakeProducer();
    auto meter = MakeMeter(producer, reg);
    const std::vector<mt::KeyValue> attrs{Kv("k", 1)};

    auto counter = meter->CreateCounter<std::int64_t>("requests", "", "");
    counter->Add(5, mt::AttributeSpan{attrs});

    const auto handles = producer->Collect();
    ASSERT_EQ(handles.size(), 1u);
    ASSERT_EQ(handles[0].Metrics().size(), 1u);
    EXPECT_EQ(handles[0].Metrics()[0].name, "requests");
}

// ── Smoke tests for other instrument kinds ────────────────────────────────────

TEST(ViewWiringTest, Gauge_ViewRename)
{
    auto reg = std::make_shared<mts::ViewRegistry>();
    reg->Add(mt::ViewConfig{.transform = {.name = "cpu.utilization.renamed"}});
    auto producer = MakeProducer();
    auto meter = MakeMeter(producer, reg);
    const std::vector<mt::KeyValue> attrs{Kv("k", 1)};

    auto gauge = meter->CreateGauge<double>("cpu.utilization", "", "");
    gauge->Record(0.75, mt::AttributeSpan{attrs});

    const auto handles = producer->Collect();
    ASSERT_EQ(handles.size(), 1u);
    ASSERT_EQ(handles[0].Metrics().size(), 1u);
    EXPECT_EQ(handles[0].Metrics()[0].name, "cpu.utilization.renamed");
}

TEST(ViewWiringTest, Histogram_ViewDrop)
{
    auto reg = std::make_shared<mts::ViewRegistry>();
    reg->Add(mt::ViewConfig{.transform = {.drop = true}});
    auto producer = MakeProducer();
    auto meter = MakeMeter(producer, reg);
    const std::vector<mt::KeyValue> attrs{Kv("k", 1)};

    auto hist = meter->CreateHistogram<double>("latency", "", "");
    hist->Record(1.5, mt::AttributeSpan{attrs});

    EXPECT_TRUE(producer->Collect().empty());
}
