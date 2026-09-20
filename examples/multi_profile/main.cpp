// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// multi_profile — two independent named providers in one process.
//
// Builds a "frontend" and a "backend" profile with different service names and
// different samplers, emits from both, and looks the backend up by name from a
// function that was handed no pointer. Also shows the three registry rules:
// a duplicate name is refused, Shutdown does not release a name, destruction
// does.
//
// Usage:
//   multi_profile [endpoint]
//
// where [endpoint] defaults to http://localhost:4317 — the OTLP/gRPC receiver
// of the shared examples stack, started with:
//   examples/stack/up.sh
//
// Both profiles export to the same collector here, which is the least
// interesting thing they could do: each one owns its endpoint, protocol, TLS
// material, sampler, Resource, pipelines and I/O thread, and shares none of
// them (ICP 0027).

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

namespace
{

constexpr std::chrono::seconds kFlushTimeout{5};
constexpr std::chrono::seconds kShutdownTimeout{5};
constexpr const char* kDefaultEndpoint{"http://localhost:4317"};

constexpr const char* kFrontendProfile{"frontend"};
constexpr const char* kBackendProfile{"backend"};
constexpr const char* kFrontendService{"microtel-frontend-example"};
constexpr const char* kBackendService{"microtel-backend-example"};

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
        // Setter-only outcomes. ForceFlush and Shutdown never return these,
        // but the switch is exhaustive so -Wswitch keeps this honest if the
        // enum grows again.
        case microtel::Status::InvalidArgument:
            return "InvalidArgument";
        case microtel::Status::Unsupported:
            return "Unsupported";
    }
    return "Unknown";
}

/// @brief Backend work, written the way library code has to be written.
///
/// It received no provider, no tracer, and no configuration: it finds its
/// pipeline by profile name. A host that builds its profiles in one place can
/// pass the `shared_ptr` around instead and never call `GetProvider` — this is
/// for the code that cannot be reached that way, such as a plugin or a
/// callback.
///
/// @return the trace ID of the exported span, or empty if nothing was exported.
std::string RecordBackendWork()
{
    microtel::Provider* const provider = microtel::GetProvider(kBackendProfile);
    if (provider == nullptr)
    {
        std::cerr << "no live provider named \"" << kBackendProfile << "\"\n";
        return {};
    }

    const std::shared_ptr<microtel::Tracer> tracer =
        provider->GetTracer("backend.library", "1.0.0");

    // The backend profile's sampler drops this one by name. Health checks are
    // the canonical thing a service does not want in its trace bill.
    const auto health_check = tracer->StartSpan(
        "backend.healthz",
        {.kind = microtel::SpanKind::Server, .parent = {}, .start_time = {}, .attributes = {}});
    std::cout << "  backend.healthz sampled: " << (health_check->IsSampled() ? "yes" : "no")
              << "  (the backend sampler drops it)\n";
    health_check->End();

    const auto span = tracer->StartSpan(
        "backend.query",
        {.kind = microtel::SpanKind::Server, .parent = {}, .start_time = {}, .attributes = {}});
    span->SetAttribute("db.system", std::string{"postgresql"});
    span->SetStatus(microtel::StatusCode::Ok);

    const microtel::SpanContext ctx = span->GetContext();
    span->End();
    return ctx.trace_id.ToHex();
}

/// @brief Emit one frontend span through the pointer the host already holds.
std::string RecordFrontendWork(microtel::Tracer& tracer)
{
    const auto span = tracer.StartSpan(
        "frontend.request",
        {.kind = microtel::SpanKind::Server, .parent = {}, .start_time = {}, .attributes = {}});
    span->SetAttribute("http.request.method", std::string{"GET"});
    span->SetStatus(microtel::StatusCode::Ok);

    const microtel::SpanContext ctx = span->GetContext();
    span->End();
    return ctx.trace_id.ToHex();
}

/// @brief Try to claim a name that is already taken, and report what happened.
void ShowDuplicateNameIsRefused(const std::string& endpoint)
{
    auto duplicate = microtel::SdkBuilder{}
                         .WithEndpoint(endpoint)
                         .WithProtocol(microtel::Protocol::Grpc)
                         .WithServiceName("microtel-impostor-example")
                         .WithProfileName(kBackendProfile)
                         .Build();

    if (duplicate)
    {
        std::cout << "  unexpected: the duplicate build succeeded\n";
        const microtel::Status shutdown = (*duplicate)->Shutdown(kShutdownTimeout);
        std::cout << "  shut the impostor down: " << StatusToString(shutdown) << '\n';
        return;
    }

    const bool as_documented =
        duplicate.error().kind == microtel::ConfigError::Kind::DuplicateProfileName;
    std::cout << "  Build() with WithProfileName(\"" << kBackendProfile
              << "\") refused: " << duplicate.error().message << '\n'
              << "  kind is DuplicateProfileName: " << (as_documented ? "yes" : "no")
              << "  (the live provider keeps the name; this is never last-wins)\n";
}

/// @brief `live` or `nullptr`, for a line of registry output.
const char* Found(std::string_view name) noexcept
{
    return (microtel::GetProvider(name) != nullptr) ? "live" : "nullptr";
}

