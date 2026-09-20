// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// context_propagation — implicit parenting inside one process.
//
// Three functions build a three-level trace without ever passing a span, a
// parent, or a context to each other. Then a `std::thread` shows what does
// *not* cross a thread boundary, and the one-line pattern that carries a
// context to a worker explicitly.
//
// What it prints, and what each line proves:
//
//   * the current-span slot at four nesting depths, and empty again on the way
//     out — the RAII restore;
//   * three trace IDs: the request's, a bare worker thread's (**different** —
//     a new thread starts at the root context), and a worker the caller handed
//     a context to (**the same** as the request's);
//   * what the slot holds under a sampler that drops everything.
//
// Usage:
//   context_propagation [endpoint]
//
// where [endpoint] defaults to http://localhost:4317 — the OTLP/gRPC receiver
// of the shared examples stack, started with:
//   examples/stack/up.sh
//
// Two traces land in the backend: the request trace (with the carried worker
// span inside it) and the orphan worker's own single-span trace.

#include "microtel/context.hpp"
#include "microtel/provider.hpp"
#include "microtel/sampler.hpp"
#include "microtel/sdk_builder.hpp"
#include "microtel/span.hpp"
#include "microtel/tracer.hpp"

#include <chrono>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <thread>

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

/// @brief What are the ids in the calling thread's current-span slot?
///
/// `CurrentContext()` is never null: a thread that has installed nothing sees
/// a default-constructed `Context`, whose `active_span_context` is all zeros.
/// The returned reference is borrowed from the thread-local slot and is
/// invalidated by the next scope construction or destruction on this thread —
/// so read it, do not keep it.
void ShowSlot(std::string_view where)
{
    const microtel::SpanContext& active = microtel::CurrentContext().active_span_context;
    std::cout << "  " << where << "\n      current span_id = " << active.span_id.ToHex()
              << "  valid=" << (active.IsValid() ? "yes" : "no") << '\n';
}

// ---------------------------------------------------------------------------
// Level 3. Note the signature: a tracer, and nothing else.
// ---------------------------------------------------------------------------
void QueryDb(microtel::Tracer& tracer)
{
    // `StartSpanOptions::parent` is unset, so the parent is the
    // `active_span_context` of this thread's `CurrentContext()` — which
    // LoadUser installed, two frames up, without telling anyone.
    const auto scope = tracer.StartAsCurrentSpan("db.query");
    scope->SetAttribute("db.system", std::string{"postgresql"});
    scope->SetAttribute("db.operation", std::string{"SELECT"});
    ShowSlot("db.query        (level 3)");
}

// ---------------------------------------------------------------------------
// Level 2.
// ---------------------------------------------------------------------------
void LoadUser(microtel::Tracer& tracer)
{
    const auto scope = tracer.StartAsCurrentSpan("user.load");
    ShowSlot("user.load       (level 2)");
    QueryDb(tracer);
}

void RenderPage(microtel::Tracer& tracer)
{
    const auto scope = tracer.StartAsCurrentSpan("page.render");
    scope->SetAttribute("template", std::string{"profile.html"});
    ShowSlot("page.render     (level 2)");
}

// ---------------------------------------------------------------------------
// The thread boundary.
// ---------------------------------------------------------------------------

/// @brief A bare `std::thread`: the worker starts from the **root** context.
///
/// There is no cross-thread inheritance and microtel will not grow one (ICP
/// 0025 §3 contract 4). Installing a hook at thread creation is not something
/// a library can do portably, and every mechanism that fakes it — wrapping
/// `std::thread`, interposing `pthread_create` — is a surprise in someone
/// else's process.
///
/// So the span below has an unset parent, reads a root `CurrentContext()`, and
/// starts a **new trace**. In a real service that is the bug where half your
/// work disappears from the request trace.
///
/// @return the orphan's trace ID, so the caller can show it differs.
std::string OrphanOnNewThread(microtel::Tracer& tracer)
{
    std::string trace_id;
    std::thread worker(
        [&tracer, &trace_id]
        {
            std::cout << "  -- inside a bare std::thread --\n";
            ShowSlot("worker thread, before starting a span");
            const auto scope = tracer.StartAsCurrentSpan("worker.orphan");
            scope->SetAttribute("thread.role", std::string{"orphan"});
            trace_id = scope->GetContext().trace_id.ToHex();
        });
    worker.join();
    return trace_id;
}

/// @brief The documented pattern: copy the context, install it on the worker.
///
/// One line on each side. `Context` is a value whose two growable members —
/// `SpanContext::trace_state` and `baggage` — hold their entries behind a
/// `shared_ptr`, so the copy is `noexcept`, allocation-free, and cheap enough
/// to put in a lambda capture or a queued task.
///
/// `ScopedContext` is **thread-confined**: construct and destroy it on the
/// same thread, and destroy scopes in reverse order of creation. The per-thread
/// "stack" is the C++ stack — the slot holds one `Context` by value and each
/// live scope holds the value it displaced — so ordinary block scope gives the
/// right order for free, and destroying out of order writes back a stale
/// context. That is a programming error, not a diagnosed condition.
///
/// @return the carried worker's trace ID, which is the caller's.
std::string CarriedToNewThread(microtel::Tracer& tracer)
{
    // Side one: copy the calling thread's context out.
    const microtel::Context carried = microtel::CurrentContext();

    std::string trace_id;
    std::thread worker(
        [&tracer, carried, &trace_id]
        {
            std::cout << "  -- inside a std::thread handed a context --\n";

            // Side two: install it. Everything this thread starts until
            // `installed` dies now parents into the caller's trace, with no
            // parent argument anywhere.
            const microtel::ScopedContext installed{carried};
            ShowSlot("worker thread, after ScopedContext");

            const auto scope = tracer.StartAsCurrentSpan("worker.carried");
            scope->SetAttribute("thread.role", std::string{"carried"});
            trace_id = scope->GetContext().trace_id.ToHex();
        });
    worker.join();
    return trace_id;
}

