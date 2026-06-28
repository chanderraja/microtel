// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Tests for the public microtel::Meter API via Provider::GetMeter().
//
// Contract under test:
//  - Provider::GetMeter() returns a non-null shared_ptr<microtel::Meter>.
//  - Same (name, version) scope returns the same Meter pointer (identity cache).
//  - Different scopes return different Meter pointers.
//  - CreateCounter<T>(), CreateUpDownCounter<T>(), CreateGauge<T>(),
//    CreateHistogram<T>() return non-null instrument shared_ptrs.
//  - Instrument Add()/Record() calls do not crash.

#include "microtel/internal/sampler.hpp"
#include "microtel/meter.hpp"
#include "microtel/provider.hpp"
#include "microtel/resource.hpp"
#include "microtel/sampler.hpp"

#include "mocks/mock_exporter.hpp"
#include "mocks/mock_span_processor.hpp"
#include "mocks/mock_transport.hpp"
#include "sdk/sdk_provider.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>

namespace mt = microtel;
namespace mts = microtel::sdk;
namespace mtm = microtel::testing;

namespace
{

std::shared_ptr<mt::Provider> MakeProvider()
{
    auto proc = std::make_unique<mtm::MockSpanProcessor>();
    auto exp = std::make_unique<mtm::MockExporter>();
    auto transport = std::make_unique<mtm::MockTransport>();

    return std::make_shared<mts::SdkProvider>(mts::SdkProviderArgs{
        .encoder = nullptr,
        .auth = nullptr,
        .transport = std::move(transport),
        .codec = nullptr,
        .exporter = std::move(exp),
        .processor = std::move(proc),
        .resource = std::make_shared<mt::Resource>(),
        .sampler = mt::MakeAlwaysOnSampler(),
        .span_limits = {},
        .connect_opts = {},
        .metric_codec = nullptr,
        .metric_exporter = nullptr,
    });
}

}  // namespace

// ── Provider::GetMeter ─────────────────────────────────────────────────────────

TEST(MeterApiTest, GetMeter_ReturnsNonNull)
{
    const auto provider = MakeProvider();
    EXPECT_NE(provider->GetMeter("test.lib"), nullptr);
}

TEST(MeterApiTest, GetMeter_SameScopeReturnsSameInstance)
{
    const auto provider = MakeProvider();
    const auto a = provider->GetMeter("my.lib", "1.0");
    const auto b = provider->GetMeter("my.lib", "1.0");
    EXPECT_EQ(a.get(), b.get());
}

TEST(MeterApiTest, GetMeter_DifferentNamesReturnDifferentInstances)
{
    const auto provider = MakeProvider();
    const auto a = provider->GetMeter("lib.a");
    const auto b = provider->GetMeter("lib.b");
    EXPECT_NE(a.get(), b.get());
}

// ── Counter<int64_t> ──────────────────────────────────────────────────────────

TEST(MeterApiTest, CreateCounterI64_ReturnsNonNull)
{
    const auto meter = MakeProvider()->GetMeter("test.lib");
    EXPECT_NE(meter->CreateCounter<std::int64_t>("req.count", "Requests", "1"), nullptr);
}

TEST(MeterApiTest, CounterI64_Add_DoesNotCrash)
{
    const auto meter = MakeProvider()->GetMeter("test.lib");
    const auto counter = meter->CreateCounter<std::int64_t>("req.count", "Requests", "1");
    counter->Add(42, {});
}

// ── Counter<double> ───────────────────────────────────────────────────────────

TEST(MeterApiTest, CreateCounterDouble_ReturnsNonNull)
{
    const auto meter = MakeProvider()->GetMeter("test.lib");
    EXPECT_NE(meter->CreateCounter<double>("latency", "Latency", "s"), nullptr);
}

TEST(MeterApiTest, CounterDouble_Add_DoesNotCrash)
{
    const auto meter = MakeProvider()->GetMeter("test.lib");
    const auto counter = meter->CreateCounter<double>("latency", "Latency", "s");
    counter->Add(1.5, {});
}

// ── UpDownCounter<int64_t> ────────────────────────────────────────────────────

TEST(MeterApiTest, UpDownCounterI64_Add_DoesNotCrash)
{
    const auto meter = MakeProvider()->GetMeter("test.lib");
    const auto c =
        meter->CreateUpDownCounter<std::int64_t>("active.conn", "Active connections", "1");
    c->Add(-1, {});
}

