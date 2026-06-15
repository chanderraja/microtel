// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// basic_trace — a minimal, standalone example of the microtel public API.
//
// Builds an SDK provider, opens the OTLP/HTTP-protobuf connection, emits one
// request trace (a server parent span with two child spans), flushes, prints
// exporter health, and shuts down cleanly.
//
// Usage:
//   basic_trace [endpoint]
//
// where [endpoint] defaults to http://localhost:4318 — an OTLP/HTTP-protobuf
// collector. Start one first, e.g.:
//   docker run --rm -p 4318:4318 otel/opentelemetry-collector

#include "microtel/provider.hpp"
#include "microtel/sdk_builder.hpp"
#include "microtel/span.hpp"
#include "microtel/tracer.hpp"

#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>

namespace
{

constexpr std::chrono::seconds kFlushTimeout{5};
constexpr std::chrono::seconds kShutdownTimeout{5};
constexpr std::int64_t kHttpStatusOk{200};
constexpr const char* kDefaultEndpoint{"http://localhost:4318"};

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
    }
    return "Unknown";
}

// Emit one "request" trace: a server parent span with two internal children.
//
// Two things worth copying into real code:
//  - StartSpanOptions is initialised with all fields (.kind/.parent/.start_time/
//    .attributes). Partial designated initialisers trip -Wextra's
//    -Wmissing-field-initializers under -Werror.
//  - String attribute values are constructed as std::string explicitly — a bare
//    string literal is a const char* and would bind to the bool alternative of
//    microtel::AttributeValue.
void EmitRequestTrace(microtel::Tracer& tracer)
{
    const auto parent = tracer.StartSpan(
        "example.request",
        {.kind = microtel::SpanKind::Server, .parent = {}, .start_time = {}, .attributes = {}});
    parent->SetAttribute("http.request.method", std::string{"GET"});
    parent->SetAttribute("url.path", std::string{"/api/widgets"});

    const microtel::SpanContext parent_ctx = parent->GetContext();

    {
        const auto query = tracer.StartSpan("example.db.query",
                                            {.kind = microtel::SpanKind::Internal,
                                             .parent = parent_ctx,
                                             .start_time = {},
                                             .attributes = {}});
        query->SetAttribute("db.system", std::string{"postgresql"});
        query->AddEvent("query.start");
        query->End();
    }

    {
        const auto render = tracer.StartSpan("example.render",
                                             {.kind = microtel::SpanKind::Internal,
                                              .parent = parent_ctx,
                                              .start_time = {},
                                              .attributes = {}});
        render->SetAttribute("template", std::string{"widgets.html"});
        render->End();
    }

    parent->SetAttribute("http.response.status_code", kHttpStatusOk);
    parent->SetStatus(microtel::StatusCode::Ok);
    parent->End();
}

}  // namespace

int main(int argc, char** argv)
{
    const std::string endpoint{(argc > 1) ? argv[1] : kDefaultEndpoint};

    auto built = microtel::SdkBuilder{}
                     .WithEndpoint(endpoint)
                     .WithProtocol(microtel::Protocol::Http)
                     .WithServiceName("microtel-basic-example")
                     .WithServiceVersion("1.0.0")
                     .Build();

    if (!built)
    {
        std::cerr << "SdkBuilder::Build() failed: " << built.error().message << '\n';
        return 1;
    }

    const std::shared_ptr<microtel::Provider> provider = std::move(*built);

    // Connect() is optional (the connection is established lazily on first
    // export). We call it eagerly so a misconfigured endpoint is reported up
    // front, but continue regardless so the full flow still runs.
    if (auto connected = provider->Connect(); !connected)
    {
        std::cerr << "warning: Connect() to " << endpoint
                  << " failed: " << connected.error().message
                  << "\n         is an OTLP/HTTP collector listening there? "
                     "continuing — export will be retried on flush.\n";
    }

    const std::shared_ptr<microtel::Tracer> tracer =
        provider->GetTracer("microtel-basic-example", "1.0.0");

    EmitRequestTrace(*tracer);

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

    return (flush == microtel::Status::Completed) ? 0 : 2;
}
