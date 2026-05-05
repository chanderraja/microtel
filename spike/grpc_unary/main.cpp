// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// M1 spike — OTLP/gRPC unary on nghttp2, no gRPC library.
//
// THROWAWAY CODE. Deleted at the end of M1. See spike/README.md.
//
// Validates the load-bearing claim of the project: gRPC unary works on top
// of nghttp2 + OpenSSL with only the 5-byte length-prefix framing and the
// trailer HEADERS frame handled in user code. No gRPC library link.
//
// This binary exercises three wire-protocol variants per
// docs/grpc-wire-protocol.md §7. All three are run from the start so the
// happy path doesn't paper over framing or trailer bugs that the harder
// variants would catch.
//
// Variant selection: `argv[3]` ∈ { "happy", "trailer_only", "split_frame" }.

// THROWAWAY: direct system + library includes; no project-internal headers.
#include <netdb.h>
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

// --- Variant selection ----------------------------------------------------

enum class Variant {
    Happy,        // single DATA frame, valid request, expect grpc-status: 0.
    TrailerOnly,  // bad path → collector emits trailer-only response with
                  // grpc-status: 12 (UNIMPLEMENTED) in initial HEADERS.
    SplitFrame,   // request body split: prefix-only DATA frame, then body
                  // DATA frame. Collector should still accept.
};

Variant parse_variant(std::string_view name) {
    if (name == "happy")        return Variant::Happy;
    if (name == "trailer_only") return Variant::TrailerOnly;
    if (name == "split_frame")  return Variant::SplitFrame;
    std::fprintf(stderr, "unknown variant: %.*s\n",
                 static_cast<int>(name.size()), name.data());
    std::exit(2);
}

const char* variant_name(Variant v) {
    switch (v) {
        case Variant::Happy:       return "happy";
        case Variant::TrailerOnly: return "trailer_only";
        case Variant::SplitFrame:  return "split_frame";
    }
    return "?";
}

// THROWAWAY: dump-and-exit error helpers.
[[noreturn]] void die(const char* what) {
    std::fprintf(stderr, "spike_grpc_unary: %s: %s\n",
                 what, std::strerror(errno));
    std::exit(1);
}
[[noreturn]] void die_ssl(const char* what) {
    std::fprintf(stderr, "spike_grpc_unary: %s: ", what);
    ERR_print_errors_fp(stderr);
    std::exit(1);
}

// --- URL parsing + TCP/TLS setup (copied from spike_http_post; spike code
//     is throwaway, no shared header) -----------------------------------

struct Url {
    std::string host;
    std::string port;
};

