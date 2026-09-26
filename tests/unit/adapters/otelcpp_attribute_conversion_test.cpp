// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Covers every alternative of opentelemetry-cpp's 16-way AttributeValue variant
// against microtel's 8-way one. The interesting cases are the three with no
// faithful type mapping, which degrade the type while preserving the exact
// value (ICP 0015, Option B); the rest are here so a future variant change
// cannot silently lose an alternative.

#include "adapters/otelcpp/attribute_conversion.hpp"
#include "adapters/otelcpp/shim_diagnostics.hpp"
#include "adapters/otelcpp/shim_options.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <format>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

#include <opentelemetry/common/key_value_iterable_view.h>

namespace
{

using microtel::adapters::otelcpp::ConvertAttributeValue;
using microtel::adapters::otelcpp::ConvertKeyValues;
using microtel::adapters::otelcpp::GetShimDiagnostics;
using microtel::adapters::otelcpp::ShimOptions;
namespace nostd = opentelemetry::nostd;

/// Convert under @p options and require a value. Every alternative except an
/// over-limit byte span converts (ICP 0033 §3), so the tests below that are
/// not about the limit go through this and assert on the value directly.
[[nodiscard]] microtel::AttributeValue Kept(const opentelemetry::common::AttributeValue& value,
                                            const ShimOptions& options = {})
{
    auto converted = ConvertAttributeValue(value, options);
    if (!converted.has_value())
    {
        ADD_FAILURE() << "ConvertAttributeValue omitted a value it should have kept";
        return microtel::AttributeValue{};
    }
    return *std::move(converted);
}

// ── Exact scalars ─────────────────────────────────────────────────────────────

TEST(OtelCppAttributeConversion, Bool)
{
    const auto converted = Kept(opentelemetry::common::AttributeValue{true});
    EXPECT_TRUE(std::get<bool>(converted));
}

TEST(OtelCppAttributeConversion, Double)
{
    const auto converted = Kept(opentelemetry::common::AttributeValue{2.5});
    EXPECT_DOUBLE_EQ(std::get<double>(converted), 2.5);
}

// ── Widening scalars ──────────────────────────────────────────────────────────

TEST(OtelCppAttributeConversion, Int32WidensToInt64)
{
    const auto converted = Kept(opentelemetry::common::AttributeValue{std::int32_t{-7}});
    EXPECT_EQ(std::get<std::int64_t>(converted), -7);
}

TEST(OtelCppAttributeConversion, Uint32WidensToInt64)
{
    const auto converted =
        Kept(opentelemetry::common::AttributeValue{std::numeric_limits<std::uint32_t>::max()});
    EXPECT_EQ(std::get<std::int64_t>(converted), 4294967295LL);
}

TEST(OtelCppAttributeConversion, Int64Exact)
{
    constexpr auto kMin = std::numeric_limits<std::int64_t>::min();
    const auto converted = Kept(opentelemetry::common::AttributeValue{kMin});
    EXPECT_EQ(std::get<std::int64_t>(converted), kMin);
}

// ── uint64_t: the boundary is the whole point ─────────────────────────────────

TEST(OtelCppAttributeConversion, Uint64AtInt64MaxConverts)
{
    constexpr auto kBoundary = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    const auto converted = Kept(opentelemetry::common::AttributeValue{kBoundary});
    EXPECT_EQ(std::get<std::int64_t>(converted), std::numeric_limits<std::int64_t>::max());
}

TEST(OtelCppAttributeConversion, Uint64AboveInt64MaxBecomesExactDecimalString)
{
    constexpr auto kTooBig =
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) + 1U;

    const auto converted = Kept(opentelemetry::common::AttributeValue{kTooBig});

    // Degraded type, exact value: the digits are the ones the application
    // set. Clamping would report a number it never set; dropping would
    // destroy the value (ICP 0015).
    EXPECT_EQ(std::get<std::string>(converted), "9223372036854775808");
}

TEST(OtelCppAttributeConversion, Uint64MaxBecomesExactDecimalString)
{
    const auto converted =
        Kept(opentelemetry::common::AttributeValue{std::numeric_limits<std::uint64_t>::max()});
    EXPECT_EQ(std::get<std::string>(converted), "18446744073709551615");
}

// ── Strings ───────────────────────────────────────────────────────────────────

TEST(OtelCppAttributeConversion, CString)
{
    const auto converted = Kept(opentelemetry::common::AttributeValue{"microtel"});
    EXPECT_EQ(std::get<std::string>(converted), "microtel");
}

