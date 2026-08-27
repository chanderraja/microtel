// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Covers MeterShim / MeterProviderShim (otel-cpp ABI v1): every sync
// instrument's forwarding (all Add/Record overloads, the uint64 → int64
// omit-above-INT64_MAX rule), the observable
// callback-registry bridging, provider pass-through (schema_url included),
// and global registration via metrics::Provider.

#include "adapters/otelcpp/meter_shim.hpp"
#include "fakes/fake_provider.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <variant>

#include <opentelemetry/metrics/provider.h>
#include <opentelemetry/nostd/variant.h>

namespace
{

using microtel::adapters::otelcpp::MeterProviderShim;
using microtel::adapters::otelcpp::MeterShim;
namespace otel_metrics = opentelemetry::metrics;

struct MeterFixture
{
    std::shared_ptr<microtel::testing::FakeMeter> fake =
        std::make_shared<microtel::testing::FakeMeter>();
    MeterShim shim{fake};
};

// ── Counters ──────────────────────────────────────────────────────────────────

TEST(OtelCppMeterShim, Uint64CounterForwardsAllAddOverloads)
{
    MeterFixture f;
    auto counter = f.shim.CreateUInt64Counter("requests", "desc", "1");

    ASSERT_EQ(f.fake->created.size(), 1U);
    EXPECT_EQ(f.fake->created[0].kind, "counter_i64");
    EXPECT_EQ(f.fake->created[0].name, "requests");
    EXPECT_EQ(f.fake->created[0].unit, "1");

    counter->Add(1U);
    counter->Add(2U, opentelemetry::context::Context{});
    counter->Add(3U, {{"route", "/api"}});
    counter->Add(4U, {{"route", "/api"}}, opentelemetry::context::Context{});

    ASSERT_EQ(f.fake->counters_i64.size(), 1U);
    const auto& calls = f.fake->counters_i64[0]->calls;
    ASSERT_EQ(calls.size(), 4U);
    EXPECT_EQ(calls[0].value, 1);
    EXPECT_TRUE(calls[0].attributes.empty());
    EXPECT_EQ(calls[2].value, 3);
    ASSERT_EQ(calls[2].attributes.size(), 1U);
    EXPECT_EQ(calls[2].attributes[0].key, "route");
    EXPECT_EQ(std::get<std::string>(calls[2].attributes[0].value), "/api");
    EXPECT_EQ(calls[3].value, 4);
}

TEST(OtelCppMeterShim, Uint64AboveInt64MaxIsOmittedNotWrapped)
{
    MeterFixture f;
    auto counter = f.shim.CreateUInt64Counter("c");

    counter->Add(std::numeric_limits<std::uint64_t>::max());
    counter->Add(static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()));

    // The unrepresentable increment is omitted (never a wrapped negative);
    // the boundary value converts exactly.
    const auto& calls = f.fake->counters_i64[0]->calls;
    ASSERT_EQ(calls.size(), 1U);
    EXPECT_EQ(calls[0].value, std::numeric_limits<std::int64_t>::max());
}

TEST(OtelCppMeterShim, DoubleCounterForwardsExactly)
{
    MeterFixture f;
    auto counter = f.shim.CreateDoubleCounter("c");
    counter->Add(2.5);

    ASSERT_EQ(f.fake->counters_double.size(), 1U);
    ASSERT_EQ(f.fake->counters_double[0]->calls.size(), 1U);
    EXPECT_DOUBLE_EQ(f.fake->counters_double[0]->calls[0].value, 2.5);
}


// ── UpDownCounter ─────────────────────────────────────────────────────────────

TEST(OtelCppMeterShim, Int64UpDownCounterAcceptsNegative)
{
    MeterFixture f;
    auto counter = f.shim.CreateInt64UpDownCounter("inflight");
    counter->Add(std::int64_t{3});
    counter->Add(std::int64_t{-2}, {{"pool", "db"}});

    EXPECT_EQ(f.fake->created[0].kind, "updown_i64");
    const auto& calls = f.fake->updowns_i64[0]->calls;
    ASSERT_EQ(calls.size(), 2U);
    EXPECT_EQ(calls[1].value, -2);
    ASSERT_EQ(calls[1].attributes.size(), 1U);
    EXPECT_EQ(calls[1].attributes[0].key, "pool");
}

// ── Histograms ────────────────────────────────────────────────────────────────

