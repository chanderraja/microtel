# `logs`

OTel log records from microtel, and how they pick up the trace they were
written in.

It builds a `Provider`, gets a `Logger` with `GetLogger`, and emits three
records with a severity, a body and attributes. The first is written before
any span exists. The other two are written inside a span made current with
`StartAsCurrentSpan`, and arrive carrying that span's trace ID and span ID
although the code never passes them. Then it calls `ForceFlush`, prints
`GetExporterHealth()`, and calls `Shutdown`.

## Run it

Start the shared stack, then run the binary with no arguments. Its default
endpoint is the stack's collector, `http://localhost:4317`; pass another as
`argv[1]`.

```bash
examples/stack/up.sh

cmake -S . -B build -DMICROTEL_BUILD_EXAMPLES=ON
cmake --build build --target microtel_example_logs

./build/examples/microtel_example_logs
```

Real output from one run (IDs differ every time):

```
[microtel info] resolved resource (profile "default", 2 attributes): service.name="microtel-logs-example", service.version="1.0.0"
checkout trace_id: 7a58f391b576b28d5e43a9a4d1d96d2d
checkout span_id:  c6f865baaa6d65ca
ForceFlush: Completed
batches_sent=2 batches_failed=0 queue_depth=0
Shutdown: Completed
```

The first line is microtel's own startup log, on stderr. `batches_sent=2` is
one batch of log records and one of spans: the health counters cover every
signal the provider exports, not just traces.

## See the records arrive

The stack has no log store, so there is no Grafana view for logs. Its
collector prints every log record it receives in full through a `debug`
exporter (the `logs` pipeline in
[`stack/collector-config.yaml`](../stack/collector-config.yaml)):

```bash
podman-compose -f examples/stack/compose.yaml -p microtel-stack logs otel-collector
# or: docker compose -f examples/stack/compose.yaml -p microtel-stack logs otel-collector
```

What the collector printed for the run above, trimmed to the record dump:

```
ResourceLog #0
Resource SchemaURL: 
Resource attributes:
     -> service.name: Str(microtel-logs-example)
     -> service.version: Str(1.0.0)
ScopeLogs #0
ScopeLogs SchemaURL: 
InstrumentationScope microtel-logs-example 1.0.0
LogRecord #0
ObservedTimestamp: 2026-10-01 14:00:59.966773792 +0000 UTC
Timestamp: 2026-10-01 14:00:59.966773532 +0000 UTC
SeverityText: INFO
SeverityNumber: Info(9)
Body: Str(service started)
Attributes:
     -> server.port: Int(8080)
Trace ID: 
Span ID: 
Flags: 0
LogRecord #1
ObservedTimestamp: 2026-10-01 14:00:59.966786817 +0000 UTC
Timestamp: 2026-10-01 14:00:59.966786727 +0000 UTC
SeverityText: WARN
SeverityNumber: Warn(13)
Body: Str(payment gateway slow, retrying)
Attributes:
     -> payment.attempt: Int(2)
     -> payment.gateway: Str(acme-pay)
Trace ID: 7a58f391b576b28d5e43a9a4d1d96d2d
Span ID: c6f865baaa6d65ca
Flags: 1
LogRecord #2
ObservedTimestamp: 2026-10-01 14:00:59.966787768 +0000 UTC
Timestamp: 2026-10-01 14:00:59.966787438 +0000 UTC
SeverityText: ERROR
SeverityNumber: Error(17)
EventName: checkout.failed
Body: Str(checkout failed)
Attributes:
     -> http.response.status_code: Int(502)
Trace ID: 7a58f391b576b28d5e43a9a4d1d96d2d
Span ID: c6f865baaa6d65ca
Flags: 1
```

`service started` has no trace ID: no span was current when it was written.
The two records written inside `example.checkout` carry the IDs the program
printed, and `Flags: 1` is the span's sampled flag. The span itself went to
Tempo as usual, so the same trace ID finds `example.checkout`, status `Error`,
in Grafana's Explore → Tempo. That is the point of correlation: from a log line
to the trace it happened in, and back.

