// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Tests for microtel::W3CTraceContextPropagator::Inject / Extract.
//
// Contract under test (issue #188 — both were declared in the public header
// and defined in no shipped translation unit, so every out-of-library caller
// got an undefined-symbol link error, the same defect class as #168):
//  - Inject writes a canonical version-00 `traceparent`; an invalid context
//    writes nothing at all (propagator.hpp:42).
//  - Extract returns an invalid SpanContext if extraction fails "for any
//    reason", and sets `remote == true` only on success (propagator.hpp:47-49).
//
// The reject vectors below are the W3C Trace Context Level 1 §3.2.2 parsing
// rules: fixed 55-char version-00 shape, lower-case hex only, non-zero
// trace-id and parent-id, version `ff` invalid, and forward tolerance of a
// higher version carrying extra dash-separated fields.

#include "microtel/propagator.hpp"

#include "microtel/trace.hpp"

#include <gtest/gtest.h>

#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace mt = microtel;

namespace
{

/// @brief The W3C Trace Context specification's own example `traceparent`.
constexpr std::string_view kCanonical = "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01";

constexpr std::string_view kTraceparent = "traceparent";
constexpr std::string_view kTracestate = "tracestate";

/// @brief `std::less<>` makes the map heterogeneously searchable by
/// `string_view`, which is what `HeaderGetter` hands us.
using Headers = std::map<std::string, std::string, std::less<>>;

[[nodiscard]] mt::HeaderGetter GetterFor(const Headers& headers)
{
    return [&headers](std::string_view name) -> std::optional<std::string_view>
    {
        const auto entry = headers.find(name);
        if (entry == headers.end())
        {
            return std::nullopt;
        }
        return std::string_view(entry->second);
    };
}

[[nodiscard]] mt::HeaderSetter SetterFor(Headers& headers)
{
    return [&headers](std::string_view name, std::string_view value)
    { headers.insert_or_assign(std::string(name), std::string(value)); };
}

/// @brief Extracts from a carrier holding only @p traceparent.
[[nodiscard]] mt::SpanContext Extract(std::string_view traceparent)
{
    const Headers headers{{std::string(kTraceparent), std::string(traceparent)}};
    return mt::W3CTraceContextPropagator().Extract(GetterFor(headers));
}

/// @brief Asserts @p traceparent is rejected: invalid context, not remote.
void ExpectRejected(std::string_view traceparent)
{
    const mt::SpanContext context = Extract(traceparent);
    EXPECT_FALSE(context.IsValid()) << "should not have parsed: " << traceparent;
    EXPECT_FALSE(context.remote) << "remote must be set only on success: " << traceparent;
}

// ── Extract: the happy path ───────────────────────────────────────────────────

TEST(W3CPropagatorExtractTest, ParsesTheSpecExampleTraceparent)
{
    const mt::SpanContext context = Extract(kCanonical);
    ASSERT_TRUE(context.IsValid());
    EXPECT_EQ(context.trace_id.ToHex(), "4bf92f3577b34da6a3ce929d0e0e4736");
    EXPECT_EQ(context.span_id.ToHex(), "00f067aa0ba902b7");
    EXPECT_TRUE(context.trace_flags.IsSampled());
}

TEST(W3CPropagatorExtractTest, SetsRemoteOnSuccess)
{
    EXPECT_TRUE(Extract(kCanonical).remote);
}

TEST(W3CPropagatorExtractTest, ParsesTheUnsampledFlag)
{
    const mt::SpanContext context =
        Extract("00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-00");
    ASSERT_TRUE(context.IsValid());
    EXPECT_FALSE(context.trace_flags.IsSampled());
    EXPECT_EQ(context.trace_flags.AsByte(), 0x00);
}

TEST(W3CPropagatorExtractTest, KeepsReservedFlagBitsVerbatim)
{
    // Version 00 defines only bit 0; the remaining bits are propagated as
    // received rather than cleared, so an upstream's bits survive us.
    const mt::SpanContext context =
        Extract("00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-ff");
    ASSERT_TRUE(context.IsValid());
    EXPECT_TRUE(context.trace_flags.IsSampled());
    EXPECT_EQ(context.trace_flags.AsByte(), 0xFF);
}

TEST(W3CPropagatorExtractTest, AcceptsAHigherVersionWithExtraFields)
{
    // W3C §3.2.2.1 forward compatibility: a future version may append further
    // dash-separated fields, which a version-00 parser ignores.
    const mt::SpanContext context =
        Extract("01-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01-what-the-future-brings");
    ASSERT_TRUE(context.IsValid());
    EXPECT_EQ(context.trace_id.ToHex(), "4bf92f3577b34da6a3ce929d0e0e4736");
    EXPECT_EQ(context.span_id.ToHex(), "00f067aa0ba902b7");
    EXPECT_TRUE(context.trace_flags.IsSampled());
}

TEST(W3CPropagatorExtractTest, AcceptsAHigherVersionOfExactlyFiftyFiveChars)
{
    const mt::SpanContext context =
        Extract("cd-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01");
    EXPECT_TRUE(context.IsValid());
}

// ── Extract: the reject vectors ───────────────────────────────────────────────

TEST(W3CPropagatorExtractTest, RejectsAHigherVersionWhoseExtraCharsAreNotAField)
{
    // Longer than 55 chars but char 55 is not the `-` that starts a field.
    ExpectRejected("01-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01xyz");
}

TEST(W3CPropagatorExtractTest, RejectsTrailingCharsOnVersionZero)
{
    // Version 00 is exactly 55 chars; anything longer is invalid even when the
    // extra characters look like a well-formed future field.
    ExpectRejected("00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01-extra");
}

TEST(W3CPropagatorExtractTest, RejectsVersionFf)
{
    ExpectRejected("ff-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01");
}

TEST(W3CPropagatorExtractTest, RejectsNonHexVersion)
{
    ExpectRejected("zz-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01");
}

TEST(W3CPropagatorExtractTest, RejectsTooShort)
{
    ExpectRejected("00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-0");
}

TEST(W3CPropagatorExtractTest, RejectsTruncatedHeader)
{
    ExpectRejected("00-4bf92f3577b34da6a3ce929d0e0e4736");
}

TEST(W3CPropagatorExtractTest, RejectsEmptyHeader)
{
    ExpectRejected("");
}

TEST(W3CPropagatorExtractTest, RejectsMisplacedSeparators)
{
    // Right length, right charset, `-` moved out of its fixed offset.
    ExpectRejected("000-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-1");
}

TEST(W3CPropagatorExtractTest, RejectsNonHexTraceId)
{
    ExpectRejected("00-4bf92f3577b34da6a3ce929d0e0e473g-00f067aa0ba902b7-01");
}

TEST(W3CPropagatorExtractTest, RejectsNonHexSpanId)
{
    ExpectRejected("00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b_-01");
}

TEST(W3CPropagatorExtractTest, RejectsNonHexFlags)
{
    ExpectRejected("00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-x1");
}

TEST(W3CPropagatorExtractTest, RejectsAllZeroTraceId)
{
    ExpectRejected("00-00000000000000000000000000000000-00f067aa0ba902b7-01");
}

TEST(W3CPropagatorExtractTest, RejectsAllZeroSpanId)
{
    ExpectRejected("00-4bf92f3577b34da6a3ce929d0e0e4736-0000000000000000-01");
}

// Upper-case hex: W3C §3.2.2.3 requires the hex fields to be lower-case, and
// the specification's own validation corpus rejects an upper-case id rather
// than folding its case. microtel parses lower-case only, in every field.

TEST(W3CPropagatorExtractTest, RejectsUpperCaseTraceId)
{
    ExpectRejected("00-4BF92F3577B34DA6A3CE929D0E0E4736-00f067aa0ba902b7-01");
}

TEST(W3CPropagatorExtractTest, RejectsUpperCaseSpanId)
{
    ExpectRejected("00-4bf92f3577b34da6a3ce929d0e0e4736-00F067AA0BA902B7-01");
}

TEST(W3CPropagatorExtractTest, RejectsUpperCaseVersion)
{
    ExpectRejected("0A-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01");
}

TEST(W3CPropagatorExtractTest, RejectsUpperCaseFlags)
{
    ExpectRejected("00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-0A");
}

// ── Extract: carrier-level failures ───────────────────────────────────────────

TEST(W3CPropagatorExtractTest, ReturnsInvalidWhenTraceparentIsAbsent)
{
    const Headers headers{{"some-other-header", "value"}};
    const mt::SpanContext context = mt::W3CTraceContextPropagator().Extract(GetterFor(headers));
    EXPECT_FALSE(context.IsValid());
    EXPECT_FALSE(context.remote);
}

TEST(W3CPropagatorExtractTest, ReturnsInvalidForAnEmptyGetter)
{
    // "fails for any reason" includes a carrier that supplied no callable.
    const mt::SpanContext context = mt::W3CTraceContextPropagator().Extract(mt::HeaderGetter{});
    EXPECT_FALSE(context.IsValid());
    EXPECT_FALSE(context.remote);
}

TEST(W3CPropagatorExtractTest, IgnoresTracestateWhenTraceparentIsInvalid)
{
    // W3C: a malformed traceparent discards tracestate along with it.
    const Headers headers{
        {std::string(kTraceparent), "ff-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01"},
        {std::string(kTracestate), "vendor=value"}};
    const mt::SpanContext context = mt::W3CTraceContextPropagator().Extract(GetterFor(headers));
    EXPECT_FALSE(context.IsValid());
    EXPECT_TRUE(context.trace_state.Empty());
}

TEST(W3CPropagatorExtractTest, ParsesTraceparentAlongsideATracestate)
{
    const Headers headers{{std::string(kTraceparent), std::string(kCanonical)},
                          {std::string(kTracestate), "vendor=value"}};
    const mt::SpanContext context = mt::W3CTraceContextPropagator().Extract(GetterFor(headers));
    ASSERT_TRUE(context.IsValid());
    EXPECT_TRUE(context.remote);
}

// ── Inject ────────────────────────────────────────────────────────────────────

TEST(W3CPropagatorInjectTest, WritesTheCanonicalTraceparent)
{
    Headers headers;
    mt::W3CTraceContextPropagator().Inject(Extract(kCanonical), SetterFor(headers));
    ASSERT_EQ(headers.count(std::string(kTraceparent)), 1U);
    EXPECT_EQ(headers.at(std::string(kTraceparent)), kCanonical);
}

TEST(W3CPropagatorInjectTest, AlwaysWritesVersionZeroEvenForAHigherParsedVersion)
{
    const mt::SpanContext context =
        Extract("01-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01-future");
    ASSERT_TRUE(context.IsValid());

    Headers headers;
    mt::W3CTraceContextPropagator().Inject(context, SetterFor(headers));
    EXPECT_EQ(headers.at(std::string(kTraceparent)), kCanonical);
}

TEST(W3CPropagatorInjectTest, RoundTripsThroughExtract)
{
    Headers headers;
    mt::W3CTraceContextPropagator().Inject(Extract(kCanonical), SetterFor(headers));

    const mt::SpanContext round_tripped =
        mt::W3CTraceContextPropagator().Extract(GetterFor(headers));
    ASSERT_TRUE(round_tripped.IsValid());
    EXPECT_EQ(round_tripped.trace_id.ToHex(), "4bf92f3577b34da6a3ce929d0e0e4736");
    EXPECT_EQ(round_tripped.span_id.ToHex(), "00f067aa0ba902b7");
    EXPECT_EQ(round_tripped.trace_flags.AsByte(), 0x01);
}

TEST(W3CPropagatorInjectTest, RendersTheUnsampledFlagAsZeroZero)
{
    const mt::SpanContext context =
        Extract("00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-00");
    Headers headers;
    mt::W3CTraceContextPropagator().Inject(context, SetterFor(headers));
    EXPECT_EQ(headers.at(std::string(kTraceparent)),
              "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-00");
}

TEST(W3CPropagatorInjectTest, RendersEveryFlagBitAsLowerCaseHex)
{
    const mt::SpanContext context =
        Extract("00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-ff");
    Headers headers;
    mt::W3CTraceContextPropagator().Inject(context, SetterFor(headers));
    EXPECT_EQ(headers.at(std::string(kTraceparent)),
              "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-ff");
}

TEST(W3CPropagatorInjectTest, WritesNothingForADefaultContext)
{
    Headers headers;
    mt::W3CTraceContextPropagator().Inject(mt::SpanContext{}, SetterFor(headers));
    EXPECT_TRUE(headers.empty());
}

TEST(W3CPropagatorInjectTest, WritesNothingWhenTheTraceIdIsAllZero)
{
    mt::SpanContext context;
    context.span_id = mt::SpanId(mt::SpanId::Bytes{0x00, 0xF0, 0x67, 0xAA, 0x0B, 0xA9, 0x02, 0xB7});
    ASSERT_FALSE(context.IsValid());

    Headers headers;
    mt::W3CTraceContextPropagator().Inject(context, SetterFor(headers));
    EXPECT_TRUE(headers.empty());
}

TEST(W3CPropagatorInjectTest, WritesNothingWhenTheSpanIdIsAllZero)
{
    mt::SpanContext context = Extract(kCanonical);
    context.span_id = mt::SpanId{};
    ASSERT_FALSE(context.IsValid());

    Headers headers;
    mt::W3CTraceContextPropagator().Inject(context, SetterFor(headers));
    EXPECT_TRUE(headers.empty());
}

TEST(W3CPropagatorInjectTest, ToleratesAnEmptySetter)
{
    mt::W3CTraceContextPropagator().Inject(Extract(kCanonical), mt::HeaderSetter{});
}

TEST(W3CPropagatorInjectTest, WritesNoTracestateForAnEmptyTraceState)
{
    // An empty `tracestate` is not a legal header value, so none is written.
    // microtel's TraceState carries no storage today (issue #188), so this is
    // every context — see trace_state_test.cpp.
    Headers headers;
    mt::W3CTraceContextPropagator().Inject(Extract(kCanonical), SetterFor(headers));
    EXPECT_EQ(headers.count(std::string(kTracestate)), 0U);
}

}  // namespace
