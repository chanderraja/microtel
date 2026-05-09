// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#include "http2_transport.hpp"

#include "microtel/error.hpp"
#include "microtel/internal/transport.hpp"

#include <nghttp2/nghttp2.h>
#include <openssl/ssl.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <future>
#include <memory>
#include <string>
#include <utility>

#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

namespace microtel::transport
{

// ---------------------------------------------------------------------------
// Internal helpers (anonymous namespace)
// ---------------------------------------------------------------------------

namespace
{

struct EndpointInfo
{
    std::string host;
    std::string port;
    bool use_tls = true;
};

microtel::Expected<EndpointInfo, microtel::Error> ParseEndpoint(const std::string& endpoint,
                                                                bool insecure)
{
    if (endpoint.empty())
    {
        return microtel::Unexpected<microtel::Error>{
            {.kind = microtel::Error::Kind::Network, .message = "empty endpoint"}};
    }

    std::string_view sv{endpoint};
    bool use_tls = !insecure;

    if (sv.starts_with("https://"))
    {
        use_tls = true;
        sv.remove_prefix(8);
    }
    else if (sv.starts_with("http://"))
    {
        use_tls = false;
        sv.remove_prefix(7);
    }

    const auto colon = sv.rfind(':');
    std::string host;
    std::string port;
    if (colon == std::string_view::npos)
    {
        host = std::string{sv};
        port = use_tls ? "443" : "80";
    }
    else
    {
        host = std::string{sv.substr(0, colon)};
        port = std::string{sv.substr(colon + 1)};
    }

    if (host.empty() || port.empty())
    {
        return microtel::Unexpected<microtel::Error>{
            {.kind = microtel::Error::Kind::Network, .message = "malformed endpoint"}};
    }
    return EndpointInfo{.host = std::move(host), .port = std::move(port), .use_tls = use_tls};
}

microtel::Expected<common::raii::UniqueFd, microtel::Error> TcpConnect(
    const std::string& host, const std::string& port, std::chrono::milliseconds timeout)
{
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    addrinfo* res = nullptr;
    if (::getaddrinfo(host.c_str(), port.c_str(), &hints, &res) != 0)
    {
        return microtel::Unexpected<microtel::Error>{
            {.kind = microtel::Error::Kind::Network, .message = "DNS resolution failed"}};
    }
    const std::unique_ptr<addrinfo, decltype(&::freeaddrinfo)> guard{res, &::freeaddrinfo};

    const auto deadline = std::chrono::steady_clock::now() + timeout;

    for (const addrinfo* ai = res; ai != nullptr; ai = ai->ai_next)
    {
        // NOLINTBEGIN(cppcoreguidelines-pro-type-vararg,hicpp-vararg,hicpp-signed-bitwise)
        common::raii::UniqueFd fd{
            ::socket(ai->ai_family, ai->ai_socktype | SOCK_CLOEXEC, ai->ai_protocol)};
        // NOLINTEND(cppcoreguidelines-pro-type-vararg,hicpp-vararg,hicpp-signed-bitwise)
        if (!fd.IsValid())
        {
            continue;
        }

        // Non-blocking connect so we can enforce the timeout.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg,hicpp-vararg,hicpp-signed-bitwise)
        ::fcntl(fd.Get(), F_SETFL, ::fcntl(fd.Get(), F_GETFL) | O_NONBLOCK);

        const int ret = ::connect(fd.Get(), ai->ai_addr, ai->ai_addrlen);
        if (ret == 0)
        {
            return fd;
        }
        if (errno != EINPROGRESS)
        {
            continue;
        }

        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline)
        {
            return microtel::Unexpected<microtel::Error>{
                {.kind = microtel::Error::Kind::Cancelled, .message = "connect timeout"}};
        }
        const auto ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
        pollfd pfd{.fd = fd.Get(), .events = POLLOUT, .revents = 0};
        if (::poll(&pfd, 1, static_cast<int>(ms)) <= 0)
        {
            continue;
        }

        int sock_err = 0;
        socklen_t sock_err_len = sizeof(sock_err);
        ::getsockopt(fd.Get(), SOL_SOCKET, SO_ERROR, &sock_err, &sock_err_len);
        if (sock_err != 0)
        {
            continue;
        }
        return fd;
    }

