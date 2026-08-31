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

/// @brief Configuration for `GrpcWireCodec`.
struct GrpcWireCodecConfig
{
    std::string host;    ///< value for the `:authority` header (e.g. "host:4317")
    std::string scheme;  ///< "https" or "http"
    std::vector<internal::HeaderField> extra_headers;  ///< forwarded verbatim
    /// @brief gRPC method path override. When non-empty, used as-is for the
    /// `:path` pseudo-header instead of the default trace service path.
    /// Use the metrics service path to point this codec at the metrics endpoint.
    std::string service_path;
    /// @brief When true, the request message is gzip-compressed, the frame's
    /// compression flag is `0x01`, and `grpc-encoding: gzip` is set. No
    /// `grpc-accept-encoding` is advertised, so responses stay uncompressed.
    bool compression_gzip{false};
};

/// @brief OTLP/gRPC implementation of `IWireCodec` — no gRPC library.
///
/// Wraps the encoded payload in a 5-byte gRPC length-prefix frame, submits
/// the request via `ITransport`, parses `grpc-status` from the trailing
/// HEADERS frame (or the initial HEADERS frame for trailer-only responses),
/// and classifies the result per `docs/error-model.md §7.2`.
///
/// **RESOURCE_EXHAUSTED (8):** retryable only when `grpc-status-details-bin`
/// contains a `google.rpc.RetryInfo` entry. The codec decodes the header
/// using a minimal hand-written proto wire-format reader; no upb dependency
/// for this path in M4.
///
/// **M4 note:** compression (`grpc-encoding: gzip`), `grpc-timeout`, and
/// partial-success body parsing are deferred to M5.
///
/// **Dependencies (all non-owning):**
/// - `ITransport` — required; connected lazily on the first `Send` call if
///   not already connected (ICP 0017). A failed connect attempt is reported
///   as an ordinary retryable `WireResult`, not a distinct shape.
/// - `IAuthProvider` — optional; `Authorization` header populated if set.
/// - `IDiagnosticsSink` — optional; used from M5 onward.
/// - `ISteadyClock` — optional; passed to `IAuthProvider` for TTL arithmetic.
///
/// @threadsafety Not thread-safe — single caller (exporter worker).
/// @see docs/grpc-wire-protocol.md
/// @see docs/error-model.md §7.2
/// @see docs/icps/0017-lazy-transport-connect.md
class GrpcWireCodec final : public internal::IWireCodec
{
public:
    explicit GrpcWireCodec(internal::ITransport* transport,
                           GrpcWireCodecConfig config,
                           internal::IAuthProvider* auth = nullptr,
                           internal::IDiagnosticsSink* diag = nullptr,
                           internal::ISteadyClock* clock = nullptr,
                           internal::ConnectOptions connect_opts = {}) noexcept;

    ~GrpcWireCodec() noexcept override = default;

    GrpcWireCodec(const GrpcWireCodec&) = delete;
    GrpcWireCodec& operator=(const GrpcWireCodec&) = delete;
    GrpcWireCodec(GrpcWireCodec&&) = delete;
    GrpcWireCodec& operator=(GrpcWireCodec&&) = delete;

    [[nodiscard]] internal::WireResult Send(internal::EncodedPayload&& payload,
                                            std::chrono::milliseconds deadline) override;

private:
    [[nodiscard]] std::vector<internal::HeaderField> BuildHeaders() const;
    void AppendAuthHeader(std::vector<internal::HeaderField>& headers) const;
    /// @brief Connects `m_transport` if it isn't already (ICP 0017).
    /// @return `nullopt` when the transport is connected (already, or newly);
    ///         otherwise the retryable `WireResult` to return immediately.
    [[nodiscard]] std::optional<internal::WireResult> EnsureConnected();

    internal::ITransport* m_transport;
    GrpcWireCodecConfig m_config;
    internal::IAuthProvider* m_auth;
    // NOLINTNEXTLINE(clang-diagnostic-unused-private-field) — used from M5 onward
    [[maybe_unused]] internal::IDiagnosticsSink* m_diag;
    internal::ISteadyClock* m_clock;
    internal::ConnectOptions m_connect_opts;
};

}  // namespace microtel::wire
