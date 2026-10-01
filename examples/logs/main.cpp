// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// logs — emit OTel log records through microtel, and see them carry the
// active span's trace and span IDs.
//
// Builds a provider, gets a Logger, emits one record outside any span and two
// inside one, flushes, prints exporter health, and shuts down. The records
// inside the span are correlated automatically: the SDK copies the calling
// thread's current span into each record whose trace_id was left unset.
//
// Usage:
//   logs [endpoint]
//
// where [endpoint] defaults to http://localhost:4317, the OTLP/gRPC receiver
// of the shared examples stack, started with:
//   examples/stack/up.sh
//
// The stack's collector prints every log record it receives through its
// `debug` exporter, so the run is verifiable with:
//   podman logs microtel-stack_otel-collector_1    (or docker compose logs)
//
// There is no logs switch to turn on. A provider exporting over HTTP/2 (gRPC
// or OTLP/HTTP) sends logs to the same collector as its spans, on the logs
// service path. Only a provider built with SdkBuilder::WithExportTransport
// needs `.logs = true` in its ExportTransportOptions.

#include "microtel/log_record.hpp"
#include "microtel/logger.hpp"
#include "microtel/provider.hpp"
#include "microtel/sdk_builder.hpp"
#include "microtel/span.hpp"
#include "microtel/tracer.hpp"

#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace
{

constexpr std::chrono::seconds kFlushTimeout{5};
constexpr std::chrono::seconds kShutdownTimeout{5};
constexpr std::int64_t kListenPort{8080};
constexpr std::int64_t kPaymentAttempt{2};
constexpr std::int64_t kHttpStatusBadGateway{502};
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
        case microtel::Status::InvalidArgument:
            return "InvalidArgument";
        case microtel::Status::Unsupported:
            return "Unsupported";
    }
    return "Unknown";
}

// A LogRecord with the fields most records set. Everything else keeps its
// default: `observed_time` is stamped by the SDK at Emit(), `time` 0 means
// "unknown", and an unset trace_id is what lets the SDK correlate the record
// with the current span.
//
// As with span attributes, a string value is built as std::string explicitly:
// a bare literal is a const char* and would bind to AttributeValue's bool.
microtel::LogRecord MakeRecord(microtel::SeverityNumber severity,
                               std::string severity_text,
                               std::string body,
                               std::vector<microtel::KeyValue> attributes)
{
    microtel::LogRecord record;
    record.time = std::chrono::system_clock::now();
    record.severity_number = severity;
    record.severity_text = std::move(severity_text);
    record.body = std::move(body);
    record.attributes = std::move(attributes);
    return record;
}

// One request handled inside a span made current with StartAsCurrentSpan.
// Neither record names the span, yet both arrive carrying its IDs. Returns the
// span's context, so main() can print what the records should carry.
microtel::SpanContext HandleCheckout(microtel::Tracer& tracer, microtel::Logger& logger)
{
    const microtel::ScopedSpan span = tracer.StartAsCurrentSpan(
        "example.checkout",
        {.kind = microtel::SpanKind::Server, .parent = {}, .start_time = {}, .attributes = {}});
    span->SetAttribute("http.request.method", std::string{"POST"});

    logger.Emit(MakeRecord(microtel::SeverityNumber::Warn,
                           "WARN",
                           "payment gateway slow, retrying",
                           {{.key = "payment.attempt", .value = kPaymentAttempt},
                            {.key = "payment.gateway", .value = std::string{"acme-pay"}}}));

    microtel::LogRecord failed =
        MakeRecord(microtel::SeverityNumber::Error,
                   "ERROR",
                   "checkout failed",
                   {{.key = "http.response.status_code", .value = kHttpStatusBadGateway}});
    failed.event_name = "checkout.failed";
    logger.Emit(std::move(failed));

    span->SetStatus(microtel::StatusCode::Error, "payment gateway unavailable");
    return span->GetContext();
}  // the span ends here, then the previous context is restored

}  // namespace

int main(int argc, char** argv)
{
    const std::string endpoint{(argc > 1) ? argv[1] : kDefaultEndpoint};

    auto built = microtel::SdkBuilder{}
                     .WithEndpoint(endpoint)
                     .WithProtocol(microtel::Protocol::Grpc)
                     .WithServiceName("microtel-logs-example")
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
                     "continuing; export will be retried on flush.\n";
    }

    // One Logger per instrumentation scope, cached by (name, version). Hold it
    // no longer than the provider: it borrows the provider's pipeline.
    const std::shared_ptr<microtel::Logger> logger =
        provider->GetLogger("microtel-logs-example", "1.0.0");
    const std::shared_ptr<microtel::Tracer> tracer =
        provider->GetTracer("microtel-logs-example", "1.0.0");

    // No span is current here, so this record goes out uncorrelated.
    logger->Emit(MakeRecord(microtel::SeverityNumber::Info,
                            "INFO",
                            "service started",
                            {{.key = "server.port", .value = kListenPort}}));

    const microtel::SpanContext checkout = HandleCheckout(*tracer, *logger);
    std::cout << "checkout trace_id: " << checkout.trace_id.ToHex() << '\n'
              << "checkout span_id:  " << checkout.span_id.ToHex() << '\n';

    // Logs are batched like spans. ForceFlush exports both pipelines now.
    const microtel::Status flush = provider->ForceFlush(kFlushTimeout);
    std::cout << "ForceFlush: " << StatusToString(flush) << '\n';

    // One health surface for every signal: batches_sent counts the trace
    // batch and the log batch together.
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

    // Completed only means the queues drained; a batch the collector rejected
    // still counts as drained. Success needs no failed batch as well.
    const bool delivered = flush == microtel::Status::Completed && health.batches_failed == 0;
    return delivered ? 0 : 2;
}
