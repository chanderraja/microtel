# microtel Logs Design

**Status:** Draft — pending reviewer sign-off (M14 / v1.3). Precedes the L4/L5
implementation the way `docs/metrics-design.md` (M11) preceded M12.

This document settles the pipeline and semantic decisions for the OTel logs
signal so that L4 (SDK log pipeline) and L5 (`Provider::GetLogger` wiring) can be
implemented against a locked design. The L1 protos, L2 API surface
(`Logger`, `LogRecord`, `SeverityNumber`, `LogBatchHandle`), and L3 encoder
(`ILogEncoder`, `OtlpEncoder::Encode(LogBatchHandle)`) already shipped; this doc
retroactively records their contracts and decides everything downstream. It is a
design document, not an interface header: it decides *what* and *why*. Concrete
`include/microtel/` headers it implies are drafted during L4/L5 and, if they
touch a locked surface, follow the ICP process.

The through-line: **the log pipeline mirrors the trace pipeline part-for-part.**
Where a decision could diverge, the default is "do what traces do" unless a
project constraint says otherwise.

## Scope

In scope for v1.3 (this doc decides all of these):

- The SDK log pipeline: `SdkLogger` → `ILogRecordProcessor` → `ILogExporter` →
  `ILogEncoder` (L3, done) → `IWireCodec` → `ITransport`.
- `ILogRecordProcessor` contract + `BatchLogRecordProcessor` (production) and
  `SimpleLogRecordProcessor` (tests), mirroring the span processors.
- `ILogExporter` + `OtlpLogExporter` reusing the OTLP encoder and the shared
  nghttp2 transport.
- Trace correlation from the active span context via the existing
  `ICurrentSpanSource` seam.
- Per-record attribute limits, `observed_time` backfill, and drop accounting.
- `Provider::GetLogger` and `SdkBuilder` wiring (L5).
- Configuration surface (`OTEL_EXPORTER_OTLP_LOGS_*`, `microtel.toml [logs]`).
- The spdlog bridge adapter (L6), specified here, implemented last.

Explicitly **out of scope** (anti-goals):

- Log sampling / filtering beyond severity threshold (v1.4+).
- Log-to-metric or log-to-span derivation.
- Persistence / backfill / disk buffering.
- A second logs transport; logs share the one nghttp2 session.

## Sign-off checklist

Each item has a proposed **Decision** below. Reviewer approves by checking every
box (and editing any decision they want changed first).

- [ ] §1 Pipeline shape — mirror traces; share the transport, separate codec + exporter
- [ ] §2 Log record processor — `Batch` (prod) + `Simple` (tests); inherit BSP defaults
- [ ] §3 Log exporter — `OtlpLogExporter` reusing L3 encoder + shared transport
- [ ] §4 Trace correlation — fill from `ICurrentSpanSource` when `trace_id` invalid; on by default
- [ ] §5 Attribute limits & drop accounting — new `LogAttributeLimit` **DropReason (ICP)**
- [ ] §6 Batching, memory & threading model
- [ ] §7 Wire mapping (OTLP logs; encoder/transport reuse)
- [ ] §8 Provider / Builder API surface (`GetLogger`)
- [ ] §9 Configuration & compatibility
- [ ] §10 spdlog bridge adapter

---

## §1 Pipeline shape

**Decision.** The pipeline is the trace pipeline with `Log` substituted for
`Span`:

```
Logger::Emit(LogRecord)              [caller thread, noexcept]
  → SdkLogger                        stamps observed_time, trace correlation
  → ILogRecordProcessor::OnEmit      enqueue (BatchLogRecordProcessor)
  → worker thread                    drains into LogBatchHandle (records+Resource+Scope)
  → ILogExporter::Export             hand to exporter worker, return
  → ILogEncoder::Encode  [L3 done]   → EncodedPayload (OTLP logs bytes)
  → IWireCodec                       logs endpoint (/v1/logs or LogsService)
  → ITransport                       the one shared nghttp2 session
```

The single `ITransport` (one nghttp2 session) is shared across traces, metrics,
and logs, exactly as metrics reuse it today. Logs add their **own** `IWireCodec`
(pointed at the logs endpoint) and their **own** `ILogExporter` worker, mirroring
`m_metric_codec` / `m_metric_exporter` in `SdkProvider`. Member declaration order
must preserve reverse-destruction (processor torn down before exporter, exporter
before codec/transport), consistent with `SdkProvider`'s documented ordering.

