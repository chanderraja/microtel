// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// udp_concentrator.cpp: the concentrator half of the leaf example.
//
// A normal microtel Provider with its leaf receiver turned on. It owns a UDP
// socket (microtel opens none), names each leaf by the datagram's source
// address:port, and hands every datagram to LeafReceiver::Ingest. The spans
// then leave through the Provider's usual batching and export, many leaves
// to one request.
//
//   microtel_example_leaf_concentrator [endpoint] [listen-port] [config]
//
//   endpoint     OTLP/gRPC collector; default $OTEL_EXPORTER_OTLP_ENDPOINT,
//                else http://localhost:4317
//   listen-port  UDP port on 127.0.0.1; default 9310
//   config       the TOML file with the [concentrator] table; default this
//                directory's microtel.toml
//
// It exits once no datagram has arrived for a few seconds (after the first),
// so the example can show the flush and the receiver's counters.

#include "microtel/leaf_receiver.hpp"
#include "microtel/protocol.hpp"
#include "microtel/provider.hpp"
#include "microtel/sdk_builder.hpp"
#include "microtel/status.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <utility>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#ifndef MICROTEL_EXAMPLE_LEAF_TOML
#define MICROTEL_EXAMPLE_LEAF_TOML "microtel.toml"
#endif

namespace
{

constexpr const char* kDefaultEndpoint{"http://localhost:4317"};
constexpr std::uint16_t kDefaultPort{9310};
constexpr std::chrono::seconds kFirstDatagramWait{60};
constexpr std::chrono::seconds kIdleExit{3};
constexpr std::chrono::seconds kFlushTimeout{5};
constexpr std::chrono::seconds kShutdownTimeout{5};
// The largest UDP payload; the receiver's own limit (max_payload_bytes,
// 64 KiB by default) decides what it accepts.
constexpr std::size_t kMaxDatagram{65535};

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

Fd BindLoopback(const std::uint16_t port)
{
    Fd fd{::socket(AF_INET, SOCK_DGRAM, 0)};
    if (fd.Get() < 0)
    {
        return fd;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) — the sockets API
    if (::bind(fd.Get(), reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0)
    {
        return Fd{-1};
    }
    return fd;
}

const char* StatusName(const microtel::IngestStatus status) noexcept
{
    switch (status)
    {
        case microtel::IngestStatus::Accepted:
            return "Accepted";
        case microtel::IngestStatus::PartiallyAccepted:
            return "PartiallyAccepted";
        case microtel::IngestStatus::Malformed:
            return "Malformed";
        case microtel::IngestStatus::TooLarge:
            return "TooLarge";
        case microtel::IngestStatus::UnknownLeaf:
            return "UnknownLeaf";
        case microtel::IngestStatus::ShutDown:
            return "ShutDown";
        case microtel::IngestStatus::Disabled:
            return "Disabled";
        case microtel::IngestStatus::OutOfMemory:
            return "OutOfMemory";
    }
    return "Unknown";
}

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

/// $OTEL_EXPORTER_OTLP_ENDPOINT, else the shared stack's gRPC port.
std::string DefaultEndpoint()
{
    // NOLINTNEXTLINE(concurrency-mt-unsafe) — read before any thread starts
    const char* const env = std::getenv("OTEL_EXPORTER_OTLP_ENDPOINT");
    return env != nullptr ? env : kDefaultEndpoint;
}

/// The leaf id: the sender's address:port, the one thing a leaf cannot choose.
std::string LeafId(const sockaddr_in& from)
{
    std::array<char, INET_ADDRSTRLEN> host{};
    ::inet_ntop(AF_INET, &from.sin_addr, host.data(), host.size());
    return std::string{host.data()} + ":" + std::to_string(ntohs(from.sin_port));
}

/// Receives datagrams and ingests each, until the link has been idle for
/// kIdleExit after the first one.
void ServeDatagrams(const Fd& socket, microtel::LeafReceiver& receiver)
{
    std::array<std::byte, kMaxDatagram> buffer{};
    std::chrono::milliseconds wait = kFirstDatagramWait;
    pollfd pfd{.fd = socket.Get(), .events = POLLIN, .revents = 0};
    while (::poll(&pfd, 1, static_cast<int>(wait.count())) > 0)
    {
        sockaddr_in from{};
        socklen_t from_len = sizeof(from);
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) — the sockets API
        const ssize_t n = ::recvfrom(socket.Get(),
                                     buffer.data(),
                                     buffer.size(),
                                     0,
                                     reinterpret_cast<sockaddr*>(&from),
                                     &from_len);
        if (n < 0)
        {
            break;
        }
        const std::string leaf_id = LeafId(from);
        const microtel::IngestResult result = receiver.Ingest(microtel::IngestRequest{
            .leaf_id = leaf_id,
            .payload = std::span<const std::byte>(buffer.data(), static_cast<std::size_t>(n)),
            .received_at = std::chrono::system_clock::now(),
        });
        std::cout << leaf_id << "  " << n << " bytes  " << StatusName(result.status)
                  << "  spans_accepted=" << result.spans_accepted << '\n';
        wait = kIdleExit;
    }
}

}  // namespace

int main(int argc, char** argv)
{
    const std::string endpoint{argc > 1 ? argv[1] : DefaultEndpoint()};
    const auto port = static_cast<std::uint16_t>(argc > 2 ? std::atoi(argv[2]) : kDefaultPort);
    const std::string config{argc > 3 ? argv[3] : MICROTEL_EXAMPLE_LEAF_TOML};

    auto built = microtel::SdkBuilder{}
                     .FromFile(config)
                     .WithEndpoint(endpoint)
                     .WithProtocol(microtel::Protocol::Grpc)
                     .WithServiceName("microtel-leaf-concentrator")
                     .Build();
    if (!built)
    {
        std::cerr << "SdkBuilder::Build() failed: " << built.error().message << '\n';
        return 1;
    }
    const std::shared_ptr<microtel::Provider> provider = std::move(*built);
    const std::shared_ptr<microtel::LeafReceiver> receiver = provider->GetLeafReceiver();

    const Fd socket = BindLoopback(port);
    if (socket.Get() < 0)
    {
        std::cerr << "cannot bind UDP 127.0.0.1:" << port << '\n';
        return 1;
    }
    std::cout << "concentrator: UDP 127.0.0.1:" << port << " -> " << endpoint << " (OTLP/gRPC)\n"
              << "config: " << config << '\n';

    ServeDatagrams(socket, *receiver);

    const microtel::LeafReceiverStats stats = receiver->Stats();
    std::cout << "payloads_accepted=" << stats.payloads_accepted
              << " payloads_rejected=" << stats.payloads_rejected
              << " leaves_tracked=" << stats.leaves_tracked
              << " time_fallbacks=" << stats.time_fallbacks << '\n';

    const microtel::Status flush = provider->ForceFlush(kFlushTimeout);
    std::cout << "ForceFlush: " << StatusName(flush) << '\n';
    const microtel::HealthSnapshot health = provider->GetExporterHealth();
    std::cout << "batches_sent=" << health.batches_sent
              << " batches_failed=" << health.batches_failed << '\n';
    std::cout << "Shutdown: " << StatusName(provider->Shutdown(kShutdownTimeout)) << '\n';
    std::cout << "\nview it: http://localhost:3000  (TraceQL: { resource.service.name =~ "
                 "\"greenhouse-.*\" })\n";
    return flush == microtel::Status::Completed ? 0 : 2;
}
