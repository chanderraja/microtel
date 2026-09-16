// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// src/api/ — the W3C Trace Context propagator declared in
// include/microtel/propagator.hpp. Issue #188: it was declared in the public
// header and defined in no shipped translation unit. `microtel::TraceState`,
// which used to share this file, moved to trace_state.cpp when it gained
// storage (issue #208 / ICP 0025 packet 2.3a).
//
// Dependency-free by design: the public headers plus the standard library.

#include "microtel/propagator.hpp"

#include "microtel/baggage.hpp"
#include "microtel/trace.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace microtel
{

namespace
{

constexpr std::string_view kTraceparentHeader = "traceparent";
constexpr std::string_view kTracestateHeader = "tracestate";
constexpr std::string_view kBaggageHeader = "baggage";

constexpr std::string_view kLowerHexDigits = "0123456789abcdef";
constexpr unsigned int kNibbleShift = 4U;
constexpr unsigned int kLowNibbleMask = 0x0FU;
constexpr std::size_t kHexCharsPerByte = 2U;
constexpr unsigned int kDecimalDigitCount = 10U;

// A version-00 `traceparent` is a fixed 55-character layout —
// `vv-<32 hex trace id>-<16 hex parent id>-<2 hex flags>` — with `-` at three
// fixed offsets. Deriving the offsets from the id widths keeps them honest.
constexpr char kFieldSeparator = '-';
constexpr std::size_t kSeparatorChars = 1U;
constexpr std::size_t kVersionOffset = 0U;
constexpr std::size_t kVersionChars = 2U;
constexpr std::size_t kTraceIdOffset = kVersionOffset + kVersionChars + kSeparatorChars;
constexpr std::size_t kTraceIdChars = TraceId::kSizeBytes * kHexCharsPerByte;
constexpr std::size_t kSpanIdOffset = kTraceIdOffset + kTraceIdChars + kSeparatorChars;
constexpr std::size_t kSpanIdChars = SpanId::kSizeBytes * kHexCharsPerByte;
constexpr std::size_t kFlagsOffset = kSpanIdOffset + kSpanIdChars + kSeparatorChars;
constexpr std::size_t kFlagsChars = kHexCharsPerByte;
constexpr std::size_t kTraceparentChars = kFlagsOffset + kFlagsChars;

/// @brief The only version microtel emits.
constexpr std::string_view kInjectedVersion = "00";
constexpr std::uint8_t kVersion00 = 0x00U;

/// @brief Version `ff` is forbidden outright by W3C Trace Context §3.2.2.1.
constexpr std::uint8_t kForbiddenVersion = 0xFFU;

/// @brief True for a lower-case hex digit.
///
/// W3C Trace Context requires every `traceparent` hex field to be lower-case
/// (§3.2.2.2-§3.2.2.4, and the specification's own validation corpus rejects
/// rather than case-folds an upper-case id). microtel parses lower-case only,
/// in every field including version and flags, and emits lower-case only.
[[nodiscard]] constexpr bool IsLowerHexDigit(char c) noexcept
{
    const bool is_decimal_digit = (c >= '0') && (c <= '9');
    const bool is_lower_hex_letter = (c >= 'a') && (c <= 'f');
    return is_decimal_digit || is_lower_hex_letter;
}

/// @brief Numeric value of a digit satisfying `IsLowerHexDigit`.
[[nodiscard]] constexpr unsigned int HexDigitValue(char c) noexcept
{
    const bool is_decimal_digit = (c >= '0') && (c <= '9');
    return is_decimal_digit ? static_cast<unsigned int>(c - '0')
                            : (static_cast<unsigned int>(c - 'a') + kDecimalDigitCount);
}

/// @brief Decodes exactly `N * 2` lower-case hex characters into @p out.
///
/// @return false if the width is wrong or any character is not a lower-case
///         hex digit. @p out is then partially written and must be discarded.
template <std::size_t N>
[[nodiscard]] bool ParseLowerHexBytes(std::string_view hex,
                                      std::array<std::uint8_t, N>& out) noexcept
{
    if (hex.size() != (N * kHexCharsPerByte))
    {
        return false;
    }

    std::size_t offset = 0;
    for (std::uint8_t& byte : out)
    {
        const char high = hex[offset];
        const char low = hex[offset + 1U];
        if (!IsLowerHexDigit(high) || !IsLowerHexDigit(low))
        {
            return false;
        }
        byte =
            static_cast<std::uint8_t>((HexDigitValue(high) << kNibbleShift) | HexDigitValue(low));
        offset += kHexCharsPerByte;
    }
    return true;
}

/// @brief True if `-` sits at each of the three fixed offsets.
/// @pre `header.size() >= kTraceparentChars`.
[[nodiscard]] bool HasFixedSeparators(std::string_view header) noexcept
{
    return (header[kTraceIdOffset - kSeparatorChars] == kFieldSeparator) &&
           (header[kSpanIdOffset - kSeparatorChars] == kFieldSeparator) &&
           (header[kFlagsOffset - kSeparatorChars] == kFieldSeparator);
}

/// @brief Applies the W3C version/length rule.
///
/// A version-00 header is exactly 55 characters and nothing may follow. A
/// higher version keeps the same prefix and may append further
/// `-`-separated fields, which this parser ignores (§3.2.2.1 forward
/// compatibility); anything else after the flags is malformed.
///
/// @pre `header.size() >= kTraceparentChars`.
[[nodiscard]] bool HasParsableLength(std::string_view header, std::uint8_t version) noexcept
{
    if (header.size() == kTraceparentChars)
    {
        return true;
    }
    return (version != kVersion00) && (header[kTraceparentChars] == kFieldSeparator);
}

/// @brief Validates the fixed prefix shape and the version field.
[[nodiscard]] bool HasParsableShape(std::string_view header) noexcept
{
    if (header.size() < kTraceparentChars)
    {
        return false;
    }
    if (!HasFixedSeparators(header))
    {
        return false;
    }

    std::array<std::uint8_t, 1U> version{};
    if (!ParseLowerHexBytes(header.substr(kVersionOffset, kVersionChars), version))
    {
        return false;
    }
    if (version.front() == kForbiddenVersion)
    {
        return false;
    }
    return HasParsableLength(header, version.front());
}

/// @brief Parses a `traceparent` value into @p out.
///
/// @return false if @p header is malformed, leaving @p out untouched.
[[nodiscard]] bool ParseTraceparent(std::string_view header, SpanContext& out) noexcept
{
    if (!HasParsableShape(header))
    {
        return false;
    }

    TraceId::Bytes trace_bytes{};
    SpanId::Bytes span_bytes{};
    std::array<std::uint8_t, 1U> flags{};
    const bool decoded =
        ParseLowerHexBytes(header.substr(kTraceIdOffset, kTraceIdChars), trace_bytes) &&
        ParseLowerHexBytes(header.substr(kSpanIdOffset, kSpanIdChars), span_bytes) &&
        ParseLowerHexBytes(header.substr(kFlagsOffset, kFlagsChars), flags);
    if (!decoded)
    {
        return false;
    }

    // W3C: an all-zero trace id or parent id is invalid, not merely unsampled.
    const TraceId trace_id(trace_bytes);
    const SpanId span_id(span_bytes);
    if (!trace_id.IsValid() || !span_id.IsValid())
    {
        return false;
    }

    out.trace_id = trace_id;
    out.span_id = span_id;
    out.trace_flags = TraceFlags(flags.front());
    return true;
}

/// @brief Renders @p context as a canonical version-00 `traceparent`.
///
/// Version 00 defines only the sampled bit, but the remaining flag bits are
/// emitted as held rather than cleared, so an upstream's bits survive a hop
/// through microtel.
[[nodiscard]] std::string FormatTraceparent(const SpanContext& context)
{
    const auto flags = static_cast<unsigned int>(context.trace_flags.AsByte());

    std::string header;
    header.reserve(kTraceparentChars);
    header.append(kInjectedVersion);
    header.push_back(kFieldSeparator);
    header.append(context.trace_id.ToHex());
    header.push_back(kFieldSeparator);
    header.append(context.span_id.ToHex());
    header.push_back(kFieldSeparator);
    header.push_back(kLowerHexDigits[(flags >> kNibbleShift) & kLowNibbleMask]);
    header.push_back(kLowerHexDigits[flags & kLowNibbleMask]);
    return header;
}

}  // namespace

// ── W3CTraceContextPropagator ────────────────────────────────────────────────
//
// `TraceState` itself — storage, the W3C §3.3 grammar, and the copy-on-write
// mutators — lives in trace_state.cpp as of issue #208 / ICP 0025 packet 2.3a.
// Both directions below round-trip a vendor's `tracestate` for real; the
// header is no longer dropped across a microtel hop.

// NOLINTBEGIN(readability-convert-member-functions-to-static)
// Locked public API (include/microtel/propagator.hpp). Both propagators are
// documented as stateless and thread-safe, so no method needs `this`.

void W3CTraceContextPropagator::Inject(const SpanContext& context, const HeaderSetter& setter) const
{
    if (!context.IsValid() || !setter)
    {
        return;
    }

    setter(kTraceparentHeader, FormatTraceparent(context));

    // An empty `tracestate` is not a legal header value, so it is omitted
    // rather than sent blank.
    const std::string state = context.trace_state.ToHeader();
    if (!state.empty())
    {
        setter(kTracestateHeader, state);
    }
}

SpanContext W3CTraceContextPropagator::Extract(const HeaderGetter& getter) const
{
    if (!getter)
    {
        return {};
    }

    const std::optional<std::string_view> traceparent = getter(kTraceparentHeader);
    if (!traceparent.has_value())
    {
        return {};
    }

    SpanContext context;
    if (!ParseTraceparent(*traceparent, context))
    {
        return {};
    }

    // Reached only once the parent has parsed: W3C discards `tracestate`
    // alongside a malformed `traceparent`.
    const std::optional<std::string_view> tracestate = getter(kTracestateHeader);
    if (tracestate.has_value())
    {
        context.trace_state = TraceState::FromHeader(*tracestate);
    }

    context.remote = true;
    return context;
}

// ── W3CBaggagePropagator ─────────────────────────────────────────────────────
//
// The grammar, the percent codec, the `;`-metadata tail and the three limits
// all live in `Baggage` itself (baggage.cpp, ICP 0025 §2), so the propagator
// is only the carrier half: which header name, and when to omit it. That split
// is deliberate — `Baggage::FromHeader` / `ToHeader` are public, so a caller on
// a carrier microtel does not model can reach the same parser without going
// through a propagator.

void W3CBaggagePropagator::Inject(const Baggage& baggage, const HeaderSetter& setter) const
{
    if (baggage.Empty() || !setter)
    {
        return;
    }

    // A non-empty `Baggage` always serialises to at least `key=`, so the
    // header value written here is never the empty (illegal) one. A header
    // whose every member was dropped on the way in parsed to the *empty*
    // baggage, which the guard above already turned into "write nothing".
    setter(kBaggageHeader, baggage.ToHeader());
}

Baggage W3CBaggagePropagator::Extract(const HeaderGetter& getter) const
{
    if (!getter)
    {
        return {};
    }

    const std::optional<std::string_view> baggage = getter(kBaggageHeader);
    if (!baggage.has_value())
    {
        return {};
    }
    return Baggage::FromHeader(*baggage);
}

// NOLINTEND(readability-convert-member-functions-to-static)

}  // namespace microtel
