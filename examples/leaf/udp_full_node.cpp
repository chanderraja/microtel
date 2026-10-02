// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// udp_full_node.cpp: a full C++ microtel node on the leaves' link.
//
// A device that can run the C++ runtime, but whose only route out is the same
// UDP link the C leaves use, exports through an ExportTransport of its own
// (ICP 0036). It keeps the whole Tracer API, sampling, batching and retries;
// only the last hop changes: each encoded OTLP request goes to the
// concentrator as one datagram, from a fixed source port that the
// concentrator's microtel.toml names with time_mode = "unix".
//
//   microtel_example_leaf_full_node [concentrator-port] [source-port] [cycles]
//                                   [leaf-port]
//
//   concentrator-port  UDP port on 127.0.0.1; default 9310
//   source-port        this node's UDP port, its id at the concentrator;
//                      default 9313
//   cycles             greenhouse cycles to trace; default 5
//   leaf-port          if given, the node plays the greenhouse controller:
//                      each cycle sends one command datagram to the leaf on
//                      127.0.0.1:leaf-port carrying the W3C traceparent of
//                      its irrigation.check span, so the leaf's sensor.read
//                      joins the same trace. No reply, no retry.

#include "microtel/export_transport.hpp"
#include "microtel/propagator.hpp"
#include "microtel/provider.hpp"
#include "microtel/sdk_builder.hpp"
#include "microtel/span.hpp"
#include "microtel/status.hpp"
#include "microtel/tracer.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

namespace
{

/// @brief Items the collector rejected via OTLP partial success. It answered
///        success, so their batch counts as sent, never as failed.
std::uint64_t PartialSuccessRejected(const microtel::HealthSnapshot& health)
{
    return health
        .drop_counters[static_cast<std::size_t>(microtel::DropReason::PartialSuccessRejection)];
}

constexpr std::uint16_t kDefaultConcentratorPort{9310};
constexpr std::uint16_t kDefaultSourcePort{9313};
constexpr int kDefaultCycles{5};
/// The largest UDP payload over IPv4.
constexpr std::size_t kMaxDatagram{65507};
/// Under kMaxDatagram, so a joined request is never too big to send, and
/// under the concentrator's max_payload_bytes (64 KiB by default).
constexpr std::uint32_t kMaxRequestBytes{60U * 1024U};
/// SO_SNDTIMEO of zero means "block forever", so a deadline already due still
/// gets this much.
constexpr std::chrono::microseconds kMinSendTimeout{1000};
constexpr std::chrono::milliseconds kCycleGap{200};
constexpr std::chrono::milliseconds kSensorRead{40};
constexpr std::chrono::seconds kFlushTimeout{5};
constexpr std::chrono::seconds kShutdownTimeout{5};

/// Move-only owner of a socket, as rule 5 of CLAUDE.md asks of every resource.
class Fd
{
public:
    explicit Fd(int fd) noexcept : m_fd(fd) {}
    ~Fd() noexcept
    {
        if (m_fd >= 0)
        {
            ::close(m_fd);
        }
    }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    Fd(Fd&& other) noexcept : m_fd(std::exchange(other.m_fd, -1)) {}
    Fd& operator=(Fd&&) = delete;

    [[nodiscard]] int Get() const noexcept
    {
        return m_fd;
    }

private:
    int m_fd = -1;
};

sockaddr_in Loopback(const std::uint16_t port)
{
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    return addr;
}

/// A socket bound to 127.0.0.1:source and connected to 127.0.0.1:dest, or -1.
Fd OpenLink(const std::uint16_t source, const std::uint16_t dest)
{
    Fd fd{::socket(AF_INET, SOCK_DGRAM, 0)};
    if (fd.Get() < 0)
    {
        return fd;
    }
    const sockaddr_in local = Loopback(source);
    const sockaddr_in peer = Loopback(dest);
    // NOLINTBEGIN(cppcoreguidelines-pro-type-reinterpret-cast) — the sockets API
    if (::bind(fd.Get(), reinterpret_cast<const sockaddr*>(&local), sizeof(local)) != 0 ||
        ::connect(fd.Get(), reinterpret_cast<const sockaddr*>(&peer), sizeof(peer)) != 0)
    // NOLINTEND(cppcoreguidelines-pro-type-reinterpret-cast)
    {
        return Fd{-1};
    }
    return fd;
}

/// Sends each OTLP request as one UDP datagram.
///
/// The part every ExportTransport has to get right is the last one: Send
/// never blocks past its deadline (SO_SNDTIMEO, from the deadline, never
/// zero), and Cancel wakes one that is blocked anyway (a flag, and shutdown()
/// on the socket). A transport without both can make the Provider's
/// destructor wait forever.
///
/// Thread safety: with the default options only the trace exporter's worker
/// calls Send. The socket calls are safe from several threads regardless.
class UdpExportTransport final : public microtel::ExportTransport
{
public:
    explicit UdpExportTransport(Fd socket) noexcept : m_socket(std::move(socket)) {}

