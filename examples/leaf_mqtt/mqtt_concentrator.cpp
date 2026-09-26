// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// mqtt_concentrator.cpp: the concentrator half of the MQTT leaf example.
//
// A normal microtel Provider with its leaf receiver turned on, and an MQTT
// client (libmosquitto) subscribed to microtel/+/traces. Each message's topic
// names the leaf that sent it, microtel/<leaf-id>/traces, and its payload goes
// to LeafReceiver::Ingest. The spans then leave through the Provider's usual
// batching and export, many leaves to one request.
//
//   microtel_example_leaf_mqtt_concentrator [endpoint] [broker-port] [config]
//
//   endpoint     OTLP/gRPC collector; default $OTEL_EXPORTER_OTLP_ENDPOINT,
//                else http://localhost:4317
//   broker-port  MQTT broker on 127.0.0.1; default 1883
//   config       the TOML file with the [concentrator] table; default this
//                directory's microtel.toml
//
// Threads: libmosquitto's network thread (mosquitto_loop_start) runs every
// callback below, so Ingest is called from that thread, not from main.
// LeafReceiver::Ingest is thread-safe, and main touches the receiver again
// only after mosquitto_loop_stop has joined that thread.
//
// It exits once no message has arrived for a few seconds (after the first),
// so the example can show the flush and the receiver's counters.

#include "microtel/leaf_receiver.hpp"
#include "microtel/protocol.hpp"
#include "microtel/provider.hpp"
#include "microtel/sdk_builder.hpp"
#include "microtel/status.hpp"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include <mosquitto.h>

#ifndef MICROTEL_EXAMPLE_LEAF_MQTT_TOML
#define MICROTEL_EXAMPLE_LEAF_MQTT_TOML "microtel.toml"
#endif

