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
#include <vector>

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

// Issue #223: the parser reports which of three things it found, because
// "absent" and "unparseable" both meant 0 before and a malformed
// partial-success body was accounted as a clean success. The layout is the
// same for all three signals — ExportTracePartialSuccess.rejected_spans,
// ExportMetricsPartialSuccess.rejected_data_points and
// ExportLogsPartialSuccess.rejected_log_records are all field 1, int64, and
// error_message is field 2 in each — so one parser serves every codec.

using Outcome = mtw::PartialSuccessOutcome;

static void ExpectParsed(std::span<const std::uint8_t> body, std::uint32_t rejected)
{
    const auto r = mtw::ParseRejectedSpans(AsSpan(body));
    EXPECT_EQ(r.outcome, Outcome::Parsed);
    EXPECT_EQ(r.rejected, rejected);
}

static void ExpectAbsent(std::span<const std::uint8_t> body)
{
    const auto r = mtw::ParseRejectedSpans(AsSpan(body));
    EXPECT_EQ(r.outcome, Outcome::Absent);
    EXPECT_EQ(r.rejected, 0U);
}

static void ExpectUnparseable(std::span<const std::uint8_t> body)
{
    const auto r = mtw::ParseRejectedSpans(AsSpan(body));
    EXPECT_EQ(r.outcome, Outcome::Unparseable);
    EXPECT_EQ(r.rejected, 0U);
}

// --- Absent ----------------------------------------------------------------

TEST(ParseRejectedSpansTest, EmptyBody_IsAbsent)
{
    ExpectAbsent({});
}

TEST(ParseRejectedSpansTest, NoPartialSuccessField_IsAbsent)
{
    ExpectAbsent(kUnknownField);
}

TEST(ParseRejectedSpansTest, UnknownFieldsOfEveryWireType_AreSkipped_IsAbsent)
{
    // field 2 varint, field 3 fixed64, field 4 LEN, field 5 fixed32.
    const std::vector<std::uint8_t> body{
        0x10, 0x01, 0x19, 0, 0, 0, 0, 0, 0, 0, 0, 0x22, 0x01, 0x41, 0x2D, 0, 0, 0, 0};
    ExpectAbsent(body);
}

// --- Parsed ----------------------------------------------------------------

TEST(ParseRejectedSpansTest, RejectedSpans42_Parsed42)
{
    ExpectParsed(kRejected42, 42U);
}

TEST(ParseRejectedSpansTest, RejectedSpans1000_Parsed1000)
{
    ExpectParsed(kRejected1000, 1000U);
}

TEST(ParseRejectedSpansTest, EmptyPartialSuccessMessage_ParsedZero)
{
    // partial_success present, inner message 0 bytes: a valid rejected = 0.
    const std::vector<std::uint8_t> empty_inner{0x0A, 0x00};
    ExpectParsed(empty_inner, 0U);
}

TEST(ParseRejectedSpansTest, ExplicitZeroRejected_ParsedZero)
{
    const std::vector<std::uint8_t> body{0x0A, 0x02, 0x08, 0x00};
    ExpectParsed(body, 0U);
}

TEST(ParseRejectedSpansTest, RejectedWithErrorMessage_ParsedN)
{
    // partial_success { rejected: 3, error_message: "no" }
    const std::vector<std::uint8_t> body{0x0A, 0x06, 0x08, 0x03, 0x12, 0x02, 'n', 'o'};
    ExpectParsed(body, 3U);
}

TEST(ParseRejectedSpansTest, ErrorMessageBeforeRejected_ParsedN)
{
    const std::vector<std::uint8_t> body{0x0A, 0x06, 0x12, 0x02, 'n', 'o', 0x08, 0x05};
    ExpectParsed(body, 5U);
}

TEST(ParseRejectedSpansTest, UnknownFieldBeforePartialSuccess_ParsedN)
{
    const std::vector<std::uint8_t> body{0x10, 0x05, 0x0A, 0x02, 0x08, 0x07};
    ExpectParsed(body, 7U);
}

TEST(ParseRejectedSpansTest, RepeatedPartialSuccess_LastValueWins)
{
    // Protobuf merges a repeated singular message field; the later scalar wins.
    const std::vector<std::uint8_t> body{0x0A, 0x02, 0x08, 0x01, 0x0A, 0x02, 0x08, 0x09};
    ExpectParsed(body, 9U);
}

TEST(ParseRejectedSpansTest, RejectedAboveUint32_IsCapped)
{
    // varint(2^32) = 80 80 80 80 10
    const std::vector<std::uint8_t> body{0x0A, 0x06, 0x08, 0x80, 0x80, 0x80, 0x80, 0x10};
    ExpectParsed(body, 0xFFFFFFFFU);
}

// --- Unparseable -----------------------------------------------------------

TEST(ParseRejectedSpansTest, TruncatedAfterTag_IsUnparseable)
{
    // Only the outer tag byte — length and inner data absent.
    const std::vector<std::uint8_t> truncated{0x0A};
    ExpectUnparseable(truncated);
}

TEST(ParseRejectedSpansTest, LengthBeyondBody_IsUnparseable)
{
    const std::vector<std::uint8_t> body{0x0A, 0x05, 0x08, 0x2A};
    ExpectUnparseable(body);
}

TEST(ParseRejectedSpansTest, TruncatedInnerVarint_IsUnparseable)
{
    // rejected varint has its continuation bit set and nothing follows.
    const std::vector<std::uint8_t> body{0x0A, 0x02, 0x08, 0x80};
    ExpectUnparseable(body);
}

TEST(ParseRejectedSpansTest, TruncatedInnerTag_IsUnparseable)
{
    const std::vector<std::uint8_t> body{0x0A, 0x01, 0x80};
    ExpectUnparseable(body);
}

TEST(ParseRejectedSpansTest, TrailingGarbageAfterValidCount_IsUnparseable)
{
    // A valid partial_success followed by a truncated tag. A protobuf parser
    // rejects the whole message, so the count before the noise is not trusted.
    const std::vector<std::uint8_t> body{0x0A, 0x02, 0x08, 0x2A, 0xFF};
    ExpectUnparseable(body);
}

TEST(ParseRejectedSpansTest, TrailingGarbageInsidePartialSuccess_IsUnparseable)
{
    const std::vector<std::uint8_t> body{0x0A, 0x03, 0x08, 0x2A, 0x0F};
    ExpectUnparseable(body);
}

TEST(ParseRejectedSpansTest, FieldNumberZero_IsUnparseable)
{
    const std::vector<std::uint8_t> body{0x00, 0x00};
    ExpectUnparseable(body);
}

TEST(ParseRejectedSpansTest, GroupWireType_IsUnparseable)
{
    // field 1, wire type 3 (start group) — deprecated and never OTLP.
    const std::vector<std::uint8_t> body{0x0B, 0x0C};
    ExpectUnparseable(body);
}

TEST(ParseRejectedSpansTest, OverlongVarint_IsUnparseable)
{
    const std::vector<std::uint8_t> body{
        0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x01};
    ExpectUnparseable(body);
}

TEST(ParseRejectedSpansTest, PlainText_IsUnparseable)
{
    // What a misconfigured proxy answers with: not a protobuf message.
    const std::vector<std::uint8_t> body{'<', 'h', 't', 'm', 'l', '>'};
    ExpectUnparseable(body);
}
