// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#include "wire/grpc/grpc_status.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace microtel::wire
{
namespace
{

/// The status matrix from `docs/error-model.md` §7.2, in code order.
///
/// This is the single source of retryability for `GrpcWireCodec`; it replaced a
/// row of loose `constexpr int` comparisons that had no connection to the
/// matrix and no test holding them to it.
///
/// UNKNOWN (2) and ALREADY_EXISTS (6) are the two codes §7.2 does not list.
/// They take their names from the gRPC spec and are non-retryable, which is
/// what the previous code did with them by falling off the end of its
/// retryable list — so this table changes no behaviour, it only writes the
/// behaviour down.
constexpr std::array<GrpcStatusInfo, static_cast<std::size_t>(kGrpcStatusCount)> kStatusTable{{
    {.name = "OK", .retryable = false},
    {.name = "CANCELLED", .retryable = true},
    {.name = "UNKNOWN", .retryable = false},
    {.name = "INVALID_ARGUMENT", .retryable = false},
    {.name = "DEADLINE_EXCEEDED", .retryable = true},
    {.name = "NOT_FOUND", .retryable = false},
    {.name = "ALREADY_EXISTS", .retryable = false},
    {.name = "PERMISSION_DENIED", .retryable = false},
    // Retryable only with RetryInfo — see the warning on GrpcStatusInfo.
    {.name = "RESOURCE_EXHAUSTED", .retryable = false},
    {.name = "FAILED_PRECONDITION", .retryable = false},
    {.name = "ABORTED", .retryable = true},
    {.name = "OUT_OF_RANGE", .retryable = true},
    {.name = "UNIMPLEMENTED", .retryable = false},
    {.name = "INTERNAL", .retryable = false},
    {.name = "UNAVAILABLE", .retryable = true},
    {.name = "DATA_LOSS", .retryable = true},
    {.name = "UNAUTHENTICATED", .retryable = false},
}};

/// Stand-in name for a code outside the canonical range.
constexpr std::string_view kUnrecognizedName = "UNRECOGNIZED";

constexpr int kHexLetterOffset = 10;
constexpr unsigned kNibbleShift = 4U;
/// A percent escape is three characters: '%' plus two hex digits.
constexpr std::size_t kEscapeLength = 3U;

/// Room for `" (NN): "` and the odd negative sign, so the format never
/// reallocates. An over-estimate by a few bytes costs nothing.
constexpr std::size_t kFormatOverhead = 16U;

[[nodiscard]] std::optional<unsigned> HexValue(char c) noexcept
{
    if (c >= '0' && c <= '9')
    {
        return static_cast<unsigned>(c - '0');
    }
    if (c >= 'a' && c <= 'f')
    {
        return static_cast<unsigned>(c - 'a' + kHexLetterOffset);
    }
    if (c >= 'A' && c <= 'F')
    {
        return static_cast<unsigned>(c - 'A' + kHexLetterOffset);
    }
    return std::nullopt;
}

/// @brief Decode the escape starting at @p i, if there is a well-formed one.
/// @return the decoded byte, or `nullopt` when @p i is not the start of a
///         complete `%XX` — including a truncated one at the end of @p in.
[[nodiscard]] std::optional<char> DecodeEscapeAt(std::string_view in, std::size_t i) noexcept
{
    if (in[i] != '%' || (i + kEscapeLength) > in.size())
    {
        return std::nullopt;
    }
    const auto high = HexValue(in[i + 1U]);
    const auto low = HexValue(in[i + 2U]);
    if (!high.has_value() || !low.has_value())
    {
        return std::nullopt;
    }
    return static_cast<char>((*high << kNibbleShift) | *low);
}

}  // namespace

std::optional<GrpcStatusInfo> LookupGrpcStatus(int code) noexcept
{
    if (code < 0 || code >= kGrpcStatusCount)
    {
        return std::nullopt;
    }
    return kStatusTable.at(static_cast<std::size_t>(code));
}

std::string PercentDecode(std::string_view in)
{
    std::string out;
    // The decode never grows the input, so one reservation is exact-or-generous
    // and no reallocation happens mid-loop.
    out.reserve(in.size());
    std::size_t i = 0;
    while (i < in.size())
    {
        const auto decoded = DecodeEscapeAt(in, i);
        if (decoded.has_value())
        {
            out.push_back(*decoded);
            i += kEscapeLength;
            continue;
        }
        out.push_back(in[i]);
        ++i;
    }
    return out;
}

std::string FormatGrpcError(int code, std::string_view decoded_message)
{
    const auto info = LookupGrpcStatus(code);
    const std::string_view name = info.has_value() ? info->name : kUnrecognizedName;
    const std::string_view message =
        decoded_message.substr(0, std::min(decoded_message.size(), kMaxGrpcMessageChars));

    std::string out;
    out.reserve(name.size() + message.size() + kFormatOverhead);
    out.append(name);
    out.append(" (");
    out.append(std::to_string(code));
    out.push_back(')');
    if (!message.empty())
    {
        out.append(": ");
        out.append(message);
    }
    return out;
}

}  // namespace microtel::wire
