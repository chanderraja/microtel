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

constexpr std::uint32_t kWtVarint = 0U;
constexpr std::uint32_t kWt64Bit = 1U;
constexpr std::uint32_t kWtLenDelim = 2U;
constexpr std::uint32_t kWt32Bit = 5U;

constexpr unsigned kTagFieldShift = 3U;
constexpr std::uint64_t kTagWireTypeMask = 0x7U;
// Protobuf field numbers are 1 .. 2^29 - 1; anything else is not a message.
constexpr std::uint64_t kMaxFieldNumber = (std::uint64_t{1} << 29U) - 1U;

// ---------------------------------------------------------------------------
// Proto field numbers
// ---------------------------------------------------------------------------

// ExportTraceServiceResponse
constexpr std::uint32_t kFieldPartialSuccess = 1U;

// Export{Trace,Metrics,Logs}PartialSuccess: rejected_spans /
// rejected_data_points / rejected_log_records.
constexpr std::uint32_t kFieldRejected = 1U;

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

struct Tag
{
    std::uint32_t field = 0;
    std::uint32_t wire_type = 0;
};

// Reads a field tag, rejecting a truncated varint and an out-of-range field
// number (0 included).
[[nodiscard]] std::optional<Tag> ReadTag(ByteSpan& buf)
{
    const auto raw = ReadVarint(buf);
    if (!raw.has_value())
    {
        return std::nullopt;
    }
    const std::uint64_t field = *raw >> kTagFieldShift;
    if (field == 0U || field > kMaxFieldNumber)
    {
        return std::nullopt;
    }
    return Tag{
        .field = static_cast<std::uint32_t>(field),
        .wire_type = static_cast<std::uint32_t>(*raw & kTagWireTypeMask),
    };
}

[[nodiscard]] bool SkipField(ByteSpan& buf, std::uint32_t wire_type)
{
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
// Export*PartialSuccess parser
// ---------------------------------------------------------------------------

constexpr auto kMaxRejected = static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max());

// Reads the rejected-count varint and caps it to uint32_t.
[[nodiscard]] bool ReadRejected(ByteSpan& data, std::uint32_t& rejected)
{
    const auto v = ReadVarint(data);
    if (!v.has_value())
    {
        return false;
    }
    rejected = static_cast<std::uint32_t>(std::min(*v, kMaxRejected));
    return true;
}

// Walks one Export*PartialSuccess message to its end. A rejected count found
// on the way overwrites @p rejected (protobuf merge: the last value wins).
// @return false on any wire-format error.
[[nodiscard]] bool ParsePartialSuccess(ByteSpan data, std::uint32_t& rejected)
{
    while (!data.empty())
    {
        const auto tag = ReadTag(data);
        if (!tag.has_value())
        {
            return false;
        }
        const bool is_rejected = (tag->field == kFieldRejected && tag->wire_type == kWtVarint);
        const bool ok =
            is_rejected ? ReadRejected(data, rejected) : SkipField(data, tag->wire_type);
        if (!ok)
        {
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Export*ServiceResponse parser
// ---------------------------------------------------------------------------

constexpr PartialSuccessResult kUnparseable{
    .outcome = PartialSuccessOutcome::Unparseable,
    .rejected = 0,
};

// Reads the partial_success embedded message (field 1, wt=2) into @p result.
[[nodiscard]] bool ExtractPartialSuccess(ByteSpan& data, PartialSuccessResult& result)
{
    const auto inner = ReadLenDelim(data);
    if (!inner.has_value() || !ParsePartialSuccess(*inner, result.rejected))
    {
        return false;
    }
    result.outcome = PartialSuccessOutcome::Parsed;
    return true;
}

// Walks an Export*ServiceResponse to its end: a count is only trusted when
// the whole message around it parses.
[[nodiscard]] PartialSuccessResult ParseResponse(ByteSpan data)
{
    PartialSuccessResult result{};
    while (!data.empty())
    {
        const auto tag = ReadTag(data);
        if (!tag.has_value())
        {
            return kUnparseable;
        }
        const bool is_partial_success =
            (tag->field == kFieldPartialSuccess && tag->wire_type == kWtLenDelim);
        const bool ok = is_partial_success ? ExtractPartialSuccess(data, result)
                                           : SkipField(data, tag->wire_type);
        if (!ok)
        {
            return kUnparseable;
        }
    }
    return result;
}

}  // namespace

PartialSuccessResult ParseRejectedSpans(std::span<const std::byte> body) noexcept
{
    if (body.empty())
    {
        return {};
    }
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    const ByteSpan span{reinterpret_cast<const std::uint8_t*>(body.data()), body.size()};
    return ParseResponse(span);
}

}  // namespace microtel::wire