## §2 Log record processor

**Decision.** Two implementations behind one internal interface, mirroring
`ISpanProcessor`:

```cpp
class ILogRecordProcessor {
public:
    virtual ~ILogRecordProcessor() noexcept = default;
    virtual void OnEmit(LogRecord&& record) noexcept = 0;      // any caller thread
    [[nodiscard]] virtual Status ForceFlush(std::chrono::milliseconds) noexcept = 0;
    [[nodiscard]] virtual Status Shutdown(std::chrono::milliseconds) noexcept = 0;
};
```

- `BatchLogRecordProcessor` — queue + worker + batch, drained on the
  `schedule_delay` timer, on reaching `max_export_batch_size`, or on
  `ForceFlush`. **Inherits the trace BSP defaults:** `max_queue_size = 8192`,
  `max_export_batch_size = 512`, `schedule_delay = 5s`, same
  drop-oldest/drop-newest policy surface.
- `SimpleLogRecordProcessor` — synchronous passthrough, for tests and the dumb
  mock, mirroring `SimpleSpanProcessor`.

There is no `OnStart`/`OnEnd` split — a log record has no lifetime, so the
processor has a single `OnEmit` entry point. Records are grouped into a
`LogBatchHandle` per `(Resource, InstrumentationScope)`; a single flush may
produce more than one handle when a batch spans scopes.

## §3 Log exporter

**Decision.** `ILogExporter` mirrors `IExporter` (same `ExportResult` enum,
same non-blocking `Export`), and `OtlpLogExporter` reuses the L3 `ILogEncoder`
(no new encoding code) plus the shared codec/transport:

```cpp
class ILogExporter {
public:
    virtual ~ILogExporter() noexcept = default;
    [[nodiscard]] virtual ExportResult Export(LogBatchHandle&&) noexcept = 0;
    [[nodiscard]] virtual Status ForceFlush(std::chrono::milliseconds) noexcept = 0;
    [[nodiscard]] virtual Status Shutdown(std::chrono::milliseconds) noexcept = 0;
};
```

`ExportResult` (`Success`/`Failure`/`Dropped`/`AlreadyShutDown`) is reused
verbatim from `internal/exporter.hpp` — no new enum. Retry classification,
partial-success parsing, and drop accounting reuse the same machinery as the
trace and metric exporters.

## §4 Trace correlation

**Decision.** On by default. When `Emit()` is called with `trace_id` left as the
invalid all-zeros default and a span is active on the calling thread, `SdkLogger`
fills `trace_id`, `span_id`, and `trace_flags` from
`ICurrentSpanSource::GetCurrentSpan()` — the same seam metrics already use for
exemplars. If the caller set `trace_id` explicitly, the SDK never overwrites it.
Correlation is disabled by a builder flag / `microtel.toml` setting for callers
who bridge already-correlated records.

`observed_time` is stamped to `system_clock::now()` at `Emit()` when unset;
`time` is left untouched (0 = unknown, per the L2 header contract).

## §5 Attribute limits & drop accounting

**Decision.** A per-record attribute cap (default 128, configurable) is enforced
at `Emit()`; excess attributes are dropped and `dropped_attributes_count` is
incremented on the record, matching the OTLP field already present in
`LogRecord`.

Drop reasons reuse the existing `DropReason` taxonomy where they apply:
`QueueFull` (queue saturated), `PostShutdown` (emit after shutdown),
`RecordTooLarge` (oversized encoded record). The per-record attribute cap needs a
**new enumerator `LogAttributeLimit`** — and because every `DropReason` is part
of the public `HealthSnapshot` counter array (the header calls adding one an ICP,
the same rule that produced ICP 0008 for metrics), **this requires an ICP** to be
merged before L4 lands. Flagged as an open item below.

## §6 Batching, memory & threading model

**Decision.** Same three-thread model as traces:

- **Caller thread** — `Emit()` stamps time/correlation, moves the record into the
  processor queue, returns. `noexcept`, never blocks on I/O.
- **Log worker thread** (one, owned by `BatchLogRecordProcessor`) — drains the
  queue into `LogBatchHandle`s and calls `ILogExporter::Export`.
- **Exporter worker + I/O thread** — the shared nghttp2 machinery.