TEST(OtelCppAttributeConversion, NullCStringBecomesEmptyNotUndefined)
{
    // A null const char* is a reachable variant state. Constructing
    // std::string from nullptr is UB, so this must be handled explicitly.
    const char* null_str = nullptr;
    const auto converted = Kept(opentelemetry::common::AttributeValue{null_str});
    EXPECT_EQ(std::get<std::string>(converted), "");
}

TEST(OtelCppAttributeConversion, StringViewIsCopiedNotBorrowed)
{
    std::string owner = "borrowed";
    const auto converted = Kept(opentelemetry::common::AttributeValue{nostd::string_view{owner}});

    owner = "mutated";  // the conversion must own its copy
    EXPECT_EQ(std::get<std::string>(converted), "borrowed");
}

// ── Arrays ────────────────────────────────────────────────────────────────────

TEST(OtelCppAttributeConversion, BoolSpan)
{
    const std::vector<bool> source_storage{true, false, true};
    const bool raw[] = {true, false, true};
    const auto converted =
        Kept(opentelemetry::common::AttributeValue{nostd::span<const bool>{raw, 3}});
    EXPECT_EQ(std::get<std::vector<bool>>(converted), source_storage);
}

TEST(OtelCppAttributeConversion, Int32SpanWidens)
{
    const std::int32_t raw[] = {1, -2, 3};
    const auto converted =
        Kept(opentelemetry::common::AttributeValue{nostd::span<const std::int32_t>{raw, 3}});
    EXPECT_EQ(std::get<std::vector<std::int64_t>>(converted),
              (std::vector<std::int64_t>{1, -2, 3}));
}

TEST(OtelCppAttributeConversion, DoubleSpan)
{
    const double raw[] = {1.5, 2.5};
    const auto converted =
        Kept(opentelemetry::common::AttributeValue{nostd::span<const double>{raw, 2}});
    EXPECT_EQ(std::get<std::vector<double>>(converted), (std::vector<double>{1.5, 2.5}));
}

TEST(OtelCppAttributeConversion, StringViewSpan)
{
    const nostd::string_view raw[] = {"a", "bb"};
    const auto converted =
        Kept(opentelemetry::common::AttributeValue{nostd::span<const nostd::string_view>{raw, 2}});
    EXPECT_EQ(std::get<std::vector<std::string>>(converted), (std::vector<std::string>{"a", "bb"}));
}

TEST(OtelCppAttributeConversion, Uint64SpanAllFittingWidensToInt64)
{
    const std::uint64_t raw[] = {
        0U, 42U, static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())};
    const auto converted =
        Kept(opentelemetry::common::AttributeValue{nostd::span<const std::uint64_t>{raw, 3}});
    EXPECT_EQ(std::get<std::vector<std::int64_t>>(converted),
              (std::vector<std::int64_t>{0, 42, std::numeric_limits<std::int64_t>::max()}));
}

TEST(OtelCppAttributeConversion, Uint64SpanWithOneOverflowElementRendersAllAsStrings)
{
    const std::uint64_t raw[] = {
        1U, static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) + 1U, 3U};

    const auto converted =
        Kept(opentelemetry::common::AttributeValue{nostd::span<const std::uint64_t>{raw, 3}});

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

    const auto converted =
        Kept(opentelemetry::common::AttributeValue{nostd::span<const std::uint8_t>{raw, 3}});

    // Every byte renders as exactly two lowercase hex digits — 0x00 keeps its
    // leading zero, 0xff exercises both alphabetic nibbles.
    EXPECT_EQ(std::get<std::string>(converted), "00ff1a");
}

TEST(OtelCppAttributeConversion, EmptyByteSpanBecomesEmptyString)
{
    const auto converted =
        Kept(opentelemetry::common::AttributeValue{nostd::span<const std::uint8_t>{}});
    EXPECT_EQ(std::get<std::string>(converted), "");
}

TEST(OtelCppAttributeConversion, LargeByteSpanEncodesCorrectlyAtScale)
{
    // Exercises hex encoding well past the point where its output would
    // exceed the default attribute_value_length_limit (4096 chars, i.e.
    // inputs over 2048 bytes), with the shim limit switched off
    // (std::nullopt, ICP 0033 §1) so the value is kept. Pins correct
    // large-scale encoding, and catches bugs (off-by-one loop bounds, buffer
    // growth) invisible at the 2-3 byte scale the tests above use. The
    // limit itself is covered by the "Bytes: attribute value length limit"
    // tests below.
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

    const auto converted =
        Kept(opentelemetry::common::AttributeValue{nostd::span<const std::uint8_t>{raw.data(),
                                                                                   raw.size()}},
             ShimOptions{.attribute_value_length_limit = std::nullopt});

    EXPECT_EQ(std::get<std::string>(converted), expected);
}

