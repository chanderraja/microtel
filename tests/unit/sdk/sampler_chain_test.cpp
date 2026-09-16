// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Behavioural tests for the v1.1 sampler rule combinators and the two chain
// composition modes (`microtel-roadmap.md` §4 v1.1, "Composable sampler
// chains").
//
// A rule combinator is a predicate over what a head sampler can see at
// `ShouldSample` time — an initial attribute, the span name, the span kind —
// plus two delegates, one for a match and one for a miss. That is the
// `ParentBasedSampler` shape (predicate on the parent, delegate to `root`)
// widened to a predicate the caller chooses.
//
// Sample-on-duration is deliberately absent: the decision is made before the
// span runs, so duration-based selection is tail sampling. See ICP 0024.
//
// The two modes differ in what they look at:
// - `FirstMatch` reads *predicates*. The first rule whose predicate matches
//   decides; a non-rule child matches unconditionally and terminates the
//   chain. A rule that does not match is skipped whole — its own miss
//   delegate is not consulted, because in a chain the chain is the miss path.
// - `AllMustAgree` reads *decisions*. Every child must return
//   `RecordAndSample`; the first that does not ends the walk with `Drop`.
//
// Both modes drop an empty chain.

#include "microtel/attribute.hpp"
#include "microtel/internal/sampler.hpp"
#include "microtel/sampler.hpp"
#include "microtel/trace.hpp"

#include "mocks/mock_sampler.hpp"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace mt = microtel;
namespace mti = microtel::internal;
namespace mtmk = microtel::testing;