    [[nodiscard]] microtel::SendResult Send(const microtel::ExportRequest& request) override
    {
        if (m_cancelled.load(std::memory_order_acquire))
        {
            return Failure(microtel::SendOutcome::NonRetryable, "cancelled");
        }
        if (request.bytes.size() > kMaxDatagram)
        {
            // Retrying cannot shrink it. max_request_bytes keeps joined
            // requests under this; only one oversized batch can get here.
            return Failure(microtel::SendOutcome::NonRetryable, "request exceeds one datagram");
        }
        SetSendTimeout(request.deadline);
        if (::send(m_socket.Get(), request.bytes.data(), request.bytes.size(), 0) < 0)
        {
            return FromErrno(errno);
        }
        // Fire and forget: Success means the datagram left this host.
        return microtel::SendResult{.outcome = microtel::SendOutcome::Success,
                                    .retry_after = std::nullopt,
                                    .rejected = 0,
                                    .message = {}};
    }

    void Cancel() noexcept override
    {
        m_cancelled.store(true, std::memory_order_release);
        (void)::shutdown(m_socket.Get(), SHUT_RDWR);
    }

private:
    [[nodiscard]] static microtel::SendResult Failure(microtel::SendOutcome outcome,
                                                      std::string message)
    {
        return microtel::SendResult{.outcome = outcome,
                                    .retry_after = std::nullopt,
                                    .rejected = 0,
                                    .message = std::move(message)};
    }

    /// EAGAIN is the send timeout; ECONNREFUSED is an ICMP port-unreachable
    /// from an earlier datagram, i.e. the concentrator is not up yet. Both
    /// may clear. Anything else will not.
    [[nodiscard]] static microtel::SendResult FromErrno(const int err)
    {
        const bool transient = err == EAGAIN || err == EWOULDBLOCK || err == ECONNREFUSED ||
                               err == ENOBUFS || err == EINTR;
        return Failure(transient ? microtel::SendOutcome::Retryable
                                 : microtel::SendOutcome::NonRetryable,
                       std::string{"sendto: "} + std::strerror(err));
    }