Url parse_authority(std::string_view url) {
    Url out;
    constexpr std::string_view scheme = "https://";
    if (url.substr(0, scheme.size()) != scheme) {
        std::fprintf(stderr, "url must start with https://\n");
        std::exit(2);
    }
    url.remove_prefix(scheme.size());
    auto slash = url.find('/');
    std::string_view authority = (slash == std::string_view::npos)
        ? url : url.substr(0, slash);
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

std::vector<std::uint8_t> read_fixture() {
    std::ifstream f(MICROTEL_SPIKE_FIXTURE, std::ios::binary);
    if (!f) {
        std::fprintf(stderr, "cannot open fixture %s\n", MICROTEL_SPIKE_FIXTURE);
        std::exit(1);
    }
    return {std::istreambuf_iterator<char>(f),
            std::istreambuf_iterator<char>{}};
}

int tcp_connect(const std::string& host, const std::string& port) {
    addrinfo hints{};
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    int rc = ::getaddrinfo(host.c_str(), port.c_str(), &hints, &res);
    if (rc != 0) {
        std::fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rc));
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

SSL* ssl_setup(SSL_CTX* ctx, int fd, const std::string& host) {
    SSL* ssl = SSL_new(ctx);
    if (!ssl) die_ssl("SSL_new");
    if (SSL_set_tlsext_host_name(ssl, host.c_str()) != 1) {
        die_ssl("SSL_set_tlsext_host_name");
    }
    if (SSL_set_fd(ssl, fd) != 1) die_ssl("SSL_set_fd");
    if (SSL_connect(ssl) != 1)    die_ssl("SSL_connect");

    const unsigned char* alpn = nullptr;
    unsigned int alpn_len     = 0;
    SSL_get0_alpn_selected(ssl, &alpn, &alpn_len);
    if (alpn_len != 2 || std::memcmp(alpn, "h2", 2) != 0) {
        std::fprintf(stderr, "ALPN did not select h2 (got %.*s)\n",
                     static_cast<int>(alpn_len),
                     reinterpret_cast<const char*>(alpn));
        std::exit(1);
    }
    return ssl;
}

// --- gRPC framing ---------------------------------------------------------

// Builds the gRPC unary message: 5-byte prefix [CF=0x00][length BE 4] + body.
std::vector<std::uint8_t> wrap_grpc(const std::vector<std::uint8_t>& body) {
    std::vector<std::uint8_t> out(5 + body.size());
    out[0] = 0x00;
    std::uint32_t len = static_cast<std::uint32_t>(body.size());
    out[1] = static_cast<std::uint8_t>((len >> 24) & 0xff);
    out[2] = static_cast<std::uint8_t>((len >> 16) & 0xff);
    out[3] = static_cast<std::uint8_t>((len >>  8) & 0xff);
    out[4] = static_cast<std::uint8_t>( len        & 0xff);
    std::memcpy(out.data() + 5, body.data(), body.size());
    return out;
}

// --- nghttp2 callbacks + state -------------------------------------------

// Per-stream state collected from the response. THROWAWAY: a single global
// suffices for the spike; M3 distributes this across IWireCodec.
struct State {
    Variant                          variant = Variant::Happy;

    // Request body: gRPC-framed bytes.
    std::vector<std::uint8_t>        request_body;
    std::size_t                      request_pos        = 0;
    bool                             split_frame_first  = true;  // SplitFrame only

    // Response state.
    int                              http_status        = 0;
    std::string                      grpc_status;        // raw value, decimal
    std::string                      grpc_message;
    bool                             initial_headers    = true;  // false after first END_HEADERS
    bool                             initial_end_stream = false; // initial HEADERS had END_STREAM?
    std::vector<std::uint8_t>        response_data;      // raw DATA bytes
    bool                             stream_done        = false;
};

// Data provider — returns bytes from `request_body`. For SplitFrame, the
// first invocation returns only the 5-byte prefix without setting EOF; the
// second returns the rest with EOF.
ssize_t request_read_cb(nghttp2_session* /*session*/,
                        std::int32_t /*stream_id*/,
                        std::uint8_t* buf, std::size_t length,
                        std::uint32_t* data_flags,
                        nghttp2_data_source* source,
                        void* /*user_data*/) {
    auto* st = static_cast<State*>(source->ptr);

    if (st->variant == Variant::SplitFrame && st->split_frame_first) {
        // First chunk: exactly the 5-byte gRPC prefix. Do NOT set EOF;
        // nghttp2 emits a DATA frame with these 5 bytes, then calls the
        // callback again for the body.
        std::size_t to_copy = std::min<std::size_t>(5, length);
        std::memcpy(buf, st->request_body.data() + st->request_pos, to_copy);
        st->request_pos += to_copy;
        st->split_frame_first = false;
        std::fprintf(stderr, "    request DATA chunk: %zu bytes (prefix only)\n",
                     to_copy);
        return static_cast<ssize_t>(to_copy);
    }

    std::size_t remaining = st->request_body.size() - st->request_pos;
    std::size_t to_copy   = remaining < length ? remaining : length;
    std::memcpy(buf, st->request_body.data() + st->request_pos, to_copy);
    st->request_pos += to_copy;
    if (st->request_pos == st->request_body.size()) {
        *data_flags |= NGHTTP2_DATA_FLAG_EOF;
    }
    if (to_copy > 0) {
        std::fprintf(stderr, "    request DATA chunk: %zu bytes%s\n",
                     to_copy,
                     (*data_flags & NGHTTP2_DATA_FLAG_EOF) ? " (EOF)" : "");
    }
    return static_cast<ssize_t>(to_copy);
}

// Capture every response header. Distinguish initial-vs-trailer via
// frame->headers.cat. NGHTTP2_HCAT_RESPONSE is the first HEADERS frame on a
// client stream; NGHTTP2_HCAT_HEADERS is anything subsequent (i.e. trailers
// for OTLP unary, since we never see PUSH_RESPONSE on a client).
//
// FINDING (likely ICP candidate): docs/interfaces.md §4.3 says the codec
// parses `grpc-status` from trailers, but nghttp2 surfaces every header via
// the same callback. The codec must track frame->headers.cat (or equivalent
// per-frame state) to decide which bucket each header lands in. Worth
// clarifying in the contract.
int on_header_cb(nghttp2_session* /*session*/,
                 const nghttp2_frame* frame,
                 const std::uint8_t* name,  std::size_t name_len,
                 const std::uint8_t* value, std::size_t value_len,
                 std::uint8_t /*flags*/, void* user_data) {
    auto* st = static_cast<State*>(user_data);
    std::string_view n{reinterpret_cast<const char*>(name),  name_len};
    std::string_view v{reinterpret_cast<const char*>(value), value_len};

    const bool is_trailer =
        (frame->headers.cat == NGHTTP2_HCAT_HEADERS);

    if (n == ":status") {
        st->http_status = std::atoi(std::string{v}.c_str());
    } else if (n == "grpc-status") {
        st->grpc_status = std::string{v};
    } else if (n == "grpc-message") {
        st->grpc_message = std::string{v};
    }

    std::fprintf(stderr, "    %s header: %.*s = %.*s\n",
                 is_trailer ? "trailer" : "initial",
                 static_cast<int>(n.size()), n.data(),
                 static_cast<int>(v.size()), v.data());
    return 0;
}

int on_frame_recv_cb(nghttp2_session* /*session*/,
                     const nghttp2_frame* frame, void* user_data) {
    auto* st = static_cast<State*>(user_data);
    if (frame->hd.type == NGHTTP2_HEADERS) {
        const bool end_stream =
            (frame->hd.flags & NGHTTP2_FLAG_END_STREAM) != 0;
        if (frame->headers.cat == NGHTTP2_HCAT_RESPONSE) {
            // First HEADERS frame on this client stream.
            st->initial_headers = false;
            st->initial_end_stream = end_stream;
            std::fprintf(stderr, "    [initial HEADERS%s]\n",
                         end_stream ? " + END_STREAM (trailer-only)" : "");
        } else if (frame->headers.cat == NGHTTP2_HCAT_HEADERS) {
            std::fprintf(stderr, "    [trailer HEADERS%s]\n",
                         end_stream ? " + END_STREAM" : "");
        }
    } else if (frame->hd.type == NGHTTP2_DATA) {
        std::fprintf(stderr, "    [response DATA frame: %zu bytes%s]\n",
                     static_cast<std::size_t>(frame->hd.length),
                     (frame->hd.flags & NGHTTP2_FLAG_END_STREAM)
                         ? " + END_STREAM" : "");
    }
    return 0;
}

int on_data_chunk_cb(nghttp2_session* /*session*/, std::uint8_t /*flags*/,
                     std::int32_t /*stream_id*/,
                     const std::uint8_t* data, std::size_t len,
                     void* user_data) {
    auto* st = static_cast<State*>(user_data);
    st->response_data.insert(st->response_data.end(), data, data + len);
    return 0;
}

int on_stream_close_cb(nghttp2_session* /*session*/,
                       std::int32_t /*stream_id*/,
                       std::uint32_t error_code, void* user_data) {
    auto* st = static_cast<State*>(user_data);
    st->stream_done = true;
    if (error_code != 0) {
        std::fprintf(stderr, "stream closed with error 0x%x\n", error_code);
    }
    return 0;
}

// --- Build the request ---------------------------------------------------

constexpr const char* kOtlpExportPath =
    "/opentelemetry.proto.collector.trace.v1.TraceService/Export";

// For TrailerOnly variant: bogus path → expected UNIMPLEMENTED.
constexpr const char* kBogusPath =
    "/spike.deliberately.unknown.Service/NoSuchMethod";

// --- main ---------------------------------------------------------------

}  // namespace

