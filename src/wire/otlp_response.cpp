// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#include "wire/otlp_response.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>

namespace microtel::wire
{

namespace
{

using ByteSpan = std::span<const std::uint8_t>;

// ---------------------------------------------------------------------------
// Proto varint decoding constants
// ---------------------------------------------------------------------------

constexpr std::uint8_t kVarintContinueBit = 0x80U;
constexpr std::uint64_t kVarintDataMask = 0x7FU;
constexpr unsigned kVarintShiftStep = 7U;
constexpr unsigned kVarintMaxShift = 64U;

// ---------------------------------------------------------------------------
// Proto wire-type fixed-width field sizes
// ---------------------------------------------------------------------------

constexpr std::size_t kWireWidth64Bit = 8U;
constexpr std::size_t kWireWidth32Bit = 4U;

// ---------------------------------------------------------------------------
// Proto field numbers
// ---------------------------------------------------------------------------

// ExportTraceServiceResponse
constexpr std::uint32_t kFieldPartialSuccess = 1U;

// ExportTracePartialSuccess
constexpr std::uint32_t kFieldRejectedSpans = 1U;

// ---------------------------------------------------------------------------
// Proto wire-format reader
// ---------------------------------------------------------------------------

[[nodiscard]] std::optional<std::uint64_t> ReadVarint(ByteSpan& buf)
{
    std::uint64_t result = 0;
    unsigned shift = 0U;
    while (!buf.empty())
    {
        const std::uint8_t b = buf.front();
        buf = buf.subspan(1);
        result |= static_cast<std::uint64_t>(b & kVarintDataMask) << shift;
        if ((b & kVarintContinueBit) == 0U)
        {
            return result;
        }
        shift += kVarintShiftStep;
        if (shift >= kVarintMaxShift)
        {
            return std::nullopt;
        }
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<ByteSpan> ReadLenDelim(ByteSpan& buf)
{
    const auto len = ReadVarint(buf);
    if (!len.has_value() || *len > buf.size())
    {
        return std::nullopt;
    }
    const ByteSpan result = buf.subspan(0, static_cast<std::size_t>(*len));
    buf = buf.subspan(static_cast<std::size_t>(*len));
    return result;
}

[[nodiscard]] bool SkipField(ByteSpan& buf, std::uint32_t wire_type)
{
    constexpr std::uint32_t kWtVarint = 0;
    constexpr std::uint32_t kWtLenDelim = 2;
    constexpr std::uint32_t kWt64Bit = 1;
    constexpr std::uint32_t kWt32Bit = 5;
    if (wire_type == kWtVarint)
    {
        return ReadVarint(buf).has_value();
    }
    if (wire_type == kWtLenDelim)
    {
        return ReadLenDelim(buf).has_value();
    }
    if (wire_type == kWt64Bit)
    {
        if (buf.size() < kWireWidth64Bit)
        {
            return false;
        }
        buf = buf.subspan(kWireWidth64Bit);
        return true;
    }
    if (wire_type == kWt32Bit)
    {
        if (buf.size() < kWireWidth32Bit)
        {
            return false;
        }
        buf = buf.subspan(kWireWidth32Bit);
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// ExportTracePartialSuccess parser
// ---------------------------------------------------------------------------

constexpr auto kMaxRejected = static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max());

// Reads the rejected_spans varint (field 1) and caps it to uint32_t.
[[nodiscard]] std::uint32_t ReadRejectedSpans(ByteSpan& data)
{
    const auto v = ReadVarint(data);
    if (!v.has_value())
    {
        return 0;
    }
    return static_cast<std::uint32_t>(std::min(*v, kMaxRejected));
}

// Parses ExportTracePartialSuccess → rejected_spans.
[[nodiscard]] std::uint32_t ParsePartialSuccess(ByteSpan data)
{
    while (!data.empty())
    {
        const auto tag = ReadVarint(data);
        if (!tag.has_value())
        {
            return 0;
        }
        const auto fn = static_cast<std::uint32_t>(*tag >> 3U);
        const auto wt = static_cast<std::uint32_t>(*tag & 0x7U);
        if (fn == kFieldRejectedSpans && wt == 0U)
        {
            return ReadRejectedSpans(data);
        }
        if (!SkipField(data, wt))
        {
            return 0;
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------
// ExportTraceServiceResponse parser
// ---------------------------------------------------------------------------

// Reads the partial_success embedded message (field 1, wt=2).
[[nodiscard]] std::uint32_t ExtractPartialSuccess(ByteSpan& data)
{
    const auto inner = ReadLenDelim(data);
    if (!inner.has_value())
    {
        return 0;
    }
    return ParsePartialSuccess(*inner);
}

// Parses ExportTraceServiceResponse → partial_success.rejected_spans.
[[nodiscard]] std::uint32_t ParseResponse(ByteSpan data)
{
    while (!data.empty())
    {
        const auto tag = ReadVarint(data);
        if (!tag.has_value())
        {
            return 0;
        }
        const auto fn = static_cast<std::uint32_t>(*tag >> 3U);
        const auto wt = static_cast<std::uint32_t>(*tag & 0x7U);
        if (fn == kFieldPartialSuccess && wt == 2U)
        {
            return ExtractPartialSuccess(data);
        }
        if (!SkipField(data, wt))
        {
            return 0;
        }
    }
    return 0;
}

}  // namespace

std::uint32_t ParseRejectedSpans(std::span<const std::byte> body) noexcept
{
    if (body.empty())
    {
        return 0;
    }
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    const ByteSpan span{reinterpret_cast<const std::uint8_t*>(body.data()), body.size()};
    return ParseResponse(span);
}

}  // namespace microtel::wire
