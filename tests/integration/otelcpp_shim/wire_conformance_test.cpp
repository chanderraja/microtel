// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Integration test: M17 L5 wire conformance. Drives all three signals
// (traces, metrics, logs) purely through the otel-cpp API, registered onto
// a real SdkBuilder-built microtel::Provider via RegisterGlobally, over a
// real loopback HTTP/2 connection to an in-process capturing server. The
// captured request bodies are parsed with upb (the same round-trip pattern
// tests/unit/wire/otlp_*_encoder_test.cpp use — the "only otlp_encoder.cpp
// touches upb" restriction applies to src/, not tests/) and asserted against
// what the otel-cpp calls actually set.
//
// This is the proof that the shim's output is not just "reaches a fake
// in-memory Provider" (L2-L4's unit tests) but "produces correct OTLP bytes
// on the wire" — the second half of the L5 README line item.

#include "microtel/sdk_builder.hpp"

#include "adapters/otelcpp/global_registration.hpp"

#include <gtest/gtest.h>
#include <nghttp2/nghttp2.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <vector>

#include <fcntl.h>
#include <netinet/in.h>
#include <opentelemetry/common/attribute_value.h>
#include <opentelemetry/logs/noop.h>
#include <opentelemetry/logs/provider.h>
#include <opentelemetry/metrics/noop.h>
#include <opentelemetry/metrics/provider.h>
#include <opentelemetry/trace/noop.h>
#include <opentelemetry/trace/provider.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

// upb C headers use flexible array members — suppress pedantic warning, same
// treatment tests/unit/wire/otlp_*_encoder_test.cpp give them.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#include "opentelemetry/proto/collector/logs/v1/logs_service.upb.h"
#include "opentelemetry/proto/collector/metrics/v1/metrics_service.upb.h"
#include "opentelemetry/proto/collector/trace/v1/trace_service.upb.h"
#include "opentelemetry/proto/common/v1/common.upb.h"
#include "opentelemetry/proto/logs/v1/logs.upb.h"
#include "opentelemetry/proto/metrics/v1/metrics.upb.h"
#include "opentelemetry/proto/trace/v1/trace.upb.h"
#include "upb/mem/arena.h"
#pragma GCC diagnostic pop

namespace
{

namespace otel_trace = opentelemetry::trace;
namespace otel_metrics = opentelemetry::metrics;
namespace otel_logs = opentelemetry::logs;

constexpr std::string_view kTracesPath = "/v1/traces";
constexpr std::string_view kMetricsPath = "/v1/metrics";
constexpr std::string_view kLogsPath = "/v1/logs";

// ---------------------------------------------------------------------------
// Minimal in-process HTTP/2 server that captures request bodies by path.
//
// Adapted from tests/integration/transport/http2_send_test.cpp's
// MinimalHttp2RequestServer, generalized from "one request, one stream" to
// "N requests, possibly-concurrent streams on one connection" — the SDK
// builds exactly one Http2Transport shared by all three exporters
// (src/sdk/sdk_builder.cpp), so all three exports arrive over one
// connection, each on its own HTTP/2 stream.
// ---------------------------------------------------------------------------

struct CapturedRequest
{
    std::string path;
    std::string body;
};

class CapturingHttp2Server
{
public:
    CapturingHttp2Server() = default;
    ~CapturingHttp2Server()
    {
        Stop();
    }

    CapturingHttp2Server(const CapturingHttp2Server&) = delete;
    CapturingHttp2Server& operator=(const CapturingHttp2Server&) = delete;
    CapturingHttp2Server(CapturingHttp2Server&&) = delete;
    CapturingHttp2Server& operator=(CapturingHttp2Server&&) = delete;

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

