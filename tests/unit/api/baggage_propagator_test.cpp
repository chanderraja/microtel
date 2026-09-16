// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Tests for microtel::W3CBaggagePropagator::Inject / Extract, and for the
// `Baggage` slot on `microtel::Context` that the propagator exists to fill.
//
// ICP 0025 §2 and §4, packet 2.3c. Structured after `propagator_test.cpp`
// (the traceparent/tracestate suite): a `std::map` carrier, an `Inject` half,
// an `Extract` half, and the round-trips that join them.
//
// ── The design wall this file guards ─────────────────────────────────────────
//
// Baggage lives on `Context`, never on `SpanContext`. `Span::GetContext()
// const noexcept` returns a `SpanContext` **by value** (hard rule 14), so a
// baggage member there would put a second growable member inside a `noexcept`
// accessor — the exact trap ICP 0025 §1 put `TraceState` behind a `shared_ptr`
// to escape. Baggage is also per-*context*, not per-*span*: a request carries
// it whether or not a span is active, and baggage set inside a span must
// outlive that span within the enclosing scope. The static_asserts below pin
// the `noexcept` half of that; `SpanContext` having no baggage member is
// pinned by `baggage.hpp` not being included from `trace.hpp`.
//
// The propagator's surface is the one ICP 0025 §4 locked: `Baggage` in,
// `Baggage` out. Reaching the current thread's `Context` is one expression at
// the call site — `Inject(CurrentContext().baggage, setter)` — which the
// round-trip tests at the bottom exercise exactly as a caller would write it.

#include "microtel/baggage.hpp"
#include "microtel/context.hpp"
#include "microtel/propagator.hpp"
#include "microtel/trace.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>

namespace mt = microtel;

