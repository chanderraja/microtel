// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/internal/auth_provider.hpp"
#include "microtel/internal/clock.hpp"
#include "microtel/internal/diagnostics_sink.hpp"
#include "microtel/internal/transport.hpp"
#include "microtel/internal/wire_codec.hpp"

#include <chrono>
#include <string>
#include <vector>

namespace microtel::wire
{

/// @brief Configuration for `HttpWireCodec`.
struct HttpWireCodecConfig
{
    std::string host;    ///< value for the `:authority` header (e.g. "host:4318")
    std::string scheme;  ///< "http" or "https"
    /// @brief URL base path. Empty string or "/" resolves to `/v1/traces`.
    /// Any other value is used as a prefix: `<path>/v1/traces`.
    std::string path;
    std::vector<internal::HeaderField> extra_headers;  ///< forwarded verbatim
};

/// @brief OTLP/HTTP-protobuf implementation of `IWireCodec`.
///
/// Builds an HTTP/2 POST request from the encoded payload, submits it via the
/// injected `ITransport`, waits for the response, and classifies the HTTP
/// status code per the matrix in `docs/error-model.md` §7.1.
///
/// **Dependencies (all non-owning):**
/// - `ITransport` — required; must be Connected before first `Send` call.
/// - `IAuthProvider` — optional; if non-null, the `Authorization` header is
///   populated on every request.
/// - `IDiagnosticsSink` — optional; if non-null, non-retryable failures are
///   counted.
/// - `ISteadyClock` — optional; if non-null, passed to `IAuthProvider::Get-
///   Authorization` for TTL arithmetic. Falls back to `steady_clock::now()`.
///
/// @threadsafety Not thread-safe — single caller (exporter worker).
/// @see docs/error-model.md §7.1
class HttpWireCodec final : public internal::IWireCodec
{
public:
    explicit HttpWireCodec(internal::ITransport* transport,
                           HttpWireCodecConfig config,
                           internal::IAuthProvider* auth = nullptr,
                           internal::IDiagnosticsSink* diag = nullptr,
                           internal::ISteadyClock* clock = nullptr) noexcept;

    ~HttpWireCodec() noexcept override = default;

    HttpWireCodec(const HttpWireCodec&) = delete;
    HttpWireCodec& operator=(const HttpWireCodec&) = delete;
    HttpWireCodec(HttpWireCodec&&) = delete;
    HttpWireCodec& operator=(HttpWireCodec&&) = delete;

    [[nodiscard]] internal::WireResult Send(internal::EncodedPayload&& payload,
                                            std::chrono::milliseconds deadline) override;

    [[nodiscard]] std::vector<internal::WireResult> SendAll(
        std::vector<internal::EncodedPayload> payloads,
        std::chrono::milliseconds deadline) override;

private:
    struct InFlight
    {
        internal::EncodedPayload payload;
        internal::RequestHandle handle;
    };

    [[nodiscard]] std::string ResolvePath() const noexcept;
    [[nodiscard]] std::vector<internal::HeaderField> BuildHeaders(
        std::size_t content_length) const noexcept;
    void AppendAuthHeader(std::vector<internal::HeaderField>& headers) const;
    [[nodiscard]] static std::string BuildExcerpt(const std::vector<std::byte>& body);
    [[nodiscard]] internal::WireResult CollectOneResult(
        InFlight& item, std::chrono::steady_clock::time_point deadline_point);

    internal::ITransport* m_transport;
    HttpWireCodecConfig m_config;
    internal::IAuthProvider* m_auth;
    // NOLINTNEXTLINE(clang-diagnostic-unused-private-field) — used from M3-C onward
    [[maybe_unused]] internal::IDiagnosticsSink* m_diag;
    internal::ISteadyClock* m_clock;
};

}  // namespace microtel::wire
