// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// The demo's Linux side: one process that is both an instrumented service and
// the concentrator for a microcontroller behind a UART.
//
// Each request:
//   GET /api/greenhouse/reading      SERVER span, service "edge-gateway"
//     device.read_sensor             CLIENT span; its traceparent goes to the
//                                    device as "CMD <traceparent>"
//       sensor.read                  emitted by the Cortex-M4 firmware with the
//         adc.sample                 microtel leaf library, service "mcu-sensor"
//
// The device answers with "PAYLOAD <hex>": an encoded leaf payload, which the
// gateway hands to its LeafReceiver (the concentrator). From there the device's
// spans take the Provider's normal export path, so both services' spans reach
// the collector — and Tempo — as one trace.
//
// The UART is QEMU's serial port exposed over TCP (-serial tcp:...). On real
// hardware it would be /dev/ttyUSB0; nothing microtel-shaped changes.
//
// Environment:
//   DEVICE_ADDR                  host:port of the device UART (default device:5555)
//   OTEL_EXPORTER_OTLP_ENDPOINT  OTLP/gRPC collector (default http://localhost:4317)
//   CYCLES                       requests to make; 0 = run forever (default 0)
//   INTERVAL_MS                  pause between requests (default 5000)

#include "microtel/leaf_receiver.hpp"
#include "microtel/propagator.hpp"
#include "microtel/protocol.hpp"
#include "microtel/provider.hpp"
#include "microtel/sdk_builder.hpp"
#include "microtel/span.hpp"
#include "microtel/status.hpp"
#include "microtel/tracer.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace
{

constexpr std::chrono::seconds kConnectWait{120};
constexpr std::chrono::seconds kReplyWait{10};
constexpr std::chrono::seconds kFlushTimeout{5};
constexpr std::chrono::seconds kShutdownTimeout{5};
constexpr const char* kLeafId = "uart0:mps2-an386";

std::string Env(const char* name, const char* fallback)
{
    const char* v = std::getenv(name);  // NOLINT(concurrency-mt-unsafe)
    return (v != nullptr && *v != '\0') ? std::string{v} : std::string{fallback};
}

/// A line-oriented TCP link to the device's UART.
class UartLink
{
public:
    UartLink() = default;
    UartLink(const UartLink&) = delete;
    UartLink& operator=(const UartLink&) = delete;
    ~UartLink()
    {
        if (m_fd >= 0)
        {
            ::close(m_fd);
        }
    }

    bool Connect(const std::string& host_port)
    {
        const auto colon = host_port.rfind(':');
        if (colon == std::string::npos)
        {
            return false;
        }
        const std::string host = host_port.substr(0, colon);
        const std::string port = host_port.substr(colon + 1);
        const auto deadline = std::chrono::steady_clock::now() + kConnectWait;
        while (std::chrono::steady_clock::now() < deadline)
        {
            addrinfo hints{};
            hints.ai_family = AF_UNSPEC;
            hints.ai_socktype = SOCK_STREAM;
            addrinfo* res = nullptr;
            if (::getaddrinfo(host.c_str(), port.c_str(), &hints, &res) == 0)
            {
                for (addrinfo* ai = res; ai != nullptr; ai = ai->ai_next)
                {
                    const int fd = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
                    if (fd >= 0 && ::connect(fd, ai->ai_addr, ai->ai_addrlen) == 0)
                    {
                        m_fd = fd;
                        break;
                    }
                    if (fd >= 0)
                    {
                        ::close(fd);
                    }
                }
                ::freeaddrinfo(res);
            }
            if (m_fd >= 0)
            {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        return false;
    }

    bool Send(std::string_view line)
    {
        std::string out{line};
        out.push_back('\n');
        std::size_t off = 0;
        while (off < out.size())
        {
            const ssize_t n = ::send(m_fd, out.data() + off, out.size() - off, MSG_NOSIGNAL);
            if (n <= 0)
            {
                return false;
            }
            off += static_cast<std::size_t>(n);
        }
        return true;
    }

    /// The next line, or nullopt on timeout or a closed link.
    std::optional<std::string> ReadLine(std::chrono::milliseconds timeout)
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        for (;;)
        {
            const auto nl = m_buf.find('\n');
            if (nl != std::string::npos)
            {
                std::string line = m_buf.substr(0, nl);
                m_buf.erase(0, nl + 1);
                if (!line.empty() && line.back() == '\r')
                {
                    line.pop_back();
                }
                return line;
            }
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now());
            if (left.count() <= 0)
            {
                return std::nullopt;
            }
            pollfd pfd{.fd = m_fd, .events = POLLIN, .revents = 0};
            if (::poll(&pfd, 1, static_cast<int>(left.count())) <= 0)
            {
                return std::nullopt;
            }
            char chunk[4096];
            const ssize_t n = ::recv(m_fd, chunk, sizeof(chunk), 0);
            if (n <= 0)
            {
                return std::nullopt;
            }
            m_buf.append(chunk, static_cast<std::size_t>(n));
        }
    }

private:
    int m_fd = -1;
    std::string m_buf;
};

std::optional<std::vector<std::byte>> FromHex(std::string_view hex)
{
    if (hex.size() % 2 != 0)
    {
        return std::nullopt;
    }
    auto nibble = [](char c) -> int
    {
        if (c >= '0' && c <= '9')
        {
            return c - '0';
        }
        if (c >= 'a' && c <= 'f')
        {
            return c - 'a' + 10;
        }
        return -1;
    };
    std::vector<std::byte> out;
    out.reserve(hex.size() / 2);
    for (std::size_t i = 0; i < hex.size(); i += 2)
    {
        const int hi = nibble(hex[i]);
        const int lo = nibble(hex[i + 1]);
        if (hi < 0 || lo < 0)
        {
            return std::nullopt;
        }
        out.push_back(static_cast<std::byte>(hi * 16 + lo));
    }
    return out;
}

std::string TraceparentOf(const microtel::SpanContext& context)
{
    std::string traceparent;
    microtel::W3CTraceContextPropagator{}.Inject(
        context,
        [&traceparent](const std::string_view header, const std::string_view value)
        {
            if (header == "traceparent")
            {
                traceparent = std::string{value};
            }
        });
    return traceparent;
}

/// One request through the gateway and down to the device. Returns true when
/// the device's spans were accepted by the concentrator.
bool HandleRequest(microtel::Tracer& tracer,
                   UartLink& uart,
                   microtel::LeafReceiver& receiver,
                   const int seq)
{
    microtel::StartSpanOptions server;
    server.kind = microtel::SpanKind::Server;
    auto root = tracer.StartSpan("GET /api/greenhouse/reading", server);
    root->SetAttribute("http.request.method", std::string{"GET"});
    root->SetAttribute("http.route", std::string{"/api/greenhouse/reading"});
    root->SetAttribute("request.seq", static_cast<std::int64_t>(seq));

    microtel::StartSpanOptions child;
    child.kind = microtel::SpanKind::Client;
    child.parent = root->GetContext();
    auto call = tracer.StartSpan("device.read_sensor", child);
    call->SetAttribute("device.link", std::string{"uart0"});
    call->SetAttribute("device.id", std::string{kLeafId});

    const std::string traceparent = TraceparentOf(call->GetContext());
    bool ok = uart.Send("CMD " + traceparent);
    std::size_t spans = 0;
    while (ok)
    {
        const auto line = uart.ReadLine(kReplyWait);
        if (!line)
        {
            ok = false;
            call->SetStatus(microtel::StatusCode::Error, "device did not answer");
            break;
        }
        if (line->rfind("LOG ", 0) == 0)
        {
            std::cout << "  device: " << line->substr(4) << '\n';
            continue;
        }
        if (line->rfind("PAYLOAD ", 0) != 0)
        {
            continue;
        }
        const auto payload = FromHex(std::string_view{*line}.substr(8));
        if (!payload || payload->empty())
        {
            ok = false;
            call->SetStatus(microtel::StatusCode::Error, "bad payload from device");
            break;
        }
        const microtel::IngestResult result = receiver.Ingest(microtel::IngestRequest{
            .leaf_id = kLeafId,
            .payload = std::span<const std::byte>(payload->data(), payload->size()),
            .received_at = std::chrono::system_clock::now(),
        });
        spans = result.spans_accepted;
        ok = result.status == microtel::IngestStatus::Accepted;
        call->SetAttribute("device.payload.bytes", static_cast<std::int64_t>(payload->size()));
        call->SetAttribute("device.spans", static_cast<std::int64_t>(spans));
        if (!ok)
        {
            call->SetStatus(microtel::StatusCode::Error, "concentrator rejected payload");
        }
        break;
    }
    call->End();
    root->End();
    std::cout << "request " << seq << ": trace_id=" << traceparent.substr(3, 32)
              << " device_spans=" << spans << (ok ? "" : " FAILED") << '\n'
              << std::flush;
    return ok;
}

}  // namespace

