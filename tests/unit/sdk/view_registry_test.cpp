// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// TDD tests for ViewRegistry selector matching — M13 increment 1
// (docs/metrics-design.md §6).
//
// Contract under test:
//  - An empty selector matches any instrument.
//  - name supports exact match and a single trailing * wildcard.
//  - kind and meter_name are optional filters that narrow the match.
//  - All fields must match simultaneously.
//  - Match() returns all matching ViewConfigs in registration order.
//  - An empty result means "no match — use the default stream".

#include "sdk/view_registry.hpp"

#include "microtel/view.hpp"

#include <gtest/gtest.h>

#include <string_view>

namespace mt = microtel;
namespace sdk = microtel::sdk;

namespace
{

sdk::InstrumentDescriptor MakeDesc(std::string_view name,
                                   mt::InstrumentKind kind = mt::InstrumentKind::Counter,
                                   std::string_view meter_name = "test.meter")
{
    return {.name = name, .kind = kind, .meter_name = meter_name};
}

}  // namespace

// ── Empty selector ────────────────────────────────────────────────────────────

TEST(ViewRegistryTest, EmptySelector_MatchesAnyInstrument)
{
    sdk::ViewRegistry reg;
    reg.Add(mt::ViewConfig{});
    EXPECT_EQ(reg.Match(MakeDesc("anything")).size(), 1u);
    EXPECT_EQ(reg.Match(MakeDesc("other", mt::InstrumentKind::Gauge, "other.meter")).size(), 1u);
}

// ── Name matching ─────────────────────────────────────────────────────────────

TEST(ViewRegistryTest, ExactName_MatchesOnlyThatName)
{
    sdk::ViewRegistry reg;
    reg.Add(mt::ViewConfig{.selector = {.name = "http.requests"}});
    EXPECT_EQ(reg.Match(MakeDesc("http.requests")).size(), 1u);
    EXPECT_EQ(reg.Match(MakeDesc("http.requests.total")).size(), 0u);
    EXPECT_EQ(reg.Match(MakeDesc("http")).size(), 0u);
}

TEST(ViewRegistryTest, PrefixWildcard_MatchesAnySuffix)
{
    sdk::ViewRegistry reg;
    reg.Add(mt::ViewConfig{.selector = {.name = "http.*"}});
    EXPECT_EQ(reg.Match(MakeDesc("http.requests")).size(), 1u);
    EXPECT_EQ(reg.Match(MakeDesc("http.duration")).size(), 1u);
    EXPECT_EQ(reg.Match(MakeDesc("http.")).size(), 1u);
    EXPECT_EQ(reg.Match(MakeDesc("grpc.requests")).size(), 0u);
}

TEST(ViewRegistryTest, PrefixWildcard_BareStarMatchesAll)
{
    sdk::ViewRegistry reg;
    reg.Add(mt::ViewConfig{.selector = {.name = "*"}});
    EXPECT_EQ(reg.Match(MakeDesc("anything")).size(), 1u);
    EXPECT_EQ(reg.Match(MakeDesc("")).size(), 1u);
}

TEST(ViewRegistryTest, PrefixWildcard_DoesNotMatchShorterThanPrefix)
{
    sdk::ViewRegistry reg;
    reg.Add(mt::ViewConfig{.selector = {.name = "http.*"}});
    // "http" lacks the "." that follows the prefix after stripping '*'
    EXPECT_EQ(reg.Match(MakeDesc("http")).size(), 0u);
}

// ── Kind matching ─────────────────────────────────────────────────────────────

TEST(ViewRegistryTest, KindFilter_MatchesOnlyThatKind)
{
    sdk::ViewRegistry reg;
    reg.Add(mt::ViewConfig{.selector = {.kind = mt::InstrumentKind::Histogram}});
    EXPECT_EQ(reg.Match(MakeDesc("latency", mt::InstrumentKind::Histogram)).size(), 1u);
    EXPECT_EQ(reg.Match(MakeDesc("latency", mt::InstrumentKind::Counter)).size(), 0u);
    EXPECT_EQ(reg.Match(MakeDesc("latency", mt::InstrumentKind::Gauge)).size(), 0u);
}

