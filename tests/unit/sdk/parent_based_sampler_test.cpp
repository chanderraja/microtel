// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Behavioural tests for the ParentBased sampler.
//
// v1 ParentBased semantics per `include/microtel/sampler.hpp`:
// - If the parent context exists and is sampled, samples (regardless of
//   the root sampler's decision).
// - Otherwise, delegates to the configured `root` sampler.
//
// Tests cover the contract surface above plus description formatting.

#include "microtel/internal/sampler.hpp"
#include "microtel/sampler.hpp"
#include "microtel/trace.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <string>

namespace mt = microtel;

namespace
{

mt::SpanContext MakeParentContext(bool sampled)
{
    mt::SpanContext sc;
    // Non-zero trace + span ids so SpanContext::IsValid() returns true.
    sc.trace_id = mt::TraceId{std::array<std::uint8_t, mt::TraceId::kSizeBytes>{0x01,
                                                                                0x02,
                                                                                0x03,
                                                                                0x04,
                                                                                0x05,
                                                                                0x06,
                                                                                0x07,
                                                                                0x08,
                                                                                0x09,
                                                                                0x0a,
                                                                                0x0b,
                                                                                0x0c,
                                                                                0x0d,
                                                                                0x0e,
                                                                                0x0f,
                                                                                0x10}};
    sc.span_id = mt::SpanId{std::array<std::uint8_t, mt::SpanId::kSizeBytes>{
        0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18}};
    sc.trace_flags = mt::TraceFlags{sampled ? mt::TraceFlags::kSampled : std::uint8_t{0}};
    return sc;
}

mt::internal::SamplingContext MakeCtxWithParent(const mt::SpanContext& parent)
{
    mt::internal::SamplingContext ctx{};
    ctx.parent = parent;
    return ctx;
}

mt::internal::SamplingContext MakeCtxWithoutParent()
{
    return mt::internal::SamplingContext{};  // parent default-constructed → invalid
}

TEST(ParentBasedSampler, FactoryReturnsNonNullHandle)
{
    const auto handle = mt::MakeParentBasedSampler(mt::MakeAlwaysOnSampler());
    EXPECT_NE(handle.Get(), nullptr);
}

TEST(ParentBasedSampler, NoParentDelegatesToRootAlwaysOn)
{
    const auto handle = mt::MakeParentBasedSampler(mt::MakeAlwaysOnSampler());
    const auto ctx = MakeCtxWithoutParent();
    EXPECT_EQ(handle.Get()->ShouldSample(ctx).decision,
              mt::internal::SamplingDecision::RecordAndSample);
}

TEST(ParentBasedSampler, NoParentDelegatesToRootAlwaysOff)
{
    const auto handle = mt::MakeParentBasedSampler(mt::MakeAlwaysOffSampler());
    const auto ctx = MakeCtxWithoutParent();
    EXPECT_EQ(handle.Get()->ShouldSample(ctx).decision, mt::internal::SamplingDecision::Drop);
}

TEST(ParentBasedSampler, SampledParentSamplesRegardlessOfRoot)
{
    const auto handle = mt::MakeParentBasedSampler(mt::MakeAlwaysOffSampler());
    const auto ctx = MakeCtxWithParent(MakeParentContext(/*sampled=*/true));
    EXPECT_EQ(handle.Get()->ShouldSample(ctx).decision,
              mt::internal::SamplingDecision::RecordAndSample);
}

TEST(ParentBasedSampler, NotSampledParentDelegatesToRoot)
{
    const auto handle = mt::MakeParentBasedSampler(mt::MakeAlwaysOnSampler());
    const auto ctx = MakeCtxWithParent(MakeParentContext(/*sampled=*/false));
    EXPECT_EQ(handle.Get()->ShouldSample(ctx).decision,
              mt::internal::SamplingDecision::RecordAndSample);

    const auto handle_off = mt::MakeParentBasedSampler(mt::MakeAlwaysOffSampler());
    EXPECT_EQ(handle_off.Get()->ShouldSample(ctx).decision, mt::internal::SamplingDecision::Drop);
}

TEST(ParentBasedSampler, DescriptionIncludesRootDescription)
{
    const auto handle = mt::MakeParentBasedSampler(mt::MakeAlwaysOnSampler());
    ASSERT_NE(handle.Get(), nullptr);
    const std::string desc{handle.Get()->Description()};
    EXPECT_NE(desc.find("ParentBased"), std::string::npos);
    EXPECT_NE(desc.find("AlwaysOnSampler"), std::string::npos);
}

}  // namespace