    return microtel::Unexpected<microtel::Error>{
        {.kind = microtel::Error::Kind::Network, .message = "connection refused"}};
}

// ---------------------------------------------------------------------------
// SSL helpers (reduce nesting depth and cognitive complexity)
// ---------------------------------------------------------------------------

std::ptrdiff_t SslSend(SSL* ssl, const std::uint8_t* data, std::size_t len) noexcept
{
    const int n = ::SSL_write(ssl, data, static_cast<int>(len));
    if (n <= 0)
    {
        const int err = ::SSL_get_error(ssl, n);
        if (err == SSL_ERROR_WANT_WRITE || err == SSL_ERROR_WANT_READ)
        {
            return NGHTTP2_ERR_WOULDBLOCK;
        }
        return NGHTTP2_ERR_CALLBACK_FAILURE;
    }
    return n;
}

std::ptrdiff_t SslRecv(SSL* ssl, std::uint8_t* buf, std::size_t len) noexcept
{
    const int n = ::SSL_read(ssl, buf, static_cast<int>(len));
    if (n <= 0)
    {
        const int err = ::SSL_get_error(ssl, n);
        if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE)
        {
            return NGHTTP2_ERR_WOULDBLOCK;
        }
        return NGHTTP2_ERR_CALLBACK_FAILURE;
    }
    return n;
}

microtel::Expected<void, microtel::Error> LoadSslCtxCredentials(
    SSL_CTX* ctx, const internal::ConnectOptions& opts)
{
    if (!opts.ca_bundle.empty())
    {
        if (::SSL_CTX_load_verify_locations(ctx, opts.ca_bundle.string().c_str(), nullptr) != 1)
        {
            return microtel::Unexpected<microtel::Error>{
                {.kind = microtel::Error::Kind::Network, .message = "CA bundle load failed"}};
        }
    }
    else
    {
        ::SSL_CTX_set_default_verify_paths(ctx);
    }

    if (!opts.client_cert.empty())
    {
        if (::SSL_CTX_use_certificate_file(
                ctx, opts.client_cert.string().c_str(), SSL_FILETYPE_PEM) != 1)
        {
            return microtel::Unexpected<microtel::Error>{
                {.kind = microtel::Error::Kind::Network, .message = "client cert load failed"}};
        }
        if (::SSL_CTX_use_PrivateKey_file(
                ctx, opts.client_key.string().c_str(), SSL_FILETYPE_PEM) != 1)
        {
            return microtel::Unexpected<microtel::Error>{
                {.kind = microtel::Error::Kind::Network, .message = "client key load failed"}};
        }
    }
    return {};
}

microtel::Expected<void, microtel::Error> SslConnectLoop(
    SSL* ssl, int fd, std::chrono::steady_clock::time_point deadline)
{
    while (true)
    {
        const int ret = ::SSL_connect(ssl);
        if (ret == 1)
        {
            return {};
        }
        const int err = ::SSL_get_error(ssl, ret);
        const int ev = (err == SSL_ERROR_WANT_READ) ? POLLIN : POLLOUT;

        if (err != SSL_ERROR_WANT_READ && err != SSL_ERROR_WANT_WRITE)
        {
            return microtel::Unexpected<microtel::Error>{
                {.kind = microtel::Error::Kind::Network, .message = "TLS handshake failed"}};
        }

        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline)
        {
            return microtel::Unexpected<microtel::Error>{
                {.kind = microtel::Error::Kind::Cancelled, .message = "TLS handshake timeout"}};
        }
        const auto ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
        pollfd pfd{.fd = fd, .events = static_cast<short>(ev), .revents = 0};
        if (::poll(&pfd, 1, static_cast<int>(ms)) <= 0)
        {
            return microtel::Unexpected<microtel::Error>{
                {.kind = microtel::Error::Kind::Cancelled, .message = "TLS handshake timeout"}};
        }
    }
}

// ---------------------------------------------------------------------------
// nghttp2 C-callback trampolines — userdata is Http2Transport*
// ---------------------------------------------------------------------------

ssize_t NgHttp2SendCb(
    nghttp2_session* /*s*/, const uint8_t* data, size_t len, int /*flags*/, void* ud) noexcept
{
    return static_cast<ssize_t>(static_cast<Http2Transport*>(ud)->NgHttp2DoSend(data, len));
}

