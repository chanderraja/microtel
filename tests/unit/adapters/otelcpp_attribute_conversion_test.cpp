// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Covers every alternative of opentelemetry-cpp's 16-way AttributeValue variant
// against microtel's 8-way one. The interesting cases are the three with no
// faithful type mapping, which degrade the type while preserving the exact
// value (ICP 0015, Option B); the rest are here so a future variant change
// cannot silently lose an alternative.

#include "adapters/otelcpp/attribute_conversion.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <format>
#include <limits>
#include <string>
#include <vector>

namespace
{

using microtel::adapters::otelcpp::ConvertAttributeValue;
namespace nostd = opentelemetry::nostd;

// ── Exact scalars ─────────────────────────────────────────────────────────────

TEST(OtelCppAttributeConversion, Bool)
{
    const auto converted = ConvertAttributeValue(opentelemetry::common::AttributeValue{true});
    EXPECT_TRUE(std::get<bool>(converted));
}

TEST(OtelCppAttributeConversion, Double)
{
    const auto converted = ConvertAttributeValue(opentelemetry::common::AttributeValue{2.5});
    EXPECT_DOUBLE_EQ(std::get<double>(converted), 2.5);
}

// ── Widening scalars ──────────────────────────────────────────────────────────

TEST(OtelCppAttributeConversion, Int32WidensToInt64)
{
    const auto converted =
        ConvertAttributeValue(opentelemetry::common::AttributeValue{std::int32_t{-7}});
    EXPECT_EQ(std::get<std::int64_t>(converted), -7);
}

TEST(OtelCppAttributeConversion, Uint32WidensToInt64)
{
    const auto converted = ConvertAttributeValue(
        opentelemetry::common::AttributeValue{std::numeric_limits<std::uint32_t>::max()});
    EXPECT_EQ(std::get<std::int64_t>(converted), 4294967295LL);
}

TEST(OtelCppAttributeConversion, Int64Exact)
{
    constexpr auto kMin = std::numeric_limits<std::int64_t>::min();
    const auto converted = ConvertAttributeValue(opentelemetry::common::AttributeValue{kMin});
    EXPECT_EQ(std::get<std::int64_t>(converted), kMin);
}

// ── uint64_t: the boundary is the whole point ─────────────────────────────────

TEST(OtelCppAttributeConversion, Uint64AtInt64MaxConverts)
{
    constexpr auto kBoundary = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    const auto converted = ConvertAttributeValue(opentelemetry::common::AttributeValue{kBoundary});
    EXPECT_EQ(std::get<std::int64_t>(converted), std::numeric_limits<std::int64_t>::max());
}

TEST(OtelCppAttributeConversion, Uint64AboveInt64MaxBecomesExactDecimalString)
{
    constexpr auto kTooBig =
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) + 1U;

    const auto converted = ConvertAttributeValue(opentelemetry::common::AttributeValue{kTooBig});

    // Degraded type, exact value: the digits are the ones the application
    // set. Clamping would report a number it never set; dropping would
    // destroy the value (ICP 0015).
    EXPECT_EQ(std::get<std::string>(converted), "9223372036854775808");
}

TEST(OtelCppAttributeConversion, Uint64MaxBecomesExactDecimalString)
{
    const auto converted = ConvertAttributeValue(
        opentelemetry::common::AttributeValue{std::numeric_limits<std::uint64_t>::max()});
    EXPECT_EQ(std::get<std::string>(converted), "18446744073709551615");
}

// ── Strings ───────────────────────────────────────────────────────────────────

TEST(OtelCppAttributeConversion, CString)
{
    const auto converted = ConvertAttributeValue(opentelemetry::common::AttributeValue{"microtel"});
    EXPECT_EQ(std::get<std::string>(converted), "microtel");
}

TEST(OtelCppAttributeConversion, NullCStringBecomesEmptyNotUndefined)
{
    // A null const char* is a reachable variant state. Constructing
    // std::string from nullptr is UB, so this must be handled explicitly.
    const char* null_str = nullptr;
    const auto converted = ConvertAttributeValue(opentelemetry::common::AttributeValue{null_str});
    EXPECT_EQ(std::get<std::string>(converted), "");
}

TEST(OtelCppAttributeConversion, StringViewIsCopiedNotBorrowed)
{
    std::string owner = "borrowed";
    const auto converted =
        ConvertAttributeValue(opentelemetry::common::AttributeValue{nostd::string_view{owner}});

    owner = "mutated";  // the conversion must own its copy
    EXPECT_EQ(std::get<std::string>(converted), "borrowed");
}

// ── Arrays ────────────────────────────────────────────────────────────────────

TEST(OtelCppAttributeConversion, BoolSpan)
{
    const std::vector<bool> source_storage{true, false, true};
    const bool raw[] = {true, false, true};
    const auto converted = ConvertAttributeValue(
        opentelemetry::common::AttributeValue{nostd::span<const bool>{raw, 3}});
    EXPECT_EQ(std::get<std::vector<bool>>(converted), source_storage);
}