This run used the stack's pinned collector and Tempo images with the committed
`collector-config.yaml` and `tempo.yaml`, started by hand with `podman run`
rather than through `up.sh`; the configuration and the output are the same.

## What to notice in the code

**There is no logs switch.** A provider exporting over HTTP/2 (OTLP/gRPC, as
here, or OTLP/HTTP) sends log records to the same collector as its spans, on
the logs service path. `GetLogger` returns a working `Logger` without any extra
configuration. The exception is a provider built with
`SdkBuilder::WithExportTransport`: logs are off there until
`ExportTransportOptions::logs` is `true`, and `GetLogger` returns a no-op logger
until then, with one `Warn`.

**`Logger::Emit` takes a `LogRecord` by value** and is `noexcept`. It never
blocks on I/O: the record joins a batch, and the batch is exported on the
exporter's worker thread, as spans are. A record that cannot be queued is
dropped and counted in `GetExporterHealth()`, never reported to the caller.
`MakeRecord` in `main.cpp` fills the fields most records set; everything else
keeps its default. `observed_time` is stamped by the SDK at `Emit`, and a zero
`time` means "unknown". As with span attributes, string values are built as
`std::string` explicitly: a bare literal is a `const char*` and would bind to
the `bool` alternative of `AttributeValue`.

**Correlation needs the span to be current, not merely open.**
`StartAsCurrentSpan` installs the span as the calling thread's current context
until the returned `ScopedSpan` dies; `Emit` reads that context when the
record's `trace_id` was left unset. A span from plain `StartSpan` is not
current, and a record written while it is open is not correlated. Neither is a
record written on another thread, unless that thread installs the context with
`ScopedContext` (see [`context_propagation`](../context_propagation/)). Only a
sampled span is copied in. A `trace_id` you set yourself is never overwritten,
which is how a bridge forwards records that are already correlated.

**One `Logger` per instrumentation scope.** `GetLogger(name, version)` returns
the same instance for the same pair. A `Logger` borrows the provider's
pipeline, so don't keep one past the provider's `Shutdown`.

## Already using a logging library?

You don't have to call `Logger::Emit` yourself. Three bridges turn an existing
library's output into OTel log records. Each ends in `Logger::Emit`, so the
correlation rules above apply to them too:

- [spdlog](../../src/adapters/spdlog/README.md)
- [glog](../../src/adapters/glog/README.md)
- [log4cxx](../../src/adapters/log4cxx/README.md)

## Without a collector

With nothing on 4317, the run still completes and says what went wrong:

```
[microtel info] resolved resource (profile "default", 2 attributes): service.name="microtel-logs-example", service.version="1.0.0"
warning: Connect() to http://localhost:4317 failed: connection refused
         is an OTLP/gRPC collector listening there? continuing; export will be retried on flush.
checkout trace_id: d3c89a5ff2020c1e6e34ff0c717c3b0c
checkout span_id:  5d1e3c81b576d5ae
ForceFlush: TimedOut
batches_sent=0 batches_failed=0 queue_depth=1
Shutdown: Completed
```

The program exits with status 2. It exits 0 only when `ForceFlush` returns
`Completed` **and** `batches_failed` is 0 **and** the `PartialSuccessRejection`
drop counter is 0: `Completed` means the queues drained, which is also true
when the collector rejected a batch, and a collector answering OTLP partial
success counts the batch as sent while rejecting some or all of its items. Against a
collector with no logs pipeline (traces only), the flush completes but the
log batch fails, and the program exits 2:

```
checkout trace_id: 265ebef56577cf9604222301d9e7db51
checkout span_id:  0149ecd5cd283332
ForceFlush: Completed
batches_sent=1 batches_failed=1 queue_depth=0
last_error: UNIMPLEMENTED (12): unknown service opentelemetry.proto.collector.logs.v1.LogsService
Shutdown: Completed
```

To see records with no collector at all,
[`console_trace`](../console_trace/) shows the pattern: an `ExportTransport`
of the application's own that prints what it is handed. For logs, enable
`ExportTransportOptions::logs` and decode an `ExportLogsServiceRequest`.
