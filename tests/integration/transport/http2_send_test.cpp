// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Integration test: Http2Transport::Send over a real loopback TCP socket.
// Spins an in-process minimal nghttp2 server on a random port, completes
// the SETTINGS exchange, receives one POST request and replies with 200.
// No TLS (insecure=true).

#include "microtel/provider.hpp"

#include "transport/epoll_reactor.hpp"
#include "transport/http2_transport.hpp"

#include <gtest/gtest.h>
#include <nghttp2/nghttp2.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <future>
#include <string>
#include <thread>

#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace mtt = microtel::transport;
namespace mti = microtel::internal;

// ---------------------------------------------------------------------------
// Minimal in-process HTTP/2 server: completes SETTINGS, responds to one POST
// ---------------------------------------------------------------------------

namespace
{

struct RequestServerCtx
{
    int fd = -1;
    std::atomic<bool> settings_ack_received{false};
    std::atomic<bool> response_sent{false};
    int32_t request_stream_id = -1;
};

ssize_t SrvSendCb(
    nghttp2_session* /*s*/, const uint8_t* data, size_t len, int /*flags*/, void* ud) noexcept
{
    const int fd = static_cast<RequestServerCtx*>(ud)->fd;
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

ssize_t SrvRecvCb(
    nghttp2_session* /*s*/, uint8_t* buf, size_t len, int /*flags*/, void* ud) noexcept
{
    const int fd = static_cast<RequestServerCtx*>(ud)->fd;
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

// Record the stream id when a request HEADERS frame begins.
int SrvOnBeginHeadersCb(nghttp2_session* /*s*/, const nghttp2_frame* frame, void* ud) noexcept
{
    auto* ctx = static_cast<RequestServerCtx*>(ud);
    if (frame->hd.type == NGHTTP2_HEADERS && frame->headers.cat == NGHTTP2_HCAT_REQUEST)
    {
        ctx->request_stream_id = frame->hd.stream_id;
    }
    return 0;
}

// On SETTINGS_ACK: mark handshake done.
// On END_STREAM for our request stream: submit a 200 response.
int SrvOnFrameRecvCb(nghttp2_session* s, const nghttp2_frame* frame, void* ud) noexcept
{
    auto* ctx = static_cast<RequestServerCtx*>(ud);

    if (frame->hd.type == NGHTTP2_SETTINGS && (frame->hd.flags & NGHTTP2_FLAG_ACK) != 0U)
    {
        ctx->settings_ack_received.store(true, std::memory_order_release);
    }

    const bool is_our_stream =
        (frame->hd.stream_id == ctx->request_stream_id) && (ctx->request_stream_id > 0);
    const bool end_stream = (frame->hd.flags & NGHTTP2_FLAG_END_STREAM) != 0U;

    if (is_our_stream && end_stream && !ctx->response_sent.load(std::memory_order_acquire))
    {
        // nghttp2 nv fields are non-const even though the library never writes through them.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
        static const auto* const kStatusName = reinterpret_cast<uint8_t*>(
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
            const_cast<char*>(":status"));
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
        static const auto* const kStatusValue = reinterpret_cast<uint8_t*>(
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
            const_cast<char*>("200"));

        nghttp2_nv nv{};
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
        nv.name = const_cast<uint8_t*>(kStatusName);
        nv.namelen = 7;
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
        nv.value = const_cast<uint8_t*>(kStatusValue);
        nv.valuelen = 3;
        nv.flags = NGHTTP2_NV_FLAG_NONE;

        ::nghttp2_submit_response(s, ctx->request_stream_id, &nv, 1, nullptr);
        ctx->response_sent.store(true, std::memory_order_release);
    }
    return 0;
}

class MinimalHttp2RequestServer
{
public:
    MinimalHttp2RequestServer() = default;
    ~MinimalHttp2RequestServer()
    {
        Stop();
    }

    MinimalHttp2RequestServer(const MinimalHttp2RequestServer&) = delete;
    MinimalHttp2RequestServer& operator=(const MinimalHttp2RequestServer&) = delete;
    MinimalHttp2RequestServer(MinimalHttp2RequestServer&&) = delete;
    MinimalHttp2RequestServer& operator=(MinimalHttp2RequestServer&&) = delete;

    // Bind to 127.0.0.1:0, listen, start accept thread. Returns port or -1.
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

    bool WaitForResponse(std::chrono::milliseconds timeout) const
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (!m_response_done.load(std::memory_order_acquire))
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
            m_response_done.store(true, std::memory_order_release);
            return;
        }
        RunSession(client_fd);
        ::close(client_fd);
        m_response_done.store(true, std::memory_order_release);
    }

    static void RunSession(int fd)
    {
        // Non-blocking so nghttp2 recv_callback returns WOULDBLOCK instead of blocking.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg,hicpp-vararg,hicpp-signed-bitwise)
        ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | O_NONBLOCK);

        RequestServerCtx ctx;
        ctx.fd = fd;

        nghttp2_session_callbacks* cbs = nullptr;
        ::nghttp2_session_callbacks_new(&cbs);
        ::nghttp2_session_callbacks_set_send_callback(cbs, SrvSendCb);
        ::nghttp2_session_callbacks_set_recv_callback(cbs, SrvRecvCb);
        ::nghttp2_session_callbacks_set_on_frame_recv_callback(cbs, SrvOnFrameRecvCb);
        ::nghttp2_session_callbacks_set_on_begin_headers_callback(cbs, SrvOnBeginHeadersCb);

        nghttp2_session* session = nullptr;
        ::nghttp2_session_server_new(&session, cbs, &ctx);
        ::nghttp2_session_callbacks_del(cbs);

        const nghttp2_settings_entry iv[1] = {{NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, 100U}};
        ::nghttp2_submit_settings(session, NGHTTP2_FLAG_NONE, iv, 1);

        static constexpr int kPollMs = 50;
        static constexpr int kTimeoutMs = 10000;
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(kTimeoutMs);

        while (!ctx.response_sent.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < deadline)
        {
            ::nghttp2_session_send(session);
            pollfd pfd{.fd = fd, .events = POLLIN, .revents = 0};
            if (::poll(&pfd, 1, kPollMs) > 0)
            {
                ::nghttp2_session_recv(session);
                ::nghttp2_session_send(session);
            }
        }

        // Flush any remaining output after response is submitted.
        ::nghttp2_session_send(session);
        ::nghttp2_session_send(session);
        ::nghttp2_session_del(session);
    }

