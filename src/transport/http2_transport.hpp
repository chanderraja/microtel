// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/error.hpp"
#include "microtel/expected.hpp"
#include "microtel/internal/reactor.hpp"
#include "microtel/internal/transport.hpp"
#include "microtel/provider.hpp"
#include "microtel/status.hpp"

#include "common/raii/nghttp2_session.hpp"
#include "common/raii/ssl_ctx.hpp"
#include "common/raii/ssl_session.hpp"
#include "common/raii/unique_fd.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace microtel::transport
{

/// @brief nghttp2 + OpenSSL implementation of `ITransport`.
///
/// One socket, one TLS session, one nghttp2 client session, one I/O thread.
/// Reconnect is internal; callers see only `ConnectionState` transitions.
///
/// The reactor is injected so unit tests can drive the I/O loop from a script
/// without a real socket (`FakeReactor`). Production code injects an
/// `EpollReactor` created by `EpollReactor::Create()`.
///
/// @threadsafety `GetState` is thread-safe. `Connect` and `Close` are
///               caller-thread (single caller at a time). `Send` is
///               single-threaded; only the exporter worker may call it.
///               (All threading contracts LOCKED — `interfaces.md` §4.1.)
/// @see docs/interfaces.md §4.1
class Http2Transport final : public internal::ITransport
{
public:
    /// @brief Factory — creates the transport and starts the I/O thread.
    ///
    /// Returns an error if `reactor` is null or memory allocation fails.
    [[nodiscard]] static microtel::Expected<std::unique_ptr<Http2Transport>, microtel::Error>
    Create(std::unique_ptr<internal::IReactor> reactor) noexcept;

    ~Http2Transport() noexcept override;

    Http2Transport(const Http2Transport&) = delete;
    Http2Transport& operator=(const Http2Transport&) = delete;
    Http2Transport(Http2Transport&&) = delete;
    Http2Transport& operator=(Http2Transport&&) = delete;

    /// @brief DNS + TCP + optional TLS + nghttp2 SETTINGS exchange.
    [[nodiscard]] microtel::Expected<void, microtel::Error> Connect(
        const internal::ConnectOptions& opts) override;

    /// @brief Stub — stream submission in M3-D5.
    [[nodiscard]] internal::RequestHandle Send(internal::RequestSpec spec) noexcept override;

    /// @brief Stub — RST_STREAM in M3-D5.
    void Cancel(const internal::RequestHandle& handle) noexcept override;

    /// @brief Returns the current connection state. Thread-safe.
    [[nodiscard]] microtel::ConnectionState GetState() const noexcept override;

    /// @brief Signal the I/O thread to stop, wait up to `timeout` for it to
    /// join, then return the outcome. Idempotent — second call returns
    /// `AlreadyShutDown`.
    [[nodiscard]] microtel::Status Close(std::chrono::milliseconds timeout) noexcept override;

    // Called by the nghttp2 C-callback trampolines defined in the .cpp file.
    // Not part of the public API; must be public so the free-function trampolines
    // can reach them without friendship (C-linkage functions cannot be friends).
    std::ptrdiff_t NgHttp2DoSend(const std::uint8_t* data, std::size_t len) noexcept;
    std::ptrdiff_t NgHttp2DoRecv(std::uint8_t* buf, std::size_t len) noexcept;
    void OnSettingsAck() noexcept;
    void OnStreamClose(std::int32_t stream_id, std::uint32_t error_code) noexcept;
    void OnResponseHeader(std::int32_t stream_id,
                          bool is_trailer,
                          std::string_view name,
                          std::string_view value) noexcept;
    void OnResponseData(std::int32_t stream_id, const std::uint8_t* data, std::size_t len) noexcept;

    /// @brief Per-stream state owned by the I/O thread.
    ///
    /// Public so the `PayloadReadCb` C trampoline can cast `source->ptr`
    /// to `Http2Transport::StreamState*`.
    struct StreamState
    {
        internal::RequestSpec spec;
        std::size_t payload_offset = 0;
        std::promise<internal::TransportResult> promise;
        internal::TransportResult result;
        std::uint64_t handle_id = 0;
    };

private:
    /// @brief Request queued by Send(); drained by the I/O thread.
    struct PendingRequest
    {
        internal::RequestSpec spec;
        std::promise<internal::TransportResult> promise;
        std::uint64_t handle_id = 0;
    };

    explicit Http2Transport(std::unique_ptr<internal::IReactor> reactor) noexcept;

    void IoThreadLoop() noexcept;
    void OnIoEvent(int fd, internal::EventMask events) noexcept;
    void DrainPendingRequests() noexcept;
    void DrainCancelQueue() noexcept;
    void SubmitStream(PendingRequest req) noexcept;
    void FulfillStream(std::int32_t stream_id, std::uint32_t nghttp2_error_code) noexcept;

    [[nodiscard]] microtel::Expected<std::pair<common::raii::SslCtx, common::raii::SslSession>,
                                     microtel::Error>
    TlsHandshake(const internal::ConnectOptions& opts, const std::string& host);

    [[nodiscard]] microtel::Expected<common::raii::Nghttp2Session, microtel::Error> Http2Handshake(
        const internal::ConnectOptions& opts);

    std::unique_ptr<internal::IReactor> m_reactor;
    std::atomic<microtel::ConnectionState> m_state{microtel::ConnectionState::Disconnected};
    std::atomic<bool> m_stop{false};
    std::thread m_io_thread;
    /// Signalled by the I/O thread as its loop exits, so `Close` can bound how
    /// long it waits. `std::thread` has no timed join, and detaching is not an
    /// option here because the loop touches members of `this` — so the wait is
    /// what the timeout actually governs, and a timeout is reported honestly
    /// rather than silently ignored (`Provider::Shutdown` contract, CLAUDE.md
    /// rule 15). Same shape as `OtlpExporter::Shutdown`.
    std::mutex m_io_done_mu;
    std::condition_variable m_io_done_cv;
    bool m_io_done{false};

    // Connection resources — written by Connect(), read by I/O-thread callbacks.
    // Thread-safety: Connect() writes these before the release-store of
    // m_state = Connected; I/O callbacks acquire-load m_state before use.
    common::raii::UniqueFd m_socket;
    common::raii::SslCtx m_ssl_ctx;
    common::raii::SslSession m_ssl_session;
    common::raii::Nghttp2Session m_nghttp2_session;
    std::atomic<bool> m_settings_ack_received{false};

    // Send queues — caller-thread writes, I/O thread drains.
    std::mutex m_pending_mu;
    std::vector<PendingRequest> m_pending_queue;
    std::mutex m_cancel_mu;
    std::vector<std::uint64_t> m_cancel_queue;

    // I/O-thread-only stream tracking (no mutex needed).
    std::unordered_map<std::int32_t, std::unique_ptr<StreamState>> m_streams;
    std::unordered_map<std::uint64_t, std::int32_t> m_handle_to_stream;

    std::atomic<std::uint64_t> m_next_handle_id{1};
};

}  // namespace microtel::transport