ssize_t NgHttp2RecvCb(
    nghttp2_session* /*s*/, uint8_t* buf, size_t len, int /*flags*/, void* ud) noexcept
{
    return static_cast<ssize_t>(static_cast<Http2Transport*>(ud)->NgHttp2DoRecv(buf, len));
}

int NgHttp2OnFrameRecvCb(nghttp2_session* /*s*/, const nghttp2_frame* frame, void* ud) noexcept
{
    if (frame->hd.type == NGHTTP2_SETTINGS && (frame->hd.flags & NGHTTP2_FLAG_ACK) != 0U)
    {
        static_cast<Http2Transport*>(ud)->OnSettingsAck();
    }
    return 0;
}

constexpr int kPollIntervalMs = 50;

}  // namespace

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

Http2Transport::Http2Transport(std::unique_ptr<internal::IReactor> reactor) noexcept
    : m_reactor(std::move(reactor))
{
}

// static
microtel::Expected<std::unique_ptr<Http2Transport>, microtel::Error> Http2Transport::Create(
    std::unique_ptr<internal::IReactor> reactor) noexcept
{
    if (!reactor)
    {
        return microtel::Unexpected<microtel::Error>{microtel::Error{
            .kind = microtel::Error::Kind::InternalFailure, .message = "null reactor"}};
    }
    try
    {
        // Private constructor; make_unique can't reach it.
        // NOLINTNEXTLINE(cppcoreguidelines-owning-memory)
        auto t = std::unique_ptr<Http2Transport>(new Http2Transport(std::move(reactor)));
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
    const internal::ConnectOptions& opts)
{
    // Guard: only transition from Disconnected → Connecting.
    microtel::ConnectionState expected = microtel::ConnectionState::Disconnected;
    if (!m_state.compare_exchange_strong(expected,
                                         microtel::ConnectionState::Connecting,
                                         std::memory_order_acq_rel,
                                         std::memory_order_acquire))
    {
        const auto* const msg = (expected == microtel::ConnectionState::Closed)
                                    ? "transport is closed"
                                    : "already connecting";
        return microtel::Unexpected<microtel::Error>{
            {.kind = microtel::Error::Kind::Network, .message = msg}};
    }

    auto rollback = [this]() noexcept
    {
        m_nghttp2_session.Reset();
        m_ssl_session.Reset();
        m_ssl_ctx.Reset();
        m_socket.Close();
        m_state.store(microtel::ConnectionState::Disconnected, std::memory_order_release);
    };

    auto ep = ParseEndpoint(opts.endpoint, opts.insecure);
    if (!ep)
    {
        rollback();
        return microtel::Unexpected<microtel::Error>{ep.error()};
    }

    auto fd = TcpConnect(ep->host, ep->port, opts.connect_timeout);
    if (!fd)
    {
        rollback();
        return microtel::Unexpected<microtel::Error>{fd.error()};
    }
    m_socket = std::move(*fd);

    if (ep->use_tls)
    {
        auto tls = TlsHandshake(opts, ep->host);
        if (!tls)
        {
            rollback();
            return microtel::Unexpected<microtel::Error>{tls.error()};
        }
        m_ssl_ctx = std::move(tls->first);
        m_ssl_session = std::move(tls->second);
    }

    auto h2 = Http2Handshake(opts);
    if (!h2)
    {
        rollback();
        return microtel::Unexpected<microtel::Error>{h2.error()};
    }
    m_nghttp2_session = std::move(*h2);

    auto reg = m_reactor->Register(m_socket.Get(),
                                   internal::EventMask::Read | internal::EventMask::Error,
                                   [this](int fd, internal::EventMask ev) { OnIoEvent(fd, ev); });
    if (!reg)
    {
        rollback();
        return microtel::Unexpected<microtel::Error>{
            {.kind = microtel::Error::Kind::InternalFailure, .message = "reactor register failed"}};
    }

    m_state.store(microtel::ConnectionState::Connected, std::memory_order_release);
    return {};
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

    // I/O thread has stopped — safe to tear down connection resources.
    if (m_nghttp2_session.IsValid())
    {
        m_reactor->Unregister(m_socket.Get());
    }
    m_nghttp2_session.Reset();
    m_ssl_session.Reset();
    m_ssl_ctx.Reset();
    m_socket.Close();

    return microtel::Status::Completed;
}

// ---------------------------------------------------------------------------
// ITransport — request handling (M3-D5)
// ---------------------------------------------------------------------------

internal::RequestHandle Http2Transport::Send(internal::RequestSpec /*spec*/) noexcept
{
    std::promise<internal::TransportResult> p;
    internal::TransportResult result;
    result.error =
        microtel::Error{.kind = microtel::Error::Kind::Network, .message = "not connected"};
    p.set_value(std::move(result));
    return internal::RequestHandle{0, p.get_future()};
}

void Http2Transport::Cancel(const internal::RequestHandle& /*handle*/) noexcept
{
    // M3-D5: RST_STREAM on the in-flight stream.
}

// ---------------------------------------------------------------------------
// TLS handshake
// ---------------------------------------------------------------------------

microtel::Expected<std::pair<common::raii::SslCtx, common::raii::SslSession>, microtel::Error>
Http2Transport::TlsHandshake(const internal::ConnectOptions& opts, const std::string& host)
{
    common::raii::SslCtx ssl_ctx{::SSL_CTX_new(::TLS_client_method())};
    if (!ssl_ctx.IsValid())
    {
        return microtel::Unexpected<microtel::Error>{
            {.kind = microtel::Error::Kind::Network, .message = "SSL_CTX_new failed"}};
    }

    auto creds = LoadSslCtxCredentials(ssl_ctx.Get(), opts);
    if (!creds)
    {
        return microtel::Unexpected<microtel::Error>{creds.error()};
    }

    // Advertise HTTP/2 via ALPN.
    static constexpr uint8_t kAlpn[] = {2, 'h', '2'};
    ::SSL_CTX_set_alpn_protos(ssl_ctx.Get(), kAlpn, sizeof(kAlpn));

    common::raii::SslSession ssl{::SSL_new(ssl_ctx.Get())};
    if (!ssl.IsValid())
    {
        return microtel::Unexpected<microtel::Error>{
            {.kind = microtel::Error::Kind::Network, .message = "SSL_new failed"}};
    }

    ::SSL_set_fd(ssl.Get(), m_socket.Get());
    ::SSL_set_connect_state(ssl.Get());

    const std::string& sni = opts.sni_override.empty() ? host : opts.sni_override;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    ::SSL_set_tlsext_host_name(ssl.Get(), reinterpret_cast<const void*>(sni.c_str()));

    const auto deadline = std::chrono::steady_clock::now() + opts.tls_handshake_timeout;
    auto conn = SslConnectLoop(ssl.Get(), m_socket.Get(), deadline);
    if (!conn)
    {
        return microtel::Unexpected<microtel::Error>{conn.error()};
    }

    return std::make_pair(std::move(ssl_ctx), std::move(ssl));
}

// ---------------------------------------------------------------------------
// nghttp2 SETTINGS exchange
// ---------------------------------------------------------------------------

microtel::Expected<common::raii::Nghttp2Session, microtel::Error> Http2Transport::Http2Handshake(
    const internal::ConnectOptions& opts)
{
    nghttp2_session_callbacks* cbs = nullptr;
    ::nghttp2_session_callbacks_new(&cbs);
    ::nghttp2_session_callbacks_set_send_callback(cbs, NgHttp2SendCb);
    ::nghttp2_session_callbacks_set_recv_callback(cbs, NgHttp2RecvCb);
    ::nghttp2_session_callbacks_set_on_frame_recv_callback(cbs, NgHttp2OnFrameRecvCb);

    nghttp2_session* raw = nullptr;
    ::nghttp2_session_client_new(&raw, cbs, this);
    ::nghttp2_session_callbacks_del(cbs);

    common::raii::Nghttp2Session session{raw};
    if (!session.IsValid())
    {
        return microtel::Unexpected<microtel::Error>{
            {.kind = microtel::Error::Kind::InternalFailure,
             .message = "nghttp2_session_client_new failed"}};
    }

    const nghttp2_settings_entry iv[2] = {
        {NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, opts.max_concurrent_streams},
        {NGHTTP2_SETTINGS_INITIAL_WINDOW_SIZE, opts.initial_window_size},
    };
    ::nghttp2_submit_settings(session.Get(), NGHTTP2_FLAG_NONE, iv, 2);

    // Send client connection preface + initial SETTINGS.
    ::nghttp2_session_send(session.Get());

    m_settings_ack_received.store(false, std::memory_order_release);
    const auto deadline = std::chrono::steady_clock::now() + opts.connect_timeout;

    while (!m_settings_ack_received.load(std::memory_order_acquire))
    {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline)
        {
            return microtel::Unexpected<microtel::Error>{
                {.kind = microtel::Error::Kind::Cancelled, .message = "SETTINGS ACK timeout"}};
        }
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
        const int poll_ms =
            (remaining < kPollIntervalMs) ? static_cast<int>(remaining) : kPollIntervalMs;
        pollfd pfd{.fd = m_socket.Get(), .events = POLLIN, .revents = 0};
        const int rv = (::poll(&pfd, 1, poll_ms) > 0) ? ::nghttp2_session_recv(session.Get()) : 0;
        // nghttp2_session_recv may have fired OnSettingsAck (m_settings_ack_received = true)
        // and then read a FIN if the server closed immediately after — that EOF is not an error.
        if (!m_settings_ack_received.load(std::memory_order_acquire) && rv != 0 &&
            rv != NGHTTP2_ERR_WOULDBLOCK)
        {
            return microtel::Unexpected<microtel::Error>{
                {.kind = microtel::Error::Kind::Network,
                 .message = "nghttp2 recv failed during SETTINGS exchange"}};
        }
        ::nghttp2_session_send(session.Get());
    }

    return session;
}

