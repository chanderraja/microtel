// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// `ISampler::ShouldSample` must not allocate on the hot path (LOCKED --
// docs/interfaces.md 4.5, docs/memory-model.md 8.1). For the built-in
// samplers that is self-evident from reading them; for the chain combinators
// it is a property of how they are *built* -- children sized once at
// construction, the composed description formatted once at construction,
// short-circuit walks over a vector that is never resized -- and a property
// that a later edit can silently lose.
//
// So this counts. Global operator new is replaced with a counting one and the
// count is read either side of the call under test; anything the chain
// allocated lands between the two reads.
//
// Replacing operator new is process-wide, hence a dedicated binary, and the
// target is skipped under sanitizers: asan and tsan install their own
// global new/delete, and a second replacement either fails to link or trips
// alloc-dealloc-mismatch. Same arrangement as noexcept_alloc_failure_test.

#include "microtel/attribute.hpp"
#include "microtel/internal/sampler.hpp"
#include "microtel/sampler.hpp"
#include "microtel/trace.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <new>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{

// A replaced global operator new has nowhere else to keep its counter.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
std::atomic<std::size_t> g_allocation_count{0};

std::size_t AllocationCount() noexcept
{
    return g_allocation_count.load(std::memory_order_acquire);
}

}  // namespace

// NOLINTBEGIN(cppcoreguidelines-owning-memory,cppcoreguidelines-no-malloc)
void* operator new(std::size_t size)
{
    g_allocation_count.fetch_add(1, std::memory_order_acq_rel);
    // The pointee cannot be const: this is an allocator handing back
    // writable storage.
    // NOLINTNEXTLINE(misc-const-correctness)
    if (void* const allocated = std::malloc(size); allocated != nullptr)
    {
        return allocated;
    }
    throw std::bad_alloc{};
}

void* operator new(std::size_t size, const std::nothrow_t& /*tag*/) noexcept
{
    g_allocation_count.fetch_add(1, std::memory_order_acq_rel);
    return std::malloc(size);
}

void* operator new[](std::size_t size)
{
    return ::operator new(size);
}

void* operator new[](std::size_t size, const std::nothrow_t& tag) noexcept
{
    return ::operator new(size, tag);
}

void operator delete(void* p) noexcept
{
    std::free(p);
}

void operator delete(void* p, std::size_t /*size*/) noexcept
{
    std::free(p);
}

void operator delete(void* p, const std::nothrow_t& /*tag*/) noexcept
{
    std::free(p);
}

void operator delete[](void* p) noexcept
{
    std::free(p);
}

void operator delete[](void* p, std::size_t /*size*/) noexcept
{
    std::free(p);
}

void operator delete[](void* p, const std::nothrow_t& /*tag*/) noexcept
{
    std::free(p);
}
// NOLINTEND(cppcoreguidelines-owning-memory,cppcoreguidelines-no-malloc)

