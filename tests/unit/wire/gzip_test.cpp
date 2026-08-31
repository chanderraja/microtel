// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for GzipCompress — the request-compression primitive shared by
// the HTTP and gRPC codecs (grpc-wire-protocol.md §5.1).

#include "wire/gzip.hpp"

#include "helpers/gunzip.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <limits>
#include <random>
#include <string>
#include <vector>

namespace mtw = microtel::wire;
namespace mtfk = microtel::testing;

namespace
{

/// Distinguishable stand-in for a missing optional, so `value_or` never makes
/// a decompression failure look like a legitimately empty result.
std::vector<std::byte> NoValue()
{
    return std::vector<std::byte>{std::byte{0xFF}};
}

std::vector<std::byte> Bytes(const std::string& s)
{
    std::vector<std::byte> v;
    v.reserve(s.size());
    for (const char c : s)
    {
        v.push_back(static_cast<std::byte>(c));
    }
    return v;
}

}  // namespace

TEST(GzipTest, RoundTripsTypicalPayload)
{
    const auto input = Bytes("resource spans for service.name=checkout");

    const auto compressed = mtw::GzipCompress(input);
    ASSERT_TRUE(compressed.has_value());

    const auto restored = mtfk::GunzipToString(*compressed);
    ASSERT_TRUE(restored.has_value());
    EXPECT_EQ(restored.value_or(""), "resource spans for service.name=checkout");
}

TEST(GzipTest, RoundTripsEmptyInput)
{
    const auto compressed = mtw::GzipCompress({});
    ASSERT_TRUE(compressed.has_value());
    // An empty gzip stream is still a stream: header plus trailer, never zero.
    EXPECT_GT(compressed->size(), 0U);

    const auto restored = mtfk::Gunzip(*compressed);
    ASSERT_TRUE(restored.has_value());
    EXPECT_EQ(restored.value_or(NoValue()), std::vector<std::byte>{});
}

TEST(GzipTest, EmitsGzipMagicBytes)
{
    // 0x1f 0x8b distinguishes the gzip wrapper from a raw zlib stream. Getting
    // this wrong yields bytes a server rejects as `content-encoding: gzip`.
    const auto compressed = mtw::GzipCompress(Bytes("payload"));
    ASSERT_TRUE(compressed.has_value());
    ASSERT_GE(compressed->size(), 2U);
    EXPECT_EQ((*compressed)[0], std::byte{0x1F});
    EXPECT_EQ((*compressed)[1], std::byte{0x8B});
}

TEST(GzipTest, ActuallyShrinksCompressibleInput)
{
    const auto input = Bytes(std::string(4096, 'a'));

    const auto compressed = mtw::GzipCompress(input);
    ASSERT_TRUE(compressed.has_value());
    EXPECT_LT(compressed->size(), input.size());

    const auto restored = mtfk::Gunzip(*compressed);
    ASSERT_TRUE(restored.has_value());
    EXPECT_EQ(restored.value_or(NoValue()).size(), input.size());
}

TEST(GzipTest, RoundTripsBinaryDataIncludingNulls)
{
    // Protobuf payloads are not text; a length-honest implementation must not
    // stop at an embedded NUL.
    std::vector<std::byte> input;
    input.reserve(512);
    for (int i = 0; i < 512; ++i)
    {
        input.push_back(static_cast<std::byte>(i % 256));
    }

    const auto compressed = mtw::GzipCompress(input);
    ASSERT_TRUE(compressed.has_value());

    const auto restored = mtfk::Gunzip(*compressed);
    ASSERT_TRUE(restored.has_value());
    EXPECT_EQ(restored.value_or(NoValue()), input);
}

TEST(GzipTest, HandlesIncompressibleInputThatExpands)
{
    // gzip has a fixed header and trailer, so a short high-entropy payload
    // comes out LARGER than it went in. The output buffer is sized from
    // deflateBound rather than the input length precisely so this is not an
    // overrun; a bound taken from the input would corrupt memory here.
    // A fixed seed is deliberate: this asserts an exact size relationship, so
    // a run-to-run varying input would make the test flaky rather than strong.
    // NOLINTNEXTLINE(cert-msc32-c,cert-msc51-cpp)
    std::mt19937 rng{42};
    for (const std::size_t n : {std::size_t{1}, std::size_t{8}, std::size_t{40}})
    {
        std::vector<std::byte> input;
        input.reserve(n);
        for (std::size_t i = 0; i < n; ++i)
        {
            input.push_back(static_cast<std::byte>(rng() & 0xFFU));
        }

        const auto compressed = mtw::GzipCompress(input);
        ASSERT_TRUE(compressed.has_value()) << "n=" << n;
        EXPECT_GT(compressed->size(), n) << "expected expansion at n=" << n;

        const auto restored = mtfk::Gunzip(*compressed);
        ASSERT_TRUE(restored.has_value()) << "n=" << n;
        EXPECT_EQ(restored.value_or(NoValue()), input) << "n=" << n;
    }
}

TEST(GzipTest, RejectsInputLargerThanOneDeflatePass)
{
    // A single deflate pass takes its lengths as uInt. GzipCompress checks the
    // size before touching the bytes, so a span that merely *claims* to be
    // oversized exercises the guard without any memory being read.
    const std::byte probe{0x00};
    const std::span<const std::byte> oversized{
        &probe, static_cast<std::size_t>(std::numeric_limits<uInt>::max()) + 1U};

    const auto result = mtw::GzipCompress(oversized);
    EXPECT_FALSE(result.has_value());
}