    int m_listen_fd = -1;
    int m_port = 0;
    std::thread m_thread;
    std::atomic<bool> m_response_done{false};
};

}  // namespace

// ---------------------------------------------------------------------------
// Test
// ---------------------------------------------------------------------------

TEST(Http2TransportSendIntegrationTest, Send_InsecureLoopback_Succeeds)
{
    MinimalHttp2RequestServer server;
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

    const auto connect_result = t->Connect(opts);
    ASSERT_TRUE(connect_result.has_value())
        << (connect_result.has_value() ? "" : connect_result.error().message);

    // Build a minimal POST request.
    const std::string authority = "127.0.0.1:" + std::to_string(port);
    mti::RequestSpec spec;
    spec.headers.push_back({":method", "POST"});
    spec.headers.push_back({":scheme", "http"});
    spec.headers.push_back({":path", "/test"});
    spec.headers.push_back({":authority", authority});
    spec.headers.push_back({"content-type", "application/grpc+proto"});
    spec.deadline = std::chrono::milliseconds(8000);

    // Small payload so we test the data-provider path.
    const std::array<std::byte, 4> payload_bytes{
        std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}};
    spec.payload = std::span<const std::byte>{payload_bytes};

    auto handle = t->Send(std::move(spec));
    EXPECT_NE(handle.Id(), 0U);
    ASSERT_TRUE(handle.Future().valid());

    const auto status = handle.Future().wait_for(std::chrono::seconds(8));
    ASSERT_EQ(status, std::future_status::ready);

    const auto result = handle.Future().get();
    EXPECT_TRUE(result.success) << (result.error ? result.error->message : "");

    bool found_status_200 = false;
    for (const auto& hdr : result.response_headers)
    {
        if (hdr.name == ":status" && hdr.value == "200")
        {
            found_status_200 = true;
            break;
        }
    }
    EXPECT_TRUE(found_status_200);

    EXPECT_TRUE(server.WaitForResponse(std::chrono::milliseconds(5000)));
    (void)t->Close(std::chrono::milliseconds(1000));
    server.Stop();
}
