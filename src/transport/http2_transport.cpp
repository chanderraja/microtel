// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#include "http2_transport.hpp"

#include "microtel/error.hpp"
#include "microtel/internal/transport.hpp"

#include <chrono>
#include <future>
#include <memory>

namespace microtel::transport
{

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

Http2Transport::Http2Transport(std::unique_ptr<internal::IReactor> reactor) noexcept
    : m_reactor(std::move(reactor))
{
}

// static
microtel::Expected<std::unique_ptr<Http2Transport>, microtel::Error>
Http2Transport::Create(std::unique_ptr<internal::IReactor> reactor) noexcept
{
    if (!reactor)
    {
        return microtel::Unexpected<microtel::Error>{
            microtel::Error{.kind    = microtel::Error::Kind::InternalFailure,
                            .message = "null reactor"}};
    }
    try
    {
        // Private constructor; make_unique can't reach it.
        // NOLINTNEXTLINE(cppcoreguidelines-owning-memory)
        auto t      = std::unique_ptr<Http2Transport>(new Http2Transport(std::move(reactor)));
        t->m_io_thread = std::thread(&Http2Transport::IoThreadLoop, t.get());
        return t;
    }
    catch (const std::bad_alloc&)
    {
        return microtel::Unexpected<microtel::Error>{
            microtel::Error{.kind = microtel::Error::Kind::InternalFailure, .message = "OOM"}};
    }
}

Http2Transport::~Http2Transport() noexcept
{
    (void)Close(std::chrono::milliseconds(2000));
}

// ---------------------------------------------------------------------------
// ITransport — state
// ---------------------------------------------------------------------------

microtel::ConnectionState Http2Transport::GetState() const noexcept
{
    return m_state.load(std::memory_order_acquire);
}

// ---------------------------------------------------------------------------
// ITransport — lifecycle
// ---------------------------------------------------------------------------

microtel::Expected<void, microtel::Error> Http2Transport::Connect(
    const internal::ConnectOptions& /*opts*/)
{
    // M3-D4: DNS + TCP connect + TLS handshake + nghttp2 SETTINGS exchange.
    return microtel::Unexpected<microtel::Error>{
        microtel::Error{.kind    = microtel::Error::Kind::Network,
                        .message = "Connect not yet implemented (M3-D4)"}};
}

microtel::Status Http2Transport::Close(std::chrono::milliseconds /*timeout*/) noexcept
{
    const auto prev =
        m_state.exchange(microtel::ConnectionState::Closed, std::memory_order_acq_rel);
    if (prev == microtel::ConnectionState::Closed)
    {
        return microtel::Status::AlreadyShutDown;
    }

    m_stop.store(true, std::memory_order_release);
    m_reactor->Wake();

    if (m_io_thread.joinable())
    {
        m_io_thread.join();
    }
    return microtel::Status::Completed;
}

// ---------------------------------------------------------------------------
// ITransport — request handling (M3-D4)
// ---------------------------------------------------------------------------

internal::RequestHandle Http2Transport::Send(internal::RequestSpec /*spec*/) noexcept
{
    std::promise<internal::TransportResult> p;
    internal::TransportResult result;
    result.error = microtel::Error{.kind = microtel::Error::Kind::Network, .message = "not connected"};
    p.set_value(std::move(result));
    return internal::RequestHandle{0, p.get_future()};
}

void Http2Transport::Cancel(const internal::RequestHandle& /*handle*/) noexcept
{
    // M3-D4: RST_STREAM on the in-flight stream.
}

// ---------------------------------------------------------------------------
// I/O thread
// ---------------------------------------------------------------------------

void Http2Transport::IoThreadLoop() noexcept
{
    while (!m_stop.load(std::memory_order_acquire))
    {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
        m_reactor->WaitAndDispatch(deadline);
    }
}

}  // namespace microtel::transport
