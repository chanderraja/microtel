// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// resource_detectors — filling the Resource from the running process and host.
//
// Registers the two built-in detectors, emits one trace, and prints the values
// the detectors are expected to have found, so the console can be compared
// against the resource attributes on the span in Grafana.
//
// Usage:
//   resource_detectors [endpoint]
//
// where [endpoint] defaults to http://localhost:4317 — the OTLP/gRPC receiver
// of the shared examples stack, started with:
//   examples/stack/up.sh
//
// Precedence (microtel-spec.md §12.7) runs defaults -> detectors -> environment
// -> file/code, each layer overriding the one before it. To watch the
// environment beat a detector, run it again with:
//
//   OTEL_RESOURCE_ATTRIBUTES=host.name=resource-demo-override \
//       ./build/examples/microtel_example_resource_detectors
//
// The example prints what it read from that variable, so the two runs label
// themselves.

#include "microtel/provider.hpp"
#include "microtel/resource_detectors.hpp"
#include "microtel/sdk_builder.hpp"
#include "microtel/span.hpp"
#include "microtel/tracer.hpp"

#include <array>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>

#include <unistd.h>

namespace
{

constexpr std::chrono::seconds kFlushTimeout{5};
constexpr std::chrono::seconds kShutdownTimeout{5};
constexpr const char* kDefaultEndpoint{"http://localhost:4317"};
constexpr const char* kServiceName{"microtel-resource-detectors-example"};

/// `HOST_NAME_MAX` is 64 on Linux; this is the POSIX-portable slack plus room
/// for the terminator.
constexpr std::size_t kHostNameBufferSize{256};

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

/// @brief What `MakeHostDetector()` should have put in `host.name`.
///
/// Read here through the same syscall the detector uses, purely so the console
/// and the span can be compared. Application code has no reason to do this.
std::string HostName()
{
    std::array<char, kHostNameBufferSize> buffer{};
    if (gethostname(buffer.data(), buffer.size() - 1) != 0)
    {
        return "<gethostname failed>";
    }
    return std::string{buffer.data()};
}

/// @brief Emit one trace and return its ID as lowercase hex.
std::string EmitTrace(microtel::Tracer& tracer)
{
    const auto span = tracer.StartSpan(
        "resource.demo",
        {.kind = microtel::SpanKind::Server, .parent = {}, .start_time = {}, .attributes = {}});
    span->SetAttribute("demo.note",
                       std::string{"every resource attribute on this span came from "
                                   "the Resource, not from here"});
    span->SetStatus(microtel::StatusCode::Ok);

    const microtel::SpanContext ctx = span->GetContext();
    span->End();
    return ctx.trace_id.ToHex();
}

}  // namespace

int main(int argc, char** argv)
{
    const std::string endpoint{(argc > 1) ? argv[1] : kDefaultEndpoint};

    // NOLINTNEXTLINE(concurrency-mt-unsafe) — single-threaded startup read.
    const char* const env_attrs = std::getenv("OTEL_RESOURCE_ATTRIBUTES");

    std::cout << "endpoint: " << endpoint << '\n'
              << "service.name: " << kServiceName << '\n'
              << "OTEL_RESOURCE_ATTRIBUTES: " << ((env_attrs != nullptr) ? env_attrs : "<unset>")
              << "\n\n"
              << "what the detectors should report:\n"
              << "  process.pid: " << getpid() << '\n'
              << "  host.name:   " << HostName() << '\n';

    // Registration order is significant — a later detector overrides an earlier
    // one on the same key. These two share no keys, so the order is free here.
    auto built = microtel::SdkBuilder{}
                     .WithEndpoint(endpoint)
                     .WithProtocol(microtel::Protocol::Grpc)
                     .WithServiceName(kServiceName)
                     .WithServiceVersion("1.0.0")
                     .WithResourceDetector(microtel::MakeProcessDetector())
                     .WithResourceDetector(microtel::MakeHostDetector())
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

    const std::shared_ptr<microtel::Tracer> tracer = provider->GetTracer("resource.demo", "1.0.0");

    const std::string trace_id = EmitTrace(*tracer);
    std::cout << "\ntrace_id: " << trace_id << '\n';

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
