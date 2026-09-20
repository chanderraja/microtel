// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// distributed_handoff / receiver — the server half.
//
// Accepts one message, extracts the W3C `traceparent` / `tracestate` /
// `baggage` headers, installs the extracted `Context` as this thread's
// current one, and starts a `Server` span. Because the extracted context is
// current, that span parents to the sender's client span **without a parent
// argument** — the same implicit mechanism as inside one process, with a
// socket in the middle.
//
// Usage:
//   handoff_receiver [endpoint] [port]     # start this first
//   handoff_sender   [endpoint] [port]
//
//   [endpoint]  OTLP/gRPC collector, default http://localhost:4317
//   [port]      the loopback port the two halves meet on, default 9099
//
// It handles exactly one message and exits, so it can flush its trace and shut
// down cleanly. A server would loop.

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
struct ServedIds
{
    std::string trace_id;
    std::string span_id;
    std::string parent_span_id;
    bool joined{false};
    bool served{false};
};

/// @brief Handle the accepted connection.
ServedIds Serve(microtel::Tracer& tracer, const handoff::Fd& conn)
{
    ServedIds ids;

    handoff::Headers headers;
    const std::optional<std::string> start_line = handoff::ReadMessage(conn, headers);
    if (!start_line.has_value())
    {
        std::cerr << "receiver: the sender closed before a complete message arrived\n";
        return ids;
    }

    std::cout << "request: " << *start_line << "\nheaders read:\n";
    for (const auto& [name, value] : headers)
    {
        std::cout << "  " << name << ": " << value << '\n';
    }

    // ---------------------------------------------------------------------
    // Extract. The same carrier-agnostic callback shape as Inject, read-side:
    // one lambda, and neither propagator learns what the carrier is.
    // ---------------------------------------------------------------------
    const microtel::HeaderGetter getter =
        [&headers](std::string_view name) -> std::optional<std::string_view>
    {
        const auto found = headers.find(name);
        if (found == headers.end())
        {
            return std::nullopt;
        }
        return std::string_view{found->second};
    };

    const microtel::SpanContext remote = microtel::W3CTraceContextPropagator{}.Extract(getter);
    const microtel::Baggage bag = microtel::W3CBaggagePropagator{}.Extract(getter);

    if (!remote.IsValid())
    {
        // `Extract` reports failure by returning an invalid context rather
        // than throwing — there is no error channel on a hot path. An invalid
        // parent makes the span below an ordinary root, so the service still
        // produces a trace; it just is not joined to anyone's.
        std::cerr << "receiver: no usable traceparent arrived — this span will start a new "
                     "trace instead of joining the sender's.\n";
    }

    std::cout << "extracted: remote=" << (remote.remote ? "true" : "false")
              << " sampled=" << (remote.trace_flags.IsSampled() ? "true" : "false")
              << " tracestate=\"" << remote.trace_state.ToHeader()
              << "\" baggage_entries=" << bag.Size() << '\n';

    // ---------------------------------------------------------------------
    // Install the extracted context, then start the span with **no parent
    // argument**. This is the same implicit parenting as inside one process:
    // `StartSpanOptions::parent` is unset, so the parent is the
    // `active_span_context` of the current context — which is the sender's
    // span, five lines up, having crossed a socket to get here.
    //
    // Installing rather than passing `.parent = remote` is also what puts the
    // baggage on the context, where anything this handler calls can read it
    // through `CurrentContext().baggage` with no argument threading — see the
    // note below for how far that currently reaches.
    // ---------------------------------------------------------------------
    const microtel::ScopedContext incoming{microtel::Context{remote, bag}};

    // Baggage read back from the **context**, not from the local `bag` — that
    // is the point of putting it on the context in the first place. Promoting
    // one value to a span attribute is the usual reason to carry baggage at
    // all: the tenant is known at the edge and wanted on the spans downstream.
    //
    // Read here, before the span scope opens, and held in a local. That is
    // correct in any case, but today it is also necessary:
    // `StartAsCurrentSpan` installs a `Context` built from the span context
    // alone, so the thread's baggage is empty for the span's whole scope —
    // issue #283. Once that is fixed this read can move down beside the
    // `SetAttribute` call, where it reads more naturally.
    const std::optional<std::string_view> tenant = microtel::CurrentContext().baggage.Get("tenant");

    const auto server = tracer.StartAsCurrentSpan(
        "order.receive",
        {.kind = microtel::SpanKind::Server, .parent = {}, .start_time = {}, .attributes = {}});
    server->SetAttribute("rpc.system", std::string{"microtel-example-text"});
    server->SetAttribute("rpc.request", *start_line);

    if (tenant.has_value())
    {
        server->SetAttribute("tenant.id", std::string{*tenant});
    }

    ids.trace_id = server->GetContext().trace_id.ToHex();
    ids.span_id = server->GetContext().span_id.ToHex();
    ids.parent_span_id = remote.span_id.ToHex();
    ids.joined = remote.IsValid();

    server->AddEvent("order.accepted");
    server->SetStatus(microtel::StatusCode::Ok);

    ids.served = handoff::WriteMessage(conn, "200 OK", {});
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
        std::cerr << "receiver: ignoring unusable port '" << argv[2] << "'\n";
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
                     .WithServiceName("microtel-handoff-receiver")
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
        provider->GetTracer("microtel-handoff-receiver", "1.0.0");

    std::cout << "receiver: waiting for one message on 127.0.0.1:" << port << '\n';
    const handoff::Fd conn = handoff::ListenAndAcceptOne(port);
    if (!conn.IsOpen())
    {
        std::cerr << "receiver: could not accept a connection on port " << port
                  << " — is something else bound to it?\n";
        const microtel::Status early = provider->Shutdown(kShutdownTimeout);
        std::cout << "Shutdown: " << StatusToString(early) << '\n';
        return 1;
    }

    const ServedIds ids = Serve(*tracer, conn);

    std::cout << "\nreceiver trace_id:      " << ids.trace_id
              << "\nreceiver span_id:       " << ids.span_id
              << "\nreceiver parent span:   " << ids.parent_span_id << "   "
              << (ids.joined ? "(the sender's client span)" : "(none — this span is a root)")
              << '\n';

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

    if (!ids.served)
    {
        return 3;
    }
    return (flush == microtel::Status::Completed) ? 0 : 2;
}