int main(int argc, char* argv[]) {
    if (argc < 4) {
        std::fprintf(stderr,
            "usage: %s <https://host:port> <ca-bundle.pem> "
            "<happy|trailer_only|split_frame>\n",
            argv[0]);
        return 2;
    }
    Url url     = parse_authority(argv[1]);
    std::string ca = argv[2];
    Variant variant = parse_variant(argv[3]);

    auto fixture = read_fixture();
    auto framed  = wrap_grpc(fixture);

    std::fprintf(stderr,
        "spike_grpc_unary[%s]: %zu byte fixture (%zu byte gRPC-framed) -> "
        "https://%s:%s%s\n",
        variant_name(variant),
        fixture.size(), framed.size(),
        url.host.c_str(), url.port.c_str(),
        variant == Variant::TrailerOnly ? kBogusPath : kOtlpExportPath);

    // OpenSSL + ALPN h2.
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

    nghttp2_session_callbacks* cbs = nullptr;
    nghttp2_session_callbacks_new(&cbs);
    nghttp2_session_callbacks_set_on_header_callback(cbs, on_header_cb);
    nghttp2_session_callbacks_set_on_frame_recv_callback(cbs, on_frame_recv_cb);
    nghttp2_session_callbacks_set_on_data_chunk_recv_callback(cbs, on_data_chunk_cb);
    nghttp2_session_callbacks_set_on_stream_close_callback(cbs, on_stream_close_cb);

    State state;
    state.variant = variant;
    state.request_body = std::move(framed);

    nghttp2_session* session = nullptr;
    if (nghttp2_session_client_new(&session, cbs, &state) != 0) {
        std::fprintf(stderr, "nghttp2_session_client_new failed\n");
        return 1;
    }
    nghttp2_session_callbacks_del(cbs);

    if (nghttp2_submit_settings(session, NGHTTP2_FLAG_NONE, nullptr, 0) != 0) {
        std::fprintf(stderr, "nghttp2_submit_settings failed\n");
        return 1;
    }

    // Build headers.
    std::string authority = url.host + ":" + url.port;
    std::string method_str  = "POST";
    std::string scheme_str  = "https";
    std::string ct_str      = "application/grpc+proto";
    std::string te_str      = "trailers";
    std::string ua_str      = "microtel-cpp-spike/0.1";
    std::string accept_enc  = "identity";
    std::string path_str    = (variant == Variant::TrailerOnly)
        ? kBogusPath : kOtlpExportPath;

    auto nv = [](const char* name, const std::string& value) {
        return nghttp2_nv{
            const_cast<std::uint8_t*>(reinterpret_cast<const std::uint8_t*>(name)),
            const_cast<std::uint8_t*>(reinterpret_cast<const std::uint8_t*>(value.data())),
            std::strlen(name), value.size(),
            NGHTTP2_NV_FLAG_NONE
        };
    };

    std::vector<nghttp2_nv> headers = {
        nv(":method",            method_str),
        nv(":scheme",            scheme_str),
        nv(":authority",         authority),
        nv(":path",              path_str),
        nv("te",                 te_str),
        nv("content-type",       ct_str),
        nv("user-agent",         ua_str),
        nv("grpc-accept-encoding", accept_enc),
    };

    nghttp2_data_provider data_provider{};
    data_provider.source.ptr   = &state;
    data_provider.read_callback = request_read_cb;

    std::int32_t stream_id = nghttp2_submit_request(
        session, nullptr, headers.data(), headers.size(),
        &data_provider, nullptr);
    if (stream_id < 0) {
        std::fprintf(stderr, "nghttp2_submit_request failed: %s\n",
                     nghttp2_strerror(stream_id));
        return 1;
    }

    // Drive the I/O loop synchronously.
    constexpr std::size_t kRecvBuf = 16 * 1024;
    std::vector<std::uint8_t> recv_buf(kRecvBuf);

    while (!state.stream_done &&
           (nghttp2_session_want_read(session) ||
            nghttp2_session_want_write(session))) {
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

    // --- Report --------------------------------------------------------

    std::fprintf(stderr,
        "spike_grpc_unary[%s]: HTTP %d, grpc-status=%s, grpc-message=%s\n",
        variant_name(variant),
        state.http_status,
        state.grpc_status.empty() ? "<absent>" : state.grpc_status.c_str(),
        state.grpc_message.empty() ? "<absent>" : state.grpc_message.c_str());
    std::fprintf(stderr,
        "  initial_end_stream=%s, response DATA bytes=%zu\n",
        state.initial_end_stream ? "yes" : "no",
        state.response_data.size());

    // Per-variant pass/fail.
    int rc = 1;
    switch (variant) {
        case Variant::Happy:
            // Expect grpc-status: 0 with normal trailers (DATA + trailer HEADERS).
            if (state.grpc_status == "0" && !state.initial_end_stream) {
                std::printf("OK: happy path — span accepted\n");
                rc = 0;
            } else {
                std::printf("FAIL: happy path expected grpc-status=0 with "
                            "DATA, got grpc-status=%s, initial_end_stream=%d\n",
                            state.grpc_status.c_str(),
                            static_cast<int>(state.initial_end_stream));
            }
            break;

        case Variant::TrailerOnly:
            // Expect a non-zero grpc-status, ideally in the initial HEADERS
            // (END_STREAM=1, no DATA). Some servers use trailers anyway —
            // accept either as long as grpc-status is non-zero.
            if (!state.grpc_status.empty() && state.grpc_status != "0") {
                std::printf("OK: trailer_only — grpc-status=%s%s\n",
                            state.grpc_status.c_str(),
                            state.initial_end_stream
                                ? " (true trailer-only)"
                                : " (status arrived in trailers, not initial)");
                rc = 0;
            } else {
                std::printf("FAIL: trailer_only expected non-zero grpc-status, "
                            "got %s\n",
                            state.grpc_status.empty()
                                ? "<absent>" : state.grpc_status.c_str());
            }
            break;

        case Variant::SplitFrame:
            // Expect grpc-status: 0 — collector accepts the request even
            // though the request body arrived as two DATA frames.
            if (state.grpc_status == "0") {
                std::printf("OK: split_frame — server accepted body across "
                            "two DATA frames\n");
                rc = 0;
            } else {
                std::printf("FAIL: split_frame expected grpc-status=0, got %s\n",
                            state.grpc_status.empty()
                                ? "<absent>" : state.grpc_status.c_str());
            }
            break;
    }

    // THROWAWAY: cleanup; OS reaps on exit.
    nghttp2_session_del(session);
    SSL_shutdown(ssl);
    SSL_free(ssl);
    ::close(fd);
    SSL_CTX_free(ctx);
    return rc;
}
