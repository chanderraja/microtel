// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/error.hpp"
#include "microtel/expected.hpp"
#include "microtel/protocol.hpp"
#include "microtel/provider.hpp"  // ConnectionState
#include "microtel/status.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <future>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace microtel::internal
{

/// @brief Inputs to `ITransport::Connect`. Six independent timeouts per
/// `microtel-spec.md` §7.3 (the connect-relevant subset is on this struct;
/// others live on the exporter's per-request `RequestSpec`).
struct ConnectOptions
{
    std::string endpoint;  ///< host:port URL (https:// or http://)
    Protocol protocol = Protocol::Grpc;
    std::chrono::milliseconds connect_timeout = std::chrono::seconds(10);
    std::chrono::milliseconds tls_handshake_timeout = std::chrono::seconds(10);

    // TLS material; empty paths mean default behaviour.
    bool insecure = false;
    std::filesystem::path ca_bundle;
    std::filesystem::path client_cert;
    std::filesystem::path client_key;
    std::string sni_override;

    // HTTP/2 settings (sensible defaults; tunable via configuration).
    std::uint32_t max_concurrent_streams = 100;
    std::uint32_t initial_window_size = 1U << 20;  // 1 MiB
};

/// @brief HTTP/2 header (name, value) pair.
struct HeaderField
{
    std::string name;
    std::string value;
};

/// @brief Inputs to a single `ITransport::Send` call.
///
/// The bytes referenced by `payload` are **borrowed**; the caller (the wire
/// codec) retains ownership and must not free them until the request
/// completion fires. (LOCKED — `docs/memory-model.md` §3.3.)
struct RequestSpec
{
    std::vector<HeaderField> headers;
    std::span<const std::byte> payload;
    std::chrono::milliseconds deadline = std::chrono::seconds(10);
};

/// @brief Result of a single transport request.
struct TransportResult
{
    bool success = false;
    std::vector<HeaderField> response_headers;
    std::vector<HeaderField> response_trailers;
    std::vector<std::byte> response_body;  ///< capped at max_response_bytes
    std::optional<Error> error;
};

/// @brief Move-only handle to an in-flight request.
///
/// The `future` resolves on completion (success, failure, cancellation).
/// `Cancel(handle)` requests cancellation; the future still resolves, with
/// `Error::Kind::Cancelled` populated. Methods are inline so mocks can
/// construct a `RequestHandle` without a separate translation unit.
class RequestHandle
{
public:
    RequestHandle() noexcept = default;
    explicit RequestHandle(std::uint64_t id, std::future<TransportResult> f) noexcept
        : m_id(id), m_future(std::move(f))
    {
    }

    RequestHandle(const RequestHandle&) = delete;
    RequestHandle& operator=(const RequestHandle&) = delete;
    RequestHandle(RequestHandle&&) noexcept = default;
    RequestHandle& operator=(RequestHandle&&) noexcept = default;

    [[nodiscard]] std::uint64_t Id() const noexcept
    {
        return m_id;
    }
    [[nodiscard]] std::future<TransportResult>& Future() noexcept
    {
        return m_future;
    }

private:
    std::uint64_t m_id = 0;
    std::future<TransportResult> m_future;
};

/// @brief A connection to one OTLP endpoint over HTTP/2.
///
/// One transport instance manages exactly one socket and one nghttp2 session.
/// Reconnect is internal — clients do not see it.
///
/// `Send` is single-threaded — only the exporter worker calls it. (LOCKED.)
///
/// @threadsafety See per-method threading notes.
/// @see docs/interfaces.md §4.1
class ITransport
{
public:
    virtual ~ITransport() noexcept = default;

    /// @brief Open the connection (DNS + TCP + TLS + HTTP/2 SETTINGS).
    /// @threadsafety Caller-thread; called once during `Provider::Build` or
    ///               `Provider::Connect`.
    [[nodiscard]] virtual microtel::Expected<void, microtel::Error> Connect(
        const ConnectOptions& opts) = 0;

    /// @brief Submit a request. Returns immediately with a handle.
    /// @threadsafety Single-threaded; only the exporter worker may call.
    [[nodiscard]] virtual RequestHandle Send(RequestSpec spec) noexcept = 0;

    /// @brief Cancel an in-flight request.
    /// @threadsafety Thread-safe.
    virtual void Cancel(const RequestHandle& handle) noexcept = 0;

    /// @threadsafety Thread-safe.
    [[nodiscard]] virtual ConnectionState GetState() const noexcept = 0;

    /// @brief Initiate orderly shutdown. Idempotent.
    /// @threadsafety Thread-safe.
    [[nodiscard]] virtual microtel::Status Close(std::chrono::milliseconds timeout) noexcept = 0;
};

}  // namespace microtel::internal