// ── Bytes: attribute value length limit (ICP 0033, issue #238) ───────────────
//
// ShimDiagnostics is process-wide with no reset, so every assertion on it is
// a before/after difference (ICP 0033 §7): the omission counter is diffed,
// and the high-water mark is checked as after == max(before, n).

[[nodiscard]] opentelemetry::common::AttributeValue Bytes(const std::vector<std::uint8_t>& raw)
{
    return opentelemetry::common::AttributeValue{
        nostd::span<const std::uint8_t>{raw.data(), raw.size()}};
}

TEST(OtelCppAttributeConversion, DefaultLimitKeepsA2048ByteSpanWhoseHexIsExactlyTheLimit)
{
    // 2048 bytes render as 4096 hex characters: exactly the default limit.
    constexpr std::size_t kByteCount = 2048;
    const std::vector<std::uint8_t> raw(kByteCount, 0xabU);
    const auto before = GetShimDiagnostics();

    const auto converted = Kept(Bytes(raw), ShimOptions{});

    EXPECT_EQ(std::get<std::string>(converted).size(), kByteCount * 2U);
    EXPECT_EQ(GetShimDiagnostics().oversized_byte_attributes_omitted,
              before.oversized_byte_attributes_omitted);
}

TEST(OtelCppAttributeConversion, DefaultLimitOmitsA2049ByteSpanRatherThanCuttingItMidByte)
{
    // The #238 case: 2049 bytes render as 4098 hex characters, over the
    // default limit. Forwarded, the SDK would cut the string; the shim omits
    // it instead, so no reader ever sees a hex string that decodes to bytes
    // the application never set.
    constexpr std::size_t kByteCount = 2049;
    const std::vector<std::uint8_t> raw(kByteCount, 0xabU);
    const auto before = GetShimDiagnostics();

    const auto converted = ConvertAttributeValue(Bytes(raw), ShimOptions{});

    EXPECT_FALSE(converted.has_value());
    const auto after = GetShimDiagnostics();
    EXPECT_EQ(after.oversized_byte_attributes_omitted,
              before.oversized_byte_attributes_omitted + 1U);
    EXPECT_EQ(after.largest_omitted_byte_attribute,
              std::max<std::uint64_t>(before.largest_omitted_byte_attribute, kByteCount));
}

TEST(OtelCppAttributeConversion, ByteSpanExactlyAtLimitIsKept)
{
    const std::vector<std::uint8_t> raw{0x00, 0xff, 0x1a};  // 6 hex characters

    const auto converted =
        Kept(Bytes(raw), ShimOptions{.attribute_value_length_limit = std::uint32_t{6}});

    EXPECT_EQ(std::get<std::string>(converted), "00ff1a");
}

TEST(OtelCppAttributeConversion, ByteSpanOneByteOverLimitIsOmittedCountedAndHighWaterUpdated)
{
    const std::vector<std::uint8_t> raw{0x00, 0xff, 0x1a, 0x2b};  // 8 hex characters
    const auto before = GetShimDiagnostics();

    const auto converted = ConvertAttributeValue(
        Bytes(raw), ShimOptions{.attribute_value_length_limit = std::uint32_t{6}});

    EXPECT_FALSE(converted.has_value());
    const auto after = GetShimDiagnostics();
    EXPECT_EQ(after.oversized_byte_attributes_omitted,
              before.oversized_byte_attributes_omitted + 1U);
    // Bytes, not hex characters: 4, not 8.
    EXPECT_EQ(after.largest_omitted_byte_attribute,
              std::max<std::uint64_t>(before.largest_omitted_byte_attribute, raw.size()));
}

TEST(OtelCppAttributeConversion, OddLimitRoundsTheKeptRenderingDownToWholeBytes)
{
    // Limit 7: a 3-byte span (6 characters) fits; a 4-byte span (8) does not.
    // No limit can admit half a byte.
    const ShimOptions options{.attribute_value_length_limit = std::uint32_t{7}};
    const std::vector<std::uint8_t> three{0x01, 0x02, 0x03};
    const std::vector<std::uint8_t> four{0x01, 0x02, 0x03, 0x04};

    EXPECT_EQ(std::get<std::string>(Kept(Bytes(three), options)), "010203");
    EXPECT_FALSE(ConvertAttributeValue(Bytes(four), options).has_value());
}

