// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for the gRPC status table, grpc-message percent-decoding, and the
// operator-visible message format (docs/grpc-wire-protocol.md §4.4,
// docs/error-model.md §7.2).
//
// The table is the single source of retryability for GrpcWireCodec, so these
// tests check it row by row against the matrix rather than spot-checking: a
// wrong bool here silently changes retry behaviour for one status and nothing
// else in the suite would notice.

#include "wire/grpc/grpc_status.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <string_view>

namespace mtw = microtel::wire;

// ---------------------------------------------------------------------------
// Status table — names and retryability (error-model.md §7.2)
// ---------------------------------------------------------------------------

TEST(GrpcStatusTest, EveryCanonicalCodeHasAnEntry)
{
    for (int code = 0; code < mtw::kGrpcStatusCount; ++code)
    {
        const auto info = mtw::LookupGrpcStatus(code);
        ASSERT_TRUE(info.has_value()) << "no entry for code " << code;
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access) — guarded by ASSERT_TRUE above
        EXPECT_FALSE(info->name.empty()) << "empty name for code " << code;
    }
}

TEST(GrpcStatusTest, NamesMatchTheMatrix)
{
    struct Row
    {
        int code;
        std::string_view name;
    };
    // Canonical gRPC names. Every code error-model.md §7.2 lists appears here
    // spelled as that table spells it; UNKNOWN (2) and ALREADY_EXISTS (6) are
    // the two the matrix omits and take their names from the gRPC spec.
    constexpr Row kRows[] = {
        {.code = 0, .name = "OK"},
        {.code = 1, .name = "CANCELLED"},
        {.code = 2, .name = "UNKNOWN"},
        {.code = 3, .name = "INVALID_ARGUMENT"},
        {.code = 4, .name = "DEADLINE_EXCEEDED"},
        {.code = 5, .name = "NOT_FOUND"},
        {.code = 6, .name = "ALREADY_EXISTS"},
        {.code = 7, .name = "PERMISSION_DENIED"},
        {.code = 8, .name = "RESOURCE_EXHAUSTED"},
        {.code = 9, .name = "FAILED_PRECONDITION"},
        {.code = 10, .name = "ABORTED"},
        {.code = 11, .name = "OUT_OF_RANGE"},
        {.code = 12, .name = "UNIMPLEMENTED"},
        {.code = 13, .name = "INTERNAL"},
        {.code = 14, .name = "UNAVAILABLE"},
        {.code = 15, .name = "DATA_LOSS"},
        {.code = 16, .name = "UNAUTHENTICATED"},
    };
    for (const auto& row : kRows)
    {
        const auto info = mtw::LookupGrpcStatus(row.code);
        ASSERT_TRUE(info.has_value()) << "no entry for code " << row.code;
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access) — guarded by ASSERT_TRUE above
        EXPECT_EQ(info->name, row.name) << "code " << row.code;
    }
}

TEST(GrpcStatusTest, RetryableCodesMatchTheMatrix)
{
    // error-model.md §7.2: CANCELLED, DEADLINE_EXCEEDED, ABORTED, OUT_OF_RANGE,
    // UNAVAILABLE and DATA_LOSS get jittered backoff. Everything else is
    // terminal.
    constexpr int kRetryable[] = {1, 4, 10, 11, 14, 15};
    for (int code = 0; code < mtw::kGrpcStatusCount; ++code)
    {
        const auto info = mtw::LookupGrpcStatus(code);
        ASSERT_TRUE(info.has_value());
        const bool expected = std::ranges::find(kRetryable, code) != std::ranges::end(kRetryable);
        // NOLINTNEXTLINE(bugprone-unchecked-optional-access) — guarded by ASSERT_TRUE above
        EXPECT_EQ(info->retryable, expected) << "code " << code << " (" << info->name << ")";
    }
}

TEST(GrpcStatusTest, ResourceExhaustedIsNotRetryableInTheTable)
{
    // Called out on its own because it is the one row where the table is not
    // the whole answer: RESOURCE_EXHAUSTED is retryable only when RetryInfo is
    // present, which the codec decides before consulting the table. A future
    // reader flipping this to true would silently retry every overload without
    // the server's delay.
    const auto info = mtw::LookupGrpcStatus(8);
    ASSERT_TRUE(info.has_value());
    // NOLINTBEGIN(bugprone-unchecked-optional-access) — guarded by ASSERT_TRUE above
    EXPECT_EQ(info->name, "RESOURCE_EXHAUSTED");
    EXPECT_FALSE(info->retryable);
    // NOLINTEND(bugprone-unchecked-optional-access)
}

TEST(GrpcStatusTest, CodesOutsideTheCanonicalRangeHaveNoEntry)
{
    EXPECT_FALSE(mtw::LookupGrpcStatus(-1).has_value());
    EXPECT_FALSE(mtw::LookupGrpcStatus(17).has_value());
    EXPECT_FALSE(mtw::LookupGrpcStatus(9999).has_value());
}

// ---------------------------------------------------------------------------
// PercentDecode (grpc-wire-protocol.md §4.4)
// ---------------------------------------------------------------------------

TEST(PercentDecodeTest, PassesThroughTextWithNoEscapes)
{
    EXPECT_EQ(mtw::PercentDecode("provided authorization does not match"),
              "provided authorization does not match");
}