namespace
{

constexpr std::string_view kBaggageHeader = "baggage";

/// @brief The W3C Baggage specification's own example header value.
constexpr std::string_view kSpecExample = "key1=value1;property1;property2,key2=value2";

/// @brief `std::less<>` makes the map heterogeneously searchable by
/// `string_view`, which is what `HeaderGetter` hands us.
using Headers = std::map<std::string, std::string, std::less<>>;

[[nodiscard]] mt::HeaderGetter GetterFor(const Headers& headers)
{
    return [&headers](std::string_view name) -> std::optional<std::string_view>
    {
        const auto entry = headers.find(name);
        if (entry == headers.end())
        {
            return std::nullopt;
        }
        return std::string_view(entry->second);
    };
}

[[nodiscard]] mt::HeaderSetter SetterFor(Headers& headers)
{
    return [&headers](std::string_view name, std::string_view value)
    { headers.insert_or_assign(std::string(name), std::string(value)); };
}

/// @brief Extracts from a carrier holding only @p baggage.
[[nodiscard]] mt::Baggage Extract(std::string_view baggage)
{
    const Headers headers{{std::string(kBaggageHeader), std::string(baggage)}};
    return mt::W3CBaggagePropagator().Extract(GetterFor(headers));
}

// ── The guards ICP 0025 §2 asks for ──────────────────────────────────────────

static_assert(std::is_nothrow_copy_constructible_v<mt::Context>,
              "Context is copied into every ScopedContext — see ICP 0025 §2");
static_assert(std::is_nothrow_copy_assignable_v<mt::Context>);
static_assert(std::is_nothrow_move_constructible_v<mt::Context>);
static_assert(std::is_nothrow_default_constructible_v<mt::Context>);

// The wall: `SpanContext` — which `Span::GetContext() const noexcept` returns
// by value — stays `noexcept` to copy because baggage is not on it.
static_assert(std::is_nothrow_copy_constructible_v<mt::SpanContext>);

// ── Context carries the baggage ──────────────────────────────────────────────

TEST(ContextBaggageTest, DefaultContextHasEmptyBaggage)
{
    const mt::Context ctx;
    EXPECT_TRUE(ctx.baggage.Empty());
}

TEST(ContextBaggageTest, SpanContextOnlyConstructorLeavesBaggageEmpty)
{
    const mt::Context ctx{mt::SpanContext{}};
    EXPECT_TRUE(ctx.baggage.Empty());
}

TEST(ContextBaggageTest, TwoArgumentConstructorCarriesBoth)
{
    mt::TraceId::Bytes trace_bytes{};
    trace_bytes[0] = 0x7FU;
    mt::SpanId::Bytes span_bytes{};
    span_bytes[0] = 0x7FU;
    const mt::SpanContext active{
        .trace_id = mt::TraceId{trace_bytes},
        .span_id = mt::SpanId{span_bytes},
        .trace_flags = mt::TraceFlags{mt::TraceFlags::kSampled},
        .trace_state = {},
        .remote = false,
    };

    const mt::Context ctx{active, mt::Baggage::FromHeader("a=1")};
    EXPECT_TRUE(ctx.active_span_context.IsValid());
    ASSERT_TRUE(ctx.baggage.Get("a").has_value());
    EXPECT_EQ(*ctx.baggage.Get("a"), "1");
}

TEST(ContextBaggageTest, BaggageSurvivesAScopedContextInstallAndRestore)
{
    const mt::Context outer{mt::SpanContext{}, mt::Baggage::FromHeader("tier=outer")};
    const mt::ScopedContext outer_scope{outer};
    ASSERT_EQ(*mt::CurrentContext().baggage.Get("tier"), "outer");

    {
        const mt::ScopedContext inner_scope{
            mt::Context{mt::SpanContext{}, mt::Baggage::FromHeader("tier=inner")}};
        EXPECT_EQ(*mt::CurrentContext().baggage.Get("tier"), "inner");
    }

    EXPECT_EQ(*mt::CurrentContext().baggage.Get("tier"), "outer");
}

TEST(ContextBaggageTest, CopyingAContextSharesTheEntryList)
{
    const mt::Context original{mt::SpanContext{}, mt::Baggage::FromHeader("a=1")};
    const mt::Context copy = original;  // NOLINT(performance-unnecessary-copy-initialization)
    EXPECT_EQ(copy.baggage.Size(), 1U);
    EXPECT_EQ(original.baggage.Size(), 1U);
}

// ── Extract ──────────────────────────────────────────────────────────────────

TEST(W3CBaggagePropagatorExtractTest, ParsesTheSpecExample)
{
    const mt::Baggage bag = Extract(kSpecExample);
    ASSERT_EQ(bag.Size(), 2U);
    EXPECT_EQ(*bag.Get("key1"), "value1");
    EXPECT_EQ(*bag.Get("key2"), "value2");
}

TEST(W3CBaggagePropagatorExtractTest, ReturnsEmptyWhenTheHeaderIsAbsent)
{
    const Headers headers{{"traceparent", "ignored"}};
    EXPECT_TRUE(mt::W3CBaggagePropagator().Extract(GetterFor(headers)).Empty());
}

TEST(W3CBaggagePropagatorExtractTest, ReturnsEmptyForAnEmptyGetter)
{
    EXPECT_TRUE(mt::W3CBaggagePropagator().Extract(mt::HeaderGetter{}).Empty());
}

TEST(W3CBaggagePropagatorExtractTest, ReturnsEmptyForAnEmptyHeaderValue)
{
    EXPECT_TRUE(Extract("").Empty());
    EXPECT_TRUE(Extract("   ").Empty());
}

TEST(W3CBaggagePropagatorExtractTest, KeepsTheGoodMembersOfAPartlyMalformedHeader)
{
    // Unlike `tracestate`, a bad baggage member costs only itself.
    const mt::Baggage bag = Extract("a=1,bad key=2,c=3");
    ASSERT_EQ(bag.Size(), 2U);
    EXPECT_EQ(bag.ToHeader(), "a=1,c=3");
}

TEST(W3CBaggagePropagatorExtractTest, ReturnsEmptyWhenEveryMemberIsMalformed)
{
    EXPECT_TRUE(Extract("bad key=1,=2,novalue").Empty());
}

TEST(W3CBaggagePropagatorExtractTest, EnforcesTheGrammarLimits)
{
    std::string header;
    for (std::size_t i = 0; i <= mt::Baggage::kMaxEntries; ++i)
    {
        if (i != 0)
        {
            header.push_back(',');
        }
        header += "k" + std::to_string(i) + "=v";
    }
    EXPECT_EQ(Extract(header).Size(), mt::Baggage::kMaxEntries);
}

// ── Inject ───────────────────────────────────────────────────────────────────

TEST(W3CBaggagePropagatorInjectTest, WritesTheBaggageHeader)
{
    Headers headers;
    mt::W3CBaggagePropagator().Inject(mt::Baggage::FromHeader(kSpecExample), SetterFor(headers));

    ASSERT_EQ(headers.count(std::string(kBaggageHeader)), 1U);
    EXPECT_EQ(headers.at(std::string(kBaggageHeader)), kSpecExample);
}

TEST(W3CBaggagePropagatorInjectTest, WritesNothingForEmptyBaggage)
{
    Headers headers;
    mt::W3CBaggagePropagator().Inject(mt::Baggage{}, SetterFor(headers));
    EXPECT_TRUE(headers.empty());
}

TEST(W3CBaggagePropagatorInjectTest, WritesNothingWhenEveryMemberWasDropped)
{
    Headers headers;
    mt::W3CBaggagePropagator().Inject(mt::Baggage::FromHeader("bad key=1"), SetterFor(headers));
    EXPECT_TRUE(headers.empty());
}

TEST(W3CBaggagePropagatorInjectTest, ToleratesAnEmptySetter)
{
    mt::W3CBaggagePropagator().Inject(mt::Baggage::FromHeader("a=1"), mt::HeaderSetter{});
    SUCCEED();  // the contract is "does not crash"; there is nowhere to write
}

TEST(W3CBaggagePropagatorInjectTest, WritesOnlyTheBaggageHeader)
{
    Headers headers;
    mt::W3CBaggagePropagator().Inject(mt::Baggage::FromHeader("a=1"), SetterFor(headers));
    EXPECT_EQ(headers.size(), 1U);
}

// ── Round-trips ──────────────────────────────────────────────────────────────

TEST(W3CBaggagePropagatorTest, RoundTripsThroughACarrier)
{
    const mt::Baggage sent = mt::Baggage{}.Set("user", "alice smith").Set("region", "eu-west-1");

    Headers headers;
    mt::W3CBaggagePropagator().Inject(sent, SetterFor(headers));
    const mt::Baggage received = mt::W3CBaggagePropagator().Extract(GetterFor(headers));

    ASSERT_EQ(received.Size(), 2U);
    EXPECT_EQ(*received.Get("user"), "alice smith");
    EXPECT_EQ(*received.Get("region"), "eu-west-1");
    EXPECT_EQ(received.ToHeader(), sent.ToHeader());
}

TEST(W3CBaggagePropagatorTest, RoundTripsPropertiesOpaquely)
{
    Headers headers;
    mt::W3CBaggagePropagator().Inject(mt::Baggage::FromHeader("k=v;md=1;flag"), SetterFor(headers));
    EXPECT_EQ(mt::W3CBaggagePropagator().Extract(GetterFor(headers)).ToHeader(), "k=v;md=1;flag");
}

TEST(W3CBaggagePropagatorTest, InjectsTheCurrentContextsBaggage)
{
    // How a caller reaches the thread's context: one expression, no extra
    // propagator surface (ICP 0025 §4).
    const mt::ScopedContext scope{
        mt::Context{mt::SpanContext{}, mt::Baggage{}.Set("tenant", "t1")}};

    Headers headers;
    mt::W3CBaggagePropagator().Inject(mt::CurrentContext().baggage, SetterFor(headers));
    EXPECT_EQ(headers.at(std::string(kBaggageHeader)), "tenant=t1");
}

TEST(W3CBaggagePropagatorTest, ExtractedBaggagePopulatesAContext)
{
    const Headers headers{{std::string(kBaggageHeader), "tenant=t2"}};

    mt::Context ctx;
    ctx.baggage = mt::W3CBaggagePropagator().Extract(GetterFor(headers));
    const mt::ScopedContext scope{ctx};

    ASSERT_TRUE(mt::CurrentContext().baggage.Get("tenant").has_value());
    EXPECT_EQ(*mt::CurrentContext().baggage.Get("tenant"), "t2");
}

TEST(W3CBaggagePropagatorTest, IsIndependentOfTheTraceContextPropagator)
{
    // Both propagators write into one carrier without colliding, which is the
    // whole point of them being siblings rather than one merged type.
    mt::TraceId::Bytes trace_bytes{};
    trace_bytes.fill(0x11U);
    mt::SpanId::Bytes span_bytes{};
    span_bytes.fill(0x22U);
    const mt::SpanContext active{
        .trace_id = mt::TraceId{trace_bytes},
        .span_id = mt::SpanId{span_bytes},
        .trace_flags = mt::TraceFlags{mt::TraceFlags::kSampled},
        .trace_state = {},
        .remote = false,
    };

    Headers headers;
    mt::W3CTraceContextPropagator().Inject(active, SetterFor(headers));
    mt::W3CBaggagePropagator().Inject(mt::Baggage::FromHeader("a=1"), SetterFor(headers));

    EXPECT_EQ(headers.size(), 2U);
    EXPECT_EQ(headers.at("baggage"), "a=1");
    EXPECT_TRUE(mt::W3CTraceContextPropagator().Extract(GetterFor(headers)).IsValid());
    EXPECT_EQ(mt::W3CBaggagePropagator().Extract(GetterFor(headers)).Size(), 1U);
}

}  // namespace
