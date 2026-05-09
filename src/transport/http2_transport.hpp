// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/error.hpp"
#include "microtel/expected.hpp"
#include "microtel/internal/reactor.hpp"
#include "microtel/internal/transport.hpp"
#include "microtel/provider.hpp"
#include "microtel/status.hpp"

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

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
    Http2Transport(Http2Transport&&)                 = delete;
    Http2Transport& operator=(Http2Transport&&)      = delete;

    /// @brief Stub — full DNS + TCP + TLS + nghttp2 SETTINGS in M3-D4.
    [[nodiscard]] microtel::Expected<void, microtel::Error> Connect(
        const internal::ConnectOptions& opts) override;

    /// @brief Stub — stream submission in M3-D4.
    [[nodiscard]] internal::RequestHandle Send(internal::RequestSpec spec) noexcept override;

    /// @brief Stub — RST_STREAM in M3-D4.
    void Cancel(const internal::RequestHandle& handle) noexcept override;

    /// @brief Returns the current connection state. Thread-safe.
    [[nodiscard]] microtel::ConnectionState GetState() const noexcept override;

    /// @brief Signal the I/O thread to stop, wait up to `timeout` for it to
    /// join, then return the outcome. Idempotent — second call returns
    /// `AlreadyShutDown`.
    [[nodiscard]] microtel::Status Close(std::chrono::milliseconds timeout) noexcept override;

private:
    explicit Http2Transport(std::unique_ptr<internal::IReactor> reactor) noexcept;

    void IoThreadLoop() noexcept;

    std::unique_ptr<internal::IReactor>          m_reactor;
    std::atomic<microtel::ConnectionState>        m_state{microtel::ConnectionState::Disconnected};
    std::atomic<bool>                             m_stop{false};
    std::thread                                   m_io_thread;
};

}  // namespace microtel::transport