`LogRecord` and `LogBatchHandle` are move-only and RAII-owned; the batch owns its
records `vector`, a `shared_ptr<const Resource>`, and the scope. No record is
copied on the hot path. TSAN must be run when touching the processor/worker
handoff (per CLAUDE.md threading discipline).

## §7 Wire mapping

**Decision.** Already implemented in L3 and unchanged: `LogBatchHandle` →
`ExportLogsServiceRequest` (`ResourceLogs` → `ScopeLogs` → `LogRecord`) via
`OtlpEncoder::Encode`. The HTTP path posts to `/v1/logs`; the gRPC path calls
`opentelemetry.proto.collector.logs.v1.LogsService/Export`. Both reuse the
existing `HttpWireCodec` / `GrpcWireCodec` with the logs endpoint substituted.

## §8 Provider / Builder API surface (L5)

**Decision.** `Provider` gains:

```cpp
[[nodiscard]] virtual std::shared_ptr<Logger>
GetLogger(std::string_view name, std::string_view version = {}) = 0;
```

`SdkProvider::GetLogger` lazily creates the log pipeline on first call (mirroring
the lazy metrics `GetMeter`), caches `SdkLogger`s per `(name, version)`, and
returns a shared no-op logger when no logs exporter is configured. `SdkBuilder`
gains `WithLogExporter(...)` (or auto-enables logs when a logs endpoint is
configured). Adding a pure-virtual method to the public `Provider` is a breaking
change to a locked interface → **L5 lands behind its own ICP.**

## §9 Configuration & compatibility

**Decision.** Standard OTel env precedence: `OTEL_EXPORTER_OTLP_LOGS_ENDPOINT`,
`OTEL_EXPORTER_OTLP_LOGS_PROTOCOL`, `OTEL_EXPORTER_OTLP_LOGS_HEADERS`, falling
back to the generic `OTEL_EXPORTER_OTLP_*`. `microtel.toml` gains a `[logs]`
section paralleling `[traces]`/`[metrics]`, with `endpoint`, `protocol`,
`batch`, and `correlation` keys. Logs are opt-in: absent any logs config, the
provider returns no-op loggers and spins up no log worker.

## §10 spdlog bridge adapter (L6)

**Decision.** Shipped as a separate adapter target (like the experimental compat
shims), not linked into the core. A `SpdlogSink` implements `spdlog::sinks::sink`
and forwards each `spdlog::details::log_msg` to a `Logger::Emit`, mapping spdlog
levels → `SeverityNumber` and the formatted payload → `LogRecord::body`. It is
the only place logs depend on the optional spdlog dependency; core builds without
it. Implemented after L4/L5 so it targets a stable `Logger`.

## Open items flagged for the reviewer

1. **`LogAttributeLimit` DropReason (§5) requires an ICP** before L4 lands —
   it extends the public `HealthSnapshot` counter array. Recommend authoring it
   alongside L4.1 so the counter exists when the processor needs it.
2. **`Provider::GetLogger` (§8) is a locked-interface change → L5 ICP.**
3. **Shared vs. separate transport (§1):** decision is *share* the one nghttp2
   session. Confirm this is acceptable head-of-line-blocking-wise, or whether
   logs warrant a second session (would be a dependency-closure change).
4. **Default per-record attribute cap (§5) = 128** — confirm, or align to
   whatever the span attribute default settles at.

## Increment plan

- **L4.1** — `ILogRecordProcessor` + `SdkLogger` (Emit → processor; time stamp +
  correlation seam), dumb mock + fake. Ships the `LogAttributeLimit` ICP.
- **L4.2** — `BatchLogRecordProcessor` + `SimpleLogRecordProcessor`.
- **L4.3** — `ILogExporter` + `OtlpLogExporter` (reuses L3 encoder + transport).
- **L5** — `Provider::GetLogger` + `SdkBuilder` wiring (behind an ICP).
- **L6** — spdlog bridge adapter.

## References

- `docs/metrics-design.md` — the M11 precedent this doc mirrors.
- `docs/interfaces.md` §4.4 (`IExporter`), §4.6 (`ISpanProcessor`) — contracts mirrored.
- `include/microtel/logger.hpp`, `log_record.hpp`, `internal/log_batch.hpp`,
  `internal/log_encoder.hpp` — the L2/L3 surface.
- ICP 0008 — the metrics-drop-reason precedent for §5's new enumerator.
- OTel Logs Data Model — https://opentelemetry.io/docs/specs/otel/logs/data-model/