TEST(PercentDecodeTest, DecodesUppercaseHex)
{
    EXPECT_EQ(mtw::PercentDecode("a%2Fb"), "a/b");
    EXPECT_EQ(mtw::PercentDecode("%41%42%43"), "ABC");
}

TEST(PercentDecodeTest, DecodesLowercaseHex)
{
    // The gRPC spec allows either case on the wire; a decoder that handled only
    // one would mangle half the servers out there.
    EXPECT_EQ(mtw::PercentDecode("a%2fb"), "a/b");
    EXPECT_EQ(mtw::PercentDecode("%e2%82%ac"), "\xe2\x82\xac");  // U+20AC as UTF-8
}

TEST(PercentDecodeTest, DecodesMixedCaseHex)
{
    EXPECT_EQ(mtw::PercentDecode("%2F%2f%aB%Ab"), "//\xab\xab");
}

TEST(PercentDecodeTest, InvalidEscapePassesThroughVerbatim)
{
    // Not an error: grpc-message is human-readable text that may legitimately
    // contain a bare '%'. Dropping it, or failing the whole decode, would lose
    // the operator's message over a formatting detail.
    EXPECT_EQ(mtw::PercentDecode("100% done"), "100% done");
    EXPECT_EQ(mtw::PercentDecode("%zz"), "%zz");
    EXPECT_EQ(mtw::PercentDecode("%2g"), "%2g");
    EXPECT_EQ(mtw::PercentDecode("%g2"), "%g2");
}

TEST(PercentDecodeTest, TruncatedEscapeAtEndPassesThroughVerbatim)
{
    EXPECT_EQ(mtw::PercentDecode("%"), "%");
    EXPECT_EQ(mtw::PercentDecode("%4"), "%4");
    EXPECT_EQ(mtw::PercentDecode("done %"), "done %");
    EXPECT_EQ(mtw::PercentDecode("done %A"), "done %A");
}

TEST(PercentDecodeTest, EmptyInputProducesEmptyOutput)
{
    EXPECT_EQ(mtw::PercentDecode(""), "");
}

TEST(PercentDecodeTest, DecodesEscapedPercentSign)
{
    EXPECT_EQ(mtw::PercentDecode("50%25"), "50%");
    // The decoder is single-pass: a decoded '%' is not re-scanned, so this
    // stays "%41" rather than becoming "A".
    EXPECT_EQ(mtw::PercentDecode("%2541"), "%41");
}

TEST(PercentDecodeTest, DecodesEmbeddedNulWithoutTruncating)
{
    // grpc-message is bytes, not a C string. A length-honest decoder keeps
    // going past a NUL.
    const std::string decoded = mtw::PercentDecode("a%00b");
    ASSERT_EQ(decoded.size(), 3U);
    EXPECT_EQ(decoded[0], 'a');
    EXPECT_EQ(decoded[1], '\0');
    EXPECT_EQ(decoded[2], 'b');
}

// ---------------------------------------------------------------------------
// FormatGrpcError — what an operator actually reads
// ---------------------------------------------------------------------------

TEST(FormatGrpcErrorTest, NamesTheStatusAndIncludesTheMessage)
{
    EXPECT_EQ(mtw::FormatGrpcError(16, "provided authorization does not match expected scheme"),
              "UNAUTHENTICATED (16): provided authorization does not match expected scheme");
}

TEST(FormatGrpcErrorTest, OmitsTheSeparatorWhenThereIsNoMessage)
{
    // A dangling ": " reads like a truncated message rather than an absent one.
    EXPECT_EQ(mtw::FormatGrpcError(16, ""), "UNAUTHENTICATED (16)");
    EXPECT_EQ(mtw::FormatGrpcError(5, ""), "NOT_FOUND (5)");
}

TEST(FormatGrpcErrorTest, UnknownCodeStillCarriesTheNumber)
{
    // A server sending a code outside the canonical range is doing something we
    // cannot classify — but the number is the only lead an operator has, so it
    // has to survive.
    EXPECT_EQ(mtw::FormatGrpcError(42, ""), "UNRECOGNIZED (42)");
    EXPECT_EQ(mtw::FormatGrpcError(42, "weird"), "UNRECOGNIZED (42): weird");
    EXPECT_EQ(mtw::FormatGrpcError(-1, ""), "UNRECOGNIZED (-1)");
}

TEST(FormatGrpcErrorTest, TruncatesAnOverlongMessage)
{
    // The trailer is bounded by max_trailer_bytes, which is 64 KiB — far more
    // than the 256 chars the health snapshot keeps. Truncating here means the
    // codec never builds the big string in the first place.
    const std::string huge(4096, 'x');
    const std::string formatted = mtw::FormatGrpcError(13, huge);
    EXPECT_LT(formatted.size(), huge.size());
    EXPECT_LE(formatted.size(),
              std::string_view{"INTERNAL (13): "}.size() + mtw::kMaxGrpcMessageChars);
    EXPECT_EQ(formatted.rfind("INTERNAL (13): ", 0), 0U);
}

TEST(FormatGrpcErrorTest, TakesTheMessageAlreadyDecoded)
{
    // Decoding happens once in the codec, which needs the decoded text for the
    // response excerpt too. Formatting must not decode a second time, or a
    // message that legitimately contains "%41" would come out as "A".
    EXPECT_EQ(mtw::FormatGrpcError(13, "literal %41 here"), "INTERNAL (13): literal %41 here");
}
