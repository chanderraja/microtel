// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for GzipCompress and GzipDecompress — the compression primitives
// shared by the HTTP and gRPC codecs (grpc-wire-protocol.md §5.1 and §5.2).
//
// The decompression tests use tests/helpers/gunzip.hpp as an independent
// oracle: it is a separate, deliberately naive implementation, so a shared bug
// between the two would have to be a zlib bug rather than ours.

#include "wire/gzip.hpp"

#include "helpers/gunzip.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <limits>
#include <random>
#include <span>
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

// ---------------------------------------------------------------------------
// GzipDecompress — bounded response decompression (grpc-wire-protocol.md §5.2)
// ---------------------------------------------------------------------------

namespace
{

/// Generous cap for the tests that are not about the cap itself.
constexpr std::size_t kAmpleCap = 1U << 20U;

}  // namespace

TEST(GzipDecompressTest, RoundTripsWhatGzipCompressProduced)
{
    const auto input = Bytes("resource spans for service.name=checkout");
    const auto compressed = mtw::GzipCompress(input);
    ASSERT_TRUE(compressed.has_value());

    const auto restored = mtw::GzipDecompress(*compressed, kAmpleCap);
    ASSERT_TRUE(restored.has_value());
    EXPECT_EQ(*restored, input);
}

TEST(GzipDecompressTest, AgreesWithTheIndependentOracle)
{
    // Same bytes through two implementations. The oracle allocates the ceiling
    // up front and inflates in one pass; GzipDecompress grows in chunks. They
    // must not disagree about a single byte.
    std::vector<std::byte> input;
    input.reserve(4096);
    for (int i = 0; i < 4096; ++i)
    {
        input.push_back(static_cast<std::byte>((i * 31) % 256));
    }
    const auto compressed = mtw::GzipCompress(input);
    ASSERT_TRUE(compressed.has_value());

    const auto ours = mtw::GzipDecompress(*compressed, kAmpleCap);
    const auto theirs = mtfk::Gunzip(*compressed);
    ASSERT_TRUE(ours.has_value());
    ASSERT_TRUE(theirs.has_value());
    EXPECT_EQ(*ours, theirs.value_or(NoValue()));
    EXPECT_EQ(*ours, input);
}

TEST(GzipDecompressTest, RoundTripsEmptyPayload)
{
    const auto compressed = mtw::GzipCompress({});
    ASSERT_TRUE(compressed.has_value());

    const auto restored = mtw::GzipDecompress(*compressed, kAmpleCap);
    ASSERT_TRUE(restored.has_value());
    EXPECT_TRUE(restored->empty());
}

TEST(GzipDecompressTest, GrowsPastTheInitialChunkForLargeOutput)
{
    // Well beyond any plausible first allocation, so a single-chunk
    // implementation would truncate or fail here.
    const auto input = Bytes(std::string(512U * 1024U, 'q'));
    const auto compressed = mtw::GzipCompress(input);
    ASSERT_TRUE(compressed.has_value());
    ASSERT_LT(compressed->size(), input.size() / 100U) << "expected a high ratio for this fixture";

    const auto restored = mtw::GzipDecompress(*compressed, kAmpleCap);
    ASSERT_TRUE(restored.has_value());
    EXPECT_EQ(restored->size(), input.size());
    EXPECT_EQ(*restored, input);
}

TEST(GzipDecompressTest, OutputExactlyAtTheCapIsAccepted)
{
    // The boundary belongs to the accepted side: `max_out` is a ceiling the
    // output may reach, not one it must stay under.
    const auto input = Bytes(std::string(1024, 'x'));
    const auto compressed = mtw::GzipCompress(input);
    ASSERT_TRUE(compressed.has_value());

    const auto restored = mtw::GzipDecompress(*compressed, 1024U);
    ASSERT_TRUE(restored.has_value());
    EXPECT_EQ(restored->size(), 1024U);
}

TEST(GzipDecompressTest, OutputOneByteOverTheCapIsTooLarge)
{
    const auto input = Bytes(std::string(1024, 'x'));
    const auto compressed = mtw::GzipCompress(input);
    ASSERT_TRUE(compressed.has_value());

    const auto restored = mtw::GzipDecompress(*compressed, 1023U);
    ASSERT_FALSE(restored.has_value());
    EXPECT_EQ(restored.error(), mtw::GzipDecompressError::TooLarge);
}

