// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Integration test: Http2Transport::Connect over a real loopback TCP socket.
// Spins an in-process minimal nghttp2 server on a random port, then drives
// the transport through the full SETTINGS exchange.  No TLS (insecure=true).

#include "microtel/error.hpp"
#include "microtel/provider.hpp"

#include "transport/epoll_reactor.hpp"
#include "transport/http2_transport.hpp"

#include <gtest/gtest.h>
#include <nghttp2/nghttp2.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <thread>

#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace mt = microtel;
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

ssize_t SrvSend(
    nghttp2_session* /*s*/, const uint8_t* data, size_t len, int /*flags*/, void* ud) noexcept
{
    const int fd = static_cast<ServerCtx*>(ud)->fd;
    ssize_t n = ::write(fd, data, len);
    while (n < 0 && errno == EINTR)
    {
        n = ::write(fd, data, len);
    }
    if (n < 0)
    {
        return (errno == EAGAIN || errno == EWOULDBLOCK)
                   ? static_cast<ssize_t>(NGHTTP2_ERR_WOULDBLOCK)
                   : static_cast<ssize_t>(NGHTTP2_ERR_CALLBACK_FAILURE);
    }
    return n;
}

ssize_t SrvRecv(nghttp2_session* /*s*/, uint8_t* buf, size_t len, int /*flags*/, void* ud) noexcept
{
    const int fd = static_cast<ServerCtx*>(ud)->fd;
    ssize_t n = ::read(fd, buf, len);
    while (n < 0 && errno == EINTR)
    {
        n = ::read(fd, buf, len);
    }
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
        static_cast<ServerCtx*>(ud)->settings_ack_received.store(true, std::memory_order_release);
    }
    return 0;
}

class MinimalHttp2Server
{
public:
    MinimalHttp2Server() = default;
    ~MinimalHttp2Server()
    {
        Stop();
    }

    MinimalHttp2Server(const MinimalHttp2Server&) = delete;
    MinimalHttp2Server& operator=(const MinimalHttp2Server&) = delete;
    MinimalHttp2Server(MinimalHttp2Server&&) = delete;
    MinimalHttp2Server& operator=(MinimalHttp2Server&&) = delete;

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
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;

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
        m_stop.store(true, std::memory_order_release);
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
        // Keep the connection open until Stop().  Closing immediately after the
        // handshake makes the client's Connected state transient: the reactor
        // I/O thread observes the peer close and correctly resets to
        // Disconnected, which races the test's GetState() check (and loses under
        // TSAN's slower scheduling).
        while (!m_stop.load(std::memory_order_acquire))
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
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

        static constexpr int kPollMs = 50;
        static constexpr int kTimeoutMs = 5000;
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(kTimeoutMs);

        while (!ctx.settings_ack_received.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < deadline)
        {
            ::nghttp2_session_send(session);
            pollfd pfd{.fd = fd, .events = POLLIN, .revents = 0};
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

    int m_listen_fd = -1;
    int m_port = 0;
    std::thread m_thread;
    std::atomic<bool> m_handshake_done{false};
    std::atomic<bool> m_stop{false};
};

// ---------------------------------------------------------------------------
// A plaintext peer that never speaks HTTP/2, scripted
//
// `Http1Response` is the stock collector (issue #166): its plaintext OTLP/HTTP
// receiver does not wrap its handler in `h2c`, so it answers microtel's HTTP/2
// connection preface with an HTTP/1.1 error response. `FinBeforeSettings`
// stands in for every other way a peer can fail the SETTINGS exchange, and is
// here as the control: the two must not produce the same diagnosis.
//
// `FinBeforeSettings` half-closes rather than closing: it sends FIN and keeps
// reading. A full close makes the peer RST our in-flight preface, and the
// transport's next `::write` then raises SIGPIPE in the host process — issue
// #177, which is not this test's subject and which would kill the binary
// before the assertions run.
// ---------------------------------------------------------------------------

enum class PlaintextReply : std::uint8_t
{
    Http1Response,
    FinBeforeSettings,
};

constexpr std::string_view kHttp1Response =
    "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";

class ScriptedPlaintextServer
{
public:
    ScriptedPlaintextServer() = default;
    ~ScriptedPlaintextServer()
    {
        Stop();
    }

