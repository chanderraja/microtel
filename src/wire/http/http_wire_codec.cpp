// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#include "wire/http/http_wire_codec.hpp"

#include "microtel/error.hpp"
#include "microtel/internal/encoded_payload.hpp"
#include "microtel/internal/transport.hpp"
#include "microtel/internal/wire_result.hpp"

#include "wire/otlp_response.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace microtel::wire
{

namespace
{

constexpr std::string_view kV1TracesPath = "/v1/traces";

// HTTP status code ranges and retryable codes (error-model.md §7.1)
constexpr int kHttpSuccessMin = 200;
constexpr int kHttpSuccessMax = 300;
constexpr int kHttpTooManyRequests = 429;
constexpr int kHttpBadGateway = 502;
constexpr int kHttpServiceUnavailable = 503;
constexpr int kHttpGatewayTimeout = 504;

// Number of built-in request headers added before any extra headers
constexpr std::size_t kBuiltInHeaderCount = 6U;

/// @brief Parse an integer-form `Retry-After` value (seconds).
///
/// Handles only the integer form. HTTP-date form is not parsed in v1
/// (treated as absent). Returns `nullopt` on parse failure.
[[nodiscard]] std::optional<std::chrono::milliseconds> ParseRetryAfter(
    const std::vector<internal::HeaderField>& headers) noexcept
{
    for (const auto& h : headers)
    {
        if (h.name != "retry-after")
        {
            continue;
        }
        std::uint32_t secs = 0;
        const auto* begin = h.value.data();
        const auto* end = begin + h.value.size();
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
        const auto [ptr, ec] = std::from_chars(begin, end, secs);
        if (ec == std::errc{} && ptr == end)
        {
            return std::chrono::milliseconds(static_cast<std::int64_t>(secs) * 1000);
        }
        return std::nullopt;
    }
    return std::nullopt;
}

/// @brief Extract the numeric HTTP status code from response headers.
///
/// Returns 0 if the `:status` header is absent or not a valid integer.
[[nodiscard]] int ParseStatusCode(const std::vector<internal::HeaderField>& headers) noexcept
{
    for (const auto& h : headers)
    {
        if (h.name != ":status")
        {
            continue;
        }
        int code = 0;
        const auto* begin = h.value.data();
        const auto* end = begin + h.value.size();
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
        const auto [ptr, ec] = std::from_chars(begin, end, code);
        if (ec == std::errc{})
        {
            return code;
        }
        return 0;
    }
    return 0;
}

/// @brief Classify an HTTP status code per error-model.md §7.1.
[[nodiscard]] internal::WireResult ClassifyStatus(
    int code, const std::vector<internal::HeaderField>& headers) noexcept
{
    if (code >= kHttpSuccessMin && code < kHttpSuccessMax)
    {
        return {.success = true, .retry_after = {}, .error = {}, .response_excerpt = {}};
    }
    if (code == kHttpTooManyRequests || code == kHttpBadGateway ||
        code == kHttpServiceUnavailable || code == kHttpGatewayTimeout)
    {
        return {
            .success = false,
            .retryable = true,
            .retry_after = ParseRetryAfter(headers),
            .error = {},
            .response_excerpt = {},
        };
    }
    // All other 4xx and 5xx are non-retryable.
    return {
        .success = false,
        .retryable = false,
        .retry_after = {},
        .error = Error{.kind = Error::Kind::Protocol, .message = "HTTP " + std::to_string(code)},
        .response_excerpt = {},
    };
}

}  // namespace

HttpWireCodec::HttpWireCodec(internal::ITransport* transport,
                             HttpWireCodecConfig config,
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

std::string HttpWireCodec::ResolvePath() const noexcept
{
    if (m_config.path.empty() || m_config.path == "/")
    {
        return std::string{kV1TracesPath};
    }
    return m_config.path + std::string{kV1TracesPath};
}

std::vector<internal::HeaderField> HttpWireCodec::BuildHeaders(
    std::size_t content_length) const noexcept
{
    std::vector<internal::HeaderField> headers;
    headers.reserve(kBuiltInHeaderCount + m_config.extra_headers.size());

    headers.push_back({.name = ":method", .value = "POST"});
    headers.push_back({.name = ":scheme", .value = m_config.scheme});
    headers.push_back({.name = ":path", .value = ResolvePath()});
    headers.push_back({.name = ":authority", .value = m_config.host});
    headers.push_back({.name = "content-type", .value = "application/x-protobuf"});
    headers.push_back({.name = "content-length", .value = std::to_string(content_length)});

    for (const auto& h : m_config.extra_headers)
    {
        headers.push_back(h);
    }

    return headers;
}

void HttpWireCodec::AppendAuthHeader(std::vector<internal::HeaderField>& headers) const
{
    if (m_auth == nullptr)
    {
        return;
    }
    const auto now = (m_clock != nullptr)
                         ? m_clock->Now()
                         : internal::TimePointSteady{std::chrono::steady_clock::now()};
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

std::string HttpWireCodec::BuildExcerpt(const std::vector<std::byte>& body)
{
    if (body.empty())
    {
        return {};
    }
    constexpr std::size_t kMaxExcerpt = 256;
    const std::size_t n = std::min(body.size(), kMaxExcerpt);
    std::string excerpt(n, '\0');
    for (std::size_t i = 0; i < n; ++i)
    {
        excerpt[i] = static_cast<char>(body[i]);
    }
    return excerpt;
}

internal::WireResult HttpWireCodec::Send(internal::EncodedPayload&& payload,
                                         std::chrono::milliseconds deadline)
{
    const internal::EncodedPayload owned = std::move(payload);
    auto headers = BuildHeaders(owned.Size());
    AppendAuthHeader(headers);

    internal::RequestSpec spec{
        .headers = std::move(headers),
        .payload = owned.Bytes(),
        .deadline = deadline,
    };

    auto handle = m_transport->Send(std::move(spec));
    auto& fut = handle.Future();

    const auto status = fut.wait_for(deadline);
    if (status != std::future_status::ready)
    {
        m_transport->Cancel(handle);
        return {
            .success = false,
            .retryable = true,
            .retry_after = {},
            .error = Error{.kind = Error::Kind::Cancelled, .message = "request deadline exceeded"},
            .response_excerpt = {},
        };
    }

    auto result = fut.get();
    if (!result.success)
    {
        return {
            .success = false,
            .retryable = true,  // transport-level failure: connection reset, etc.
            .retry_after = {},
            .error = result.error,
            .response_excerpt = {},
        };
    }

    const int code = ParseStatusCode(result.response_headers);
    auto wire = ClassifyStatus(code, result.response_headers);
    if (wire.success && !result.response_body.empty())
    {
        wire.partial_success_rejected =
            ParseRejectedSpans(std::span<const std::byte>{result.response_body});
    }
    wire.response_excerpt = BuildExcerpt(result.response_body);
    return wire;
}

internal::WireResult HttpWireCodec::CollectOneResult(
    InFlight& item, std::chrono::steady_clock::time_point deadline_point)
{
    const auto remaining = deadline_point - std::chrono::steady_clock::now();
    const auto wait_ms = (remaining > std::chrono::nanoseconds{0})
                             ? std::chrono::duration_cast<std::chrono::milliseconds>(remaining)
                             : std::chrono::milliseconds{0};

    const auto status = item.handle.Future().wait_for(wait_ms);
    if (status != std::future_status::ready)
    {
        m_transport->Cancel(item.handle);
        return {
            .success = false,
            .retryable = true,
            .retry_after = {},
            .error = Error{.kind = Error::Kind::Cancelled, .message = "request deadline exceeded"},
            .response_excerpt = {},
        };
    }

    auto result = item.handle.Future().get();
    if (!result.success)
    {
        return {
            .success = false,
            .retryable = true,
            .retry_after = {},
            .error = result.error,
            .response_excerpt = {},
        };
    }

    const int code = ParseStatusCode(result.response_headers);
    auto wire = ClassifyStatus(code, result.response_headers);
    if (wire.success && !result.response_body.empty())
    {
        wire.partial_success_rejected =
            ParseRejectedSpans(std::span<const std::byte>{result.response_body});
    }
    wire.response_excerpt = BuildExcerpt(result.response_body);
    return wire;
}

std::vector<internal::WireResult> HttpWireCodec::SendAll(
    std::vector<internal::EncodedPayload> payloads, std::chrono::milliseconds deadline)
{
    if (payloads.empty())
    {
        return {};
    }

    // Keeping each EncodedPayload alive alongside its handle is required:
    // RequestSpec::payload is a span borrowing from it; the I/O thread reads
    // those bytes until the stream closes.
    std::vector<InFlight> in_flight;
    in_flight.reserve(payloads.size());
    const auto deadline_point = std::chrono::steady_clock::now() + deadline;

    // Submit all requests without waiting — each opens a separate HTTP/2 stream.
    for (auto& payload : payloads)
    {
        auto headers = BuildHeaders(payload.Size());
        AppendAuthHeader(headers);
        internal::RequestSpec spec{
            .headers = std::move(headers),
            .payload = payload.Bytes(),
            .deadline = deadline,
        };
        auto handle = m_transport->Send(std::move(spec));
        in_flight.push_back(InFlight{.payload = std::move(payload), .handle = std::move(handle)});
    }

    std::vector<internal::WireResult> results;
    results.reserve(in_flight.size());
    for (auto& item : in_flight)
    {
        results.push_back(CollectOneResult(item, deadline_point));
    }
    return results;
}

}  // namespace microtel::wire
