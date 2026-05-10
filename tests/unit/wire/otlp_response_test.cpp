// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for ParseRejectedSpans (M5-B).

#include "wire/otlp_response.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace mtw = microtel::wire;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Convenience: build a std::span<const std::byte> from a byte initializer list.
static std::span<const std::byte> AsSpan(std::span<const std::uint8_t> v)
{
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    return {reinterpret_cast<const std::byte*>(v.data()), v.size()};
}

// ---------------------------------------------------------------------------
// Encoded fixtures
//
// ExportTraceServiceResponse { partial_success { rejected_spans: N } }
//
// Outer:  field 1 (partial_success), wire type 2 (LEN).
//         tag = (1<<3)|2 = 0x0A, then length prefix.
// Inner:  field 1 (rejected_spans), wire type 0 (VARINT).
//         tag = (1<<3)|0 = 0x08, then varint value.
// ---------------------------------------------------------------------------

// { partial_success { rejected_spans: 42 } }
// Inner: [0x08, 0x2A] (2 bytes)
// Outer: [0x0A, 0x02, 0x08, 0x2A]
static constexpr std::array<std::uint8_t, 4> kRejected42{0x0A, 0x02, 0x08, 0x2A};

// { partial_success { rejected_spans: 1000 } }
// varint(1000) = 0xE8, 0x07
// Inner: [0x08, 0xE8, 0x07] (3 bytes)
// Outer: [0x0A, 0x03, 0x08, 0xE8, 0x07]
static constexpr std::array<std::uint8_t, 5> kRejected1000{0x0A, 0x03, 0x08, 0xE8, 0x07};

// Unknown field 2 (varint) with value 5 — partial_success absent.
// tag = (2<<3)|0 = 0x10, varint(5) = 0x05.
static constexpr std::array<std::uint8_t, 2> kUnknownField{0x10, 0x05};

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST(ParseRejectedSpansTest, EmptyBody_ReturnsZero)
{
    EXPECT_EQ(mtw::ParseRejectedSpans({}), 0U);
}

TEST(ParseRejectedSpansTest, NoPartialSuccessField_ReturnsZero)
{
    EXPECT_EQ(mtw::ParseRejectedSpans(AsSpan(kUnknownField)), 0U);
}

TEST(ParseRejectedSpansTest, RejectedSpans42_Returns42)
{
    EXPECT_EQ(mtw::ParseRejectedSpans(AsSpan(kRejected42)), 42U);
}

TEST(ParseRejectedSpansTest, RejectedSpans1000_Returns1000)
{
    EXPECT_EQ(mtw::ParseRejectedSpans(AsSpan(kRejected1000)), 1000U);
}

TEST(ParseRejectedSpansTest, TruncatedAfterTag_ReturnsZero)
{
    // Only the outer tag byte — length and inner data absent.
    const std::vector<std::uint8_t> truncated{0x0A};
    EXPECT_EQ(mtw::ParseRejectedSpans(AsSpan(truncated)), 0U);
}

TEST(ParseRejectedSpansTest, EmptyPartialSuccessMessage_ReturnsZero)
{
    // partial_success field present but inner message is 0 bytes.
    // Outer: [0x0A, 0x00]
    const std::vector<std::uint8_t> empty_inner{0x0A, 0x00};
    EXPECT_EQ(mtw::ParseRejectedSpans(AsSpan(empty_inner)), 0U);
}
