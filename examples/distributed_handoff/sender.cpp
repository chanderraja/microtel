// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// distributed_handoff / sender — the client half.
//
// Starts a `Client` span, writes its `SpanContext` and the request's `Baggage`
// into the outgoing message with the W3C propagators, and sends it to the
// receiver over a localhost socket. The receiver extracts them and starts a
// `Server` span as a child, so the two processes produce **one** trace.
//
// Usage:
//   handoff_receiver [endpoint] [port]     # terminal 1 — start this first
//   handoff_sender   [endpoint] [port]     # terminal 2
//
//   [endpoint]  OTLP/gRPC collector, default http://localhost:4317
//   [port]      the loopback port the two halves meet on, default 9099
//
// The stack is examples/stack/up.sh. Both halves export to it under different
// service names, which is what makes the joined trace obvious in Grafana.
//
// The socket and the text protocol live in wire.hpp, on purpose: this file is
// about what crosses the boundary, not how.

#include "microtel/baggage.hpp"
#include "microtel/context.hpp"
#include "microtel/propagator.hpp"
#include "microtel/provider.hpp"
#include "microtel/sdk_builder.hpp"
#include "microtel/span.hpp"
#include "microtel/trace.hpp"
#include "microtel/tracer.hpp"

#include "wire.hpp"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace
{

constexpr std::chrono::seconds kFlushTimeout{5};
constexpr std::chrono::seconds kShutdownTimeout{5};
constexpr const char* kDefaultEndpoint{"http://localhost:4317"};
constexpr const char* kOrderId{"ord-20260919-0042"};

const char* StatusToString(microtel::Status status) noexcept
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
        // Setter-only outcomes; ForceFlush and Shutdown never return them. The
        // switch stays exhaustive so -Wswitch catches the next enumerator.
        case microtel::Status::InvalidArgument:
            return "InvalidArgument";
        case microtel::Status::Unsupported:
            return "Unsupported";
    }
    return "Unknown";
}

/// @brief IDs this half wants to print after its spans have ended.
struct SentIds
{
    std::string trace_id;
    std::string span_id;
    bool delivered{false};
};

/// @brief Do the call: one `Client` span, injected, sent, answered.
SentIds PlaceOrder(microtel::Tracer& tracer, std::uint16_t port)
{
    SentIds ids;

    // `StartAsCurrentSpan` rather than `StartSpan`: the span becomes this
    // thread's current one, so `CurrentContext()` below is the right place to
    // read what should go on the wire — and anything this function called
    // would parent to it without being told. See examples/context_propagation/.
    const auto client = tracer.StartAsCurrentSpan(
        "order.place",
        {.kind = microtel::SpanKind::Client, .parent = {}, .start_time = {}, .attributes = {}});
    client->SetAttribute("rpc.system", std::string{"microtel-example-text"});
    client->SetAttribute("order.id", std::string{kOrderId});

    // ---------------------------------------------------------------------
    // What goes on the wire, part 1: the span context.
    // ---------------------------------------------------------------------
    //
    // `tracestate` is the vendor lane of W3C Trace Context, and it is
    // copy-on-write: `Set` returns a **new** `TraceState` and leaves the
    // receiver untouched, so this mutates a local copy of the context rather
    // than the span. The receiver prints what arrives, which is how you can
    // see the header survive the hop (issue #208).
    microtel::SpanContext outgoing = client->GetContext();
    outgoing.trace_state = outgoing.trace_state.Set("microtel", "e1");

    // ---------------------------------------------------------------------
    // What goes on the wire, part 2: baggage.
    // ---------------------------------------------------------------------
    //
    // Baggage rides `Context`, never `SpanContext`: it is per-request, not
    // per-span, and it never influences sampling or parenting (ICP 0025 §3
    // contract 6). Every mutator is copy-on-write, so this chain builds three
    // immutable values and keeps the last.
    const microtel::Baggage bag =
        microtel::Baggage{}.Set("tenant", "acme").Set("order.priority", "high");

    // ---------------------------------------------------------------------
    // Inject. The propagators are carrier-agnostic: they write through a
    // `HeaderSetter` callback and never learn what the carrier is. Two
    // propagators, two disjoint sets of headers, no coordination between them.
    // ---------------------------------------------------------------------
    handoff::Headers headers;
    const microtel::HeaderSetter setter = [&headers](std::string_view name, std::string_view value)
    { headers.insert_or_assign(std::string{name}, std::string{value}); };

    microtel::W3CTraceContextPropagator{}.Inject(outgoing, setter);
    microtel::W3CBaggagePropagator{}.Inject(bag, setter);

    std::cout << "headers written:\n";
    for (const auto& [name, value] : headers)
    {
        std::cout << "  " << name << ": " << value << '\n';
    }

    ids.trace_id = outgoing.trace_id.ToHex();
    ids.span_id = outgoing.span_id.ToHex();

    // --- everything below is the dumb part ---------------------------------
    const handoff::Fd conn = handoff::DialLocalhost(port);
    if (!conn.IsOpen())
    {
        std::cerr << "sender: nothing accepted a connection on port " << port
                  << " — start handoff_receiver first.\n";
        client->SetStatus(microtel::StatusCode::Error, "connect failed");
        return ids;
    }

    const std::string start_line = std::string{"PLACE /orders/"} + kOrderId;
    if (!handoff::WriteMessage(conn, start_line, headers))
    {
        std::cerr << "sender: the receiver closed before the request was sent\n";
        client->SetStatus(microtel::StatusCode::Error, "send failed");
        return ids;
    }

    handoff::Headers reply_headers;
    const std::optional<std::string> reply = handoff::ReadMessage(conn, reply_headers);
    if (!reply.has_value())
    {
        std::cerr << "sender: the receiver closed before answering\n";
        client->SetStatus(microtel::StatusCode::Error, "no reply");
        return ids;
    }

    std::cout << "reply: " << *reply << '\n';
    client->SetAttribute("rpc.response", *reply);
    client->SetStatus(microtel::StatusCode::Ok);
    ids.delivered = true;
    return ids;
}