TEST(GzipDecompressTest, DecompressionBombIsRefusedWithoutMaterialisingIt)
{
    // 8 MiB of zeroes compresses to a few KiB. With a 4 KiB cap the
    // implementation must stop at the cap rather than allocate the bomb — the
    // sanitiser builds are the other half of this assertion.
    const auto bomb = Bytes(std::string(8U * 1024U * 1024U, '\0'));
    const auto compressed = mtw::GzipCompress(bomb);
    ASSERT_TRUE(compressed.has_value());
    ASSERT_LT(compressed->size(), 64U * 1024U);

    const auto restored = mtw::GzipDecompress(*compressed, 4096U);
    ASSERT_FALSE(restored.has_value());
    EXPECT_EQ(restored.error(), mtw::GzipDecompressError::TooLarge);
}

TEST(GzipDecompressTest, ZeroCapAcceptsOnlyEmptyOutput)
{
    const auto empty = mtw::GzipCompress({});
    ASSERT_TRUE(empty.has_value());
    const auto restored_empty = mtw::GzipDecompress(*empty, 0U);
    ASSERT_TRUE(restored_empty.has_value());
    EXPECT_TRUE(restored_empty->empty());

    const auto one = mtw::GzipCompress(Bytes("a"));
    ASSERT_TRUE(one.has_value());
    const auto restored_one = mtw::GzipDecompress(*one, 0U);
    ASSERT_FALSE(restored_one.has_value());
    EXPECT_EQ(restored_one.error(), mtw::GzipDecompressError::TooLarge);
}

TEST(GzipDecompressTest, NonGzipInputIsCorruptNotTooLarge)
{
    // The two failures drive different counters and different operator
    // messages, so collapsing them would be a real loss of information.
    const auto garbage = Bytes("this is not a gzip stream at all");

    const auto restored = mtw::GzipDecompress(garbage, kAmpleCap);
    ASSERT_FALSE(restored.has_value());
    EXPECT_EQ(restored.error(), mtw::GzipDecompressError::Corrupt);
}

TEST(GzipDecompressTest, EmptyInputIsCorrupt)
{
    // Zero bytes is not an empty gzip stream — an empty stream is still a
    // header plus a trailer.
    const auto restored = mtw::GzipDecompress({}, kAmpleCap);
    ASSERT_FALSE(restored.has_value());
    EXPECT_EQ(restored.error(), mtw::GzipDecompressError::Corrupt);
}

TEST(GzipDecompressTest, TruncatedStreamIsCorrupt)
{
    const auto compressed = mtw::GzipCompress(Bytes(std::string(2048, 'k')));
    ASSERT_TRUE(compressed.has_value());
    ASSERT_GT(compressed->size(), 8U);
    const std::span<const std::byte> truncated{compressed->data(), compressed->size() - 4U};

    const auto restored = mtw::GzipDecompress(truncated, kAmpleCap);
    ASSERT_FALSE(restored.has_value());
    EXPECT_EQ(restored.error(), mtw::GzipDecompressError::Corrupt);
}

TEST(GzipDecompressTest, CorruptedTrailerIsCorrupt)
{
    // zlib verifies the CRC32 in the gzip trailer. A payload that inflates but
    // fails the checksum is exactly the case an implementation that stopped
    // reading at the last data byte would let through.
    auto compressed = mtw::GzipCompress(Bytes(std::string(512, 'm')));
    ASSERT_TRUE(compressed.has_value());
    ASSERT_GT(compressed->size(), 8U);
    (*compressed)[compressed->size() - 5U] ^= std::byte{0xFF};

    const auto restored = mtw::GzipDecompress(*compressed, kAmpleCap);
    ASSERT_FALSE(restored.has_value());
    EXPECT_EQ(restored.error(), mtw::GzipDecompressError::Corrupt);
}

TEST(GzipDecompressTest, TrailingGarbageAfterTheStreamIsRejected)
{
    // Padding, or a second stream, after the first one's trailer. Accepting it
    // would let a server smuggle bytes past the size accounting.
    auto compressed = mtw::GzipCompress(Bytes("payload"));
    ASSERT_TRUE(compressed.has_value());
    compressed->push_back(std::byte{0x00});
    compressed->push_back(std::byte{0x01});

    const auto restored = mtw::GzipDecompress(*compressed, kAmpleCap);
    ASSERT_FALSE(restored.has_value());
    EXPECT_EQ(restored.error(), mtw::GzipDecompressError::Corrupt);
}

TEST(GzipDecompressTest, RejectsRawZlibStream)
{
    // windowBits 15+16 accepts only the RFC 1952 gzip wrapper. A bare zlib
    // stream (0x78 ...) is what an implementation that forgot the +16 would
    // produce, and it must not be mistaken for gzip.
    const auto zlib_stream = Bytes(std::string("\x78\x9c\x4b\x04\x00\x00\x62\x00\x62", 9));

    const auto restored = mtw::GzipDecompress(zlib_stream, kAmpleCap);
    ASSERT_FALSE(restored.has_value());
    EXPECT_EQ(restored.error(), mtw::GzipDecompressError::Corrupt);
}
