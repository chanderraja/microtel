// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// v1.1 sugar layer — `TraceFunction`, `MICROTEL_TRACE_FUNCTION`, `Span` and
// `Traced` (ICP 0028 §1).
//
// Every helper here is a factory returning ICP 0025's `microtel::ScopedSpan`;
// sugar defines no RAII type of its own, so the end-then-restore ordering is
// tested where it lives (tests/unit/api/context_test.cpp) and not re-tested
// here. What is tested here is the translation from a sugar call into the
// `StartAsCurrentSpan` arguments underneath it, which `FakeTracer` records
// verbatim.
//
// `FakeTracer` hands out handles with a no-op deleter, so nothing in this file
// asserts on `End()`; the real ending — and the exported record it produces —
// is the integration test's job (tests/integration/sugar/).

#include "microtel/sugar/span.hpp"

#include "microtel/span.hpp"
#include "microtel/trace.hpp"

#include "fakes/fake_span.hpp"
#include "fakes/fake_tracer.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>

namespace mt = microtel::sugar;

namespace
{

/// Distinctively named so the `std::source_location` capture is unambiguous:
/// the default argument is evaluated at the call site, so the name recorded is
/// this function's, not `TraceFunction`'s.
void StartSpanInsideAHelperFunction(microtel::testing::FakeTracer& tracer)
{
    const microtel::ScopedSpan scope = mt::TraceFunction(tracer);
    EXPECT_NE(scope.Get(), nullptr);
}

/// Two macro uses in one block. That this compiles is the uniquing test:
/// without `__LINE__` in the generated name the second declaration would
/// redeclare the first.
void TwoMacroScopesInOneBlock(microtel::testing::FakeTracer& tracer)
{
    MICROTEL_TRACE_FUNCTION(tracer);
    MICROTEL_TRACE_FUNCTION(tracer);
}

TEST(SugarTraceFunction, NamesTheSpanAfterTheEnclosingFunction)
{
    microtel::testing::FakeTracer tracer;

    StartSpanInsideAHelperFunction(tracer);

    ASSERT_EQ(tracer.starts.size(), 1U);
    EXPECT_NE(tracer.starts[0].name.find("StartSpanInsideAHelperFunction"), std::string::npos)
        << "recorded name was: " << tracer.starts[0].name;
    EXPECT_EQ(tracer.starts[0].kind, microtel::SpanKind::Internal);
    EXPECT_TRUE(tracer.starts[0].attributes.empty());
}

TEST(SugarTraceFunction, PassesTheRequestedKindThrough)
{
    microtel::testing::FakeTracer tracer;

    const microtel::ScopedSpan scope = mt::TraceFunction(tracer, microtel::SpanKind::Server);

    ASSERT_EQ(tracer.starts.size(), 1U);
    EXPECT_EQ(tracer.starts[0].kind, microtel::SpanKind::Server);
    EXPECT_EQ(scope.Get(), tracer.spans.back().get());
}

TEST(SugarTraceFunction, MacroDeclaresOneScopePerUse)
{
    microtel::testing::FakeTracer tracer;

    TwoMacroScopesInOneBlock(tracer);

    ASSERT_EQ(tracer.starts.size(), 2U);
    EXPECT_NE(tracer.starts[0].name.find("TwoMacroScopesInOneBlock"), std::string::npos);
    EXPECT_EQ(tracer.starts[0].name, tracer.starts[1].name);
}

TEST(SugarSpan, StartsANamedSpanWithNoAttributesByDefault)
{
    microtel::testing::FakeTracer tracer;

    const microtel::ScopedSpan scope = mt::Span(tracer, "checkout");

    ASSERT_EQ(tracer.starts.size(), 1U);
    EXPECT_EQ(tracer.starts[0].name, "checkout");
    EXPECT_EQ(tracer.starts[0].kind, microtel::SpanKind::Internal);
    EXPECT_TRUE(tracer.starts[0].attributes.empty());
    EXPECT_NE(scope.Get(), nullptr);
}

TEST(SugarSpan, CarriesInlineAttributesIntoTheStart)
{
    microtel::testing::FakeTracer tracer;

    const microtel::ScopedSpan scope = mt::Span(tracer,
                                                "checkout",
                                                {{.key = "cart.id", .value = std::string{"c-42"}},
                                                 {.key = "cart.items", .value = std::int64_t{3}}});

    ASSERT_EQ(tracer.starts.size(), 1U);
    ASSERT_EQ(tracer.starts[0].attributes.size(), 2U);
    EXPECT_EQ(tracer.starts[0].attributes[0].key, "cart.id");
    EXPECT_EQ(std::get<std::string>(tracer.starts[0].attributes[0].value), "c-42");
    EXPECT_EQ(tracer.starts[0].attributes[1].key, "cart.items");
    EXPECT_EQ(std::get<std::int64_t>(tracer.starts[0].attributes[1].value), 3);
}

TEST(SugarSpan, TakesAKindAfterTheAttributes)
{
    microtel::testing::FakeTracer tracer;

    const microtel::ScopedSpan scope = mt::Span(tracer, "rpc.call", {}, microtel::SpanKind::Client);

    ASSERT_EQ(tracer.starts.size(), 1U);
    EXPECT_EQ(tracer.starts[0].kind, microtel::SpanKind::Client);
    EXPECT_TRUE(tracer.starts[0].attributes.empty());
}

TEST(SugarTraced, ReturnsTheCallablesValue)
{
    microtel::testing::FakeTracer tracer;

    const int result = mt::Traced(tracer, "compute", [] { return 7; });

    EXPECT_EQ(result, 7);
    ASSERT_EQ(tracer.starts.size(), 1U);
    EXPECT_EQ(tracer.starts[0].name, "compute");
}

TEST(SugarTraced, ForwardsAReferenceWithoutCopying)
{
    microtel::testing::FakeTracer tracer;
    std::string owned{"the-original"};
    const auto by_ref = [&owned]() -> std::string& { return owned; };

    static_assert(std::is_same_v<decltype(mt::Traced(tracer, "ref", by_ref)), std::string&>,
                  "Traced must forward the callable's reference return type");

    std::string& returned = mt::Traced(tracer, "ref", by_ref);
    returned += "-mutated";

    EXPECT_EQ(&returned, &owned);
    EXPECT_EQ(owned, "the-original-mutated");
}

TEST(SugarTraced, SupportsAVoidCallable)
{
    microtel::testing::FakeTracer tracer;
    bool ran = false;
    const auto body = [&ran] { ran = true; };

    static_assert(std::is_void_v<decltype(mt::Traced(tracer, "void", body))>,
                  "a void callable must leave Traced returning void");

    mt::Traced(tracer, "void", body);

    EXPECT_TRUE(ran);
    ASSERT_EQ(tracer.starts.size(), 1U);
    EXPECT_EQ(tracer.starts[0].name, "void");
}

TEST(SugarTraced, IsNoexceptExactlyWhenTheCallableIs)
{
    microtel::testing::FakeTracer tracer;
    const auto never_throws = []() noexcept { return 1; };
    const auto may_throw = [] { return 1; };

    static_assert(noexcept(mt::Traced(tracer, "n", never_throws)),
                  "a noexcept caller must keep its guarantee through Traced");
    static_assert(!noexcept(mt::Traced(tracer, "n", may_throw)),
                  "Traced must not launder a throwing callable into a noexcept call");

    EXPECT_EQ(mt::Traced(tracer, "n", never_throws), 1);
}

TEST(SugarTraced, LetsAnExceptionPropagateUnrecorded)
{
    microtel::testing::FakeTracer tracer;

    EXPECT_THROW(mt::Traced(tracer, "boom", [] { throw std::runtime_error{"kaboom"}; }),
                 std::runtime_error);

    // The span was started, and nothing recorded the exception on it:
    // catching is `mt::TryCatch`, roadmap §5 v1.4.
    ASSERT_EQ(tracer.starts.size(), 1U);
    EXPECT_EQ(tracer.starts[0].name, "boom");
    ASSERT_EQ(tracer.spans.size(), 1U);
    EXPECT_TRUE(tracer.spans[0]->statuses.empty());
    EXPECT_TRUE(tracer.spans[0]->events.empty());
}

}  // namespace
