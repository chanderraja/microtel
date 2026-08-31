// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#include "wire/grpc/grpc_wire_codec.hpp"

#include "microtel/error.hpp"
#include "microtel/internal/encoded_payload.hpp"
#include "microtel/internal/transport.hpp"
#include "microtel/internal/wire_result.hpp"

#include "wire/gzip.hpp"
#include "wire/otlp_response.hpp"

#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <future>
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
// Base64 alphabet position constants (RFC 4648 §5)
// ---------------------------------------------------------------------------

constexpr int kB64AlphaLowerOffset = 26;   // 'a' maps to index 26
constexpr int kB64DigitOffset = 52;        // '0' maps to index 52
constexpr int kB64PlusOrMinus = 62;        // '+' (standard) or '-' (URL-safe)
constexpr int kB64SlashOrUnderscore = 63;  // '/' (standard) or '_' (URL-safe)
constexpr int kB64InvalidChar = -1;

// Base64 decode output reservation: ceil(n * 3 / 4)
constexpr std::size_t kB64OutputBytesPerGroup = 3U;
constexpr std::size_t kB64InputCharsPerGroup = 4U;

// Bit masks for Base64 carry bits between output bytes
constexpr unsigned kB64Mask4Bit = 0xFU;  // 4-bit carry (2nd output byte)
constexpr unsigned kB64Mask2Bit = 0x3U;  // 2-bit carry (3rd output byte)

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

// google.protobuf.Duration
constexpr std::uint32_t kFieldDurationSeconds = 1U;
constexpr std::uint32_t kFieldDurationNanos = 2U;

// google.rpc.RetryInfo
constexpr std::uint32_t kFieldRetryDelay = 1U;

// google.protobuf.Any
constexpr std::uint32_t kFieldAnyTypeUrl = 1U;
constexpr std::uint32_t kFieldAnyValue = 2U;

// google.rpc.Status
constexpr std::uint32_t kFieldStatusDetails = 3U;

// ---------------------------------------------------------------------------
// google.protobuf.Duration conversion factors
// ---------------------------------------------------------------------------

constexpr std::int64_t kMillisPerSecond = 1000;
constexpr std::int32_t kNanosPerMilli = 1'000'000;

// ---------------------------------------------------------------------------
// gRPC DATA frame constants
// ---------------------------------------------------------------------------

constexpr std::size_t kGrpcFrameHeaderSize = 5U;
constexpr std::uint8_t kGrpcUncompressedFlag = 0x00U;
constexpr std::uint8_t kGrpcCompressedFlag = 0x01U;
constexpr std::uint8_t kByteMask = 0xFFU;
constexpr unsigned kByteShift24 = 24U;
constexpr unsigned kByteShift16 = 16U;
constexpr unsigned kByteShift8 = 8U;

// Sentinel for a gRPC status that could not be parsed from headers
constexpr int kGrpcStatusUnparsed = -1;

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
        return c - 'a' + kB64AlphaLowerOffset;
    }
    if (c >= '0' && c <= '9')
    {
        return c - '0' + kB64DigitOffset;
    }
    if (c == '-' || c == '+')
    {
        return kB64PlusOrMinus;
    }
    if (c == '_' || c == '/')
    {
        return kB64SlashOrUnderscore;
    }
    return kB64InvalidChar;
}