    /// Blocks until at least @p count requests have been captured, or
    /// @p timeout elapses. Returns the count actually captured.
    [[nodiscard]] std::size_t WaitForCount(std::size_t count,
                                           std::chrono::milliseconds timeout) const
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        std::size_t current = CurrentCount();
        while (current < count && std::chrono::steady_clock::now() < deadline)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            current = CurrentCount();
        }
        return current;
    }

    [[nodiscard]] std::vector<CapturedRequest> Captured() const
    {
        const std::scoped_lock lock{m_mu};
        return m_captured;
    }

    [[nodiscard]] std::size_t CurrentCount() const
    {
        const std::scoped_lock lock{m_mu};
        return m_captured.size();
    }

    void Stop()
    {
        m_stop.store(true, std::memory_order_release);
        if (m_listen_fd >= 0)
        {
            // shutdown() before close(): a thread blocked in accept() on this
            // fd is only reliably woken by shutdown(); close() alone can
            // leave it parked (observed in practice, not just in theory).
            ::shutdown(m_listen_fd, SHUT_RDWR);
            ::close(m_listen_fd);
            m_listen_fd = -1;
        }
        if (m_thread.joinable())
        {
            m_thread.join();
        }
    }

private:
    struct StreamState
    {
        std::string path;
        std::string body;
    };

    void ServerThread()
    {
        const int client_fd = ::accept(m_listen_fd, nullptr, nullptr);
        if (client_fd < 0)
        {
            return;
        }
        RunSession(client_fd);
        ::close(client_fd);
    }

    void RunSession(int fd)
    {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg,hicpp-vararg,hicpp-signed-bitwise)
        ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | O_NONBLOCK);

        SessionCtx ctx{.fd = fd, .server = this, .streams = {}};

        nghttp2_session_callbacks* cbs = nullptr;
        ::nghttp2_session_callbacks_new(&cbs);
        ::nghttp2_session_callbacks_set_send_callback(cbs, SendCb);
        ::nghttp2_session_callbacks_set_recv_callback(cbs, RecvCb);
        ::nghttp2_session_callbacks_set_on_header_callback(cbs, OnHeaderCb);
        ::nghttp2_session_callbacks_set_on_data_chunk_recv_callback(cbs, OnDataChunkCb);
        ::nghttp2_session_callbacks_set_on_frame_recv_callback(cbs, OnFrameRecvCb);

        nghttp2_session* session = nullptr;
        ::nghttp2_session_server_new(&session, cbs, &ctx);
        ::nghttp2_session_callbacks_del(cbs);

        const nghttp2_settings_entry iv[1] = {{NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, 100U}};
        ::nghttp2_submit_settings(session, NGHTTP2_FLAG_NONE, iv, 1);

        static constexpr int kPollMs = 50;
        static constexpr int kTimeoutMs = 10000;
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(kTimeoutMs);

        while (!m_stop.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < deadline && PumpOnce(session, fd, kPollMs))
        {
        }
        ::nghttp2_session_send(session);
        ::nghttp2_session_del(session);
    }

    /// One send/poll/recv cycle. Returns false on a connection-level error
    /// (caller should stop the session loop).
    [[nodiscard]] static bool PumpOnce(nghttp2_session* session, int fd, int poll_ms)
    {
        ::nghttp2_session_send(session);
        pollfd pfd{.fd = fd, .events = POLLIN, .revents = 0};
        if (::poll(&pfd, 1, poll_ms) <= 0)
        {
            return true;  // nothing readable yet; keep looping
        }
        if (::nghttp2_session_recv(session) != 0)
        {
            return false;
        }
        ::nghttp2_session_send(session);
        return true;
    }

    struct SessionCtx
    {
        int fd = -1;
        CapturingHttp2Server* server = nullptr;
        std::unordered_map<std::int32_t, StreamState> streams;
    };

    static ssize_t SendCb(nghttp2_session* /*s*/,
                          const std::uint8_t* data,
                          std::size_t len,
                          int /*flags*/,
                          void* ud) noexcept
    {
        const int fd = static_cast<SessionCtx*>(ud)->fd;
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

    static ssize_t RecvCb(nghttp2_session* /*s*/,
                          std::uint8_t* buf,
                          std::size_t len,
                          int /*flags*/,
                          void* ud) noexcept
    {
        const int fd = static_cast<SessionCtx*>(ud)->fd;
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

    // nghttp2_on_header_callback's 8-parameter signature is the nghttp2 C
    // API's fixed callback shape.
    // NOLINTNEXTLINE(readability-function-size)
    static int OnHeaderCb(nghttp2_session* /*s*/,
                          const nghttp2_frame* frame,
                          const std::uint8_t* name,
                          std::size_t namelen,
                          const std::uint8_t* value,
                          std::size_t valuelen,
                          std::uint8_t /*flags*/,
                          void* ud) noexcept
    {
        static constexpr std::string_view kPathHeader = ":path";
        if (frame->hd.type != NGHTTP2_HEADERS || frame->headers.cat != NGHTTP2_HCAT_REQUEST)
        {
            return 0;
        }
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        const std::string_view name_view{reinterpret_cast<const char*>(name), namelen};
        if (name_view == kPathHeader)
        {
            auto* ctx = static_cast<SessionCtx*>(ud);
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
            ctx->streams[frame->hd.stream_id].path.assign(reinterpret_cast<const char*>(value),
                                                          valuelen);
        }
        return 0;
    }

    static int OnDataChunkCb(nghttp2_session* /*s*/,
                             std::uint8_t /*flags*/,
                             std::int32_t stream_id,
                             const std::uint8_t* data,
                             std::size_t len,
                             void* ud) noexcept
    {
        auto* ctx = static_cast<SessionCtx*>(ud);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        ctx->streams[stream_id].body.append(reinterpret_cast<const char*>(data), len);
        return 0;
    }

    static int OnFrameRecvCb(nghttp2_session* s, const nghttp2_frame* frame, void* ud) noexcept
    {
        auto* ctx = static_cast<SessionCtx*>(ud);
        const bool end_stream = (frame->hd.flags & NGHTTP2_FLAG_END_STREAM) != 0U;
        const bool is_data_or_headers =
            frame->hd.type == NGHTTP2_DATA || frame->hd.type == NGHTTP2_HEADERS;
        if (!end_stream || !is_data_or_headers)
        {
            return 0;
        }

        const auto it = ctx->streams.find(frame->hd.stream_id);
        if (it == ctx->streams.end())
        {
            return 0;
        }

        {
            const std::scoped_lock lock{ctx->server->m_mu};
            ctx->server->m_captured.push_back({.path = it->second.path, .body = it->second.body});
        }
        ctx->streams.erase(it);

        static const std::array<std::uint8_t, 7> kStatusName = {':', 's', 't', 'a', 't', 'u', 's'};
        static const std::array<std::uint8_t, 3> kStatusValue = {'2', '0', '0'};
        nghttp2_nv nv{};
        // NOLINTBEGIN(cppcoreguidelines-pro-type-const-cast)
        nv.name = const_cast<std::uint8_t*>(kStatusName.data());
        nv.value = const_cast<std::uint8_t*>(kStatusValue.data());
        // NOLINTEND(cppcoreguidelines-pro-type-const-cast)
        nv.namelen = kStatusName.size();
        nv.valuelen = kStatusValue.size();
        nv.flags = NGHTTP2_NV_FLAG_NONE;
        // An all-default-fields ExportXServiceResponse serializes to zero
        // bytes, so an empty 200 body is a genuinely valid, parseable
        // response — no need to construct one.
        ::nghttp2_submit_response(s, frame->hd.stream_id, &nv, 1, nullptr);
        return 0;
    }

    int m_listen_fd = -1;
    int m_port = 0;
    std::thread m_thread;
    std::atomic<bool> m_stop{false};
    mutable std::mutex m_mu;
    std::vector<CapturedRequest> m_captured;
};

// ---------------------------------------------------------------------------
// upb round-trip helpers, same pattern as tests/unit/wire/otlp_*_encoder_test
// ---------------------------------------------------------------------------

std::string SvStr(upb_StringView sv)
{
    return {sv.data, sv.size};
}

struct UpbArenaGuard
{
    upb_Arena* arena = upb_Arena_New();

    UpbArenaGuard() = default;
    ~UpbArenaGuard()
    {
        upb_Arena_Free(arena);
    }
    UpbArenaGuard(const UpbArenaGuard&) = delete;
    UpbArenaGuard& operator=(const UpbArenaGuard&) = delete;
    UpbArenaGuard(UpbArenaGuard&&) = delete;
    UpbArenaGuard& operator=(UpbArenaGuard&&) = delete;
};

// ---------------------------------------------------------------------------
// Test
// ---------------------------------------------------------------------------

class OtelCppWireConformanceTest : public ::testing::Test
{
protected:
    void TearDown() override
    {
        microtel::adapters::otelcpp::UnregisterGlobally();
    }
};

TEST_F(OtelCppWireConformanceTest, AllThreeSignalsProduceCorrectOtlpHttpBytes)
{
    CapturingHttp2Server server;
    const int port = server.Start();
    ASSERT_GT(port, 0);

    auto build_result = microtel::SdkBuilder()
                            .WithEndpoint("http://127.0.0.1:" + std::to_string(port))
                            .WithProtocol(microtel::Protocol::Http)
                            .WithCompressionGzip(false)
                            .WithTls({.insecure = true})
                            .Build();
    ASSERT_TRUE(build_result.has_value()) << build_result.error().message;
    const std::shared_ptr<microtel::Provider> provider = std::move(*build_result);

    // Deliberately NOT calling provider->Connect(): the point of this test is
    // that the first export below connects transparently (ICP 0017).
    microtel::adapters::otelcpp::RegisterGlobally(provider);

    // Drive all three signals through nothing but the otel-cpp API.
    auto tracer = otel_trace::Provider::GetTracerProvider()->GetTracer("wire.conformance");
    auto span = tracer->StartSpan("checkout");
    span->End();

    auto meter = otel_metrics::Provider::GetMeterProvider()->GetMeter("wire.conformance");
    auto counter = meter->CreateUInt64Counter("orders.completed");
    counter->Add(7U);

    auto logger = otel_logs::Provider::GetLoggerProvider()->GetLogger("wire.conformance");
    auto record = logger->CreateLogRecord();
    record->SetBody(opentelemetry::common::AttributeValue{"order shipped"});
    logger->EmitLogRecord(std::move(record));

    // Force all three exporters to drain now rather than on their own
    // schedule (SdkProvider::ForceFlush drains the span/log pipelines and
    // force-collects the metric reader — src/sdk/sdk_provider.cpp).
    ASSERT_EQ(provider->ForceFlush(std::chrono::seconds(5)), microtel::Status::Completed)
        << "last_error: " << provider->GetExporterHealth().last_error_message;

    ASSERT_EQ(server.WaitForCount(3, std::chrono::seconds(5)), 3U);
    std::ignore = provider->Shutdown(std::chrono::seconds(2));

    const auto captured = server.Captured();
    const CapturedRequest* traces = nullptr;
    const CapturedRequest* metrics = nullptr;
    const CapturedRequest* logs = nullptr;
    for (const auto& req : captured)
    {
        if (req.path == kTracesPath)
        {
            traces = &req;
        }
        else if (req.path == kMetricsPath)
        {
            metrics = &req;
        }
        else if (req.path == kLogsPath)
        {
            logs = &req;
        }
    }

    // --- Traces: exact OTel-spec path, and the span name arrived intact ---
    ASSERT_NE(traces, nullptr) << "no request landed on " << kTracesPath;
    {
        const UpbArenaGuard arena;
        const auto* req = opentelemetry_proto_collector_trace_v1_ExportTraceServiceRequest_parse(
            traces->body.data(), traces->body.size(), arena.arena);
        ASSERT_NE(req, nullptr);
        std::size_t rs_count = 0;
        const auto* const* rs =
            opentelemetry_proto_collector_trace_v1_ExportTraceServiceRequest_resource_spans(
                req, &rs_count);
        ASSERT_GT(rs_count, 0U);
        std::size_t ss_count = 0;
        const auto* const* ss =
            opentelemetry_proto_trace_v1_ResourceSpans_scope_spans(rs[0], &ss_count);
        ASSERT_GT(ss_count, 0U);
        std::size_t span_count = 0;
        const auto* const* spans =
            opentelemetry_proto_trace_v1_ScopeSpans_spans(ss[0], &span_count);
        ASSERT_GT(span_count, 0U);
        EXPECT_EQ(SvStr(opentelemetry_proto_trace_v1_Span_name(spans[0])), "checkout");
    }

    // --- Metrics: exact OTel-spec path, and the counter value arrived intact ---
    ASSERT_NE(metrics, nullptr) << "no request landed on " << kMetricsPath;
    {
        const UpbArenaGuard arena;
        const auto* req =
            opentelemetry_proto_collector_metrics_v1_ExportMetricsServiceRequest_parse(
                metrics->body.data(), metrics->body.size(), arena.arena);
        ASSERT_NE(req, nullptr);
        std::size_t rm_count = 0;
        const auto* const* rm =
            opentelemetry_proto_collector_metrics_v1_ExportMetricsServiceRequest_resource_metrics(
                req, &rm_count);
        ASSERT_GT(rm_count, 0U);
        std::size_t sm_count = 0;
        const auto* const* sm =
            opentelemetry_proto_metrics_v1_ResourceMetrics_scope_metrics(rm[0], &sm_count);
        ASSERT_GT(sm_count, 0U);
        std::size_t metric_count = 0;
        const auto* const* metric_arr =
            opentelemetry_proto_metrics_v1_ScopeMetrics_metrics(sm[0], &metric_count);
        ASSERT_GT(metric_count, 0U);
        EXPECT_EQ(SvStr(opentelemetry_proto_metrics_v1_Metric_name(metric_arr[0])),
                  "orders.completed");
        ASSERT_TRUE(opentelemetry_proto_metrics_v1_Metric_has_sum(metric_arr[0]));
        const auto* sum = opentelemetry_proto_metrics_v1_Metric_sum(metric_arr[0]);
        std::size_t dp_count = 0;
        const auto* const* dps = opentelemetry_proto_metrics_v1_Sum_data_points(sum, &dp_count);
        ASSERT_GT(dp_count, 0U);
        EXPECT_EQ(opentelemetry_proto_metrics_v1_NumberDataPoint_as_int(dps[0]), 7);
    }

    // --- Logs: exact OTel-spec path, and the body arrived intact ---
    ASSERT_NE(logs, nullptr) << "no request landed on " << kLogsPath;
    {
        const UpbArenaGuard arena;
        const auto* req = opentelemetry_proto_collector_logs_v1_ExportLogsServiceRequest_parse(
            logs->body.data(), logs->body.size(), arena.arena);
        ASSERT_NE(req, nullptr);
        std::size_t rl_count = 0;
        const auto* const* rl =
            opentelemetry_proto_collector_logs_v1_ExportLogsServiceRequest_resource_logs(req,
                                                                                         &rl_count);
        ASSERT_GT(rl_count, 0U);
        std::size_t sl_count = 0;
        const auto* const* sl =
            opentelemetry_proto_logs_v1_ResourceLogs_scope_logs(rl[0], &sl_count);
        ASSERT_GT(sl_count, 0U);
        std::size_t record_count = 0;
        const auto* const* records =
            opentelemetry_proto_logs_v1_ScopeLogs_log_records(sl[0], &record_count);
        ASSERT_GT(record_count, 0U);
        const auto* body = opentelemetry_proto_logs_v1_LogRecord_body(records[0]);
        ASSERT_NE(body, nullptr);
        EXPECT_EQ(SvStr(opentelemetry_proto_common_v1_AnyValue_string_value(body)),
                  "order shipped");
    }

    server.Stop();
}

}  // namespace
