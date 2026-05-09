// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Integration test: Http2Transport::Connect over a real loopback TCP socket.
// Spins an in-process minimal nghttp2 server on a random port, then drives
// the transport through the full SETTINGS exchange.  No TLS (insecure=true).

#include "transport/epoll_reactor.hpp"
#include "transport/http2_transport.hpp"

#include "microtel/provider.hpp"

#include <gtest/gtest.h>

#include <nghttp2/nghttp2.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

namespace mt  = microtel;
namespace mtt = microtel::transport;
namespace mti = microtel::internal;

// ---------------------------------------------------------------------------
// Minimal in-process HTTP/2 server (no TLS, loopback only)
// ---------------------------------------------------------------------------

namespace
{

struct ServerCtx
{
    int fd = -1;
    std::atomic<bool> settings_ack_received{false};
};

ssize_t SrvSend(nghttp2_session* /*s*/, const uint8_t* data, size_t len,
                int /*flags*/, void* ud) noexcept
{
    const int fd = static_cast<ServerCtx*>(ud)->fd;
    ssize_t n = 0;
    do
    {
        n = ::write(fd, data, len);
    } while (n < 0 && errno == EINTR);
    if (n < 0)
    {
        return (errno == EAGAIN || errno == EWOULDBLOCK)
                   ? static_cast<ssize_t>(NGHTTP2_ERR_WOULDBLOCK)
                   : static_cast<ssize_t>(NGHTTP2_ERR_CALLBACK_FAILURE);
    }
    return n;
}

ssize_t SrvRecv(nghttp2_session* /*s*/, uint8_t* buf, size_t len,
                int /*flags*/, void* ud) noexcept
{
    const int fd = static_cast<ServerCtx*>(ud)->fd;
    ssize_t n = 0;
    do
    {
        n = ::read(fd, buf, len);
    } while (n < 0 && errno == EINTR);
    if (n == 0)
    {
        return NGHTTP2_ERR_CALLBACK_FAILURE;
    }
    if (n < 0)
    {
        return (errno == EAGAIN || errno == EWOULDBLOCK) ? NGHTTP2_ERR_WOULDBLOCK
                                                         : NGHTTP2_ERR_CALLBACK_FAILURE;
    }
    return n;
}

int SrvOnFrameRecv(nghttp2_session* /*s*/, const nghttp2_frame* frame, void* ud) noexcept
{
    if (frame->hd.type == NGHTTP2_SETTINGS && (frame->hd.flags & NGHTTP2_FLAG_ACK) != 0U)
    {
        static_cast<ServerCtx*>(ud)->settings_ack_received.store(true,
                                                                  std::memory_order_release);
    }
    return 0;
}

class MinimalHttp2Server
{
public:
    ~MinimalHttp2Server()
    {
        Stop();
    }

    // Bind to 127.0.0.1:0, listen, start thread.  Returns assigned port or -1.
    int Start()
    {
        m_listen_fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (m_listen_fd < 0)
        {
            return -1;
        }

        const int opt = 1;
        ::setsockopt(m_listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        sockaddr_in addr{};
        addr.sin_family      = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port        = 0;

        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        if (::bind(m_listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0)
        {
            ::close(m_listen_fd);
            m_listen_fd = -1;
            return -1;
        }
        if (::listen(m_listen_fd, 1) < 0)
        {
            ::close(m_listen_fd);
            m_listen_fd = -1;
            return -1;
        }

        socklen_t len = sizeof(addr);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        ::getsockname(m_listen_fd, reinterpret_cast<sockaddr*>(&addr), &len);
        m_port = ntohs(addr.sin_port);

        m_thread = std::thread([this] { ServerThread(); });
        return m_port;
    }

    bool WaitForHandshake(std::chrono::milliseconds timeout) const
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (!m_handshake_done.load(std::memory_order_acquire))
        {
            if (std::chrono::steady_clock::now() >= deadline)
            {
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return true;
    }

    void Stop()
    {
        if (m_listen_fd >= 0)
        {
            ::close(m_listen_fd);
            m_listen_fd = -1;
        }
        if (m_thread.joinable())
        {
            m_thread.join();
        }
    }

private:
    void ServerThread()
    {
        const int client_fd = ::accept(m_listen_fd, nullptr, nullptr);
        if (client_fd < 0)
        {
            return;
        }
        RunHandshake(client_fd);
        ::close(client_fd);
    }

    void RunHandshake(int fd)
    {
        // Non-blocking prevents nghttp2's recv_callback from blocking after
        // exhausting available data — without this, the loop deadlocks.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg,hicpp-vararg)
        const int flags = ::fcntl(fd, F_GETFL);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg,hicpp-vararg,hicpp-signed-bitwise)
        ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);

        ServerCtx ctx;
        ctx.fd = fd;

        nghttp2_session_callbacks* cbs = nullptr;
        ::nghttp2_session_callbacks_new(&cbs);
        ::nghttp2_session_callbacks_set_send_callback(cbs, SrvSend);
        ::nghttp2_session_callbacks_set_recv_callback(cbs, SrvRecv);
        ::nghttp2_session_callbacks_set_on_frame_recv_callback(cbs, SrvOnFrameRecv);

        nghttp2_session* session = nullptr;
        ::nghttp2_session_server_new(&session, cbs, &ctx);
        ::nghttp2_session_callbacks_del(cbs);

        const nghttp2_settings_entry iv[1] = {{NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, 100U}};
        ::nghttp2_submit_settings(session, NGHTTP2_FLAG_NONE, iv, 1);

        static constexpr int kPollMs    = 50;
        static constexpr int kTimeoutMs = 5000;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kTimeoutMs);

        while (!ctx.settings_ack_received.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < deadline)
        {
            ::nghttp2_session_send(session);
            pollfd pfd{fd, POLLIN, 0};
            if (::poll(&pfd, 1, kPollMs) > 0)
            {
                ::nghttp2_session_recv(session);
                // Flush queued SETTINGS_ACK immediately after processing frames.
                ::nghttp2_session_send(session);
            }
        }

        ::nghttp2_session_del(session);
        m_handshake_done.store(true, std::memory_order_release);
    }

    int                 m_listen_fd = -1;
    int                 m_port      = 0;
    std::thread         m_thread;
    std::atomic<bool>   m_handshake_done{false};
};

}  // namespace

// ---------------------------------------------------------------------------
// Test
// ---------------------------------------------------------------------------

TEST(Http2TransportIntegrationTest, Connect_InsecureLoopback_Succeeds)
{
    MinimalHttp2Server server;
    const int port = server.Start();
    ASSERT_GT(port, 0);

    auto reactor_result = mtt::EpollReactor::Create();
    ASSERT_TRUE(reactor_result.has_value());

    auto transport_result = mtt::Http2Transport::Create(std::move(*reactor_result));
    ASSERT_TRUE(transport_result.has_value());
    auto& t = *transport_result;

    mti::ConnectOptions opts;
    opts.endpoint        = "http://127.0.0.1:" + std::to_string(port);
    opts.insecure        = true;
    opts.connect_timeout = std::chrono::milliseconds(5000);

    const auto result = t->Connect(opts);
    EXPECT_TRUE(result.has_value()) << (result.has_value() ? "" : result.error().message);

    if (result.has_value())
    {
        EXPECT_EQ(t->GetState(), mt::ConnectionState::Connected);
    }

    EXPECT_TRUE(server.WaitForHandshake(std::chrono::milliseconds(5000)));

    (void)t->Close(std::chrono::milliseconds(1000));
    server.Stop();
}