namespace
{

constexpr std::string_view kMethodKey = "http.method";
constexpr std::string_view kRouteKey = "http.route";

mti::SamplingContext MakeCtx(std::string_view span_name,
                             mt::SpanKind kind,
                             mt::AttributeSpan attributes)
{
    mti::SamplingContext ctx{};
    ctx.span_name = span_name;
    ctx.span_kind = kind;
    ctx.initial_attributes = attributes;
    return ctx;
}

mti::SamplingContext MakeEmptyCtx()
{
    return mti::SamplingContext{};
}

std::vector<mt::KeyValue> MethodAttributes(std::string value)
{
    std::vector<mt::KeyValue> attrs;
    attrs.push_back(mt::KeyValue{.key = std::string{kMethodKey},
                                 .value = mt::AttributeValue{std::move(value)}});
    return attrs;
}

mti::SamplingDecision DecisionOf(const mt::SamplerHandle& handle, const mti::SamplingContext& ctx)
{
    return handle.Get()->ShouldSample(ctx).decision;
}

/// @brief A span kind and the name its description should carry.
struct KindName
{
    mt::SpanKind kind = mt::SpanKind::Internal;
    std::string_view name;
};

/// @brief A `MockSampler` wrapped in a handle, with a borrowed pointer kept so
/// the test can read back how many times the chain reached it.
struct Probe
{
    mt::SamplerHandle handle;
    const mtmk::MockSampler* mock = nullptr;
};

Probe MakeProbe(mti::SamplingDecision decision)
{
    auto mock = std::make_unique<mtmk::MockSampler>();
    mock->result_to_return.decision = decision;
    const auto* const borrowed = mock.get();
    return Probe{.handle = mt::SamplerHandle{std::move(mock)}, .mock = borrowed};
}

std::vector<mt::SamplerHandle> MakeChildren(mt::SamplerHandle first, mt::SamplerHandle second)
{
    std::vector<mt::SamplerHandle> children;
    children.reserve(2);
    children.push_back(std::move(first));
    children.push_back(std::move(second));
    return children;
}

// --- Attribute rule -----------------------------------------------------

TEST(AttributeRuleSampler, MatchingKeyAndValueDelegatesToOnMatch)
{
    const auto rule = mt::MakeAttributeRuleSampler(std::string{kMethodKey},
                                                   mt::AttributeValue{std::string{"GET"}},
                                                   mt::MakeAlwaysOnSampler(),
                                                   mt::MakeAlwaysOffSampler());
    ASSERT_NE(rule.Get(), nullptr);
    const auto attrs = MethodAttributes("GET");
    const auto ctx = MakeCtx("span", mt::SpanKind::Internal, mt::AttributeSpan{attrs});
    EXPECT_EQ(DecisionOf(rule, ctx), mti::SamplingDecision::RecordAndSample);
}

TEST(AttributeRuleSampler, WrongValueDelegatesToOnNoMatch)
{
    const auto rule = mt::MakeAttributeRuleSampler(std::string{kMethodKey},
                                                   mt::AttributeValue{std::string{"GET"}},
                                                   mt::MakeAlwaysOnSampler(),
                                                   mt::MakeAlwaysOffSampler());
    const auto attrs = MethodAttributes("POST");
    const auto ctx = MakeCtx("span", mt::SpanKind::Internal, mt::AttributeSpan{attrs});
    EXPECT_EQ(DecisionOf(rule, ctx), mti::SamplingDecision::Drop);
}

TEST(AttributeRuleSampler, AbsentKeyDelegatesToOnNoMatch)
{
    const auto rule = mt::MakeAttributeRuleSampler(std::string{kMethodKey},
                                                   mt::AttributeValue{std::string{"GET"}},
                                                   mt::MakeAlwaysOnSampler(),
                                                   mt::MakeAlwaysOffSampler());
    std::vector<mt::KeyValue> attrs;
    attrs.push_back(mt::KeyValue{.key = std::string{kRouteKey},
                                 .value = mt::AttributeValue{std::string{"/healthz"}}});
    const auto ctx = MakeCtx("span", mt::SpanKind::Internal, mt::AttributeSpan{attrs});
    EXPECT_EQ(DecisionOf(rule, ctx), mti::SamplingDecision::Drop);
}

TEST(AttributeRuleSampler, EmptyAttributeSpanDelegatesToOnNoMatch)
{
    const auto rule = mt::MakeAttributeRuleSampler(std::string{kMethodKey},
                                                   mt::AttributeValue{std::string{"GET"}},
                                                   mt::MakeAlwaysOnSampler(),
                                                   mt::MakeAlwaysOffSampler());
    EXPECT_EQ(DecisionOf(rule, MakeEmptyCtx()), mti::SamplingDecision::Drop);
}

// A non-string alternative: the comparison is on the whole variant, so a
// matching key holding a different type is a miss, not a match.
TEST(AttributeRuleSampler, SameKeyDifferentValueTypeDoesNotMatch)
{
    const auto rule = mt::MakeAttributeRuleSampler(std::string{kMethodKey},
                                                   mt::AttributeValue{std::int64_t{200}},
                                                   mt::MakeAlwaysOnSampler(),
                                                   mt::MakeAlwaysOffSampler());
    const auto attrs = MethodAttributes("200");
    const auto ctx = MakeCtx("span", mt::SpanKind::Internal, mt::AttributeSpan{attrs});
    EXPECT_EQ(DecisionOf(rule, ctx), mti::SamplingDecision::Drop);
}

TEST(AttributeRuleSampler, DescriptionNamesKeyAndBothDelegates)
{
    const auto rule = mt::MakeAttributeRuleSampler(std::string{kMethodKey},
                                                   mt::AttributeValue{std::string{"GET"}},
                                                   mt::MakeAlwaysOnSampler(),
                                                   mt::MakeAlwaysOffSampler());
    const std::string desc{rule.Get()->Description()};
    EXPECT_NE(desc.find("AttributeRuleSampler"), std::string::npos);
    EXPECT_NE(desc.find(kMethodKey), std::string::npos);
    EXPECT_NE(desc.find("AlwaysOnSampler"), std::string::npos);
    EXPECT_NE(desc.find("AlwaysOffSampler"), std::string::npos);
}

// --- Span-name rule -----------------------------------------------------

TEST(SpanNameRuleSampler, MatchingNameDelegatesToOnMatch)
{
    const auto rule = mt::MakeSpanNameRuleSampler(
        "GET /healthz", mt::MakeAlwaysOnSampler(), mt::MakeAlwaysOffSampler());
    ASSERT_NE(rule.Get(), nullptr);
    const auto ctx = MakeCtx("GET /healthz", mt::SpanKind::Server, {});
    EXPECT_EQ(DecisionOf(rule, ctx), mti::SamplingDecision::RecordAndSample);
}

TEST(SpanNameRuleSampler, DifferentNameDelegatesToOnNoMatch)
{
    const auto rule = mt::MakeSpanNameRuleSampler(
        "GET /healthz", mt::MakeAlwaysOnSampler(), mt::MakeAlwaysOffSampler());
    const auto ctx = MakeCtx("GET /orders", mt::SpanKind::Server, {});
    EXPECT_EQ(DecisionOf(rule, ctx), mti::SamplingDecision::Drop);
}

// The match is exact, not a prefix: a longer name that starts with the
// configured one is a miss.
TEST(SpanNameRuleSampler, MatchIsExactNotPrefix)
{
    const auto rule = mt::MakeSpanNameRuleSampler(
        "GET /health", mt::MakeAlwaysOnSampler(), mt::MakeAlwaysOffSampler());
    const auto ctx = MakeCtx("GET /healthz", mt::SpanKind::Server, {});
    EXPECT_EQ(DecisionOf(rule, ctx), mti::SamplingDecision::Drop);
}

TEST(SpanNameRuleSampler, EmptySpanNameDelegatesToOnNoMatch)
{
    const auto rule = mt::MakeSpanNameRuleSampler(
        "GET /healthz", mt::MakeAlwaysOnSampler(), mt::MakeAlwaysOffSampler());
    EXPECT_EQ(DecisionOf(rule, MakeEmptyCtx()), mti::SamplingDecision::Drop);
}

TEST(SpanNameRuleSampler, DescriptionNamesNameAndBothDelegates)
{
    const auto rule = mt::MakeSpanNameRuleSampler(
        "GET /healthz", mt::MakeAlwaysOnSampler(), mt::MakeAlwaysOffSampler());
    const std::string desc{rule.Get()->Description()};
    EXPECT_NE(desc.find("SpanNameRuleSampler"), std::string::npos);
    EXPECT_NE(desc.find("GET /healthz"), std::string::npos);
    EXPECT_NE(desc.find("AlwaysOnSampler"), std::string::npos);
    EXPECT_NE(desc.find("AlwaysOffSampler"), std::string::npos);
}

// --- Span-kind rule -----------------------------------------------------

TEST(SpanKindRuleSampler, MatchingKindDelegatesToOnMatch)
{
    const auto rule = mt::MakeSpanKindRuleSampler(
        mt::SpanKind::Server, mt::MakeAlwaysOnSampler(), mt::MakeAlwaysOffSampler());
    ASSERT_NE(rule.Get(), nullptr);
    const auto ctx = MakeCtx("span", mt::SpanKind::Server, {});
    EXPECT_EQ(DecisionOf(rule, ctx), mti::SamplingDecision::RecordAndSample);
}

TEST(SpanKindRuleSampler, DifferentKindDelegatesToOnNoMatch)
{
    const auto rule = mt::MakeSpanKindRuleSampler(
        mt::SpanKind::Server, mt::MakeAlwaysOnSampler(), mt::MakeAlwaysOffSampler());
    const auto ctx = MakeCtx("span", mt::SpanKind::Client, {});
    EXPECT_EQ(DecisionOf(rule, ctx), mti::SamplingDecision::Drop);
}

// SpanKind::Internal is the default-constructed value; a rule on it must not
// be satisfied by a context nobody filled in differently.
TEST(SpanKindRuleSampler, DefaultKindIsInternal)
{
    const auto rule = mt::MakeSpanKindRuleSampler(
        mt::SpanKind::Internal, mt::MakeAlwaysOnSampler(), mt::MakeAlwaysOffSampler());
    EXPECT_EQ(DecisionOf(rule, MakeEmptyCtx()), mti::SamplingDecision::RecordAndSample);
}

TEST(SpanKindRuleSampler, DescriptionNamesKindAndBothDelegates)
{
    const auto rule = mt::MakeSpanKindRuleSampler(
        mt::SpanKind::Producer, mt::MakeAlwaysOnSampler(), mt::MakeAlwaysOffSampler());
    const std::string desc{rule.Get()->Description()};
    EXPECT_NE(desc.find("SpanKindRuleSampler"), std::string::npos);
    EXPECT_NE(desc.find("Producer"), std::string::npos);
    EXPECT_NE(desc.find("AlwaysOnSampler"), std::string::npos);
    EXPECT_NE(desc.find("AlwaysOffSampler"), std::string::npos);
}

TEST(SpanKindRuleSampler, DescriptionNamesEveryKind)
{
    const std::array<KindName, 5> kinds{{
        {.kind = mt::SpanKind::Internal, .name = "Internal"},
        {.kind = mt::SpanKind::Server, .name = "Server"},
        {.kind = mt::SpanKind::Client, .name = "Client"},
        {.kind = mt::SpanKind::Producer, .name = "Producer"},
        {.kind = mt::SpanKind::Consumer, .name = "Consumer"},
    }};
    for (const KindName& entry : kinds)
    {
        const auto rule = mt::MakeSpanKindRuleSampler(
            entry.kind, mt::MakeAlwaysOnSampler(), mt::MakeAlwaysOffSampler());
        const std::string desc{rule.Get()->Description()};
        EXPECT_NE(desc.find(entry.name), std::string::npos) << "kind missing: " << entry.name;
    }
}

// --- Empty delegates ----------------------------------------------------

// An empty `SamplerHandle` is a contract violation. Dereferencing one inside
// a `noexcept` hot-path frame would take the host process down, so a rule
// with an empty delegate answers `Drop` on that branch — and says so in its
// description rather than pretending the delegate is a sampler.
TEST(SpanKindRuleSampler, EmptyDelegateAnswersDrop)
{
    const auto rule = mt::MakeSpanKindRuleSampler(
        mt::SpanKind::Server, mt::SamplerHandle{}, mt::MakeAlwaysOnSampler());
    ASSERT_NE(rule.Get(), nullptr);

    const auto server = MakeCtx("span", mt::SpanKind::Server, {});
    EXPECT_EQ(DecisionOf(rule, server), mti::SamplingDecision::Drop);

    // The other delegate is intact and still answers.
    const auto client = MakeCtx("span", mt::SpanKind::Client, {});
    EXPECT_EQ(DecisionOf(rule, client), mti::SamplingDecision::RecordAndSample);

    const std::string desc{rule.Get()->Description()};
    EXPECT_NE(desc.find("<null>"), std::string::npos);
}

// --- Rules delegate to arbitrary samplers, not just the constants --------

TEST(SpanKindRuleSampler, DelegatesNestArbitrarily)
{
    auto inner = mt::MakeSpanNameRuleSampler(
        "GET /healthz", mt::MakeAlwaysOffSampler(), mt::MakeAlwaysOnSampler());
    const auto rule = mt::MakeSpanKindRuleSampler(
        mt::SpanKind::Server, std::move(inner), mt::MakeAlwaysOffSampler());

    const auto healthz = MakeCtx("GET /healthz", mt::SpanKind::Server, {});
    EXPECT_EQ(DecisionOf(rule, healthz), mti::SamplingDecision::Drop);

    const auto orders = MakeCtx("GET /orders", mt::SpanKind::Server, {});
    EXPECT_EQ(DecisionOf(rule, orders), mti::SamplingDecision::RecordAndSample);

    const auto client = MakeCtx("GET /orders", mt::SpanKind::Client, {});
    EXPECT_EQ(DecisionOf(rule, client), mti::SamplingDecision::Drop);
}

// --- FirstMatch composition --------------------------------------------

TEST(ChainSamplerFirstMatch, FirstMatchingRuleDecides)
{
    std::vector<mt::SamplerHandle> children;
    children.reserve(2);
    // Matches nothing in the context below.
    children.push_back(mt::MakeSpanNameRuleSampler(
        "GET /healthz", mt::MakeAlwaysOffSampler(), mt::MakeAlwaysOffSampler()));
    // Matches, and its on-match delegate samples.
    children.push_back(mt::MakeSpanKindRuleSampler(
        mt::SpanKind::Server, mt::MakeAlwaysOnSampler(), mt::MakeAlwaysOffSampler()));

    const auto chain = mt::MakeChainSampler(std::move(children), mt::ChainMode::FirstMatch);
    ASSERT_NE(chain.Get(), nullptr);
    const auto ctx = MakeCtx("GET /orders", mt::SpanKind::Server, {});
    EXPECT_EQ(DecisionOf(chain, ctx), mti::SamplingDecision::RecordAndSample);
}

// The earlier rule's own miss delegate must not swallow the chain: in a chain
// the chain is the miss path.
TEST(ChainSamplerFirstMatch, MissingRuleDoesNotConsultItsOwnNoMatchDelegate)
{
    auto probe = MakeProbe(mti::SamplingDecision::RecordAndSample);
    const auto* const no_match_probe = probe.mock;

    std::vector<mt::SamplerHandle> children;
    children.reserve(2);
    children.push_back(mt::MakeSpanNameRuleSampler(
        "GET /healthz", mt::MakeAlwaysOffSampler(), std::move(probe.handle)));
    children.push_back(mt::MakeSpanKindRuleSampler(
        mt::SpanKind::Server, mt::MakeAlwaysOnSampler(), mt::MakeAlwaysOffSampler()));

    const auto chain = mt::MakeChainSampler(std::move(children), mt::ChainMode::FirstMatch);
    const auto ctx = MakeCtx("GET /orders", mt::SpanKind::Server, {});
    EXPECT_EQ(DecisionOf(chain, ctx), mti::SamplingDecision::RecordAndSample);
    EXPECT_EQ(no_match_probe->should_sample_call_count, 0);
}

TEST(ChainSamplerFirstMatch, LaterChildrenAreNotConsultedAfterAMatch)
{
    auto after = MakeProbe(mti::SamplingDecision::RecordAndSample);
    const auto* const after_probe = after.mock;

    std::vector<mt::SamplerHandle> children;
    children.reserve(2);
    children.push_back(mt::MakeSpanKindRuleSampler(
        mt::SpanKind::Server, mt::MakeAlwaysOffSampler(), mt::MakeAlwaysOnSampler()));
    children.push_back(std::move(after.handle));

    const auto chain = mt::MakeChainSampler(std::move(children), mt::ChainMode::FirstMatch);
    const auto ctx = MakeCtx("span", mt::SpanKind::Server, {});
    EXPECT_EQ(DecisionOf(chain, ctx), mti::SamplingDecision::Drop);
    EXPECT_EQ(after_probe->should_sample_call_count, 0);
}

// A plain sampler carries no predicate, so it matches unconditionally: it is
// how a chain spells "default".
TEST(ChainSamplerFirstMatch, NonRuleChildMatchesUnconditionally)
{
    std::vector<mt::SamplerHandle> children;
    children.reserve(2);
    children.push_back(mt::MakeSpanNameRuleSampler(
        "GET /healthz", mt::MakeAlwaysOffSampler(), mt::MakeAlwaysOffSampler()));
    children.push_back(mt::MakeAlwaysOnSampler());

    const auto chain = mt::MakeChainSampler(std::move(children), mt::ChainMode::FirstMatch);
    const auto ctx = MakeCtx("GET /orders", mt::SpanKind::Server, {});
    EXPECT_EQ(DecisionOf(chain, ctx), mti::SamplingDecision::RecordAndSample);
}

TEST(ChainSamplerFirstMatch, NoRuleMatchesDrops)
{
    std::vector<mt::SamplerHandle> children;
    children.reserve(2);
    children.push_back(mt::MakeSpanNameRuleSampler(
        "GET /healthz", mt::MakeAlwaysOnSampler(), mt::MakeAlwaysOnSampler()));
    children.push_back(mt::MakeSpanKindRuleSampler(
        mt::SpanKind::Consumer, mt::MakeAlwaysOnSampler(), mt::MakeAlwaysOnSampler()));

    const auto chain = mt::MakeChainSampler(std::move(children), mt::ChainMode::FirstMatch);
    const auto ctx = MakeCtx("GET /orders", mt::SpanKind::Server, {});
    EXPECT_EQ(DecisionOf(chain, ctx), mti::SamplingDecision::Drop);
}

TEST(ChainSamplerFirstMatch, EmptyChainDrops)
{
    const auto chain =
        mt::MakeChainSampler(std::vector<mt::SamplerHandle>{}, mt::ChainMode::FirstMatch);
    ASSERT_NE(chain.Get(), nullptr);
    EXPECT_EQ(DecisionOf(chain, MakeEmptyCtx()), mti::SamplingDecision::Drop);
}

// --- AllMustAgree composition -------------------------------------------

TEST(ChainSamplerAllMustAgree, AllSamplingChildrenSample)
{
    const auto chain =
        mt::MakeChainSampler(MakeChildren(mt::MakeAlwaysOnSampler(), mt::MakeAlwaysOnSampler()),
                             mt::ChainMode::AllMustAgree);
    ASSERT_NE(chain.Get(), nullptr);
    EXPECT_EQ(DecisionOf(chain, MakeEmptyCtx()), mti::SamplingDecision::RecordAndSample);
}

TEST(ChainSamplerAllMustAgree, OneDroppingChildDrops)
{
    const auto chain =
        mt::MakeChainSampler(MakeChildren(mt::MakeAlwaysOnSampler(), mt::MakeAlwaysOffSampler()),
                             mt::ChainMode::AllMustAgree);
    EXPECT_EQ(DecisionOf(chain, MakeEmptyCtx()), mti::SamplingDecision::Drop);
}

TEST(ChainSamplerAllMustAgree, FirstChildDroppingDrops)
{
    const auto chain =
        mt::MakeChainSampler(MakeChildren(mt::MakeAlwaysOffSampler(), mt::MakeAlwaysOnSampler()),
                             mt::ChainMode::AllMustAgree);
    EXPECT_EQ(DecisionOf(chain, MakeEmptyCtx()), mti::SamplingDecision::Drop);
}

// RecordOnly is not RecordAndSample, so it does not carry the conjunction.
TEST(ChainSamplerAllMustAgree, RecordOnlyChildDrops)
{
    auto probe = MakeProbe(mti::SamplingDecision::RecordOnly);
    const auto chain =
        mt::MakeChainSampler(MakeChildren(mt::MakeAlwaysOnSampler(), std::move(probe.handle)),
                             mt::ChainMode::AllMustAgree);
    EXPECT_EQ(DecisionOf(chain, MakeEmptyCtx()), mti::SamplingDecision::Drop);
}

TEST(ChainSamplerAllMustAgree, WalkStopsAtTheFirstDissent)
{
    auto after = MakeProbe(mti::SamplingDecision::RecordAndSample);
    const auto* const after_probe = after.mock;

    std::vector<mt::SamplerHandle> children;
    children.reserve(3);
    children.push_back(mt::MakeAlwaysOnSampler());
    children.push_back(mt::MakeAlwaysOffSampler());
    children.push_back(std::move(after.handle));

    const auto chain = mt::MakeChainSampler(std::move(children), mt::ChainMode::AllMustAgree);
    EXPECT_EQ(DecisionOf(chain, MakeEmptyCtx()), mti::SamplingDecision::Drop);
    EXPECT_EQ(after_probe->should_sample_call_count, 0);
}

TEST(ChainSamplerAllMustAgree, EveryChildIsConsultedWhenAllAgree)
{
    auto first = MakeProbe(mti::SamplingDecision::RecordAndSample);
    auto second = MakeProbe(mti::SamplingDecision::RecordAndSample);
    const auto* const first_probe = first.mock;
    const auto* const second_probe = second.mock;

    const auto chain =
        mt::MakeChainSampler(MakeChildren(std::move(first.handle), std::move(second.handle)),
                             mt::ChainMode::AllMustAgree);
    EXPECT_EQ(DecisionOf(chain, MakeEmptyCtx()), mti::SamplingDecision::RecordAndSample);
    EXPECT_EQ(first_probe->should_sample_call_count, 1);
    EXPECT_EQ(second_probe->should_sample_call_count, 1);
}

// A rule child in AllMustAgree is read for its decision, so its miss delegate
// *is* what speaks when the predicate does not match.
TEST(ChainSamplerAllMustAgree, RuleChildContributesItsNoMatchDelegate)
{
    std::vector<mt::SamplerHandle> children;
    children.reserve(2);
    children.push_back(mt::MakeAlwaysOnSampler());
    children.push_back(mt::MakeSpanKindRuleSampler(
        mt::SpanKind::Server, mt::MakeAlwaysOffSampler(), mt::MakeAlwaysOnSampler()));

    const auto chain = mt::MakeChainSampler(std::move(children), mt::ChainMode::AllMustAgree);
    const auto client = MakeCtx("span", mt::SpanKind::Client, {});
    EXPECT_EQ(DecisionOf(chain, client), mti::SamplingDecision::RecordAndSample);

    const auto server = MakeCtx("span", mt::SpanKind::Server, {});
    EXPECT_EQ(DecisionOf(chain, server), mti::SamplingDecision::Drop);
}

TEST(ChainSamplerAllMustAgree, EmptyChainDrops)
{
    const auto chain =
        mt::MakeChainSampler(std::vector<mt::SamplerHandle>{}, mt::ChainMode::AllMustAgree);
    ASSERT_NE(chain.Get(), nullptr);
    EXPECT_EQ(DecisionOf(chain, MakeEmptyCtx()), mti::SamplingDecision::Drop);
}

// --- Chain construction edges -------------------------------------------

// An empty handle is a contract violation; dereferencing one on the hot path
// would be a crash inside a noexcept frame, so the chain drops such children
// once, at construction.
TEST(ChainSampler, EmptyChildHandlesAreDroppedAtConstruction)
{
    std::vector<mt::SamplerHandle> children;
    children.reserve(2);
    children.emplace_back();
    children.push_back(mt::MakeAlwaysOnSampler());

    const auto chain = mt::MakeChainSampler(std::move(children), mt::ChainMode::AllMustAgree);
    EXPECT_EQ(DecisionOf(chain, MakeEmptyCtx()), mti::SamplingDecision::RecordAndSample);
}

TEST(ChainSampler, VariadicOverloadForwardsIntoTheVector)
{
    const auto chain = mt::MakeChainSampler(mt::ChainMode::AllMustAgree,
                                            mt::MakeAlwaysOnSampler(),
                                            mt::MakeAlwaysOnSampler(),
                                            mt::MakeAlwaysOffSampler());
    ASSERT_NE(chain.Get(), nullptr);
    EXPECT_EQ(DecisionOf(chain, MakeEmptyCtx()), mti::SamplingDecision::Drop);
}

TEST(ChainSampler, VariadicOverloadAcceptsASingleChild)
{
    const auto chain = mt::MakeChainSampler(mt::ChainMode::FirstMatch, mt::MakeAlwaysOnSampler());
    EXPECT_EQ(DecisionOf(chain, MakeEmptyCtx()), mti::SamplingDecision::RecordAndSample);
}

TEST(ChainSampler, DescriptionNamesModeAndEveryChild)
{
    const auto first_match =
        mt::MakeChainSampler(MakeChildren(mt::MakeAlwaysOnSampler(), mt::MakeAlwaysOffSampler()),
                             mt::ChainMode::FirstMatch);
    const std::string desc{first_match.Get()->Description()};
    EXPECT_NE(desc.find("ChainSampler"), std::string::npos);
    EXPECT_NE(desc.find("FirstMatch"), std::string::npos);
    EXPECT_NE(desc.find("AlwaysOnSampler"), std::string::npos);
    EXPECT_NE(desc.find("AlwaysOffSampler"), std::string::npos);

    const auto all_agree =
        mt::MakeChainSampler(MakeChildren(mt::MakeAlwaysOnSampler(), mt::MakeAlwaysOffSampler()),
                             mt::ChainMode::AllMustAgree);
    const std::string all_desc{all_agree.Get()->Description()};
    EXPECT_NE(all_desc.find("AllMustAgree"), std::string::npos);
}

TEST(ChainSampler, DescriptionIsStableAcrossCalls)
{
    const auto chain =
        mt::MakeChainSampler(MakeChildren(mt::MakeAlwaysOnSampler(), mt::MakeAlwaysOffSampler()),
                             mt::ChainMode::FirstMatch);
    const std::string_view first = chain.Get()->Description();
    const std::string_view second = chain.Get()->Description();
    EXPECT_EQ(first.data(), second.data());
    EXPECT_EQ(first, second);
}

TEST(ChainSampler, NestedChainsCompose)
{
    auto inner =
        mt::MakeChainSampler(MakeChildren(mt::MakeAlwaysOnSampler(), mt::MakeAlwaysOnSampler()),
                             mt::ChainMode::AllMustAgree);
    const auto outer = mt::MakeChainSampler(
        MakeChildren(std::move(inner), mt::MakeAlwaysOffSampler()), mt::ChainMode::FirstMatch);
    // FirstMatch: the inner chain carries no predicate, so it decides.
    EXPECT_EQ(DecisionOf(outer, MakeEmptyCtx()), mti::SamplingDecision::RecordAndSample);
}

// --- Concurrency ---------------------------------------------------------

/// @brief Asks `chain` for a decision `iterations` times, counting the
/// sampled answers. Lives outside the test body to keep the thread lambda
/// within the project's three-level nesting limit.
void CountSampled(const mt::SamplerHandle& chain,
                  std::size_t iterations,
                  std::atomic<std::size_t>& sampled)
{
    const auto ctx = MakeCtx("span", mt::SpanKind::Server, {});
    for (std::size_t i = 0; i < iterations; ++i)
    {
        if (DecisionOf(chain, ctx) == mti::SamplingDecision::RecordAndSample)
        {
            sampled.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

// `ShouldSample` is called from the caller thread on the hot path and must be
// thread-safe (`docs/interfaces.md` §4.5, LOCKED). The chain is immutable
// after construction; this is the test that says so, and it is the one that
// carries weight under `-DMICROTEL_SANITIZER=tsan`.
TEST(ChainSampler, ConcurrentShouldSampleCallsAgree)
{
    constexpr std::size_t kThreads = 4;
    constexpr std::size_t kIterations = 200;

    const auto chain =
        mt::MakeChainSampler(MakeChildren(mt::MakeSpanKindRuleSampler(mt::SpanKind::Server,
                                                                      mt::MakeAlwaysOnSampler(),
                                                                      mt::MakeAlwaysOffSampler()),
                                          mt::MakeAlwaysOffSampler()),
                             mt::ChainMode::FirstMatch);

    std::atomic<std::size_t> sampled{0};
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (std::size_t t = 0; t < kThreads; ++t)
    {
        threads.emplace_back([&chain, &sampled]() { CountSampled(chain, kIterations, sampled); });
    }
    for (std::thread& thread : threads)
    {
        thread.join();
    }
    EXPECT_EQ(sampled.load(std::memory_order_relaxed), kThreads * kIterations);
}

// ---------------------------------------------------------------------------
// TrySetRatio — ICP 0026 §5: "Sampler chains inherit the obligation"
// ---------------------------------------------------------------------------

TEST(RuleSampler, TrySetRatioForwardsToADelegateAndRegeneratesTheDescription)
{
    const auto rule = mt::MakeSpanKindRuleSampler(
        mt::SpanKind::Server, mt::MakeTraceIdRatioSampler(0.25), mt::MakeAlwaysOffSampler());
    ASSERT_NE(rule.Get(), nullptr);

    EXPECT_TRUE(rule.Get()->TrySetRatio(0.01));
    const std::string desc{rule.Get()->Description()};
    EXPECT_NE(desc.find("SpanKindRuleSampler"), std::string::npos);
    EXPECT_NE(desc.find("0.010"), std::string::npos);
    EXPECT_EQ(desc.find("0.250"), std::string::npos);
}

TEST(RuleSampler, TrySetRatioIsRefusedWhenNeitherDelegateHasARatio)
{
    const auto rule =
        mt::MakeSpanNameRuleSampler("span", mt::MakeAlwaysOnSampler(), mt::MakeAlwaysOffSampler());
    ASSERT_NE(rule.Get(), nullptr);
    const std::string before{rule.Get()->Description()};
    EXPECT_FALSE(rule.Get()->TrySetRatio(0.5));
    EXPECT_EQ(std::string{rule.Get()->Description()}, before);
}

TEST(ChainSampler, TrySetRatioReachesAChildAndRegeneratesTheDescription)
{
    const auto chain = mt::MakeChainSampler(
        MakeChildren(mt::MakeTraceIdRatioSampler(0.25), mt::MakeAlwaysOnSampler()),
        mt::ChainMode::AllMustAgree);
    ASSERT_NE(chain.Get(), nullptr);

    EXPECT_TRUE(chain.Get()->TrySetRatio(0.01));
    const std::string desc{chain.Get()->Description()};
    EXPECT_NE(desc.find("ChainSampler"), std::string::npos);
    EXPECT_NE(desc.find("0.010"), std::string::npos);
    EXPECT_EQ(desc.find("0.250"), std::string::npos);
}

TEST(ChainSampler, TrySetRatioChangesEveryRatioChildsDecision)
{
    const auto chain = mt::MakeChainSampler(
        MakeChildren(mt::MakeTraceIdRatioSampler(0.0), mt::MakeTraceIdRatioSampler(0.0)),
        mt::ChainMode::AllMustAgree);
    ASSERT_NE(chain.Get(), nullptr);
    const auto ctx = MakeEmptyCtx();
    ASSERT_EQ(DecisionOf(chain, ctx), mti::SamplingDecision::Drop);

    EXPECT_TRUE(chain.Get()->TrySetRatio(1.0));
    EXPECT_EQ(DecisionOf(chain, ctx), mti::SamplingDecision::RecordAndSample);
}

TEST(ChainSampler, TrySetRatioIsRefusedWhenNoChildHasARatio)
{
    const auto chain =
        mt::MakeChainSampler(MakeChildren(mt::MakeAlwaysOnSampler(), mt::MakeAlwaysOffSampler()),
                             mt::ChainMode::FirstMatch);
    ASSERT_NE(chain.Get(), nullptr);
    const std::string before{chain.Get()->Description()};
    EXPECT_FALSE(chain.Get()->TrySetRatio(0.5));
    EXPECT_EQ(std::string{chain.Get()->Description()}, before);
}

}  // namespace