namespace
{

namespace mt = microtel;
namespace mti = microtel::internal;

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

/// @brief Asserts `sampler.ShouldSample(ctx)` allocates nothing.
///
/// One warm-up call first: this measures the steady state, not whatever the
/// first call through a fresh vtable happens to touch.
void ExpectNoAllocation(const mt::SamplerHandle& sampler, const mti::SamplingContext& ctx)
{
    ASSERT_NE(sampler.Get(), nullptr);
    const auto warmup = sampler.Get()->ShouldSample(ctx);
    EXPECT_TRUE(warmup.additional_attributes.empty());

    const std::size_t before = AllocationCount();
    const auto result = sampler.Get()->ShouldSample(ctx);
    const std::size_t after = AllocationCount();

    EXPECT_EQ(after, before) << "ShouldSample allocated " << (after - before)
                             << " time(s); the hot path must not allocate";
    EXPECT_NE(result.decision, mti::SamplingDecision::RecordOnly);
}

std::vector<mt::SamplerHandle> MakeChildren(mt::SamplerHandle first, mt::SamplerHandle second)
{
    std::vector<mt::SamplerHandle> children;
    children.reserve(2);
    children.push_back(std::move(first));
    children.push_back(std::move(second));
    return children;
}

// The counting allocator must itself be sound before anything is concluded
// from a zero.
TEST(SamplerChainAllocation, CounterObservesARealAllocation)
{
    const std::size_t before = AllocationCount();
    {
        const std::string forced(512, 'x');
        EXPECT_EQ(forced.size(), 512U);
    }
    EXPECT_GT(AllocationCount(), before);
}

TEST(SamplerChainAllocation, AttributeRuleDoesNotAllocate)
{
    std::vector<mt::KeyValue> attrs;
    attrs.push_back(
        mt::KeyValue{.key = "http.method", .value = mt::AttributeValue{std::string{"GET"}}});
    const auto ctx = MakeCtx("GET /orders", mt::SpanKind::Server, mt::AttributeSpan{attrs});

    const auto rule = mt::MakeAttributeRuleSampler("http.method",
                                                   mt::AttributeValue{std::string{"GET"}},
                                                   mt::MakeAlwaysOnSampler(),
                                                   mt::MakeAlwaysOffSampler());
    ExpectNoAllocation(rule, ctx);
}

TEST(SamplerChainAllocation, SpanNameRuleDoesNotAllocate)
{
    const auto ctx = MakeCtx("GET /orders", mt::SpanKind::Server, {});
    const auto rule = mt::MakeSpanNameRuleSampler(
        "GET /orders", mt::MakeAlwaysOnSampler(), mt::MakeAlwaysOffSampler());
    ExpectNoAllocation(rule, ctx);
}

TEST(SamplerChainAllocation, SpanKindRuleDoesNotAllocate)
{
    const auto ctx = MakeCtx("GET /orders", mt::SpanKind::Server, {});
    const auto rule = mt::MakeSpanKindRuleSampler(
        mt::SpanKind::Server, mt::MakeAlwaysOnSampler(), mt::MakeAlwaysOffSampler());
    ExpectNoAllocation(rule, ctx);
}

TEST(SamplerChainAllocation, FirstMatchChainDoesNotAllocate)
{
    std::vector<mt::KeyValue> attrs;
    attrs.push_back(
        mt::KeyValue{.key = "http.route", .value = mt::AttributeValue{std::string{"/orders"}}});
    const auto ctx = MakeCtx("GET /orders", mt::SpanKind::Server, mt::AttributeSpan{attrs});

    std::vector<mt::SamplerHandle> children;
    children.reserve(3);
    children.push_back(mt::MakeAttributeRuleSampler("http.route",
                                                    mt::AttributeValue{std::string{"/healthz"}},
                                                    mt::MakeAlwaysOffSampler(),
                                                    mt::MakeAlwaysOffSampler()));
    children.push_back(mt::MakeSpanKindRuleSampler(
        mt::SpanKind::Server, mt::MakeAlwaysOnSampler(), mt::MakeAlwaysOffSampler()));
    children.push_back(mt::MakeAlwaysOffSampler());

    const auto chain = mt::MakeChainSampler(std::move(children), mt::ChainMode::FirstMatch);
    ExpectNoAllocation(chain, ctx);
}

TEST(SamplerChainAllocation, AllMustAgreeChainDoesNotAllocate)
{
    const auto ctx = MakeCtx("GET /orders", mt::SpanKind::Server, {});
    const auto chain =
        mt::MakeChainSampler(MakeChildren(mt::MakeAlwaysOnSampler(),
                                          mt::MakeSpanKindRuleSampler(mt::SpanKind::Server,
                                                                      mt::MakeAlwaysOnSampler(),
                                                                      mt::MakeAlwaysOffSampler())),
                             mt::ChainMode::AllMustAgree);
    ExpectNoAllocation(chain, ctx);
}

// The short-circuiting path and the walk-everything path are different code;
// both are measured.
TEST(SamplerChainAllocation, ShortCircuitingAllMustAgreeChainDoesNotAllocate)
{
    const auto ctx = MakeCtx("GET /orders", mt::SpanKind::Server, {});
    const auto chain =
        mt::MakeChainSampler(MakeChildren(mt::MakeAlwaysOffSampler(), mt::MakeAlwaysOnSampler()),
                             mt::ChainMode::AllMustAgree);
    ExpectNoAllocation(chain, ctx);
}

TEST(SamplerChainAllocation, DescriptionDoesNotAllocate)
{
    const auto chain =
        mt::MakeChainSampler(MakeChildren(mt::MakeAlwaysOnSampler(), mt::MakeAlwaysOffSampler()),
                             mt::ChainMode::FirstMatch);
    ASSERT_NE(chain.Get(), nullptr);
    const std::string_view warmup = chain.Get()->Description();
    EXPECT_FALSE(warmup.empty());

    const std::size_t before = AllocationCount();
    const std::string_view desc = chain.Get()->Description();
    const std::size_t after = AllocationCount();

    EXPECT_EQ(after, before) << "Description() must return the string composed at construction";
    EXPECT_EQ(desc.data(), warmup.data());
}

}  // namespace
