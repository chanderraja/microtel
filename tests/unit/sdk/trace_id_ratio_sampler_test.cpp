// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Behavioural tests for the TraceIdRatio sampler.
//
// Sampler is deterministic on the trace id: the same input always produces
// the same decision. The lower 8 bytes of the 16-byte trace id are
// interpreted as a big-endian uint64_t and compared against
// `static_cast<uint64_t>(ratio * UINT64_MAX)`. Below threshold -> sample.
//
// Tests cover the contract, determinism, the boundary values 0 / 1 and
// out-of-range clamping, and the description string.

#include "microtel/internal/sampler.hpp"
#include "microtel/sampler.hpp"
#include "microtel/trace.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <string>

namespace mt = microtel;

namespace
{

mt::TraceId MakeTraceIdWithLowerBytes(std::uint64_t low_be)
{
    // Upper 8 bytes are arbitrary but fixed; lower 8 are the value the
    // sampler hashes against. Big-endian per the OTel sampling rule.
    std::array<std::uint8_t, mt::TraceId::kSizeBytes> bytes{
        0xa1,
        0xb2,
        0xc3,
        0xd4,
        0xe5,
        0xf6,
        0x07,
        0x18,
        // Lower 8 bytes filled in below from low_be (big-endian).
        0,
        0,
        0,
        0,
        0,
        0,
        0,
        0};
    for (std::size_t i = 0; i < 8; ++i)
    {
        const auto shift = static_cast<std::uint64_t>(56U - (8U * i));
        bytes.at(8 + i) = static_cast<std::uint8_t>((low_be >> shift) & std::uint64_t{0xffU});
    }
    return mt::TraceId{bytes};
}

mt::internal::SamplingContext MakeCtx(const mt::TraceId& trace_id)
{
    mt::internal::SamplingContext ctx{};
    ctx.trace_id = trace_id;
    return ctx;
}

TEST(TraceIdRatioSampler, FactoryReturnsNonNullHandle)
{
    const auto handle = mt::MakeTraceIdRatioSampler(0.5);
    EXPECT_NE(handle.Get(), nullptr);
}

TEST(TraceIdRatioSampler, RatioZeroAlwaysDrops)
{
    const auto handle = mt::MakeTraceIdRatioSampler(0.0);
    ASSERT_NE(handle.Get(), nullptr);
    for (const std::uint64_t low : {std::uint64_t{0},
                                    std::uint64_t{1},
                                    std::uint64_t{0x8000'0000'0000'0000ULL},
                                    std::uint64_t{0xffff'ffff'ffff'ffffULL}})
    {
        const auto ctx = MakeCtx(MakeTraceIdWithLowerBytes(low));
        EXPECT_EQ(handle.Get()->ShouldSample(ctx).decision, mt::internal::SamplingDecision::Drop);
    }
}

TEST(TraceIdRatioSampler, RatioOneAlwaysSamples)
{
    const auto handle = mt::MakeTraceIdRatioSampler(1.0);
    ASSERT_NE(handle.Get(), nullptr);
    for (const std::uint64_t low : {std::uint64_t{0},
                                    std::uint64_t{1},
                                    std::uint64_t{0x8000'0000'0000'0000ULL},
                                    std::uint64_t{0xffff'ffff'ffff'fffeULL}})
    {
        const auto ctx = MakeCtx(MakeTraceIdWithLowerBytes(low));
        EXPECT_EQ(handle.Get()->ShouldSample(ctx).decision,
                  mt::internal::SamplingDecision::RecordAndSample);
    }
}

TEST(TraceIdRatioSampler, HalfRatioPartitionsByThreshold)
{
    // Threshold for 0.5 is UINT64_MAX/2 ≈ 0x7fff'ffff'ffff'ffff.
    // Picks below sample; picks above drop.
    const auto handle = mt::MakeTraceIdRatioSampler(0.5);

    const auto below_ctx = MakeCtx(MakeTraceIdWithLowerBytes(0x1000'0000'0000'0000ULL));
    EXPECT_EQ(handle.Get()->ShouldSample(below_ctx).decision,
              mt::internal::SamplingDecision::RecordAndSample);

    const auto above_ctx = MakeCtx(MakeTraceIdWithLowerBytes(0xf000'0000'0000'0000ULL));
    EXPECT_EQ(handle.Get()->ShouldSample(above_ctx).decision, mt::internal::SamplingDecision::Drop);
}

TEST(TraceIdRatioSampler, IsDeterministicOnSameTraceId)
{
    const auto handle = mt::MakeTraceIdRatioSampler(0.5);
    const auto ctx = MakeCtx(MakeTraceIdWithLowerBytes(0x1234'5678'90ab'cdefULL));
    const auto first = handle.Get()->ShouldSample(ctx).decision;
    for (int i = 0; i < 10; ++i)
    {
        EXPECT_EQ(handle.Get()->ShouldSample(ctx).decision, first);
    }
}

TEST(TraceIdRatioSampler, RatioClampedBelowZero)
{
    const auto handle = mt::MakeTraceIdRatioSampler(-0.5);
    ASSERT_NE(handle.Get(), nullptr);
    const auto ctx = MakeCtx(MakeTraceIdWithLowerBytes(0x1000'0000'0000'0000ULL));
    EXPECT_EQ(handle.Get()->ShouldSample(ctx).decision, mt::internal::SamplingDecision::Drop);
}

TEST(TraceIdRatioSampler, RatioClampedAboveOne)
{
    const auto handle = mt::MakeTraceIdRatioSampler(1.5);
    ASSERT_NE(handle.Get(), nullptr);
    const auto ctx = MakeCtx(MakeTraceIdWithLowerBytes(0xf000'0000'0000'0000ULL));
    EXPECT_EQ(handle.Get()->ShouldSample(ctx).decision,
              mt::internal::SamplingDecision::RecordAndSample);
}

TEST(TraceIdRatioSampler, DescriptionIncludesRatio)
{
    const auto handle = mt::MakeTraceIdRatioSampler(0.25);
    ASSERT_NE(handle.Get(), nullptr);
    const std::string desc{handle.Get()->Description()};
    EXPECT_NE(desc.find("TraceIdRatio"), std::string::npos);
    EXPECT_NE(desc.find("0.25"), std::string::npos);
}

}  // namespace
