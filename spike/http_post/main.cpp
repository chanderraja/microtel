// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// M1 spike — OTLP/HTTP-protobuf single-span end-to-end.
//
// THROWAWAY CODE. Deleted at the end of M1. See spike/README.md.
//
// Structure of this file is deliberately flat and procedural. No helper
// classes, no RAII wrappers, no abstractions — those land in src/ during
// M2/M3. Anything that looks polished here is a smell; it should look like
// a research notebook entry, not production.

// THROWAWAY: Direct includes of system headers. M3 wraps these via the
// RAII types in src/common/raii/.
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <openssl/err.h>
#include <openssl/ssl.h>

#include <nghttp2/nghttp2.h>

#include <cassert>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <ios>
#include <string>
#include <string_view>
#include <vector>

namespace {

// THROWAWAY: dump-and-exit error helpers. M3 returns Status / Error
// per docs/error-model.md.
[[noreturn]] void die(const char* what) {
    std::fprintf(stderr, "spike_http_post: %s: %s\n",
                 what, std::strerror(errno));
    std::exit(1);
}

[[noreturn]] void die_ssl(const char* what) {
    std::fprintf(stderr, "spike_http_post: %s: ", what);
    ERR_print_errors_fp(stderr);
    std::exit(1);
}

// THROWAWAY: tiny URL parser. Accepts only `https://host[:port][/path]`
// with explicit scheme. Production URL parsing is a config-track concern
// (Track E) and lives in src/common/config/.
struct Url {
    std::string host;
    std::string port;
    std::string path;          // includes leading '/', empty allowed
};

Url parse_url(std::string_view url) {
    Url out;
    constexpr std::string_view scheme = "https://";
    if (url.substr(0, scheme.size()) != scheme) {
        std::fprintf(stderr, "spike_http_post: url must start with https://\n");
        std::exit(2);
    }
    url.remove_prefix(scheme.size());

    auto slash = url.find('/');
    std::string_view authority = (slash == std::string_view::npos)
        ? url : url.substr(0, slash);
    out.path = (slash == std::string_view::npos)
        ? std::string{} : std::string{url.substr(slash)};

    auto colon = authority.find(':');
    if (colon == std::string_view::npos) {
        out.host = std::string{authority};
        out.port = "443";
    } else {
        out.host = std::string{authority.substr(0, colon)};
        out.port = std::string{authority.substr(colon + 1)};
    }
    return out;
}

// Read the protoc-encoded ExportTraceServiceRequest fixture compiled in via
// MICROTEL_SPIKE_FIXTURE. See spike/fixtures/README.md.
std::vector<std::uint8_t> read_fixture() {
    const char* path = MICROTEL_SPIKE_FIXTURE;
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        std::fprintf(stderr, "spike_http_post: cannot open fixture %s\n", path);
        std::exit(1);
    }
    return {std::istreambuf_iterator<char>(f),
            std::istreambuf_iterator<char>{}};
}

// THROWAWAY: blocking TCP connect via getaddrinfo. M3 uses src/common/raii/Socket.
int tcp_connect(const std::string& host, const std::string& port) {
    addrinfo hints{};
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    addrinfo* res = nullptr;
    int rc = ::getaddrinfo(host.c_str(), port.c_str(), &hints, &res);
    if (rc != 0) {
        std::fprintf(stderr, "spike_http_post: getaddrinfo: %s\n",
                     gai_strerror(rc));
        std::exit(1);
    }

    int fd = -1;
    for (addrinfo* p = res; p != nullptr; p = p->ai_next) {
        fd = ::socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd < 0) continue;
        if (::connect(fd, p->ai_addr, p->ai_addrlen) == 0) break;
        ::close(fd);
        fd = -1;
    }
    ::freeaddrinfo(res);
    if (fd < 0) die("connect");
    return fd;
}

// SSL setup with ALPN h2 + CA bundle. THROWAWAY; M3 wraps in SslCtx/SslSession.
SSL* ssl_setup(SSL_CTX* ctx, int fd, const std::string& host) {
    SSL* ssl = SSL_new(ctx);
    if (!ssl) die_ssl("SSL_new");

    // Server Name Indication.
    if (SSL_set_tlsext_host_name(ssl, host.c_str()) != 1) {
        die_ssl("SSL_set_tlsext_host_name");
    }

    if (SSL_set_fd(ssl, fd) != 1) die_ssl("SSL_set_fd");
    if (SSL_connect(ssl) != 1)    die_ssl("SSL_connect");

    // Confirm ALPN selected h2; if not, the spike has no path forward.
    const unsigned char* alpn = nullptr;
    unsigned int alpn_len     = 0;
    SSL_get0_alpn_selected(ssl, &alpn, &alpn_len);
    if (alpn_len != 2 || std::memcmp(alpn, "h2", 2) != 0) {
        std::fprintf(stderr, "spike_http_post: ALPN did not select h2 "
                             "(got %.*s)\n", static_cast<int>(alpn_len),
                     reinterpret_cast<const char*>(alpn));
        std::exit(1);
    }

    return ssl;
}