/// @brief IDs collected from one run, so `main` can compare them.
struct RunIds
{
    std::string request;
    std::string orphan;
    std::string carried;
};

// ---------------------------------------------------------------------------
// Level 1 — the request handler.
// ---------------------------------------------------------------------------
RunIds HandleRequest(microtel::Tracer& tracer)
{
    const auto scope = tracer.StartAsCurrentSpan(
        "http.handler",
        {.kind = microtel::SpanKind::Server, .parent = {}, .start_time = {}, .attributes = {}});
    scope->SetAttribute("http.request.method", std::string{"GET"});
    scope->SetAttribute("url.path", std::string{"/profile"});
    ShowSlot("http.handler    (level 1)");

    RunIds ids;
    ids.request = scope->GetContext().trace_id.ToHex();

    LoadUser(tracer);
    RenderPage(tracer);

    ids.orphan = OrphanOnNewThread(tracer);
    ids.carried = CarriedToNewThread(tracer);

    return ids;
}

// ---------------------------------------------------------------------------
// The unsampled path.
// ---------------------------------------------------------------------------

/// @brief What the slot holds when the sampler drops the span.
///
/// `StartAsCurrentSpan` installs on the drop path too (ICP 0025 §3 contract
/// 3), and the two sources disagree on purpose:
///
///   - `span->GetContext()` is **invalid**. The handle is a process-wide no-op
///     singleton that cannot hold per-span state without breaking the
///     zero-allocation guarantee for unsampled spans
///     (`docs/memory-model.md` §8.1).
///   - `CurrentContext().active_span_context` carries the **real** trace and
///     span ids with the sampled flag cleared. That is what keeps children of
///     an unsampled span in the same trace, and what lets a propagator still
///     emit a coherent `traceparent` across a hop.
///
/// So on the unsampled path, `CurrentContext()` is the accurate source — which
/// matters for distributed_handoff/, where the thing going on the wire must be
/// right whether or not this process is recording.
void ShowUnsampledPath(microtel::Tracer& tracer)
{
    const auto scope = tracer.StartAsCurrentSpan("unsampled.parent");

    const microtel::SpanContext from_handle = scope->GetContext();
    const microtel::SpanContext& from_slot = microtel::CurrentContext().active_span_context;

    std::cout << "  span->IsSampled()                = " << (scope->IsSampled() ? "true" : "false")
              << "\n  span->GetContext().IsValid()     = "
              << (from_handle.IsValid() ? "true" : "false")
              << "\n  CurrentContext() trace_id        = " << from_slot.trace_id.ToHex()
              << "\n  CurrentContext() sampled flag    = "
              << (from_slot.trace_flags.IsSampled() ? "true" : "false") << '\n';
}

}  // namespace

int main(int argc, char** argv)
{
    const std::string endpoint{(argc > 1) ? argv[1] : kDefaultEndpoint};

    auto built = microtel::SdkBuilder{}
                     .WithEndpoint(endpoint)
                     .WithProtocol(microtel::Protocol::Grpc)
                     .WithServiceName("microtel-context-propagation")
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
        provider->GetTracer("microtel-context-propagation", "1.0.0");

    std::cout << "the current-span slot, as the call stack moves:\n";
    ShowSlot("main            (level 0, nothing installed)");
    const RunIds ids = HandleRequest(*tracer);
    ShowSlot("main            (level 0, every scope destroyed)");

    // A second provider whose sampler drops everything. It is never connected:
    // it has nothing to export, and the point is what the *context* holds.
    //
    // `WithProfileName` is not decoration here. Profile names identify live
    // providers within the process, they are compared byte-for-byte, and an
    // unnamed profile is `"default"` — so a second `Build()` without this line
    // fails with `DuplicateProfileName` while the first provider is alive
    // (ICP 0027). Each named profile is fully independent: its own endpoint,
    // sampler, resource, pipelines and threads.
    std::cout << "\nunder a sampler that drops everything:\n";
    auto unsampled = microtel::SdkBuilder{}
                         .WithEndpoint(endpoint)
                         .WithProtocol(microtel::Protocol::Grpc)
                         .WithServiceName("microtel-context-propagation-unsampled")
                         .WithProfileName("unsampled")
                         .WithSampler(microtel::MakeAlwaysOffSampler())
                         .Build();
    if (unsampled)
    {
        const std::shared_ptr<microtel::Provider> drop_provider = std::move(*unsampled);
        ShowUnsampledPath(*drop_provider->GetTracer("microtel-context-propagation", "1.0.0"));
        const microtel::Status drop_shutdown = drop_provider->Shutdown(kShutdownTimeout);
        std::cout << "  Shutdown (drop provider): " << StatusToString(drop_shutdown) << '\n';
    }
    else
    {
        std::cerr << "  warning: could not build the always-off provider: "
                  << unsampled.error().message << '\n';
    }

    std::cout << "\nrequest trace_id: " << ids.request << "\norphan  trace_id: " << ids.orphan
              << "   (different — a new thread starts at the root context)\n"
              << "carried trace_id: " << ids.carried << "   ("
              << ((ids.carried == ids.request) ? "same as the request — context handed over"
                                               : "UNEXPECTED: should equal the request trace")
              << ")\n\n";

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
              << "     or:  curl -s http://localhost:3200/api/traces/" << ids.request << '\n'
              << "          curl -s http://localhost:3200/api/traces/" << ids.orphan << '\n';

    return (flush == microtel::Status::Completed) ? 0 : 2;
}
