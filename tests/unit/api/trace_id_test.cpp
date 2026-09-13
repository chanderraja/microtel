// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Tests for microtel::TraceId::ToHex() and microtel::SpanId::ToHex().
//
// Contract under test (issue #168 — both were declared in the public header
// and defined in no shipped translation unit, so every out-of-library caller
// got an undefined-symbol link error):
//  - Lower-case hex, no separators: the W3C `traceparent` / OTLP protojson id
//    encoding.
//  - Fixed width — 32 chars for a TraceId, 16 for a SpanId — including the
//    all-zero (invalid) id, which must still render its full width.
//  - Byte order preserved: byte i renders at chars 2i and 2i+1, so decoding
//    the string round-trips back to AsBytes().

#include "microtel/trace.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <string>
#include <vector>

namespace mt = microtel;

namespace
{

constexpr std::size_t kHexCharsPerByte = 2U;
constexpr std::size_t kTraceIdHexChars = mt::TraceId::kSizeBytes * kHexCharsPerByte;
constexpr std::size_t kSpanIdHexChars = mt::SpanId::kSizeBytes * kHexCharsPerByte;
constexpr unsigned int kByteValues = 256U;

/// @brief Decodes a lower-case hex string back into bytes.
///
/// Deliberately independent of the implementation under test: a lookup in the
/// digit set rather than a shift/mask mirror of the encoder. A digit outside
/// `[0-9a-f]` fails the test, so this doubles as the lower-case assertion.
std::vector<std::uint8_t> FromHex(const std::string& hex)
{
    constexpr unsigned int kNibbleShift = 4U;
    const std::string digits = "0123456789abcdef";

    std::vector<std::uint8_t> out;
    out.reserve(hex.size() / kHexCharsPerByte);
    for (std::size_t i = 0; (i + 1) < hex.size(); i += kHexCharsPerByte)
    {
        const std::size_t high = digits.find(hex[i]);
        const std::size_t low = digits.find(hex[i + 1]);
        EXPECT_NE(high, std::string::npos) << "not a lower-case hex digit: '" << hex[i] << "'";
        EXPECT_NE(low, std::string::npos) << "not a lower-case hex digit: '" << hex[i + 1] << "'";
        out.push_back(static_cast<std::uint8_t>((high << kNibbleShift) | low));
    }
    return out;
}

/// @brief Asserts every character of @p hex is a lower-case hex digit.
void ExpectLowerCaseHex(const std::string& hex)
{
    for (const char c : hex)
    {
        const bool is_lower_hex = ((c >= '0') && (c <= '9')) || ((c >= 'a') && (c <= 'f'));
        EXPECT_TRUE(is_lower_hex) << "unexpected character '" << c << "' in " << hex;
    }
}

TEST(TraceIdToHexTest, EncodesAscendingBytePattern)
{
    mt::TraceId::Bytes bytes{};
    std::iota(bytes.begin(), bytes.end(), static_cast<std::uint8_t>(0x00));
    EXPECT_EQ(mt::TraceId(bytes).ToHex(), "000102030405060708090a0b0c0d0e0f");
}

TEST(TraceIdToHexTest, EncodesTheW3CExampleTraceId)
{
    // The trace id from the W3C Trace Context `traceparent` example header.
    const mt::TraceId::Bytes bytes = {0x4B,
                                      0xF9,
                                      0x2F,
                                      0x35,
                                      0x77,
                                      0xB3,
                                      0x4D,
                                      0xA6,
                                      0xA3,
                                      0xCE,
                                      0x92,
                                      0x9D,
                                      0x0E,
                                      0x0E,
                                      0x47,
                                      0x36};
    EXPECT_EQ(mt::TraceId(bytes).ToHex(), "4bf92f3577b34da6a3ce929d0e0e4736");
}

TEST(TraceIdToHexTest, AllZerosRendersAFullWidthRunOfZeroChars)
{
    const mt::TraceId id;
    ASSERT_FALSE(id.IsValid());
    EXPECT_EQ(id.ToHex(), std::string(kTraceIdHexChars, '0'));
}

TEST(TraceIdToHexTest, AllOnesRendersLowerCaseF)
{
    mt::TraceId::Bytes bytes{};
    bytes.fill(0xFF);
    EXPECT_EQ(mt::TraceId(bytes).ToHex(), std::string(kTraceIdHexChars, 'f'));
}

TEST(TraceIdToHexTest, LengthIsAlwaysTwoCharsPerByte)
{
    mt::TraceId::Bytes bytes{};
    bytes[0] = 0x01;
    EXPECT_EQ(mt::TraceId(bytes).ToHex().size(), kTraceIdHexChars);
    EXPECT_EQ(mt::TraceId().ToHex().size(), kTraceIdHexChars);
}

TEST(TraceIdToHexTest, UsesOnlyLowerCaseHexDigitsForEveryByteValue)
{
    for (unsigned int value = 0U; value < kByteValues; ++value)
    {
        mt::TraceId::Bytes bytes{};
        bytes.fill(static_cast<std::uint8_t>(value));
        ExpectLowerCaseHex(mt::TraceId(bytes).ToHex());
    }
}

TEST(TraceIdToHexTest, RoundTripsBackToTheSourceBytes)
{
    // 0x00, 0x11, ... 0xff — every hex digit in both nibble positions.
    constexpr std::uint8_t kStride = 0x11;
    mt::TraceId::Bytes bytes{};
    std::uint8_t value = 0x00;
    for (std::uint8_t& byte : bytes)
    {
        byte = value;
        value = static_cast<std::uint8_t>(value + kStride);
    }

    const mt::TraceId id(bytes);
    const std::vector<std::uint8_t> decoded = FromHex(id.ToHex());
    ASSERT_EQ(decoded.size(), mt::TraceId::kSizeBytes);
    EXPECT_TRUE(std::equal(decoded.begin(), decoded.end(), id.AsBytes().begin()));
}

TEST(SpanIdToHexTest, EncodesAscendingBytePattern)
{
    const mt::SpanId::Bytes bytes = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07};
    EXPECT_EQ(mt::SpanId(bytes).ToHex(), "0001020304050607");
}