TEST(ViewRegistryTest, NulloptKind_MatchesAllKinds)
{
    sdk::ViewRegistry reg;
    reg.Add(mt::ViewConfig{.selector = {.kind = std::nullopt}});
    for (const auto k : {mt::InstrumentKind::Counter,
                         mt::InstrumentKind::UpDownCounter,
                         mt::InstrumentKind::Gauge,
                         mt::InstrumentKind::Histogram,
                         mt::InstrumentKind::ExponentialHistogram,
                         mt::InstrumentKind::ObservableCounter,
                         mt::InstrumentKind::ObservableUpDownCounter,
                         mt::InstrumentKind::ObservableGauge})
    {
        EXPECT_EQ(reg.Match(MakeDesc("m", k)).size(), 1u);
    }
}

// ── Meter name matching ───────────────────────────────────────────────────────

TEST(ViewRegistryTest, MeterNameFilter_MatchesOnlyThatMeter)
{
    sdk::ViewRegistry reg;
    reg.Add(mt::ViewConfig{.selector = {.meter_name = "my.lib"}});
    EXPECT_EQ(reg.Match(MakeDesc("m", mt::InstrumentKind::Counter, "my.lib")).size(), 1u);
    EXPECT_EQ(reg.Match(MakeDesc("m", mt::InstrumentKind::Counter, "other.lib")).size(), 0u);
}

// ── Combined selector fields ──────────────────────────────────────────────────

TEST(ViewRegistryTest, AllFieldsSet_AllMustMatch)
{
    sdk::ViewRegistry reg;
    reg.Add(mt::ViewConfig{
        .selector =
            {
                .name = "http.*",
                .kind = mt::InstrumentKind::Histogram,
                .meter_name = "web.server",
            },
    });

    EXPECT_EQ(reg.Match({.name = "http.duration",
                         .kind = mt::InstrumentKind::Histogram,
                         .meter_name = "web.server"})
                  .size(),
              1u);
    EXPECT_EQ(reg.Match({.name = "grpc.duration",
                         .kind = mt::InstrumentKind::Histogram,
                         .meter_name = "web.server"})
                  .size(),
              0u);
    EXPECT_EQ(reg.Match({.name = "http.duration",
                         .kind = mt::InstrumentKind::Counter,
                         .meter_name = "web.server"})
                  .size(),
              0u);
    EXPECT_EQ(reg.Match({.name = "http.duration",
                         .kind = mt::InstrumentKind::Histogram,
                         .meter_name = "other"})
                  .size(),
              0u);
}

// ── Multiple views ────────────────────────────────────────────────────────────

TEST(ViewRegistryTest, MultipleViews_AllMatchingReturned)
{
    sdk::ViewRegistry reg;
    reg.Add(mt::ViewConfig{.selector = {.name = "http.*"}});
    reg.Add(mt::ViewConfig{.selector = {.name = "grpc.*"}});
    reg.Add(mt::ViewConfig{.selector = {.kind = mt::InstrumentKind::Counter}});

    const auto matches = reg.Match(MakeDesc("http.requests", mt::InstrumentKind::Counter));
    EXPECT_EQ(matches.size(), 2u);  // "http.*" and kind=Counter; "grpc.*" does not match
}

TEST(ViewRegistryTest, MultipleViews_RegistrationOrderPreserved)
{
    sdk::ViewRegistry reg;
    reg.Add(mt::ViewConfig{.transform = {.name = "first"}});
    reg.Add(mt::ViewConfig{.transform = {.name = "second"}});

    const auto matches = reg.Match(MakeDesc("any"));
    ASSERT_EQ(matches.size(), 2u);
    EXPECT_EQ(matches[0]->transform.name, "first");
    EXPECT_EQ(matches[1]->transform.name, "second");
}

// ── Empty registry / no match ─────────────────────────────────────────────────

TEST(ViewRegistryTest, NoMatch_ReturnsEmpty)
{
    sdk::ViewRegistry reg;
    reg.Add(mt::ViewConfig{.selector = {.name = "http.*"}});
    EXPECT_TRUE(reg.Match(MakeDesc("grpc.requests")).empty());
}

TEST(ViewRegistryTest, EmptyRegistry_ReturnsEmpty)
{
    const sdk::ViewRegistry reg;
    EXPECT_TRUE(reg.Match(MakeDesc("anything")).empty());
    EXPECT_TRUE(reg.Empty());
}