namespace
{

constexpr const char* kDefaultEndpoint{"http://localhost:4317"};
constexpr const char* kBrokerHost{"127.0.0.1"};
constexpr int kDefaultBrokerPort{1883};
constexpr int kDecimal{10};
constexpr const char* kClientId{"microtel-leaf-concentrator"};
constexpr const char* kSubscription{"microtel/+/traces"};
constexpr std::string_view kTopicPrefix{"microtel/"};
constexpr std::string_view kTopicSuffix{"/traces"};
constexpr int kQos{1};
constexpr int kKeepAliveSeconds{60};
// Reconnect after 1 s, doubling to at most 30 s while the broker stays away.
constexpr unsigned kReconnectDelaySeconds{1};
constexpr unsigned kReconnectDelayMaxSeconds{30};
constexpr std::chrono::seconds kFirstMessageWait{60};
constexpr std::chrono::seconds kIdleExit{3};
constexpr std::chrono::seconds kFlushTimeout{5};
constexpr std::chrono::seconds kShutdownTimeout{5};

/// mosquitto_lib_init / mosquitto_lib_cleanup, as one scoped object.
class MosquittoLib
{
public:
    MosquittoLib() noexcept
    {
        ::mosquitto_lib_init();
    }
    ~MosquittoLib() noexcept
    {
        ::mosquitto_lib_cleanup();
    }
    MosquittoLib(const MosquittoLib&) = delete;
    MosquittoLib& operator=(const MosquittoLib&) = delete;
    MosquittoLib(MosquittoLib&&) = delete;
    MosquittoLib& operator=(MosquittoLib&&) = delete;
};

struct MosquittoDeleter
{
    void operator()(mosquitto* client) const noexcept
    {
        ::mosquitto_destroy(client);
    }
};
using MosquittoPtr = std::unique_ptr<mosquitto, MosquittoDeleter>;

/// What the network thread shares with main: the receiver it feeds, and the
/// message count and connection state main waits on to decide when the link
/// has gone quiet.
struct Bridge
{
    microtel::LeafReceiver* receiver = nullptr;
    std::mutex mu;
    std::condition_variable cv;
    std::uint64_t messages = 0;
    bool connected = false;
};

void SetConnected(Bridge& bridge, const bool connected)
{
    {
        const std::scoped_lock lock{bridge.mu};
        bridge.connected = connected;
    }
    bridge.cv.notify_one();
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

/// The leaf id: the one topic level between "microtel/" and "/traces". The
/// subscription's '+' already guarantees one level; checking again keeps a
/// broker bug or a wider subscription from inventing ids.
std::string_view LeafIdFromTopic(const std::string_view topic) noexcept
{
    if (!topic.starts_with(kTopicPrefix) || !topic.ends_with(kTopicSuffix) ||
        topic.size() <= kTopicPrefix.size() + kTopicSuffix.size())
    {
        return {};
    }
    const std::string_view id =
        topic.substr(kTopicPrefix.size(), topic.size() - kTopicPrefix.size() - kTopicSuffix.size());
    return id.find('/') == std::string_view::npos ? id : std::string_view{};
}

// ---- libmosquitto callbacks: all run on the network thread ----------------

/// Runs after every CONNACK, including each one after a reconnect. The
/// session is clean, so the broker has forgotten the subscription and it has
/// to be made again every time.
void OnConnect(mosquitto* client, void* bridge, const int rc)
{
    if (rc != 0)
    {
        std::cout << "broker refused the connection: " << ::mosquitto_connack_string(rc) << '\n';
        return;
    }
    std::cout << "connected to the broker, subscribing to " << kSubscription << '\n';
    ::mosquitto_subscribe(client, nullptr, kSubscription, kQos);
    SetConnected(*static_cast<Bridge*>(bridge), true);
}

/// rc is 0 only for the DISCONNECT main sends at the end. Anything else is a
/// lost connection, which the network thread retries with backoff.
void OnDisconnect(mosquitto* /*client*/, void* bridge, const int rc)
{
    if (rc != 0)
    {
        std::cout << "lost the broker (" << ::mosquitto_strerror(rc) << "), reconnecting\n";
    }
    SetConnected(*static_cast<Bridge*>(bridge), false);
}

void OnMessage(mosquitto* /*client*/, void* bridge_ptr, const mosquitto_message* message)
{
    auto& bridge = *static_cast<Bridge*>(bridge_ptr);
    const auto received_at = std::chrono::system_clock::now();
    const std::string_view topic{message->topic};
    const std::string_view leaf_id = LeafIdFromTopic(topic);
    if (leaf_id.empty())
    {
        std::cout << topic << "  ignored: not microtel/<leaf-id>/traces\n";
        return;
    }
    const auto size = static_cast<std::size_t>(message->payloadlen);
    const microtel::IngestResult result = bridge.receiver->Ingest(microtel::IngestRequest{
        .leaf_id = leaf_id,
        .payload =
            std::span<const std::byte>(static_cast<const std::byte*>(message->payload), size),
        .received_at = received_at,
    });
    std::cout << leaf_id << "  " << size << " bytes  " << StatusName(result.status)
              << "  spans_accepted=" << result.spans_accepted << '\n';
    {
        const std::scoped_lock lock{bridge.mu};
        ++bridge.messages;
    }
    bridge.cv.notify_one();
}

/// Blocks until the link has been quiet for kIdleExit after the first
/// message. The short idle clock runs only while connected: during a broker
/// outage, and before anything has arrived, it waits kFirstMessageWait.
void WaitUntilIdle(Bridge& bridge)
{
    std::unique_lock lock{bridge.mu};
    while (true)
    {
        const std::uint64_t seen = bridge.messages;
        const bool connected = bridge.connected;
        const std::chrono::seconds wait = seen > 0 && connected ? kIdleExit : kFirstMessageWait;
        const bool changed = bridge.cv.wait_for(
            lock, wait, [&] { return bridge.messages != seen || bridge.connected != connected; });
        if (!changed)
        {
            return;
        }
    }
}

/// Creates the client, connects in the background and starts the network
/// thread. mosquitto_connect_async returning an error is not fatal: the
/// thread keeps retrying, so the concentrator may start before the broker.
MosquittoPtr StartClient(Bridge& bridge, const int port)
{
    MosquittoPtr client{::mosquitto_new(kClientId, true, &bridge)};
    if (!client)
    {
        return client;
    }
    ::mosquitto_connect_callback_set(client.get(), OnConnect);
    ::mosquitto_disconnect_callback_set(client.get(), OnDisconnect);
    ::mosquitto_message_callback_set(client.get(), OnMessage);
    ::mosquitto_reconnect_delay_set(
        client.get(), kReconnectDelaySeconds, kReconnectDelayMaxSeconds, true);
    const int rc = ::mosquitto_connect_async(client.get(), kBrokerHost, port, kKeepAliveSeconds);
    if (rc != MOSQ_ERR_SUCCESS)
    {
        std::cout << "broker not reachable yet (" << ::mosquitto_strerror(rc) << "), retrying\n";
    }
    if (::mosquitto_loop_start(client.get()) != MOSQ_ERR_SUCCESS)
    {
        client.reset();
    }
    return client;
}

/// Runs the MQTT client until the link goes quiet, then disconnects and joins
/// the network thread, so no Ingest is in flight once this returns. The client
/// is destroyed before `lib` cleans the library up. Returns false if the
/// client could not be started.
bool ServeBroker(microtel::LeafReceiver& receiver, const int port)
{
    const MosquittoLib lib;
    Bridge bridge;
    bridge.receiver = &receiver;
    const MosquittoPtr client = StartClient(bridge, port);
    if (!client)
    {
        std::cerr << "cannot start the MQTT client\n";
        return false;
    }
    WaitUntilIdle(bridge);
    ::mosquitto_disconnect(client.get());
    ::mosquitto_loop_stop(client.get(), false);
    return true;
}

/// Prints the receiver's counters, flushes and shuts the Provider down.
/// Returns the exit code: 0, or 2 if the flush did not complete.
int FlushAndReport(microtel::Provider& provider, const microtel::LeafReceiver& receiver)
{
    const microtel::LeafReceiverStats stats = receiver.Stats();
    std::cout << "payloads_accepted=" << stats.payloads_accepted
              << " payloads_rejected=" << stats.payloads_rejected
              << " leaves_tracked=" << stats.leaves_tracked
              << " time_fallbacks=" << stats.time_fallbacks << '\n';

    const microtel::Status flush = provider.ForceFlush(kFlushTimeout);
    std::cout << "ForceFlush: " << StatusName(flush) << '\n';
    const microtel::HealthSnapshot health = provider.GetExporterHealth();
    std::cout << "batches_sent=" << health.batches_sent
              << " batches_failed=" << health.batches_failed << '\n';
    std::cout << "Shutdown: " << StatusName(provider.Shutdown(kShutdownTimeout)) << '\n';
    std::cout << "\nview it: http://localhost:3000  (TraceQL: { resource.service.name =~ "
                 "\"greenhouse-.*\" })\n";
    return flush == microtel::Status::Completed ? 0 : 2;
}

}  // namespace

int main(int argc, char** argv)
{
    const std::string endpoint{argc > 1 ? argv[1] : DefaultEndpoint()};
    const int port =
        argc > 2 ? static_cast<int>(std::strtol(argv[2], nullptr, kDecimal)) : kDefaultBrokerPort;
    const std::string config{argc > 3 ? argv[3] : MICROTEL_EXAMPLE_LEAF_MQTT_TOML};
    // Two threads print; flush every line so a piped log reads in order.
    std::cout.setf(std::ios::unitbuf);

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

    int major = 0;
    int minor = 0;
    int revision = 0;
    ::mosquitto_lib_version(&major, &minor, &revision);
    std::cout << "concentrator: mqtt://" << kBrokerHost << ':' << port << ' ' << kSubscription
              << " -> " << endpoint << " (OTLP/gRPC)\n"
              << "config: " << config << '\n'
              << "libmosquitto " << major << '.' << minor << '.' << revision << '\n';

    if (!ServeBroker(*receiver, port))
    {
        return 1;
    }
    return FlushAndReport(*provider, *receiver);
}