/// @brief Flush, report health, shut down. True if the flush completed.
bool Finish(microtel::Provider& provider, const char* label)
{
    const microtel::Status flush = provider.ForceFlush(kFlushTimeout);
    const microtel::HealthSnapshot health = provider.GetExporterHealth();
    const microtel::Status shutdown = provider.Shutdown(kShutdownTimeout);

    std::cout << "  " << label << ": ForceFlush=" << StatusToString(flush)
              << " batches_sent=" << health.batches_sent
              << " batches_failed=" << health.batches_failed
              << " queue_depth=" << health.queue_depth_now
              << " Shutdown=" << StatusToString(shutdown) << '\n';
    if (!health.last_error_message.empty())
    {
        std::cout << "  " << label << ": last_error: " << health.last_error_message << '\n';
    }

    return flush == microtel::Status::Completed;
}

}  // namespace

int main(int argc, char** argv)
{
    const std::string endpoint{(argc > 1) ? argv[1] : kDefaultEndpoint};
    std::cout << "endpoint: " << endpoint << '\n';

    // The frontend samples everything it starts.
    auto frontend_built = microtel::SdkBuilder{}
                              .WithEndpoint(endpoint)
                              .WithProtocol(microtel::Protocol::Grpc)
                              .WithServiceName(kFrontendService)
                              .WithServiceVersion("1.0.0")
                              .WithProfileName(kFrontendProfile)
                              .WithSampler(microtel::MakeAlwaysOnSampler())
                              .Build();
    if (!frontend_built)
    {
        std::cerr << "frontend Build() failed: " << frontend_built.error().message << '\n';
        return 1;
    }
    std::shared_ptr<microtel::Provider> frontend = std::move(*frontend_built);

    // The backend keeps everything except its health check. A different
    // sampler on a different provider: nothing about this is shared with the
    // frontend's.
    auto backend_built =
        microtel::SdkBuilder{}
            .WithEndpoint(endpoint)
            .WithProtocol(microtel::Protocol::Grpc)
            .WithServiceName(kBackendService)
            .WithServiceVersion("1.0.0")
            .WithProfileName(kBackendProfile)
            .WithSampler(microtel::MakeSpanNameRuleSampler("backend.healthz",
                                                           microtel::MakeAlwaysOffSampler(),
                                                           microtel::MakeAlwaysOnSampler()))
            .Build();
    if (!backend_built)
    {
        std::cerr << "backend Build() failed: " << backend_built.error().message << '\n';
        return 1;
    }
    const std::shared_ptr<microtel::Provider> backend = std::move(*backend_built);

    if (auto connected = frontend->Connect(); !connected)
    {
        std::cerr << "warning: frontend Connect() failed: " << connected.error().message << '\n';
    }
    if (auto connected = backend->Connect(); !connected)
    {
        std::cerr << "warning: backend Connect() failed: " << connected.error().message << '\n';
    }

    std::cout << "\nemitting\n";
    {
        const std::shared_ptr<microtel::Tracer> tracer =
            frontend->GetTracer("frontend.app", "1.0.0");
        std::cout << "  " << kFrontendService << " trace_id: " << RecordFrontendWork(*tracer)
                  << '\n';
    }
    const std::string backend_trace = RecordBackendWork();
    std::cout << "  " << kBackendService << " trace_id: " << backend_trace << '\n';

    std::cout << "\nthe registry\n"
              << "  GetProvider(\"" << kFrontendProfile << "\"): " << Found(kFrontendProfile)
              << '\n'
              << "  GetProvider(\"" << kBackendProfile << "\"):  " << Found(kBackendProfile) << '\n'
              << "  GetProvider() [\"" << microtel::kDefaultProfileName
              << "\"]: " << Found(microtel::kDefaultProfileName)
              << "  (nothing was built under it)\n";
    ShowDuplicateNameIsRefused(endpoint);

    std::cout << "\nshutting down\n";
    const bool frontend_ok = Finish(*frontend, kFrontendProfile);
    const bool backend_ok = Finish(*backend, kBackendProfile);

    std::cout << "  after Shutdown, GetProvider(\"" << kFrontendProfile
              << "\"): " << Found(kFrontendProfile) << "  (a name is freed by destruction, "
              << "not by Shutdown)\n";

    // Dropping the last owning reference destroys the provider, which is what
    // releases the name. The tracer taken from it went out of scope above:
    // a Tracer borrows its provider's pipeline and must not outlive it.
    frontend.reset();
    std::cout << "  after destruction, GetProvider(\"" << kFrontendProfile
              << "\"): " << Found(kFrontendProfile) << "\n  GetProvider(\"" << kBackendProfile
              << "\"):  " << Found(kBackendProfile) << "  (still owned here)\n";

    std::cout << "\nBoth services should now appear in Grafana's service list:\n"
                 "  { resource.service.name = \""
              << kFrontendService << "\" }\n  { resource.service.name = \"" << kBackendService
              << "\" }\n";

    return (frontend_ok && backend_ok) ? 0 : 2;
}