    void SetSendTimeout(const std::chrono::steady_clock::time_point deadline) const noexcept
    {
        const auto left = std::chrono::duration_cast<std::chrono::microseconds>(
            deadline - std::chrono::steady_clock::now());
        const auto timeout = std::max(left, kMinSendTimeout);
        const auto secs = std::chrono::duration_cast<std::chrono::seconds>(timeout);
        timeval tv{};
        tv.tv_sec = static_cast<time_t>(secs.count());
        tv.tv_usec = static_cast<suseconds_t>((timeout - secs).count());
        (void)::setsockopt(m_socket.Get(), SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    }

    Fd m_socket;
    std::atomic<bool> m_cancelled{false};
};

const char* StatusName(const microtel::Status status) noexcept
{
    switch (status)
    {
        case microtel::Status::Completed:
            return "Completed";
        case microtel::Status::TimedOut:
            return "TimedOut";
        case microtel::Status::AlreadyShutDown:
            return "AlreadyShutDown";
        case microtel::Status::Failed:
            return "Failed";
        case microtel::Status::InvalidArgument:
            return "InvalidArgument";
        case microtel::Status::Unsupported:
            return "Unsupported";
    }
    return "Unknown";
}

/// One greenhouse control cycle: a root span and a child for the reading.
void TraceCycle(microtel::Tracer& tracer, const int cycle)
{
    auto root = tracer.StartSpan("greenhouse.control");
    root->SetAttribute("greenhouse.cycle", static_cast<std::int64_t>(cycle));
    {
        microtel::StartSpanOptions child;
        child.parent = root->GetContext();
        auto read = tracer.StartSpan("sensor.read", child);
        read->SetAttribute("sensor.kind", std::string{"humidity"});
        std::this_thread::sleep_for(kSensorRead);
        read->AddEvent("sample.taken");
        read->End();
    }
    root->SetAttribute("vent.open", cycle % 2 == 0);
    root->End();
}

/// The command to the leaf: its name, then the W3C traceparent of the span
/// that sends it: "irrigation.check traceparent=00-<trace-id>-<span-id>-01".
std::string CommandFor(const microtel::SpanContext& context)
{
    std::string command{"irrigation.check"};
    microtel::W3CTraceContextPropagator{}.Inject(
        context,
        [&command](const std::string_view header, const std::string_view value)
        {
            if (header == "traceparent")
            {
                command.append(" traceparent=").append(value);
            }
        });
    return command;
}

/// One controller cycle: greenhouse.control, and an irrigation.check child
/// that asks the leaf for a soil reading. The leaf starts its sensor.read
/// under irrigation.check, so the trace spans both devices. The command is one
/// datagram and nothing comes back: irrigation.check ends once it is sent, and
/// the leaf's span may well outlast it.
void TraceCommandCycle(microtel::Tracer& tracer, const Fd& command_link, const int cycle)
{
    auto root = tracer.StartSpan("greenhouse.control");
    root->SetAttribute("greenhouse.cycle", static_cast<std::int64_t>(cycle));
    microtel::StartSpanOptions child;
    child.kind = microtel::SpanKind::Client;
    child.parent = root->GetContext();
    auto check = tracer.StartSpan("irrigation.check", child);
    check->SetAttribute("greenhouse.zone", std::string{"north"});
    const std::string command = CommandFor(check->GetContext());
    if (::send(command_link.Get(), command.data(), command.size(), 0) < 0)
    {
        check->SetStatus(microtel::StatusCode::Error, std::strerror(errno));
    }
    check->End();
    root->End();
    std::cout << "cycle " << cycle << ": 2 spans, sent \"" << command << "\"\n";
}

/// Traces `cycles` cycles: plain ones, or, given a command link to a leaf,
/// controller cycles that each send it a command.
void RunCycles(microtel::Tracer& tracer, const Fd& command_link, const int cycles)
{
    for (int cycle = 1; cycle <= cycles; ++cycle)
    {
        if (command_link.Get() >= 0)
        {
            TraceCommandCycle(tracer, command_link, cycle);
        }
        else
        {
            TraceCycle(tracer, cycle);
            std::cout << "cycle " << cycle << ": 2 spans\n";
        }
        std::this_thread::sleep_for(kCycleGap);
    }
}

}  // namespace

int main(int argc, char** argv)
{
    const auto dest =
        static_cast<std::uint16_t>(argc > 1 ? std::atoi(argv[1]) : kDefaultConcentratorPort);
    const auto source =
        static_cast<std::uint16_t>(argc > 2 ? std::atoi(argv[2]) : kDefaultSourcePort);
    const int cycles = argc > 3 ? std::atoi(argv[3]) : kDefaultCycles;
    const auto leaf_port = static_cast<std::uint16_t>(argc > 4 ? std::atoi(argv[4]) : 0);

    Fd link = OpenLink(source, dest);
    if (link.Get() < 0)
    {
        std::cerr << "cannot open UDP 127.0.0.1:" << source << " -> 127.0.0.1:" << dest << '\n';
        return 1;
    }
    // Commands leave from an ephemeral port: only the export link's source
    // port names this node at the concentrator.
    const Fd command_link = leaf_port != 0 ? OpenLink(0, leaf_port) : Fd{-1};
    if (leaf_port != 0 && command_link.Get() < 0)
    {
        std::cerr << "cannot open UDP to the leaf on 127.0.0.1:" << leaf_port << '\n';
        return 1;
    }

    auto built = microtel::SdkBuilder{}
                     .WithServiceName("greenhouse-controller")
                     .WithExportTransport(std::make_unique<UdpExportTransport>(std::move(link)),
                                          microtel::ExportTransportOptions{
                                              .traces = true,
                                              .metrics = false,
                                              .logs = false,
                                              .max_request_bytes = kMaxRequestBytes,
                                          })
                     .Build();
    if (!built)
    {
        std::cerr << "SdkBuilder::Build() failed: " << built.error().message << '\n';
        return 1;
    }
    const std::shared_ptr<microtel::Provider> provider = std::move(*built);
    std::cout << "full node 127.0.0.1:" << source << " -> 127.0.0.1:" << dest
              << " (OTLP over UDP)\n";
    if (leaf_port != 0)
    {
        std::cout << "commands -> leaf 127.0.0.1:" << leaf_port << " (one datagram each)\n";
    }

    const auto tracer = provider->GetTracer("greenhouse.controller", "1.0.0");
    RunCycles(*tracer, command_link, cycles);

    const microtel::Status flush = provider->ForceFlush(kFlushTimeout);
    const microtel::HealthSnapshot health = provider->GetExporterHealth();
    std::cout << "ForceFlush: " << StatusName(flush) << '\n'
              << "batches_sent=" << health.batches_sent
              << " batches_failed=" << health.batches_failed
              << " rejected=" << PartialSuccessRejected(health) << '\n';
    std::cout << "Shutdown: " << StatusName(provider->Shutdown(kShutdownTimeout)) << '\n';
    const bool delivered = flush == microtel::Status::Completed && health.batches_failed == 0 &&
                           PartialSuccessRejected(health) == 0;
    return delivered ? 0 : 2;
}