// In-flight request state. THROWAWAY: a single global. M3 distributes
// state across IExporter / IWireCodec / ITransport per docs/interfaces.md.
struct State {
    const std::vector<std::uint8_t>* request_body = nullptr;
    std::size_t                      request_pos  = 0;

    int                              http_status  = 0;
    std::vector<std::uint8_t>        response_body;
    bool                             stream_done  = false;
};

// nghttp2 data-provider read callback for the request body.
ssize_t request_read_cb(nghttp2_session* /*session*/,
                        std::int32_t /*stream_id*/,
                        std::uint8_t* buf, std::size_t length,
                        std::uint32_t* data_flags,
                        nghttp2_data_source* source,
                        void* /*user_data*/) {
    auto* st = static_cast<State*>(source->ptr);
    std::size_t remaining = st->request_body->size() - st->request_pos;
    std::size_t to_copy   = remaining < length ? remaining : length;
    std::memcpy(buf, st->request_body->data() + st->request_pos, to_copy);
    st->request_pos += to_copy;
    if (st->request_pos == st->request_body->size()) {
        *data_flags |= NGHTTP2_DATA_FLAG_EOF;
    }
    return static_cast<ssize_t>(to_copy);
}

int on_header_cb(nghttp2_session* /*session*/,
                 const nghttp2_frame* /*frame*/,
                 const std::uint8_t* name,  std::size_t name_len,
                 const std::uint8_t* value, std::size_t value_len,
                 std::uint8_t /*flags*/, void* user_data) {
    auto* st = static_cast<State*>(user_data);
    std::string_view n{reinterpret_cast<const char*>(name),  name_len};
    std::string_view v{reinterpret_cast<const char*>(value), value_len};
    if (n == ":status") {
        st->http_status = std::atoi(std::string{v}.c_str());
    }
    // THROWAWAY: log everything else; M3 keeps a structured response header
    // capture inside the codec instead.
    std::fprintf(stderr, "    header: %.*s = %.*s\n",
                 static_cast<int>(n.size()), n.data(),
                 static_cast<int>(v.size()), v.data());
    return 0;
}

int on_data_chunk_cb(nghttp2_session* /*session*/, std::uint8_t /*flags*/,
                     std::int32_t /*stream_id*/,
                     const std::uint8_t* data, std::size_t len,
                     void* user_data) {
    auto* st = static_cast<State*>(user_data);
    st->response_body.insert(st->response_body.end(), data, data + len);
    return 0;
}

int on_stream_close_cb(nghttp2_session* /*session*/,
                       std::int32_t /*stream_id*/,
                       std::uint32_t error_code, void* user_data) {
    auto* st = static_cast<State*>(user_data);
    st->stream_done = true;
    if (error_code != 0) {
        std::fprintf(stderr, "spike_http_post: stream closed with error 0x%x\n",
                     error_code);
    }
    return 0;
}

}  // namespace