TEST(SpanIdToHexTest, EncodesTheW3CExampleSpanId)
{
    // The parent id from the W3C Trace Context `traceparent` example header.
    const mt::SpanId::Bytes bytes = {0x00, 0xF0, 0x67, 0xAA, 0x0B, 0xA9, 0x02, 0xB7};
    EXPECT_EQ(mt::SpanId(bytes).ToHex(), "00f067aa0ba902b7");
}

TEST(SpanIdToHexTest, AllZerosRendersAFullWidthRunOfZeroChars)
{
    const mt::SpanId id;
    ASSERT_FALSE(id.IsValid());
    EXPECT_EQ(id.ToHex(), std::string(kSpanIdHexChars, '0'));
}

TEST(SpanIdToHexTest, AllOnesRendersLowerCaseF)
{
    mt::SpanId::Bytes bytes{};
    bytes.fill(0xFF);
    EXPECT_EQ(mt::SpanId(bytes).ToHex(), std::string(kSpanIdHexChars, 'f'));
}

TEST(SpanIdToHexTest, LengthIsAlwaysTwoCharsPerByte)
{
    mt::SpanId::Bytes bytes{};
    bytes[0] = 0x01;
    EXPECT_EQ(mt::SpanId(bytes).ToHex().size(), kSpanIdHexChars);
    EXPECT_EQ(mt::SpanId().ToHex().size(), kSpanIdHexChars);
}

TEST(SpanIdToHexTest, UsesOnlyLowerCaseHexDigitsForEveryByteValue)
{
    for (unsigned int value = 0U; value < kByteValues; ++value)
    {
        mt::SpanId::Bytes bytes{};
        bytes.fill(static_cast<std::uint8_t>(value));
        ExpectLowerCaseHex(mt::SpanId(bytes).ToHex());
    }
}

TEST(SpanIdToHexTest, RoundTripsBackToTheSourceBytes)
{
    const mt::SpanId::Bytes bytes = {0xDE, 0xAD, 0xBE, 0xEF, 0x0F, 0xF0, 0x5A, 0xA5};
    const mt::SpanId id(bytes);
    const std::vector<std::uint8_t> decoded = FromHex(id.ToHex());
    ASSERT_EQ(decoded.size(), mt::SpanId::kSizeBytes);
    EXPECT_TRUE(std::equal(decoded.begin(), decoded.end(), id.AsBytes().begin()));
}

}  // namespace
