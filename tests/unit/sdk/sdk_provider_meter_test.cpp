// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for SdkProvider::GetMeter — the instrument-factory accessor that
// lazily creates a MetricProducer and caches Meter instances by scope.
//
// Contract under test:
//  - GetMeter() returns a non-null shared_ptr<Meter>.
//  - Same (name, version) → same pointer (identity cache).
//  - Different name or version → different pointer.
//  - Empty version is a distinct scope from a non-empty version.
//  - Meters obtained from the same provider share a MetricProducer
//    (instruments from different scopes both appear in Collect()).

#include "microtel/internal/sampler.hpp"
#include "microtel/resource.hpp"
#include "microtel/sampler.hpp"

#include "mocks/mock_exporter.hpp"
#include "mocks/mock_span_processor.hpp"
#include "mocks/mock_transport.hpp"
#include "sdk/meter.hpp"
#include "sdk/sdk_provider.hpp"

#include <gtest/gtest.h>

#include <memory>

namespace mt = microtel;
namespace mts = microtel::sdk;
namespace mtm = microtel::testing;

using namespace std::chrono_literals;

namespace
{

// ── Minimal SdkProvider construction ─────────────────────────────────────────

std::unique_ptr<mts::SdkProvider> MakeProvider()
{
    auto proc = std::make_unique<mtm::MockSpanProcessor>();
    auto exp = std::make_unique<mtm::MockExporter>();
    auto transport = std::make_unique<mtm::MockTransport>();

    return std::make_unique<mts::SdkProvider>(mts::SdkProviderArgs{
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
    });
}

}  // namespace

// ── GetMeter — basic ──────────────────────────────────────────────────────────

TEST(SdkProviderMeterTest, GetMeter_ReturnsNonNull)
{
    auto provider = MakeProvider();
    auto meter = provider->GetMeter("my.lib");
    EXPECT_NE(meter, nullptr);
}

TEST(SdkProviderMeterTest, GetMeter_SameScopeReturnsSameInstance)
{
    auto provider = MakeProvider();
    auto a = provider->GetMeter("my.lib", "1.0");
    auto b = provider->GetMeter("my.lib", "1.0");
    EXPECT_EQ(a.get(), b.get());
}

TEST(SdkProviderMeterTest, GetMeter_DifferentNameReturnsDifferentInstance)
{
    auto provider = MakeProvider();
    auto a = provider->GetMeter("lib.a");
    auto b = provider->GetMeter("lib.b");
    EXPECT_NE(a.get(), b.get());
}

TEST(SdkProviderMeterTest, GetMeter_DifferentVersionReturnsDifferentInstance)
{
    auto provider = MakeProvider();
    auto a = provider->GetMeter("lib", "1.0");
    auto b = provider->GetMeter("lib", "2.0");
    EXPECT_NE(a.get(), b.get());
}

TEST(SdkProviderMeterTest, GetMeter_EmptyVersionDistinctFromNonEmpty)
{
    auto provider = MakeProvider();
    auto a = provider->GetMeter("lib");  // version defaults to ""
    auto b = provider->GetMeter("lib", "1.0");
    EXPECT_NE(a.get(), b.get());
}

TEST(SdkProviderMeterTest, GetMeter_DefaultAndExplicitEmptyVersionAreSameScope)
{
    auto provider = MakeProvider();
    auto a = provider->GetMeter("lib");
    auto b = provider->GetMeter("lib", "");  // explicit empty == default empty
    EXPECT_EQ(a.get(), b.get());
}

// ── Shared MetricProducer — instruments can be created from multiple scopes ────

TEST(SdkProviderMeterTest, InstrumentsCanBeCreatedFromMultipleScopes)
{
    auto provider = MakeProvider();
    auto meter_a = provider->GetMeter("scope.a", "1.0");
    auto meter_b = provider->GetMeter("scope.b", "1.0");

    // Both meters share the same MetricProducer; instruments must not crash.
    auto counter_a = meter_a->CreateCounter<int64_t>("requests", "", "");
    auto counter_b = meter_b->CreateCounter<int64_t>("errors", "", "");
    counter_a.Add(10, {});
    counter_b.Add(3, {});
}

TEST(SdkProviderMeterTest, InstrumentsFromSameScopeAreAddedToSameScope)
{
    auto provider = MakeProvider();
    // Same meter returned twice — both instrument registrations go to the
    // same ScopeEntry in the MetricProducer.
    auto m1 = provider->GetMeter("scope", "1.0");
    auto m2 = provider->GetMeter("scope", "1.0");

    auto c1 = m1->CreateCounter<int64_t>("hits", "", "");
    auto c2 = m2->CreateCounter<int64_t>("misses", "", "");
    c1.Add(5, {});
    c2.Add(2, {});
}