int main(int argc, char* argv[]) {
    if (argc < 3) {
        std::fprintf(stderr,
            "usage: %s <https://host:port> <ca-bundle.pem>\n", argv[0]);
        return 2;
    }
    Url url        = parse_url(argv[1]);
    std::string ca = argv[2];

    // OTLP/HTTP path: append /v1/traces if the URL had no explicit path.
    // Per docs/configuration.md §3.3.
    std::string request_path = url.path.empty() || url.path == "/"
        ? "/v1/traces" : (url.path + "/v1/traces");

    auto body = read_fixture();
    std::fprintf(stderr,
        "spike_http_post: %zu byte fixture -> https://%s:%s%s\n",
        body.size(), url.host.c_str(), url.port.c_str(), request_path.c_str());

    // OpenSSL + ALPN h2 setup. THROWAWAY.
    SSL_library_init();
    SSL_load_error_strings();
    SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx) die_ssl("SSL_CTX_new");
    if (SSL_CTX_load_verify_locations(ctx, ca.c_str(), nullptr) != 1) {
        die_ssl("SSL_CTX_load_verify_locations");
    }
    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, nullptr);
    static constexpr unsigned char kAlpn[] = {2, 'h', '2'};
    if (SSL_CTX_set_alpn_protos(ctx, kAlpn, sizeof(kAlpn)) != 0) {
        die_ssl("SSL_CTX_set_alpn_protos");
    }

    int  fd  = tcp_connect(url.host, url.port);
    SSL* ssl = ssl_setup(ctx, fd, url.host);

    // nghttp2 client session.
    nghttp2_session_callbacks* cbs = nullptr;
    nghttp2_session_callbacks_new(&cbs);
    nghttp2_session_callbacks_set_on_header_callback(cbs, on_header_cb);
    nghttp2_session_callbacks_set_on_data_chunk_recv_callback(cbs, on_data_chunk_cb);
    nghttp2_session_callbacks_set_on_stream_close_callback(cbs, on_stream_close_cb);

    State state;
    state.request_body = &body;

    nghttp2_session* session = nullptr;
    if (nghttp2_session_client_new(&session, cbs, &state) != 0) {
        std::fprintf(stderr, "nghttp2_session_client_new failed\n");
        return 1;
    }
    nghttp2_session_callbacks_del(cbs);

    // Submit settings (mandatory first frame in HTTP/2).
    if (nghttp2_submit_settings(session, NGHTTP2_FLAG_NONE, nullptr, 0) != 0) {
        std::fprintf(stderr, "nghttp2_submit_settings failed\n");
        return 1;
    }

    // Build and submit the request.
    std::string authority = url.host + ":" + url.port;
    std::string content_length_str = std::to_string(body.size());

    auto nv = [](const char* name, const std::string& value) {
        return nghttp2_nv{
            const_cast<std::uint8_t*>(reinterpret_cast<const std::uint8_t*>(name)),
            const_cast<std::uint8_t*>(reinterpret_cast<const std::uint8_t*>(value.data())),
            std::strlen(name), value.size(),
            NGHTTP2_NV_FLAG_NONE
        };
    };
    std::string method_str = "POST";
    std::string scheme_str = "https";
    std::string ct_str     = "application/x-protobuf";
    std::string ua_str     = "microtel-cpp-spike/0.1";

    std::vector<nghttp2_nv> headers = {
        nv(":method",     method_str),
        nv(":scheme",     scheme_str),
        nv(":authority",  authority),
        nv(":path",       request_path),
        nv("content-type", ct_str),
        nv("content-length", content_length_str),
        nv("user-agent",  ua_str),
    };

    nghttp2_data_provider data_provider{};
    data_provider.source.ptr = &state;
    data_provider.read_callback = request_read_cb;

    std::int32_t stream_id = nghttp2_submit_request(
        session, nullptr, headers.data(), headers.size(),
        &data_provider, nullptr);
    if (stream_id < 0) {
        std::fprintf(stderr, "nghttp2_submit_request failed: %s\n",
                     nghttp2_strerror(stream_id));
        return 1;
    }

    // Drive the I/O loop synchronously. Read one chunk per outer pass.
    constexpr std::size_t kRecvBuf = 16 * 1024;
    std::vector<std::uint8_t> recv_buf(kRecvBuf);

    while (!state.stream_done &&
           (nghttp2_session_want_read(session) ||
            nghttp2_session_want_write(session))) {
        // Push out everything nghttp2 has queued.
        const std::uint8_t* out = nullptr;
        for (;;) {
            ssize_t n = nghttp2_session_mem_send(session, &out);
            if (n < 0) {
                std::fprintf(stderr, "nghttp2_session_mem_send: %s\n",
                             nghttp2_strerror(static_cast<int>(n)));
                return 1;
            }
            if (n == 0) break;
            std::size_t written = 0;
            while (written < static_cast<std::size_t>(n)) {
                int w = SSL_write(ssl, out + written,
                                  static_cast<int>(n - written));
                if (w <= 0) die_ssl("SSL_write");
                written += static_cast<std::size_t>(w);
            }
        }

        if (state.stream_done) break;
        if (!nghttp2_session_want_read(session)) break;

        int r = SSL_read(ssl, recv_buf.data(),
                         static_cast<int>(recv_buf.size()));
        if (r <= 0) die_ssl("SSL_read");

        ssize_t consumed = nghttp2_session_mem_recv(
            session, recv_buf.data(), static_cast<std::size_t>(r));
        if (consumed < 0) {
            std::fprintf(stderr, "nghttp2_session_mem_recv: %s\n",
                         nghttp2_strerror(static_cast<int>(consumed)));
            return 1;
        }
    }

    // Print the result.
    std::fprintf(stderr, "spike_http_post: HTTP %d, %zu byte body\n",
                 state.http_status, state.response_body.size());
    if (state.http_status >= 200 && state.http_status < 300) {
        std::printf("OK: span accepted\n");
    } else {
        std::printf("FAIL: HTTP %d\n", state.http_status);
    }

    // THROWAWAY: leak everything; the OS reaps on exit. M3 RAII-wraps each
    // resource so destruction is clean.
    nghttp2_session_del(session);
    SSL_shutdown(ssl);
    SSL_free(ssl);
    ::close(fd);
    SSL_CTX_free(ctx);

    return (state.http_status >= 200 && state.http_status < 300) ? 0 : 1;
}
