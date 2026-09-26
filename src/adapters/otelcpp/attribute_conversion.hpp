// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/attribute.hpp"

#include "adapters/otelcpp/abi_guard.hpp"
#include "adapters/otelcpp/shim_diagnostics.hpp"
#include "adapters/otelcpp/shim_options.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include <opentelemetry/common/attribute_value.h>
#include <opentelemetry/common/key_value_iterable.h>

/// @file
/// Converts `opentelemetry::common::AttributeValue` to `microtel::AttributeValue`.
///
/// The two variants do not have the same shape. otel-cpp's has 16 alternatives
/// (it "provides support for all primitive C++ types" by its own description);
/// microtel's has 8, matching what the OpenTelemetry data model actually
/// specifies. Thirteen of the sixteen map exactly or widen losslessly. The
/// remaining three have no faithful *type* mapping and are preserved with a
/// degraded type instead — the exact value the application set survives and
/// nothing is clamped. Policy: ICP 0015. The one omission: a byte span whose
/// hex rendering would exceed `ShimOptions::attribute_value_length_limit` is
/// left out and counted, rather than forwarded for the SDK to cut mid-byte
/// (ICP 0033).

namespace microtel::adapters::otelcpp
{

namespace otel_common = opentelemetry::common;

namespace detail
{

/// @brief Render one hex nibble (0–15) as its lowercase character.
[[nodiscard]] constexpr char NibbleToHex(unsigned nibble) noexcept
{
    constexpr unsigned kDecimalDigitCount = 10U;
    return nibble < kDecimalDigitCount ? static_cast<char>('0' + nibble)
                                       : static_cast<char>('a' + (nibble - kDecimalDigitCount));
}

/// @brief Render a byte span as lowercase hex, no separators:
///        `{0x00, 0xff, 0x1a}` → `"00ff1a"`.
///
/// Hex rather than base64: unambiguous, no padding, and a debugging reader
/// can eyeball it (ICP 0015, "Rationale & alternatives").
///
/// Output is 2 chars per input byte. `SdkSpan::SetAttribute` truncates any
/// string longer than its `attribute_value_length_limit` (issue #181), and an
/// odd cut of a hex string decodes to a final byte the application never set.
/// `ConvertAttributeValue` therefore never calls this for a span whose
/// rendering would exceed `ShimOptions::attribute_value_length_limit`; it
/// omits the attribute instead (ICP 0033, issue #238).
[[nodiscard]] inline std::string RenderBytesAsHex(
    opentelemetry::nostd::span<const std::uint8_t> bytes)
{
    constexpr unsigned kNibbleShift = 4U;
    constexpr unsigned kLowNibbleMask = 0x0FU;
    constexpr std::size_t kHexCharsPerByte = 2U;

    std::string out;
    out.reserve(bytes.size() * kHexCharsPerByte);
    for (const auto element : bytes)
    {
        const auto byte = static_cast<unsigned>(element);
        out.push_back(NibbleToHex(byte >> kNibbleShift));
        out.push_back(NibbleToHex(byte & kLowNibbleMask));
    }
    return out;
}

/// @brief Convert a `uint64_t` array: `vector<int64_t>` when every element
///        fits, otherwise every element rendered as its exact decimal digits.
///
/// Uniformity is deliberate: `AttributeValue`'s array alternatives cannot hold
/// mixed types, and dropping only the offending element would change the
/// array's length, silently breaking index correlation with any parallel
/// attribute. One overflowing element therefore degrades the whole array to
/// strings — homogeneous either way.
[[nodiscard]] inline microtel::AttributeValue ConvertUint64Span(
    opentelemetry::nostd::span<const std::uint64_t> values)
{
    constexpr auto kInt64Max = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());

    bool any_overflow = false;
    for (const auto element : values)
    {
        if (element > kInt64Max)
        {
            any_overflow = true;
            break;
        }
    }

    if (any_overflow)
    {
        std::vector<std::string> out;
        out.reserve(values.size());
        for (const auto element : values)
        {
            out.push_back(std::to_string(element));
        }
        return microtel::AttributeValue{std::move(out)};
    }

    std::vector<std::int64_t> out;
    out.reserve(values.size());
    for (const auto element : values)
    {
        out.push_back(static_cast<std::int64_t>(element));
    }
    return microtel::AttributeValue{std::move(out)};
}

/// @brief Convert an otel-cpp attribute value to microtel's, with no length
///        limit.
///
/// Total: every one of otel-cpp's 16 alternatives produces a value. Thirteen
/// map exactly or widen losslessly (`int32_t`, `uint32_t` → `std::int64_t`).
/// The three with no faithful type mapping degrade the *type* while
/// preserving the exact value (ICP 0015, Option B):
///
/// - `uint64_t` above `INT64_MAX` → its exact decimal digits as a string:
///   `18446744073709551615` arrives as `"18446744073709551615"`.
/// - `span<const uint64_t>` containing such an element → every element
///   rendered decimally (`vector<string>`), keeping the array homogeneous.
/// - `span<const uint8_t>` → lowercase hex string, no separators.
///
/// Clamping never happens: a reported value is always one the application
/// set. Used directly for the log body, which is not an attribute and has no
/// attribute limit (ICP 0033 §3); attribute paths go through
/// `ConvertAttributeValue`.
[[nodiscard]] inline microtel::AttributeValue ConvertUnlimitedAttributeValue(
    const otel_common::AttributeValue& value) noexcept
{
    constexpr auto kInt64Max = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());