// ── UpDownCounter<double> ─────────────────────────────────────────────────────

TEST(MeterApiTest, UpDownCounterDouble_Add_DoesNotCrash)
{
    const auto meter = MakeProvider()->GetMeter("test.lib");
    const auto c = meter->CreateUpDownCounter<double>("queue.size", "Queue size", "by");
    c->Add(-0.5, {});
}

// ── Gauge<int64_t> ────────────────────────────────────────────────────────────

TEST(MeterApiTest, GaugeI64_Record_DoesNotCrash)
{
    const auto meter = MakeProvider()->GetMeter("test.lib");
    const auto g = meter->CreateGauge<std::int64_t>("cpu.temp", "CPU temperature", "cel");
    g->Record(72, {});
}

// ── Gauge<double> ─────────────────────────────────────────────────────────────

TEST(MeterApiTest, GaugeDouble_Record_DoesNotCrash)
{
    const auto meter = MakeProvider()->GetMeter("test.lib");
    const auto g = meter->CreateGauge<double>("cpu.util", "CPU utilisation", "1");
    g->Record(0.87, {});
}

// ── Histogram<double> — default boundaries ────────────────────────────────────

TEST(MeterApiTest, HistogramDouble_DefaultBoundaries_DoesNotCrash)
{
    const auto meter = MakeProvider()->GetMeter("test.lib");
    const auto h = meter->CreateHistogram<double>("duration", "Duration", "s");
    h->Record(0.5, {});
}

// ── Histogram<int64_t> — default boundaries ───────────────────────────────────

TEST(MeterApiTest, HistogramI64_DefaultBoundaries_DoesNotCrash)
{
    const auto meter = MakeProvider()->GetMeter("test.lib");
    const auto h = meter->CreateHistogram<std::int64_t>("response.size", "Response size", "by");
    h->Record(1024, {});
}

// ── Histogram<double> — custom boundaries ─────────────────────────────────────

TEST(MeterApiTest, HistogramDouble_CustomBoundaries_DoesNotCrash)
{
    const auto meter = MakeProvider()->GetMeter("test.lib");
    const auto h = meter->CreateHistogram<double>(
        "payload.size", "Payload size", "by", {100.0, 1'000.0, 10'000.0});
    h->Record(500.0, {});
}

// ── ObservableCounter<int64_t> ────────────────────────────────────────────────

TEST(MeterApiTest, ObservableCounterI64_CreateDoesNotCrash)
{
    const auto meter = MakeProvider()->GetMeter("test.lib");
    auto handle = meter->CreateObservableCounter<std::int64_t>(
        "process.cpu_time",
        "CPU time",
        "s",
        [](mt::ObservableResult<std::int64_t>& result) { result.Observe(42, {}); });
    (void)handle;
}

// ── ObservableCounter<double> ─────────────────────────────────────────────────

TEST(MeterApiTest, ObservableCounterDouble_CreateDoesNotCrash)
{
    const auto meter = MakeProvider()->GetMeter("test.lib");
    auto handle = meter->CreateObservableCounter<double>("cache.hit_ratio",
                                                         "Cache hit ratio",
                                                         "1",
                                                         [](mt::ObservableResult<double>& result)
                                                         { result.Observe(0.95, {}); });
    (void)handle;
}

// ── ObservableUpDownCounter<int64_t> ──────────────────────────────────────────

TEST(MeterApiTest, ObservableUpDownCounterI64_CreateDoesNotCrash)
{
    const auto meter = MakeProvider()->GetMeter("test.lib");
    auto handle = meter->CreateObservableUpDownCounter<std::int64_t>(
        "process.open_fds",
        "Open file descriptors",
        "1",
        [](mt::ObservableResult<std::int64_t>& result) { result.Observe(12, {}); });
    (void)handle;
}

// ── ObservableGauge<double> ───────────────────────────────────────────────────

TEST(MeterApiTest, ObservableGaugeDouble_CreateDoesNotCrash)
{
    const auto meter = MakeProvider()->GetMeter("test.lib");
    auto handle = meter->CreateObservableGauge<double>("system.memory.usage",
                                                       "Memory usage",
                                                       "by",
                                                       [](mt::ObservableResult<double>& result)
                                                       { result.Observe(1024.0 * 1024.0, {}); });
    (void)handle;
}
