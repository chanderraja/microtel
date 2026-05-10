// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#include "wire/grpc/grpc_wire_codec.hpp"

#include "microtel/error.hpp"
#include "microtel/internal/encoded_payload.hpp"
#include "microtel/internal/transport.hpp"
#include "microtel/internal/wire_result.hpp"

#include "wire/otlp_response.hpp"

#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace microtel::wire
{

namespace
{

constexpr std::string_view kGrpcTracesPath =
    "/opentelemetry.proto.collector.trace.v1.TraceService/Export";
constexpr std::string_view kRetryInfoTypeUrl = "type.googleapis.com/google.rpc.RetryInfo";

// ---------------------------------------------------------------------------
// Base64 URL-safe decode (RFC 4648 §5, no padding required)
// ---------------------------------------------------------------------------

[[nodiscard]] int B64CharValue(char c) noexcept
{
    if (c >= 'A' && c <= 'Z')
    {
        return c - 'A';
    }
    if (c >= 'a' && c <= 'z')
    {
        return c - 'a' + 26;
    }
    if (c >= '0' && c <= '9')
    {
        return c - '0' + 52;
    }
    if (c == '-' || c == '+')
    {
        return 62;
    }
    if (c == '_' || c == '/')
    {
        return 63;
    }
    return -1;
}

[[nodiscard]] std::optional<std::vector<std::uint8_t>> Base64UrlDecode(std::string_view sv)
{
    while (!sv.empty() && sv.back() == '=')
    {
        sv.remove_suffix(1);
    }
    std::vector<std::uint8_t> out;
    out.reserve(((sv.size() * 3U) + 3U) / 4U);
    std::size_t i = 0;
    while (i + 1U < sv.size())
    {
        const int c0 = B64CharValue(sv[i]);
        const int c1 = B64CharValue(sv[i + 1U]);
        if (c0 < 0 || c1 < 0)
        {
            return std::nullopt;
        }
        out.push_back(static_cast<std::uint8_t>((static_cast<unsigned>(c0) << 2U) |
                                                (static_cast<unsigned>(c1) >> 4U)));
        i += 2U;
        if (i >= sv.size())
        {
            continue;
        }
        const int c2 = B64CharValue(sv[i]);
        if (c2 < 0)
        {
            return std::nullopt;
        }
        out.push_back(static_cast<std::uint8_t>(((static_cast<unsigned>(c1) & 0xFU) << 4U) |
                                                (static_cast<unsigned>(c2) >> 2U)));
        ++i;
        if (i >= sv.size())
        {
            continue;
        }
        const int c3 = B64CharValue(sv[i]);
        if (c3 < 0)
        {
            return std::nullopt;
        }
        out.push_back(static_cast<std::uint8_t>(((static_cast<unsigned>(c2) & 0x3U) << 6U) |
                                                static_cast<unsigned>(c3)));
        ++i;
    }
    return out;
}

// ---------------------------------------------------------------------------
// Minimal proto wire-format reader
// ---------------------------------------------------------------------------

using ByteSpan = std::span<const std::uint8_t>;

[[nodiscard]] std::optional<std::uint64_t> ReadVarint(ByteSpan& buf)
{
    std::uint64_t result = 0;
    unsigned shift = 0U;
    while (!buf.empty())
    {
        const std::uint8_t b = buf.front();
        buf = buf.subspan(1);
        result |= static_cast<std::uint64_t>(b & 0x7FU) << shift;
        if ((b & 0x80U) == 0U)
        {
            return result;
        }
        shift += 7U;
        if (shift >= 64U)
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
        if (buf.size() < 8U)
        {
            return false;
        }
        buf = buf.subspan(8U);
        return true;
    }
    if (wire_type == kWt32Bit)
    {
        if (buf.size() < 4U)
        {
            return false;
        }
        buf = buf.subspan(4U);
        return true;
    }
    return false;
}

// Handles one field in a google.protobuf.Duration message.
[[nodiscard]] bool ParseDurationField(
    ByteSpan& data, std::uint32_t fn, std::uint32_t wt, std::int64_t& seconds, std::int32_t& nanos)
{
    if (fn == 1U && wt == 0U)
    {
        const auto v = ReadVarint(data);
        if (!v.has_value())
        {
            return false;
        }
        seconds = static_cast<std::int64_t>(*v);
        return true;
    }
    if (fn == 2U && wt == 0U)
    {
        const auto v = ReadVarint(data);
        if (!v.has_value())
        {
            return false;
        }
        nanos = static_cast<std::int32_t>(*v);
        return true;
    }
    return SkipField(data, wt);
}

// Parses google.protobuf.Duration → chrono::milliseconds.
[[nodiscard]] std::optional<std::chrono::milliseconds> ParseDurationMs(ByteSpan data)
{
    std::int64_t seconds = 0;
    std::int32_t nanos = 0;
    while (!data.empty())
    {
        const auto tag = ReadVarint(data);
        if (!tag.has_value())
        {
            return std::nullopt;
        }
        const auto fn = static_cast<std::uint32_t>(*tag >> 3U);
        const auto wt = static_cast<std::uint32_t>(*tag & 0x7U);
        if (!ParseDurationField(data, fn, wt, seconds, nanos))
        {
            return std::nullopt;
        }
    }
    return std::chrono::milliseconds{(seconds * 1000) + (nanos / 1'000'000)};
}

// Reads the retry_delay field (field 1, wire type 2) from a RetryInfo message.
[[nodiscard]] std::optional<std::chrono::milliseconds> ReadRetryDelayField(ByteSpan& data)
{
    const auto delay_bytes = ReadLenDelim(data);
    if (!delay_bytes.has_value())
    {
        return std::nullopt;
    }
    return ParseDurationMs(*delay_bytes);
}

// Parses google.rpc.RetryInfo → retry_delay as chrono::milliseconds.
[[nodiscard]] std::optional<std::chrono::milliseconds> ParseRetryInfoMs(ByteSpan data)
{
    while (!data.empty())
    {
        const auto tag = ReadVarint(data);
        if (!tag.has_value())
        {
            return std::nullopt;
        }
        const auto fn = static_cast<std::uint32_t>(*tag >> 3U);
        const auto wt = static_cast<std::uint32_t>(*tag & 0x7U);
        if (fn == 1U && wt == 2U)
        {
            return ReadRetryDelayField(data);
        }
        if (!SkipField(data, wt))
        {
            return std::nullopt;
        }
    }
    return std::nullopt;
}

// Handles one field in a google.protobuf.Any message.
[[nodiscard]] bool ParseAnyField(ByteSpan& data,
                                 std::uint32_t fn,
                                 std::uint32_t wt,
                                 std::string& type_url,
                                 std::optional<ByteSpan>& value)
{
    if (fn == 1U && wt == 2U)
    {
        const auto bytes = ReadLenDelim(data);
        if (!bytes.has_value())
        {
            return false;
        }
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        type_url.assign(reinterpret_cast<const char*>(bytes->data()), bytes->size());
        return true;
    }
    if (fn == 2U && wt == 2U)
    {
        value = ReadLenDelim(data);
        return value.has_value();
    }
    return SkipField(data, wt);
}

// Parses one google.protobuf.Any and returns {type_url, value bytes}.
[[nodiscard]] std::optional<std::pair<std::string, ByteSpan>> ParseAny(ByteSpan data)
{
    std::string type_url;
    std::optional<ByteSpan> value;
    while (!data.empty())
    {
        const auto tag = ReadVarint(data);
        if (!tag.has_value())
        {
            return std::nullopt;
        }
        const auto fn = static_cast<std::uint32_t>(*tag >> 3U);
        const auto wt = static_cast<std::uint32_t>(*tag & 0x7U);
        if (!ParseAnyField(data, fn, wt, type_url, value))
        {
            return std::nullopt;
        }
    }
    if (!value.has_value())
    {
        return std::nullopt;
    }
    return std::pair{std::move(type_url), *value};
}

// Tries to extract RetryInfo from one google.protobuf.Any field.
[[nodiscard]] std::optional<std::chrono::milliseconds> TryParseRetryInfoFromAny(ByteSpan any_bytes)
{
    auto any = ParseAny(any_bytes);
    if (!any.has_value() || any->first != kRetryInfoTypeUrl)
    {
        return std::nullopt;
    }
    return ParseRetryInfoMs(any->second);
}

struct RetrySearchSignal
{
    bool stop = false;
    std::optional<std::chrono::milliseconds> delay;
};

// Processes one field from google.rpc.Status: returns {stop=true} when the search
// should end (RetryInfo found, or unrecoverable parse error), {stop=false} to continue.
[[nodiscard]] RetrySearchSignal SearchStatusField(ByteSpan& data,
                                                  std::uint32_t fn,
                                                  std::uint32_t wt)
{
    if (fn == 3U && wt == 2U)
    {
        const auto any_bytes = ReadLenDelim(data);
        if (!any_bytes.has_value())
        {
            return RetrySearchSignal{.stop = true, .delay = {}};
        }
        const auto result = TryParseRetryInfoFromAny(*any_bytes);
        return RetrySearchSignal{.stop = result.has_value(), .delay = result};
    }
    return RetrySearchSignal{.stop = !SkipField(data, wt), .delay = {}};
}

// Walks google.rpc.Status.details[] for a RetryInfo entry.
[[nodiscard]] std::optional<std::chrono::milliseconds> ParseStatusForRetryInfo(ByteSpan data)
{
    while (!data.empty())
    {
        const auto tag = ReadVarint(data);
        if (!tag.has_value())
        {
            return std::nullopt;
        }
        const auto fn = static_cast<std::uint32_t>(*tag >> 3U);
        const auto wt = static_cast<std::uint32_t>(*tag & 0x7U);
        const auto signal = SearchStatusField(data, fn, wt);
        if (signal.stop)
        {
            return signal.delay;
        }
    }
    return std::nullopt;
}

// Decodes grpc-status-details-bin (base64) and extracts RetryInfo delay.
[[nodiscard]] std::optional<std::chrono::milliseconds> TryDecodeRetryDelay(
    std::string_view details_bin)
{
    const auto bytes = Base64UrlDecode(details_bin);
    if (!bytes.has_value() || bytes->empty())
    {
        return std::nullopt;
    }
    const ByteSpan span{bytes->data(), bytes->size()};
    return ParseStatusForRetryInfo(span);
}

// ---------------------------------------------------------------------------
// Header utilities
// ---------------------------------------------------------------------------

[[nodiscard]] std::optional<std::string_view> FindHeaderValue(
    const std::vector<internal::HeaderField>& headers, std::string_view name)
{
    for (const auto& h : headers)
    {
        if (h.name == name)
        {
            return std::string_view{h.value};
        }
    }
    return std::nullopt;
}

[[nodiscard]] int ParseHttpStatus(const std::vector<internal::HeaderField>& headers) noexcept
{
    for (const auto& h : headers)
    {
        if (h.name != ":status")
        {
            continue;
        }
        int code = 0;
        const auto* const p = h.value.data();
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
        if (std::from_chars(p, p + h.value.size(), code).ec == std::errc{})
        {
            return code;
        }
    }
    return 0;
}

// ---------------------------------------------------------------------------
// gRPC status classification
// ---------------------------------------------------------------------------

[[nodiscard]] internal::WireResult ClassifyMissingGrpcStatus(
    const std::vector<internal::HeaderField>& headers)
{
    const int http_status = ParseHttpStatus(headers);
    const bool retryable =
        (http_status == 429 || http_status == 502 || http_status == 503 || http_status == 504);
    return internal::WireResult{
        .success = false,
        .retryable = retryable,
        .retry_after = {},
        .partial_success_rejected = 0,
        .error = microtel::Error{.kind = microtel::Error::Kind::Protocol,
                                 .message = "missing grpc-status"},
        .response_excerpt = {},
    };
}

[[nodiscard]] internal::WireResult ClassifyResourceExhausted(const internal::TransportResult& tr)
{
    auto details = FindHeaderValue(tr.response_trailers, "grpc-status-details-bin");
    if (!details.has_value())
    {
        details = FindHeaderValue(tr.response_headers, "grpc-status-details-bin");
    }
    if (!details.has_value())
    {
        return internal::WireResult{
            .success = false,
            .retryable = false,
            .retry_after = {},
            .partial_success_rejected = 0,
            .error = microtel::Error{.kind = microtel::Error::Kind::Protocol,
                                     .message = "RESOURCE_EXHAUSTED without RetryInfo"},
            .response_excerpt = {},
        };
    }
    const auto delay = TryDecodeRetryDelay(*details);
    if (!delay.has_value())
    {
        return internal::WireResult{
            .success = false,
            .retryable = false,
            .retry_after = {},
            .partial_success_rejected = 0,
            .error = microtel::Error{.kind = microtel::Error::Kind::Protocol,
                                     .message = "RESOURCE_EXHAUSTED without RetryInfo"},
            .response_excerpt = {},
        };
    }
    return internal::WireResult{
        .success = false,
        .retryable = true,
        .retry_after = delay,
        .partial_success_rejected = 0,
        .error = {},
        .response_excerpt = {},
    };
}

[[nodiscard]] internal::WireResult ClassifyGrpcCode(int code, const internal::TransportResult& tr)
{
    if (code == 0)
    {
        constexpr std::size_t kGrpcFrameHeaderSize = 5U;
        std::uint32_t rejected = 0;
        if (tr.response_body.size() >= kGrpcFrameHeaderSize)
        {
            const auto body =
                std::span<const std::byte>{tr.response_body}.subspan(kGrpcFrameHeaderSize);
            rejected = ParseRejectedSpans(body);
        }
        return internal::WireResult{
            .success = true,
            .retryable = false,
            .retry_after = {},
            .partial_success_rejected = rejected,
            .error = {},
            .response_excerpt = {},
        };
    }
    constexpr int kCancelled = 1;
    constexpr int kDeadlineExceeded = 4;
    constexpr int kResourceExhausted = 8;
    constexpr int kAborted = 10;
    constexpr int kOutOfRange = 11;
    constexpr int kUnavailable = 14;
    constexpr int kDataLoss = 15;
    if (code == kResourceExhausted)
    {
        return ClassifyResourceExhausted(tr);
    }
    const bool retryable = (code == kCancelled || code == kDeadlineExceeded || code == kAborted ||
                            code == kOutOfRange || code == kUnavailable || code == kDataLoss);
    return internal::WireResult{
        .success = false,
        .retryable = retryable,
        .retry_after = {},
        .partial_success_rejected = 0,
        .error = microtel::Error{.kind = microtel::Error::Kind::Protocol, .message = "grpc error"},
        .response_excerpt = {},
    };
}

[[nodiscard]] internal::WireResult ClassifyResponse(const internal::TransportResult& tr)
{
    if (tr.error.has_value())
    {
        return internal::WireResult{
            .success = false,
            .retryable = false,
            .retry_after = {},
            .partial_success_rejected = 0,
            .error = tr.error,
            .response_excerpt = {},
        };
    }
    auto status_sv = FindHeaderValue(tr.response_trailers, "grpc-status");
    if (!status_sv.has_value())
    {
        status_sv = FindHeaderValue(tr.response_headers, "grpc-status");
    }
    if (!status_sv.has_value())
    {
        return ClassifyMissingGrpcStatus(tr.response_headers);
    }
    int code = -1;
    const auto* const p = status_sv->data();
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    (void)std::from_chars(p, p + status_sv->size(), code);
    return ClassifyGrpcCode(code, tr);
}

// ---------------------------------------------------------------------------
// gRPC frame builder — 5-byte length prefix (compression flag + BE uint32)
// ---------------------------------------------------------------------------

[[nodiscard]] std::vector<std::byte> FramePayload(const internal::EncodedPayload& payload)
{
    const std::size_t n = payload.Size();
    std::vector<std::byte> framed;
    framed.reserve(5U + n);
    framed.push_back(std::byte{0x00U});  // compression flag: uncompressed
    framed.push_back(std::byte{static_cast<std::uint8_t>((n >> 24U) & 0xFFU)});
    framed.push_back(std::byte{static_cast<std::uint8_t>((n >> 16U) & 0xFFU)});
    framed.push_back(std::byte{static_cast<std::uint8_t>((n >> 8U) & 0xFFU)});
    framed.push_back(std::byte{static_cast<std::uint8_t>(n & 0xFFU)});
    for (const auto b : payload.Bytes())
    {
        framed.push_back(b);
    }
    return framed;
}

}  // namespace

// ---------------------------------------------------------------------------
// GrpcWireCodec
// ---------------------------------------------------------------------------

GrpcWireCodec::GrpcWireCodec(internal::ITransport* transport,
                             GrpcWireCodecConfig config,
                             internal::IAuthProvider* auth,
                             internal::IDiagnosticsSink* diag,
                             internal::ISteadyClock* clock) noexcept
    : m_transport(transport),
      m_config(std::move(config)),
      m_auth(auth),
      m_diag(diag),
      m_clock(clock)
{
}

std::vector<internal::HeaderField> GrpcWireCodec::BuildHeaders() const
{
    std::vector<internal::HeaderField> headers;
    headers.push_back({.name = ":method", .value = "POST"});
    headers.push_back({.name = ":scheme", .value = m_config.scheme});
    headers.push_back({.name = ":authority", .value = m_config.host});
    headers.push_back({.name = ":path", .value = std::string{kGrpcTracesPath}});
    headers.push_back({.name = "te", .value = "trailers"});
    headers.push_back({.name = "content-type", .value = "application/grpc+proto"});
    headers.push_back({.name = "user-agent", .value = "microtel-cpp/0.1.0"});
    for (const auto& h : m_config.extra_headers)
    {
        headers.push_back(h);
    }
    return headers;
}

void GrpcWireCodec::AppendAuthHeader(std::vector<internal::HeaderField>& headers) const
{
    if (m_auth == nullptr)
    {
        return;
    }
    const auto now = (m_clock != nullptr) ? m_clock->Now() : std::chrono::steady_clock::now();
    const auto auth_result = m_auth->GetAuthorization(now);
    if (!auth_result.has_value())
    {
        return;
    }
    const auto& token_opt = auth_result.value();
    if (!token_opt.has_value())
    {
        return;
    }
    headers.push_back({.name = "authorization", .value = token_opt.value()});
}

internal::WireResult GrpcWireCodec::Send(internal::EncodedPayload&& payload,
                                         std::chrono::milliseconds deadline)
{
    const internal::EncodedPayload owned = std::move(payload);
    const auto framed = FramePayload(owned);
    auto headers = BuildHeaders();
    AppendAuthHeader(headers);
    internal::RequestSpec spec{
        .headers = std::move(headers),
        .payload = std::span<const std::byte>{framed.data(), framed.size()},
        .deadline = deadline,
    };
    auto handle = m_transport->Send(std::move(spec));
    const auto tr = handle.Future().get();
    return ClassifyResponse(tr);
}

}  // namespace microtel::wire