int main()
{
    const std::string device = Env("DEVICE_ADDR", "device:5555");
    const std::string endpoint = Env("OTEL_EXPORTER_OTLP_ENDPOINT", "http://localhost:4317");
    const int cycles = std::atoi(Env("CYCLES", "0").c_str());
    const auto interval = std::chrono::milliseconds(std::atoi(Env("INTERVAL_MS", "5000").c_str()));

    auto built = microtel::SdkBuilder{}
                     .WithEndpoint(endpoint)
                     .WithProtocol(microtel::Protocol::Grpc)
                     .WithServiceName("edge-gateway")
                     .WithServiceVersion("1.0.0")
                     .WithLeafReceiver(microtel::LeafReceiverOptions{})
                     .Build();
    if (!built)
    {
        std::cerr << "SdkBuilder::Build() failed: " << built.error().message << '\n';
        return 1;
    }
    const std::shared_ptr<microtel::Provider> provider = std::move(*built);
    const std::shared_ptr<microtel::LeafReceiver> receiver = provider->GetLeafReceiver();
    const auto tracer = provider->GetTracer("edge.gateway", "1.0.0");

    UartLink uart;
    std::cout << "edge-gateway: device UART " << device << ", exporting to " << endpoint << '\n'
              << std::flush;
    if (!uart.Connect(device))
    {
        std::cerr << "cannot reach the device at " << device << '\n';
        return 1;
    }
    std::cout << "edge-gateway: connected to the device\n" << std::flush;

    int failures = 0;
    for (int seq = 1; cycles == 0 || seq <= cycles; ++seq)
    {
        if (!HandleRequest(*tracer, uart, *receiver, seq))
        {
            ++failures;
        }
        (void)provider->ForceFlush(kFlushTimeout);
        if (cycles == 0 || seq < cycles)
        {
            std::this_thread::sleep_for(interval);
        }
    }

    const microtel::Status flush = provider->ForceFlush(kFlushTimeout);
    const microtel::HealthSnapshot health = provider->GetExporterHealth();
    std::cout << "batches_sent=" << health.batches_sent
              << " batches_failed=" << health.batches_failed << '\n';
    (void)provider->Shutdown(kShutdownTimeout);
    return (failures == 0 && flush == microtel::Status::Completed && health.batches_failed == 0)
               ? 0
               : 2;
}