TEST(OtelCppAttributeConversion, Int32SpanWidens)
{
    const std::int32_t raw[] = {1, -2, 3};
    const auto converted = ConvertAttributeValue(
        opentelemetry::common::AttributeValue{nostd::span<const std::int32_t>{raw, 3}});
    EXPECT_EQ(std::get<std::vector<std::int64_t>>(converted),
              (std::vector<std::int64_t>{1, -2, 3}));
}

TEST(OtelCppAttributeConversion, DoubleSpan)
{
    const double raw[] = {1.5, 2.5};
    const auto converted = ConvertAttributeValue(
        opentelemetry::common::AttributeValue{nostd::span<const double>{raw, 2}});
    EXPECT_EQ(std::get<std::vector<double>>(converted), (std::vector<double>{1.5, 2.5}));
}

TEST(OtelCppAttributeConversion, StringViewSpan)
{
    const nostd::string_view raw[] = {"a", "bb"};
    const auto converted = ConvertAttributeValue(
        opentelemetry::common::AttributeValue{nostd::span<const nostd::string_view>{raw, 2}});
    EXPECT_EQ(std::get<std::vector<std::string>>(converted), (std::vector<std::string>{"a", "bb"}));
}

TEST(OtelCppAttributeConversion, Uint64SpanAllFittingWidensToInt64)
{
    const std::uint64_t raw[] = {
        0U, 42U, static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())};
    const auto converted = ConvertAttributeValue(
        opentelemetry::common::AttributeValue{nostd::span<const std::uint64_t>{raw, 3}});
    EXPECT_EQ(std::get<std::vector<std::int64_t>>(converted),
              (std::vector<std::int64_t>{0, 42, std::numeric_limits<std::int64_t>::max()}));
}

TEST(OtelCppAttributeConversion, Uint64SpanWithOneOverflowElementRendersAllAsStrings)
{
    const std::uint64_t raw[] = {
        1U, static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) + 1U, 3U};

    const auto converted = ConvertAttributeValue(
        opentelemetry::common::AttributeValue{nostd::span<const std::uint64_t>{raw, 3}});

    // One overflowing element degrades the WHOLE array: AttributeValue's
    // array alternatives cannot hold mixed types, and dropping just the
    // offending element would change the array's length, silently breaking
    // index correlation with a parallel attribute (ICP 0015). Every element
    // keeps its exact digits.
    EXPECT_EQ(std::get<std::vector<std::string>>(converted),
              (std::vector<std::string>{"1", "9223372036854775808", "3"}));
}

// ── Bytes: lowercase hex, no separators ───────────────────────────────────────

TEST(OtelCppAttributeConversion, ByteSpanBecomesLowercaseHex)
{
    const std::uint8_t raw[] = {0x00, 0xff, 0x1a};

    const auto converted = ConvertAttributeValue(
        opentelemetry::common::AttributeValue{nostd::span<const std::uint8_t>{raw, 3}});

    // Every byte renders as exactly two lowercase hex digits — 0x00 keeps its
    // leading zero, 0xff exercises both alphabetic nibbles.
    EXPECT_EQ(std::get<std::string>(converted), "00ff1a");
}

TEST(OtelCppAttributeConversion, EmptyByteSpanBecomesEmptyString)
{
    const auto converted = ConvertAttributeValue(
        opentelemetry::common::AttributeValue{nostd::span<const std::uint8_t>{}});
    EXPECT_EQ(std::get<std::string>(converted), "");
}

TEST(OtelCppAttributeConversion, LargeByteSpanEncodesCorrectlyAtScale)
{
    // Exercises hex encoding well past the point where its output would
    // exceed the default attribute_value_length_limit (4096 chars, i.e.
    // inputs over 2048 bytes) if that limit were enforced anywhere — it is
    // not, as of ICP 0015's addendum. Pins correct large-scale encoding so a
    // future length-limit implementation has a known-good baseline to
    // truncate from, and catches bugs (off-by-one loop bounds, buffer
    // growth) invisible at the 2-3 byte scale the tests above use.
    //
    // `expected` is built independently of RenderBytesAsHex, via
    // std::format, so this isn't just calling the same nibble math twice.
    constexpr std::size_t kByteCount = 5000;
    std::vector<std::uint8_t> raw(kByteCount);
    std::string expected;
    expected.reserve(kByteCount * 2);
    for (std::size_t i = 0; i < kByteCount; ++i)
    {
        raw[i] = static_cast<std::uint8_t>((i * 37U + 11U) % 256U);
        expected += std::format("{:02x}", raw[i]);
    }

    const auto converted = ConvertAttributeValue(opentelemetry::common::AttributeValue{
        nostd::span<const std::uint8_t>{raw.data(), raw.size()}});

    EXPECT_EQ(std::get<std::string>(converted), expected);
}

// ── Coverage guard ────────────────────────────────────────────────────────────

TEST(OtelCppAttributeConversion, EveryVariantAlternativeIsCovered)
{
    // If otel-cpp adds an alternative, this fails and forces a decision about
    // how it maps rather than letting it fall into the generic integral branch.
    static_assert(std::variant_size_v<opentelemetry::common::AttributeValue> == 16,
                  "opentelemetry-cpp's AttributeValue changed shape — revisit "
                  "ConvertAttributeValue and add a case for the new alternative.");
    SUCCEED();
}

}  // namespace