std::uint16_t ParsePort(int argc, char** argv)
{
    if (argc <= 2)
    {
        return handoff::kDefaultPort;
    }
    const long parsed = std::strtol(argv[2], nullptr, 10);
    constexpr long kMaxPort{65535};
    if (parsed <= 0 || parsed > kMaxPort)
    {
        std::cerr << "sender: ignoring unusable port '" << argv[2] << "'\n";
        return handoff::kDefaultPort;
    }
    return static_cast<std::uint16_t>(parsed);
}

}  // namespace

int main(int argc, char** argv)
{
    const std::string endpoint{(argc > 1) ? argv[1] : kDefaultEndpoint};
    const std::uint16_t port = ParsePort(argc, argv);

    auto built = microtel::SdkBuilder{}
                     .WithEndpoint(endpoint)
                     .WithProtocol(microtel::Protocol::Grpc)
                     .WithServiceName("microtel-handoff-sender")
                     .WithServiceVersion("1.0.0")
                     .Build();

    if (!built)
    {
        std::cerr << "SdkBuilder::Build() failed: " << built.error().message << '\n';
        return 1;
    }

    const std::shared_ptr<microtel::Provider> provider = std::move(*built);

    if (auto connected = provider->Connect(); !connected)
    {
        std::cerr << "warning: Connect() to " << endpoint
                  << " failed: " << connected.error().message
                  << "\n         is an OTLP/gRPC collector listening there? "
                     "continuing — export will be retried on flush.\n";
    }

    const std::shared_ptr<microtel::Tracer> tracer =
        provider->GetTracer("microtel-handoff-sender", "1.0.0");

    const SentIds ids = PlaceOrder(*tracer, port);

    std::cout << "\nsender trace_id: " << ids.trace_id << "\nsender span_id:  " << ids.span_id
              << "   (the receiver's server span should name this as its parent)\n";

    const microtel::Status flush = provider->ForceFlush(kFlushTimeout);
    std::cout << "ForceFlush: " << StatusToString(flush) << '\n';

    const microtel::HealthSnapshot health = provider->GetExporterHealth();
    std::cout << "batches_sent=" << health.batches_sent
              << " batches_failed=" << health.batches_failed
              << " queue_depth=" << health.queue_depth_now << '\n';
    if (!health.last_error_message.empty())
    {
        std::cout << "last_error: " << health.last_error_message << '\n';
    }

    const microtel::Status shutdown = provider->Shutdown(kShutdownTimeout);
    std::cout << "Shutdown: " << StatusToString(shutdown) << '\n';

    std::cout << "\nview it:  http://localhost:3000  (home dashboard: microtel — recent traces)\n"
              << "     or:  curl -s http://localhost:3200/api/traces/" << ids.trace_id << '\n';

    if (!ids.delivered)
    {
        return 3;
    }
    return (flush == microtel::Status::Completed) ? 0 : 2;
}