    return std::visit(
        [](const auto& held) -> microtel::AttributeValue
        {
            using Held = std::decay_t<decltype(held)>;

            // ── Exact scalars ────────────────────────────────────────────────
            if constexpr (std::is_same_v<Held, bool> || std::is_same_v<Held, double>)
            {
                return microtel::AttributeValue{held};
            }
            // ── Widening scalars (lossless) ──────────────────────────────────
            else if constexpr (std::is_same_v<Held, std::int32_t> ||
                               std::is_same_v<Held, std::uint32_t> ||
                               std::is_same_v<Held, std::int64_t>)
            {
                return microtel::AttributeValue{static_cast<std::int64_t>(held)};
            }
            // ── uint64_t: exact digits as string above INT64_MAX ─────────────
            else if constexpr (std::is_same_v<Held, std::uint64_t>)
            {
                if (held > kInt64Max)
                {
                    return microtel::AttributeValue{std::to_string(held)};
                }
                return microtel::AttributeValue{static_cast<std::int64_t>(held)};
            }
            // ── Strings ──────────────────────────────────────────────────────
            else if constexpr (std::is_same_v<Held, const char*>)
            {
                // A null char* is a valid variant state; treat it as empty
                // rather than constructing std::string from nullptr (UB).
                return microtel::AttributeValue{held == nullptr ? std::string{}
                                                                : std::string{held}};
            }
            else if constexpr (std::is_same_v<Held, opentelemetry::nostd::string_view>)
            {
                return microtel::AttributeValue{std::string{held.data(), held.size()}};
            }
            // ── Byte arrays: lowercase hex string ────────────────────────────
            else if constexpr (std::is_same_v<Held, opentelemetry::nostd::span<const std::uint8_t>>)
            {
                return microtel::AttributeValue{detail::RenderBytesAsHex(held)};
            }
            // ── Arrays ───────────────────────────────────────────────────────
            else if constexpr (std::is_same_v<Held, opentelemetry::nostd::span<const bool>>)
            {
                return microtel::AttributeValue{std::vector<bool>(held.begin(), held.end())};
            }
            else if constexpr (std::is_same_v<Held, opentelemetry::nostd::span<const double>>)
            {
                return microtel::AttributeValue{std::vector<double>(held.begin(), held.end())};
            }
            else if constexpr (std::is_same_v<Held,
                                              opentelemetry::nostd::span<
                                                  const opentelemetry::nostd::string_view>>)
            {
                std::vector<std::string> out;
                out.reserve(held.size());
                for (const auto& sv : held)
                {
                    out.emplace_back(sv.data(), sv.size());
                }
                return microtel::AttributeValue{std::move(out)};
            }
            else if constexpr (std::is_same_v<Held,
                                              opentelemetry::nostd::span<const std::uint64_t>>)
            {
                return detail::ConvertUint64Span(held);
            }
            else
            {
                // Remaining integral spans: int32_t, uint32_t, int64_t. All
                // widen losslessly into vector<int64_t>.
                std::vector<std::int64_t> out;
                out.reserve(held.size());
                for (const auto element : held)
                {
                    out.push_back(static_cast<std::int64_t>(element));
                }
                return microtel::AttributeValue{std::move(out)};
            }
        },
        value);
}

/// @brief True when @p byte_count bytes, rendered as hex (two characters per
///        byte), would exceed `options.attribute_value_length_limit`.
///
/// Decided from the size alone, before rendering, so an oversized span is
/// never allocated. 2n > S is tested as n > ⌊S/2⌋, which cannot overflow.
[[nodiscard]] constexpr bool HexRenderingExceedsLimit(std::size_t byte_count,
                                                      const ShimOptions& options) noexcept
{
    constexpr std::uint32_t kHexCharsPerByte = 2U;
    if (!options.attribute_value_length_limit.has_value())
    {
        return false;
    }
    return byte_count > *options.attribute_value_length_limit / kHexCharsPerByte;
}

}  // namespace detail

/// @brief Convert an otel-cpp attribute value to microtel's, applying the
///        shim's byte-span length limit.
///
/// Every alternative converts as `detail::ConvertUnlimitedAttributeValue`
/// describes (ICP 0015), except one case.
///
/// @return `std::nullopt` only for a byte span whose hex rendering exceeds
///         `options.attribute_value_length_limit`; that case is counted in
///         `ShimDiagnostics` before return. Callers drop a `std::nullopt`
///         instead of forwarding it.
[[nodiscard]] inline std::optional<microtel::AttributeValue> ConvertAttributeValue(
    const otel_common::AttributeValue& value, const ShimOptions& options) noexcept
{
    const auto* const bytes = std::get_if<opentelemetry::nostd::span<const std::uint8_t>>(&value);
    if (bytes != nullptr && detail::HexRenderingExceedsLimit(bytes->size(), options))
    {
        detail::RecordOversizedByteAttributeOmitted(bytes->size());
        return std::nullopt;
    }
    return detail::ConvertUnlimitedAttributeValue(value);
}

/// @brief Materialise an otel-cpp `KeyValueIterable` as owned microtel
///        key/values, converting every value via `ConvertAttributeValue`.
///
/// Values `ConvertAttributeValue` omits are left out of the result; the
/// order of the rest is kept. The result owns its strings, so it outlives
/// the iterable's borrowed storage; callers pass it on as an `AttributeSpan`.
[[nodiscard]] inline std::vector<microtel::KeyValue> ConvertKeyValues(
    const otel_common::KeyValueIterable& attributes, const ShimOptions& options)
{
    std::vector<microtel::KeyValue> out;
    out.reserve(attributes.size());
    attributes.ForEachKeyValue(
        [&out, &options](opentelemetry::nostd::string_view key,
                         const otel_common::AttributeValue& value) noexcept
        {
            auto converted = ConvertAttributeValue(value, options);
            if (converted.has_value())
            {
                out.push_back(
                    {.key = std::string{key.data(), key.size()}, .value = *std::move(converted)});
            }
            return true;
        });
    return out;
}

}  // namespace microtel::adapters::otelcpp
