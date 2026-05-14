// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#include "control_socket.hpp"

#include "backend.hpp"
#include "histogram.hpp"

#include <chrono>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <sstream>
#include <stdexcept>
#include <string>

namespace bench
{

namespace
{

constexpr int kBacklog = 1;
constexpr int kBufSize = 4096;

[[nodiscard]] int CreateListenSocket(int port)
{
    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
    {
        throw std::runtime_error(std::string("socket: ") + std::strerror(errno));
    }

    const int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(static_cast<uint16_t>(port));

    if (::bind(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) < 0)
    {
        ::close(fd);
        throw std::runtime_error(std::string("bind: ") + std::strerror(errno));
    }

    if (::listen(fd, kBacklog) < 0)
    {
        ::close(fd);
        throw std::runtime_error(std::string("listen: ") + std::strerror(errno));
    }

    return fd;
}

// Minimal JSON field extractors — avoids pulling in a JSON library.

[[nodiscard]] std::string ExtractStringField(const std::string& json, const std::string& key)
{
    const std::string search = "\"" + key + "\"";
    const auto key_pos = json.find(search);
    if (key_pos == std::string::npos)
    {
        return {};
    }
    const auto colon = json.find(':', key_pos + search.size());
    if (colon == std::string::npos)
    {
        return {};
    }
    const auto q1 = json.find('"', colon + 1);
    if (q1 == std::string::npos)
    {
        return {};
    }
    const auto q2 = json.find('"', q1 + 1);
    if (q2 == std::string::npos)
    {
        return {};
    }
    return json.substr(q1 + 1, q2 - q1 - 1);
}

[[nodiscard]] uint64_t ExtractUint64Field(const std::string& json, const std::string& key)
{
    const std::string search = "\"" + key + "\"";
    const auto key_pos = json.find(search);
    if (key_pos == std::string::npos)
    {
        return 0;
    }
    const auto colon = json.find(':', key_pos + search.size());
    if (colon == std::string::npos)
    {
        return 0;
    }
    // skip whitespace
    auto val_pos = colon + 1;
    while (val_pos < json.size() && (json[val_pos] == ' ' || json[val_pos] == '\t'))
    {
        ++val_pos;
    }
    return static_cast<uint64_t>(std::stoull(json.substr(val_pos)));
}

void RunSpans(uint64_t count, IBackend& backend, Histogram& hist)
{
    for (uint64_t i = 0; i < count; ++i)
    {
        using clock = std::chrono::steady_clock;
        const auto t0 = clock::now();
        backend.EmitSpan();
        const auto t1 = clock::now();
        const auto ns =
            static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
        hist.Record(ns);
    }
}

}  // namespace

std::string SerializeRunResult(const RunResult& r)
{
    const auto& d = r.spans_dropped;
    std::ostringstream os;
    os << "{\"spans_emitted\":" << r.spans_emitted
       << ",\"spans_dropped\":{"
           << "\"queue_full\":"              << d.queue_full
           << ",\"record_too_large\":"       << d.record_too_large
           << ",\"span_attribute_limit\":"   << d.span_attribute_limit
           << ",\"attribute_value_truncated\":" << d.attribute_value_truncated
           << ",\"other\":"                  << d.other
           << ",\"total\":"                  << d.total
       << "}"
       << ",\"bytes_sent\":" << r.bytes_sent
       << ",\"duration_ns\":" << r.duration_ns
       << ",\"latency_p50_ns\":" << r.latency_p50_ns
       << ",\"latency_p95_ns\":" << r.latency_p95_ns
       << ",\"latency_p99_ns\":" << r.latency_p99_ns
       << ",\"latency_min_ns\":" << r.latency_min_ns
       << ",\"latency_max_ns\":" << r.latency_max_ns
       << "}";
    return os.str();
}

void RunControlLoop(int port, IBackend& backend, Histogram& hist)
{
    const int listen_fd = CreateListenSocket(port);

    const int conn_fd = ::accept(listen_fd, nullptr, nullptr);
    ::close(listen_fd);

    if (conn_fd < 0)
    {
        throw std::runtime_error(std::string("accept: ") + std::strerror(errno));
    }

    std::string line_buf;
    char buf[kBufSize];

    bool running = true;
    while (running)
    {
        const ssize_t n = ::recv(conn_fd, buf, sizeof(buf) - 1, 0);
        if (n <= 0)
        {
            break;
        }
        buf[n] = '\0';
        line_buf += buf;

        std::size_t newline = line_buf.find('\n');
        while (newline != std::string::npos)
        {
            const std::string line = line_buf.substr(0, newline);
            line_buf.erase(0, newline + 1);

            const std::string cmd = ExtractStringField(line, "cmd");

            if (cmd == "quit")
            {
                const std::string resp = "{\"ok\":true}\n";
                ::send(conn_fd, resp.data(), resp.size(), 0);
                running = false;
                break;
            }

            if (cmd == "run")
            {
                const uint64_t spans = ExtractUint64Field(line, "spans");

                using clock = std::chrono::steady_clock;
                const auto t0 = clock::now();
                RunSpans(spans, backend, hist);
                const auto t1 = clock::now();

                const auto stats = backend.Stats();
                const uint64_t dur_ns = static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());

                RunResult result{
                    .spans_emitted = hist.Count(),
                    .spans_dropped = stats.spans_dropped,
                    .bytes_sent = stats.bytes_sent_total,
                    .duration_ns = dur_ns,
                    .latency_p50_ns = hist.Percentile(0.50),
                    .latency_p95_ns = hist.Percentile(0.95),
                    .latency_p99_ns = hist.Percentile(0.99),
                    .latency_min_ns = hist.Min(),
                    .latency_max_ns = hist.Max(),
                };

                const std::string resp = SerializeRunResult(result) + "\n";
                ::send(conn_fd, resp.data(), resp.size(), 0);
            }

            newline = line_buf.find('\n');
        }
    }

    ::close(conn_fd);
}

}  // namespace bench
