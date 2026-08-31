// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/internal/auth_provider.hpp"
#include "microtel/internal/clock.hpp"
#include "microtel/internal/diagnostics_sink.hpp"
#include "microtel/internal/transport.hpp"
#include "microtel/internal/wire_codec.hpp"

#include <chrono>
#include <optional>
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
    /// @brief Full signal path override. When non-empty, used as-is for the
    /// `:path` pseudo-header, ignoring `path` and the default `/v1/traces`
    /// suffix. Use `/v1/metrics` to point this codec at the metrics endpoint.
    std::string signal_path;
    std::vector<internal::HeaderField> extra_headers;  ///< forwarded verbatim
    /// @brief When true, request bodies are gzip-compressed and
    /// `content-encoding: gzip` is set. Responses are unaffected: no
    /// `accept-encoding` is advertised, so servers return identity.
    bool compression_gzip{false};
};

/// @brief OTLP/HTTP-protobuf implementation of `IWireCodec`.
///
/// Builds an HTTP/2 POST request from the encoded payload, submits it via the
/// injected `ITransport`, waits for the response, and classifies the HTTP
/// status code per the matrix in `docs/error-model.md` §7.1.
///
/// **Dependencies (all non-owning):**
/// - `ITransport` — required; connected lazily on the first `Send`/`SendAll`
///   call if not already connected (ICP 0017). A failed connect attempt is
///   reported as an ordinary retryable `WireResult`, not a distinct shape.
/// - `IAuthProvider` — optional; if non-null, the `Authorization` header is
///   populated on every request.
/// - `IDiagnosticsSink` — optional; if non-null, non-retryable failures are
///   counted.
/// - `ISteadyClock` — optional; if non-null, passed to `IAuthProvider::Get-
///   Authorization` for TTL arithmetic. Falls back to `steady_clock::now()`.
///
/// @threadsafety Not thread-safe — single caller (exporter worker).
/// @see docs/error-model.md §7.1
/// @see docs/icps/0017-lazy-transport-connect.md
class HttpWireCodec final : public internal::IWireCodec
{
public:
    explicit HttpWireCodec(internal::ITransport* transport,
                           HttpWireCodecConfig config,
                           internal::IAuthProvider* auth = nullptr,
                           internal::IDiagnosticsSink* diag = nullptr,
                           internal::ISteadyClock* clock = nullptr,
                           internal::ConnectOptions connect_opts = {}) noexcept;

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
        /// @brief Compressed body, when `compression_gzip` is on. Empty
        /// otherwise. Held here for the same reason as `payload`: the spec's
        /// span borrows it until the stream closes.
        std::vector<std::byte> compressed;
        /// @brief Index into the caller's `payloads`, so `SendAll` can return
        /// results in the caller's order even when an entry never went out.
        std::size_t index{0};
    };

    /// @brief Gzip-compresses @p raw into @p storage when `compression_gzip`
    ///        is set; otherwise a no-op.
    /// @param raw the uncompressed body.
    /// @param storage buffer that owns the compressed bytes on return. Must
    ///        outlive the request: the returned span borrows from it.
    /// @return the span to put on the wire, or the `WireResult` to report when
    ///         compression fails.
    [[nodiscard]] microtel::Expected<std::span<const std::byte>, internal::WireResult>
    MaybeCompress(std::span<const std::byte> raw, std::vector<std::byte>& storage) const;

    [[nodiscard]] std::string ResolvePath() const noexcept;
    [[nodiscard]] std::vector<internal::HeaderField> BuildHeaders(
        std::size_t content_length) const noexcept;
    void AppendAuthHeader(std::vector<internal::HeaderField>& headers) const;
    [[nodiscard]] static std::string BuildExcerpt(const std::vector<std::byte>& body);
    [[nodiscard]] internal::WireResult CollectOneResult(
        InFlight& item, std::chrono::steady_clock::time_point deadline_point);
    /// @brief Connects `m_transport` if it isn't already (ICP 0017).
    /// @return `nullopt` when the transport is connected (already, or newly);
    ///         otherwise the retryable `WireResult` to return immediately.
    [[nodiscard]] std::optional<internal::WireResult> EnsureConnected();

    internal::ITransport* m_transport;
    HttpWireCodecConfig m_config;
    internal::IAuthProvider* m_auth;
    // NOLINTNEXTLINE(clang-diagnostic-unused-private-field) — used from M3-C onward
    [[maybe_unused]] internal::IDiagnosticsSink* m_diag;
    internal::ISteadyClock* m_clock;
    internal::ConnectOptions m_connect_opts;
};

}  // namespace microtel::wire
