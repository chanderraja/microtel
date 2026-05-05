// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Behavioural tests for the AlwaysOn sampler.
//
// First production-code-driven test in microtel — kicks off the M3 TDD
// cadence per `microtel-spec.md` §14.2 / `docs/coding-standards.md` §11.

#include "microtel/internal/sampler.hpp"
#include "microtel/sampler.hpp"

#include <gtest/gtest.h>

#include <string_view>

namespace mt = microtel;

namespace
{

TEST(AlwaysOnSampler, FactoryReturnsNonNullHandle)
{
    const auto handle = mt::MakeAlwaysOnSampler();
    EXPECT_NE(handle.Get(), nullptr);
}

TEST(AlwaysOnSampler, ShouldSampleAlwaysReturnsRecordAndSample)
{
    const auto handle = mt::MakeAlwaysOnSampler();
    ASSERT_NE(handle.Get(), nullptr);

    const mt::internal::SamplingContext ctx{};
    const auto result = handle.Get()->ShouldSample(ctx);
    EXPECT_EQ(result.decision, mt::internal::SamplingDecision::RecordAndSample);
    EXPECT_TRUE(result.additional_attributes.empty());
    EXPECT_FALSE(result.trace_state.has_value());
}

TEST(AlwaysOnSampler, DescriptionIsAlwaysOn)
{
    const auto handle = mt::MakeAlwaysOnSampler();
    ASSERT_NE(handle.Get(), nullptr);
    EXPECT_EQ(handle.Get()->Description(), std::string_view{"AlwaysOnSampler"});
}

}  // namespace