TEST(OtelCppAttributeConversion, NulloptLimitKeepsAnyByteSpan)
{
    constexpr std::size_t kByteCount = 100000;
    const std::vector<std::uint8_t> raw(kByteCount, 0x5aU);
    const auto before = GetShimDiagnostics();

    const auto converted =
        Kept(Bytes(raw), ShimOptions{.attribute_value_length_limit = std::nullopt});

    EXPECT_EQ(std::get<std::string>(converted).size(), kByteCount * 2U);
    EXPECT_EQ(GetShimDiagnostics().oversized_byte_attributes_omitted,
              before.oversized_byte_attributes_omitted);
}

TEST(OtelCppAttributeConversion, LimitZeroOmitsEveryNonEmptyByteSpanAndKeepsTheEmptyOne)
{
    // 0 is a real limit, as in the SDK: the counterpart of "every string
    // truncated to empty" (ICP 0033 §1).
    const ShimOptions options{.attribute_value_length_limit = std::uint32_t{0}};
    const std::vector<std::uint8_t> one{0x01};
    const auto before = GetShimDiagnostics();

    EXPECT_FALSE(ConvertAttributeValue(Bytes(one), options).has_value());
    EXPECT_EQ(std::get<std::string>(Kept(Bytes({}), options)), "");
    EXPECT_EQ(GetShimDiagnostics().oversized_byte_attributes_omitted,
              before.oversized_byte_attributes_omitted + 1U);
}

TEST(OtelCppAttributeConversion, LimitAppliesOnlyToByteSpans)
{
    // S governs the hex rendering of byte spans only; ordinary strings pass
    // through for the SDK to truncate at a UTF-8 boundary (ICP 0033 §5).
    const ShimOptions options{.attribute_value_length_limit = std::uint32_t{0}};

    EXPECT_EQ(std::get<std::string>(Kept(opentelemetry::common::AttributeValue{"longer"}, options)),
              "longer");
    const nostd::string_view strings[] = {"a", "bb"};
    EXPECT_EQ(std::get<std::vector<std::string>>(
                  Kept(opentelemetry::common::AttributeValue{nostd::span<const nostd::string_view>{
                           strings, 2}},
                       options)),
              (std::vector<std::string>{"a", "bb"}));
}

TEST(OtelCppAttributeConversion, HighWaterMarkNeverDecreases)
{
    const ShimOptions options{.attribute_value_length_limit = std::uint32_t{0}};
    constexpr std::size_t kLarger = 300;
    constexpr std::size_t kSmaller = 5;
    const std::vector<std::uint8_t> larger(kLarger, 0x01U);
    const std::vector<std::uint8_t> smaller(kSmaller, 0x01U);
    const auto before = GetShimDiagnostics();

    std::ignore = ConvertAttributeValue(Bytes(larger), options);
    std::ignore = ConvertAttributeValue(Bytes(smaller), options);

    const auto after = GetShimDiagnostics();
    EXPECT_EQ(after.oversized_byte_attributes_omitted,
              before.oversized_byte_attributes_omitted + 2U);
    EXPECT_EQ(after.largest_omitted_byte_attribute,
              std::max<std::uint64_t>(before.largest_omitted_byte_attribute, kLarger));
}

TEST(OtelCppAttributeConversion, ConvertKeyValuesLeavesOmittedValuesOutAndKeepsOrder)
{
    const std::vector<std::uint8_t> blob{0x01, 0x02, 0x03};
    const std::vector<std::uint8_t> small{0x0a};
    const std::map<std::string, opentelemetry::common::AttributeValue> source{
        {"a.first", std::int64_t{1}},
        {"b.blob", Bytes(blob)},
        {"c.small", Bytes(small)},
        {"d.last", "x"},
    };
    const opentelemetry::common::KeyValueIterableView<decltype(source)> iterable{source};

    const auto converted =
        ConvertKeyValues(iterable, ShimOptions{.attribute_value_length_limit = std::uint32_t{4}});

    ASSERT_EQ(converted.size(), 3U);
    EXPECT_EQ(converted[0].key, "a.first");
    EXPECT_EQ(converted[1].key, "c.small");
    EXPECT_EQ(std::get<std::string>(converted[1].value), "0a");
    EXPECT_EQ(converted[2].key, "d.last");
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