TEST(OtelCppMeterShim, Uint64HistogramRecordsWithDefaultBoundaries)
{
    MeterFixture f;
    auto histogram = f.shim.CreateUInt64Histogram("latency", "d", "ms");

    histogram->Record(12U, opentelemetry::context::Context{});
    histogram->Record(34U, {{"phase", "tls"}}, opentelemetry::context::Context{});

    EXPECT_EQ(f.fake->created[0].kind, "histogram_i64");
    // MeterShim uses microtel's default-boundary CreateHistogram overload.
    EXPECT_EQ(f.fake->created[0].boundaries.size(), microtel::kDefaultHistogramBoundaries.size());
    const auto& calls = f.fake->histograms_i64[0]->calls;
    ASSERT_EQ(calls.size(), 2U);
    EXPECT_EQ(calls[0].value, 12);
    EXPECT_EQ(calls[1].value, 34);
    ASSERT_EQ(calls[1].attributes.size(), 1U);
    EXPECT_EQ(calls[1].attributes[0].key, "phase");
}


// ── Observables ───────────────────────────────────────────────────────────────

void RecordSeven(otel_metrics::ObserverResult result, void* /*state*/)
{
    if (auto* typed = opentelemetry::nostd::get_if<
            opentelemetry::nostd::shared_ptr<otel_metrics::ObserverResultT<std::int64_t>>>(&result))
    {
        (*typed)->Observe(std::int64_t{7}, {{"cpu", "0"}});
    }
}

TEST(OtelCppMeterShim, ObservableCounterBridgesCollectionCycle)
{
    MeterFixture f;
    auto observable = f.shim.CreateInt64ObservableCounter("cpu.time", "d", "s");

    ASSERT_EQ(f.fake->created.size(), 1U);
    EXPECT_EQ(f.fake->created[0].kind, "observable_counter_i64");
    ASSERT_EQ(f.fake->observable_callbacks_i64.size(), 1U);

    observable->AddCallback(&RecordSeven, nullptr);

    // Drive one microtel collection cycle.
    microtel::testing::FakeObservableResult<std::int64_t> result;
    f.fake->observable_callbacks_i64[0](result);

    ASSERT_EQ(result.observations.size(), 1U);
    EXPECT_EQ(result.observations[0].value, 7);
    ASSERT_EQ(result.observations[0].attributes.size(), 1U);
    EXPECT_EQ(result.observations[0].attributes[0].key, "cpu");

    // After removal the same cycle observes nothing.
    observable->RemoveCallback(&RecordSeven, nullptr);
    microtel::testing::FakeObservableResult<std::int64_t> after;
    f.fake->observable_callbacks_i64[0](after);
    EXPECT_TRUE(after.observations.empty());
}

TEST(OtelCppMeterShim, ObservableKindsRouteToMatchingMicrotelCreates)
{
    MeterFixture f;
    auto a = f.shim.CreateDoubleObservableCounter("a");
    auto b = f.shim.CreateInt64ObservableUpDownCounter("b");
    auto c = f.shim.CreateDoubleObservableGauge("c");

    ASSERT_EQ(f.fake->created.size(), 3U);
    EXPECT_EQ(f.fake->created[0].kind, "observable_counter_double");
    EXPECT_EQ(f.fake->created[1].kind, "observable_updown_i64");
    EXPECT_EQ(f.fake->created[2].kind, "observable_gauge_double");
}

// ── MeterProviderShim + global registration ───────────────────────────────────

TEST(OtelCppMeterProviderShim, GetMeterPassesSchemaUrlThrough)
{
    auto provider = std::make_shared<microtel::testing::FakeProvider>();
    MeterProviderShim shim{provider};

    auto meter = shim.GetMeter("my.lib", "2.0", "https://example.test/schema");

    ASSERT_NE(meter, nullptr);
    ASSERT_EQ(provider->meter_requests.size(), 1U);
    EXPECT_EQ(provider->meter_requests[0].name, "my.lib");
    EXPECT_EQ(provider->meter_requests[0].version, "2.0");
    ASSERT_EQ(provider->meter_schema_urls.size(), 1U);
    EXPECT_EQ(provider->meter_schema_urls[0], "https://example.test/schema");
}

TEST(OtelCppMeterProviderShim, GlobalRegistrationRoutesOtelApiCallsToMicrotel)
{
    auto provider = std::make_shared<microtel::testing::FakeProvider>();

    otel_metrics::Provider::SetMeterProvider(
        microtel::adapters::otelcpp::MakeMeterProvider(provider));

    // Pure otel-cpp application code from here on.
    auto meter = otel_metrics::Provider::GetMeterProvider()->GetMeter("app.metrics");
    auto counter = meter->CreateUInt64Counter("hits");
    counter->Add(1U);

    ASSERT_EQ(provider->meter_requests.size(), 1U);
    EXPECT_EQ(provider->meter_requests[0].name, "app.metrics");
    ASSERT_EQ(provider->meter->counters_i64.size(), 1U);
    EXPECT_EQ(provider->meter->counters_i64[0]->calls.size(), 1U);

    // Restore the noop default so no other test observes this global.
    otel_metrics::Provider::SetMeterProvider(
        opentelemetry::nostd::shared_ptr<otel_metrics::MeterProvider>{
            std::make_shared<otel_metrics::NoopMeterProvider>()});
}

}  // namespace
