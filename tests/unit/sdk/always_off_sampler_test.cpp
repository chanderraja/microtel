// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Behavioural tests for the AlwaysOff sampler.

#include "microtel/internal/sampler.hpp"
#include "microtel/sampler.hpp"

#include <gtest/gtest.h>

#include <string_view>

namespace mt = microtel;

namespace
{

TEST(AlwaysOffSampler, FactoryReturnsNonNullHandle)
{
    const auto handle = mt::MakeAlwaysOffSampler();
    EXPECT_NE(handle.Get(), nullptr);
}

TEST(AlwaysOffSampler, ShouldSampleAlwaysReturnsDrop)
{
    const auto handle = mt::MakeAlwaysOffSampler();
    ASSERT_NE(handle.Get(), nullptr);

    const mt::internal::SamplingContext ctx{};
    const auto result = handle.Get()->ShouldSample(ctx);
    EXPECT_EQ(result.decision, mt::internal::SamplingDecision::Drop);
    EXPECT_TRUE(result.additional_attributes.empty());
    EXPECT_FALSE(result.trace_state.has_value());
}

TEST(AlwaysOffSampler, DescriptionIsAlwaysOff)
{
    const auto handle = mt::MakeAlwaysOffSampler();
    ASSERT_NE(handle.Get(), nullptr);
    EXPECT_EQ(handle.Get()->Description(), std::string_view{"AlwaysOffSampler"});
}

}  // namespace