[[nodiscard]] std::optional<std::vector<std::uint8_t>> Base64UrlDecode(std::string_view sv)
{
    while (!sv.empty() && sv.back() == '=')
    {
        sv.remove_suffix(1);
    }
    std::vector<std::uint8_t> out;
    out.reserve(((sv.size() * kB64OutputBytesPerGroup) + (kB64InputCharsPerGroup - 1U)) /
                kB64InputCharsPerGroup);
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
        out.push_back(static_cast<std::uint8_t>(((static_cast<unsigned>(c1) & kB64Mask4Bit) << 4U) |
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
        out.push_back(static_cast<std::uint8_t>(((static_cast<unsigned>(c2) & kB64Mask2Bit) << 6U) |
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

// Handles one field in a google.protobuf.Duration message.
[[nodiscard]] bool ParseDurationField(
    ByteSpan& data, std::uint32_t fn, std::uint32_t wt, std::int64_t& seconds, std::int32_t& nanos)
{
    if (fn == kFieldDurationSeconds && wt == 0U)
    {
        const auto v = ReadVarint(data);
        if (!v.has_value())
        {
            return false;
        }
        seconds = static_cast<std::int64_t>(*v);
        return true;
    }
    if (fn == kFieldDurationNanos && wt == 0U)
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
    return std::chrono::milliseconds{(seconds * kMillisPerSecond) + (nanos / kNanosPerMilli)};
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
        if (fn == kFieldRetryDelay && wt == 2U)
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
    if (fn == kFieldAnyTypeUrl && wt == 2U)
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
    if (fn == kFieldAnyValue && wt == 2U)
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
    if (fn == kFieldStatusDetails && wt == 2U)
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
            // Transport-level failure: connection reset, refused, read timeout.
            // Retryable, matching HttpWireCodec and this codec's own
            // EnsureConnected path (ICP 0017) — these are the transient
            // conditions the retry engine exists for. Returning false here
            // meant a gRPC deployment dropped a batch on the first collector
            // restart that HTTP would have delivered.
            .retryable = true,
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
    int code = kGrpcStatusUnparsed;
    const auto* const p = status_sv->data();
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
    (void)std::from_chars(p, p + status_sv->size(), code);
    return ClassifyGrpcCode(code, tr);
}

// ---------------------------------------------------------------------------
// gRPC frame builder — 5-byte length prefix (compression flag + BE uint32)
// ---------------------------------------------------------------------------

[[nodiscard]] std::vector<std::byte> FramePayload(std::span<const std::byte> body, bool compressed)
{
    const std::size_t n = body.size();
    // GCC 15 false-positive -Wfree-nonheap-object fires on reserve+push_back
    // when inlined; size-constructor + index assignment avoids that analysis path.
    std::vector<std::byte> framed(kGrpcFrameHeaderSize + n);
    framed[0] = std::byte{compressed ? kGrpcCompressedFlag : kGrpcUncompressedFlag};
    framed[1] = std::byte{static_cast<std::uint8_t>((n >> kByteShift24) & kByteMask)};
    framed[2] = std::byte{static_cast<std::uint8_t>((n >> kByteShift16) & kByteMask)};
    framed[3] = std::byte{static_cast<std::uint8_t>((n >> kByteShift8) & kByteMask)};
    framed[4] = std::byte{static_cast<std::uint8_t>(n & kByteMask)};
    std::ranges::copy(body, framed.begin() + kGrpcFrameHeaderSize);
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
                             internal::ISteadyClock* clock,
                             internal::ConnectOptions connect_opts) noexcept
    : m_transport(transport),
      m_config(std::move(config)),
      m_auth(auth),
      m_diag(diag),
      m_clock(clock),
      m_connect_opts(std::move(connect_opts))
{
}

std::vector<internal::HeaderField> GrpcWireCodec::BuildHeaders(bool compressed) const
{
    std::vector<internal::HeaderField> headers;
    headers.push_back({.name = ":method", .value = "POST"});
    headers.push_back({.name = ":scheme", .value = m_config.scheme});
    headers.push_back({.name = ":authority", .value = m_config.host});
    const std::string path =
        m_config.service_path.empty() ? std::string{kGrpcTracesPath} : m_config.service_path;
    headers.push_back({.name = ":path", .value = path});
    headers.push_back({.name = "te", .value = "trailers"});
    headers.push_back({.name = "content-type", .value = "application/grpc+proto"});
    headers.push_back({.name = "user-agent", .value = "microtel-cpp/0.1.0"});
    if (compressed)
    {
        headers.push_back({.name = "grpc-encoding", .value = "gzip"});
    }
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

std::optional<internal::WireResult> GrpcWireCodec::EnsureConnected()
{
    if (m_transport->GetState() == ConnectionState::Connected)
    {
        return std::nullopt;
    }
    auto connected = m_transport->Connect(m_connect_opts);
    if (!connected)
    {
        return internal::WireResult{
            .success = false,
            .retryable = true,  // failed connect: same shape as any other transport failure
            .retry_after = {},
            .partial_success_rejected = 0,
            .error = connected.error(),
            .response_excerpt = {},
        };
    }
    return std::nullopt;
}

internal::WireResult GrpcWireCodec::Send(internal::EncodedPayload&& payload,
                                         std::chrono::milliseconds deadline)
{
    if (auto failure = EnsureConnected())
    {
        return std::move(*failure);
    }

    const internal::EncodedPayload owned = std::move(payload);
    std::vector<std::byte> compressed;
    std::span<const std::byte> body = owned.Bytes();
    bool did_compress = false;
    if (m_config.compression_gzip)
    {
        // On failure fall back to the uncompressed body rather than dropping
        // the batch. gRPC's compression flag is per-message, so an
        // uncompressed message is legal even with `grpc-encoding` negotiated
        // — but only if the flag and the header agree with what was actually
        // done, which is why `did_compress` drives both below.
        auto result = GzipCompress(body);
        if (result)
        {
            compressed = std::move(*result);
            body = compressed;
            did_compress = true;
        }
    }
    const auto framed = FramePayload(body, did_compress);
    auto headers = BuildHeaders(did_compress);
    AppendAuthHeader(headers);
    internal::RequestSpec spec{
        .headers = std::move(headers),
        .payload = std::span<const std::byte>{framed.data(), framed.size()},
        .deadline = deadline,
    };
    auto handle = m_transport->Send(std::move(spec));
    auto& fut = handle.Future();

    // Bounded, matching HttpWireCodec::Send. This was an unbounded get(),
    // which blocked forever whenever a promise went unfulfilled — as the
    // mid-connection drop path did until ICP 0018. That path now fulfils, so
    // this is defence in depth: a future bug that abandons a promise costs one
    // deadline, not a permanently wedged exporter thread.
    if (fut.wait_for(deadline) != std::future_status::ready)
    {
        m_transport->Cancel(handle);
        return internal::WireResult{
            .success = false,
            .retryable = true,
            .retry_after = {},
            .partial_success_rejected = 0,
            .error = microtel::Error{.kind = microtel::Error::Kind::Cancelled,
                                     .message = "request deadline exceeded"},
            .response_excerpt = {},
        };
    }

    const auto tr = fut.get();
    return ClassifyResponse(tr);
}

}  // namespace microtel::wire