    ScriptedPlaintextServer(const ScriptedPlaintextServer&) = delete;
    ScriptedPlaintextServer& operator=(const ScriptedPlaintextServer&) = delete;
    ScriptedPlaintextServer(ScriptedPlaintextServer&&) = delete;
    ScriptedPlaintextServer& operator=(ScriptedPlaintextServer&&) = delete;

    /// Bind 127.0.0.1:0, listen, start the accept thread. Returns the assigned
    /// port, or -1.
    int Start(PlaintextReply reply)
    {
        m_reply = reply;
        m_listen_fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (m_listen_fd < 0)
        {
            return -1;
        }

        const int opt = 1;
        ::setsockopt(m_listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;

        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        if (::bind(m_listen_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0 ||
            ::listen(m_listen_fd, 1) < 0)
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

    void Stop()
    {
        m_stop.store(true, std::memory_order_release);
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
        if (m_reply == PlaintextReply::FinBeforeSettings)
        {
            ::shutdown(client_fd, SHUT_WR);
            DrainUntilStop(client_fd);
            ::close(client_fd);
            return;
        }
        // The collector answers after reading the preface; writing immediately
        // is indistinguishable from the client's side and races nothing, since
        // the client sends its preface before it ever polls for readability.
        (void)::write(client_fd, kHttp1Response.data(), kHttp1Response.size());
        while (!m_stop.load(std::memory_order_acquire))
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        ::close(client_fd);
    }

    /// Keep reading and discarding so the client's writes keep succeeding:
    /// an unread socket eventually RSTs, which is the SIGPIPE path above.
    void DrainUntilStop(int fd) const
    {
        std::array<char, 256> scratch{};
        while (!m_stop.load(std::memory_order_acquire))
        {
            pollfd pfd{.fd = fd, .events = POLLIN, .revents = 0};
            if (::poll(&pfd, 1, 5) > 0 && ::read(fd, scratch.data(), scratch.size()) <= 0)
            {
                return;
            }
        }
    }

    PlaintextReply m_reply = PlaintextReply::Http1Response;
    /// Atomic because `Stop()` clears it on the caller thread while the accept
    /// thread may still be reading it — TSAN sees that as the race it is.
    std::atomic<int> m_listen_fd{-1};
    int m_port = 0;
    std::thread m_thread;
    std::atomic<bool> m_stop{false};
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
    opts.endpoint = "http://127.0.0.1:" + std::to_string(port);
    opts.insecure = true;
    opts.connect_timeout = std::chrono::milliseconds(5000);

    const auto result = t->Connect(opts);
    EXPECT_TRUE(result.has_value()) << (result.has_value() ? "" : result.error().message);

    if (result.has_value())
    {
        // Connect() publishes Connected synchronously, but the reactor I/O
        // thread owns the state afterward; poll briefly so the assertion does
        // not race a transient transition under heavy instrumentation (TSAN).
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (t->GetState() != mt::ConnectionState::Connected &&
               std::chrono::steady_clock::now() < deadline)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        EXPECT_EQ(t->GetState(), mt::ConnectionState::Connected);
    }

    EXPECT_TRUE(server.WaitForHandshake(std::chrono::milliseconds(5000)));

    (void)t->Close(std::chrono::milliseconds(1000));
    server.Stop();
}

// ---------------------------------------------------------------------------
// ConnectionState::Reconnecting (ICP 0018 §3)
//
// Declared since M0 and never emitted: the drop path stored Disconnected, so
// GetExporterHealth() could not tell "never came up" from "was connected and
// dropped" — operationally the difference between a config problem and a peer
// problem. These drive a real drop by stopping the server, rather than poking
// the state machine directly.
// ---------------------------------------------------------------------------

namespace
{

/// Poll until @p transport reports @p want, or the deadline passes. The I/O
/// thread notices a drop on its next 100 ms tick, so this cannot be a bare
/// read.
[[nodiscard]] bool WaitForState(const mtt::Http2Transport& transport,
                                microtel::ConnectionState want,
                                std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (transport.GetState() == want)
        {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return transport.GetState() == want;
}

}  // namespace

TEST(Http2TransportIntegrationTest, DroppedConnection_ReportsReconnecting)
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
    opts.endpoint = "http://127.0.0.1:" + std::to_string(port);
    opts.insecure = true;
    opts.connect_timeout = std::chrono::milliseconds(5000);
    ASSERT_TRUE(t->Connect(opts).has_value());
    ASSERT_EQ(t->GetState(), microtel::ConnectionState::Connected);

    // Real drop: the peer goes away underneath an established connection.
    server.Stop();

    EXPECT_TRUE(WaitForState(*t, microtel::ConnectionState::Reconnecting, std::chrono::seconds(5)))
        << "a mid-connection drop must report Reconnecting, not Disconnected — "
           "state was "
        << static_cast<int>(t->GetState());

    EXPECT_EQ(t->Close(std::chrono::milliseconds(2000)), microtel::Status::Completed);
}

TEST(Http2TransportIntegrationTest, ConnectSucceedsFromReconnecting)
{
    MinimalHttp2Server first;
    const int first_port = first.Start();
    ASSERT_GT(first_port, 0);

    auto reactor_result = mtt::EpollReactor::Create();
    ASSERT_TRUE(reactor_result.has_value());
    auto transport_result = mtt::Http2Transport::Create(std::move(*reactor_result));
    ASSERT_TRUE(transport_result.has_value());
    auto& t = *transport_result;

    mti::ConnectOptions opts;
    opts.endpoint = "http://127.0.0.1:" + std::to_string(first_port);
    opts.insecure = true;
    opts.connect_timeout = std::chrono::milliseconds(5000);
    ASSERT_TRUE(t->Connect(opts).has_value());

    first.Stop();
    ASSERT_TRUE(WaitForState(*t, microtel::ConnectionState::Reconnecting, std::chrono::seconds(5)));

    // The CAS must accept Reconnecting → Connecting, not just Disconnected →
    // Connecting. Without that, every reconnect fails with "already
    // connecting" and the transport is permanently stuck after one drop.
    MinimalHttp2Server second;
    const int second_port = second.Start();
    ASSERT_GT(second_port, 0);
    opts.endpoint = "http://127.0.0.1:" + std::to_string(second_port);

    const auto reconnected = t->Connect(opts);
    EXPECT_TRUE(reconnected.has_value())
        << (reconnected.has_value() ? "" : reconnected.error().message);
    EXPECT_EQ(t->GetState(), microtel::ConnectionState::Connected);

    EXPECT_EQ(t->Close(std::chrono::milliseconds(2000)), microtel::Status::Completed);
}

// ---------------------------------------------------------------------------
// HTTP/1.1-only peer (issue #166)
//
// A plaintext microtel endpoint is h2c with prior knowledge, and the most
// common OTLP receiver on the other end of one — the OpenTelemetry Collector's
// `http://…:4318` receiver — speaks HTTP/1.1 only. Before these tests the
// failure surfaced as the generic "nghttp2 recv failed during SETTINGS
// exchange", which is equally true of a dozen unrelated faults and steers
// nobody towards the fix.
// ---------------------------------------------------------------------------

TEST(Http2TransportIntegrationTest, Connect_PlaintextPeerSpeaksHttp1_ReportsTargetedError)
{
    ScriptedPlaintextServer server;
    const int port = server.Start(PlaintextReply::Http1Response);
    ASSERT_GT(port, 0);

    auto reactor_result = mtt::EpollReactor::Create();
    ASSERT_TRUE(reactor_result.has_value());
    auto transport_result = mtt::Http2Transport::Create(std::move(*reactor_result));
    ASSERT_TRUE(transport_result.has_value());
    auto& t = *transport_result;

    mti::ConnectOptions opts;
    opts.endpoint = "http://127.0.0.1:" + std::to_string(port);
    opts.insecure = true;
    opts.connect_timeout = std::chrono::milliseconds(5000);

    const auto result = t->Connect(opts);
    ASSERT_FALSE(result.has_value()) << "an HTTP/1.1-only peer cannot complete the h2c handshake";
    EXPECT_EQ(result.error().kind, microtel::Error::Kind::Protocol)
        << "a peer that speaks the wrong protocol is not a network fault";
    EXPECT_NE(result.error().message.find("HTTP/1.1-only"), std::string::npos)
        << "error was: " << result.error().message;

    EXPECT_EQ(t->Close(std::chrono::milliseconds(2000)), microtel::Status::Completed);
    server.Stop();
}

// The sniff sets a member flag, and a flag that survived into the next
// `Connect` would mislabel every later failure as an HTTP/1.1 peer. Reconnect
// is a first-class path since ICP 0018, so this is not hypothetical.
TEST(Http2TransportIntegrationTest, Connect_AfterHttp1Peer_ReconnectToHealthyServerSucceeds)
{
    ScriptedPlaintextServer http1;
    const int http1_port = http1.Start(PlaintextReply::Http1Response);
    ASSERT_GT(http1_port, 0);

    auto reactor_result = mtt::EpollReactor::Create();
    ASSERT_TRUE(reactor_result.has_value());
    auto transport_result = mtt::Http2Transport::Create(std::move(*reactor_result));
    ASSERT_TRUE(transport_result.has_value());
    auto& t = *transport_result;

    mti::ConnectOptions opts;
    opts.endpoint = "http://127.0.0.1:" + std::to_string(http1_port);
    opts.insecure = true;
    opts.connect_timeout = std::chrono::milliseconds(5000);
    ASSERT_FALSE(t->Connect(opts).has_value());
    http1.Stop();

    MinimalHttp2Server healthy;
    const int healthy_port = healthy.Start();
    ASSERT_GT(healthy_port, 0);
    opts.endpoint = "http://127.0.0.1:" + std::to_string(healthy_port);

    const auto reconnected = t->Connect(opts);
    EXPECT_TRUE(reconnected.has_value())
        << (reconnected.has_value() ? "" : reconnected.error().message);
    EXPECT_EQ(t->GetState(), microtel::ConnectionState::Connected);

    EXPECT_EQ(t->Close(std::chrono::milliseconds(2000)), microtel::Status::Completed);
    healthy.Stop();
}

// The control for the sniff: a peer that fails the SETTINGS exchange for any
// other reason must keep the generic message. A detector that fired on
// everything would be worse than none — it would send operators looking for an
// HTTP/1.1 receiver that is not there.
TEST(Http2TransportIntegrationTest, Connect_PeerHangsUpBeforeSettings_ReportsGenericError)
{
    ScriptedPlaintextServer server;
    const int port = server.Start(PlaintextReply::FinBeforeSettings);
    ASSERT_GT(port, 0);

    auto reactor_result = mtt::EpollReactor::Create();
    ASSERT_TRUE(reactor_result.has_value());
    auto transport_result = mtt::Http2Transport::Create(std::move(*reactor_result));
    ASSERT_TRUE(transport_result.has_value());
    auto& t = *transport_result;

    mti::ConnectOptions opts;
    opts.endpoint = "http://127.0.0.1:" + std::to_string(port);
    opts.insecure = true;
    opts.connect_timeout = std::chrono::milliseconds(5000);

    const auto result = t->Connect(opts);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().kind, microtel::Error::Kind::Network);
    EXPECT_EQ(result.error().message.find("HTTP/1.1-only"), std::string::npos)
        << "message was: " << result.error().message;

    EXPECT_EQ(t->Close(std::chrono::milliseconds(2000)), microtel::Status::Completed);
    server.Stop();
}