// ---------------------------------------------------------------------------
// nghttp2 send / recv — called from the C trampolines
// ---------------------------------------------------------------------------

std::ptrdiff_t Http2Transport::NgHttp2DoSend(const std::uint8_t* data, std::size_t len) noexcept
{
    if (m_ssl_session.IsValid())
    {
        return SslSend(m_ssl_session.Get(), data, len);
    }

    ssize_t n = ::write(m_socket.Get(), data, len);
    while (n < 0 && errno == EINTR)
    {
        n = ::write(m_socket.Get(), data, len);
    }

    if (n < 0)
    {
        return (errno == EAGAIN || errno == EWOULDBLOCK) ? NGHTTP2_ERR_WOULDBLOCK
                                                         : NGHTTP2_ERR_CALLBACK_FAILURE;
    }
    return static_cast<std::ptrdiff_t>(n);
}

std::ptrdiff_t Http2Transport::NgHttp2DoRecv(std::uint8_t* buf, std::size_t len) noexcept
{
    if (m_ssl_session.IsValid())
    {
        return SslRecv(m_ssl_session.Get(), buf, len);
    }

    ssize_t n = ::read(m_socket.Get(), buf, len);
    while (n < 0 && errno == EINTR)
    {
        n = ::read(m_socket.Get(), buf, len);
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
    return static_cast<std::ptrdiff_t>(n);
}

void Http2Transport::OnSettingsAck() noexcept
{
    m_settings_ack_received.store(true, std::memory_order_release);
}

// ---------------------------------------------------------------------------
// I/O thread
// ---------------------------------------------------------------------------

void Http2Transport::IoThreadLoop() noexcept
{
    while (!m_stop.load(std::memory_order_acquire))
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
        m_reactor->WaitAndDispatch(deadline);
    }
}

void Http2Transport::OnIoEvent(int fd, internal::EventMask events) noexcept
{
    if (m_state.load(std::memory_order_acquire) != microtel::ConnectionState::Connected)
    {
        return;
    }

    if (internal::HasEvent(events, internal::EventMask::Error))
    {
        m_state.store(microtel::ConnectionState::Disconnected, std::memory_order_release);
        return;
    }

    if (internal::HasEvent(events, internal::EventMask::Read))
    {
        if (::nghttp2_session_recv(m_nghttp2_session.Get()) != 0)
        {
            m_state.store(microtel::ConnectionState::Disconnected, std::memory_order_release);
            return;
        }
    }

    ::nghttp2_session_send(m_nghttp2_session.Get());

    const bool want_write = ::nghttp2_session_want_write(m_nghttp2_session.Get()) != 0;
    const auto interest =
        want_write
            ? (internal::EventMask::Read | internal::EventMask::Write | internal::EventMask::Error)
            : (internal::EventMask::Read | internal::EventMask::Error);
    m_reactor->Modify(fd, interest);
}

}  // namespace microtel::transport
