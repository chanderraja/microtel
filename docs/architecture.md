# microtel Architecture

**Status:** M0 deliverable. Normative for the layered structure of v1.
**Companion documents:** `threading-model.md`, `memory-model.md`, `error-model.md`, `interfaces.md`, `grpc-wire-protocol.md`, and `sequences/*.md`.
**Source of truth for rationale:** `microtel-spec.md` §5.

---

## 1. Purpose and audience

This document is the architectural entry point for microtel. It establishes the layered structure of the runtime, what each layer owns, and how they connect. It is intentionally short — detail belongs in the four sister documents:

- **`threading-model.md`** — which threads exist, which methods are safe to call from where.
- **`memory-model.md`** — ownership rules for every resource, RAII wrappers, byte budgets.
- **`error-model.md`** — `microtel::Expected` (alias — see ICP 0002) vs `noexcept` vs structured status, drop-vs-fail.
- **`interfaces.md`** — every locked internal interface in full contract form.

The audience is contributors and AI agents picking up an implementation track from M2 onward. A reader who finishes this document should know what to read next for the track they own — not have implementation answers, but know where the answers live.

This document covers v1 only. Forward-looking architecture (control plane, metrics, logs, HTTP/3, leaf/concentrator) is in `microtel-roadmap.md`.

---

## 2. Layered structure

```
┌─────────────────────────────────────────────────────────────────┐
│             Application (C++; Python optional)                  │
└─────────────────────────────────────────────────────────────────┘
                                │
                                ▼
┌─────────────────────────────────────────────────────────────────┐
│              OpenTelemetry Trace API (v1)                       │   include/microtel/
│        Tracer · Span · Context · W3C Propagators                │   src/api/        (Track A)
└─────────────────────────────────────────────────────────────────┘
                                │
                                ▼
┌─────────────────────────────────────────────────────────────────┐
│                     SDK (minimal, v1)                           │
│  Resource · AlwaysOn / AlwaysOff / TraceIdRatio / ParentBased   │   src/sdk/        (Track A)
│  BatchSpanProcessor · ForceFlush · Shutdown                     │
└─────────────────────────────────────────────────────────────────┘
                                │
                                ▼
┌─────────────────────────────────────────────────────────────────┐
│               Exporter (protocol-agnostic)                      │   src/exporter/   (Track A)
│      Batching · retry orchestration · drop accounting           │
└─────────────────────────────────────────────────────────────────┘
                                │
                                ▼
┌─────────────────────────────────────────────────────────────────┐
│                       OTLP Wire Encoder                         │
│      upb (vendored, pinned) + OTel .proto definitions           │   src/wire/encoder/ (Track F)
│           C accessors wrapped behind a thin C++ API             │
└─────────────────────────────────────────────────────────────────┘
                                │ EncodedPayload (bytes)
                                ▼
                     ┌──────────────────────┐
                     │     IWireCodec       │   one interface, two implementations
                     └──────────────────────┘
                  ▲                           ▲
                  │                           │
┌─────────────────────────┐   ┌─────────────────────────────────┐
│  OTLP/HTTP Codec        │   │  OTLP/gRPC Codec                │
│  - application/         │   │  - 5-byte length-prefix framing │
│    x-protobuf           │   │  - :path: /<svc>/<method>       │
│  - POST /v1/traces      │   │  - te: trailers                 │   src/wire/http/  (B)
│  - Retry-After          │   │  - parses grpc-status / RetryInfo│   src/wire/grpc/  (C)
└──────────┬──────────────┘   └─────────────────┬───────────────┘
           │                                    │
           └──────────────┬─────────────────────┘
                          ▼
┌─────────────────────────────────────────────────────────────────┐
│                  Transport: HTTP/2 (nghttp2)                    │
│       One connection per endpoint/protocol tuple (default)      │   src/transport/  (Track D)
│           TLS via OpenSSL · epoll / kqueue I/O loop             │
└─────────────────────────────────────────────────────────────────┘
                                │
                                ▼
                      OTel Collector (any)
                or any OTLP-compatible backend
```

Common services (logging, errors, limits, RAII wrappers, config, diagnostics, clocks) are in `src/common/` and consumed by every layer.

---

## 3. Per-component responsibilities

Each subsection lists, for one component: what it owns, what it does **not** own (its boundary), the interface it provides, and the interfaces it consumes. "Provides" and "consumes" reference entries in `interfaces.md`.

### 3.1 API — `src/api/`, public headers in `include/microtel/`

**Owns:** the `Tracer` and `Span` types as seen by application code, the W3C Trace Context propagator, the `Context` carrier.

**Does not own:** sampling decisions (delegated to the SDK), span batching, encoding, or any I/O.

**Public headers (M0 deliverables, no method bodies):** `tracer.hpp`, `provider.hpp`, `sdk_builder.hpp`, `resource.hpp`, `status.hpp`, `error.hpp`, `log_sink.hpp`.

**Hot-path guarantee.** `StartSpan`, `SetAttribute`, `AddEvent`, `End` are `noexcept`. The unsampled-span path performs no allocation; `StartSpan` always returns a usable span object even when the SDK is shut down or the queue is full (`error-model.md` §1).

### 3.2 SDK — `src/sdk/`

**Owns:** `Resource` resolution and merging, the four built-in samplers (`AlwaysOn`, `AlwaysOff`, `TraceIdRatio`, `ParentBased`), `BatchSpanProcessor`, `SimpleSpanProcessor`, `Provider` lifecycle (`ForceFlush`, `Shutdown`).

**Does not own:** wire-protocol details, retry policy, transport state. The SDK sees only the `IExporter` interface.

**Provides:** `ISpanProcessor`, `ISampler`, `IResourceDetector`. The four samplers and two processors are concrete implementations of these.

**Consumes:** `IExporter` (to push completed batches), `IClock` (for batch deadlines and time-based behaviour), `IDiagnosticsSink` (drop counters, batch counts).

### 3.3 Exporter — `src/exporter/`

**Owns:** the worker thread that drains the processor queue, batch construction, retry orchestration, backoff with jitter, drop accounting at the export-pipeline boundary.

**Does not own:** wire-protocol shape (delegated to `IWireCodec`) or socket-level I/O (delegated to `ITransport`). The exporter is **protocol-agnostic** — it cannot tell HTTP from gRPC.

**Provides:** `IExporter`.

**Consumes:** `IOtlpEncoder` (to produce `EncodedPayload`), `IWireCodec` (to send and interpret the response), `IClock`, `IDiagnosticsSink`.

The exporter respects the result returned by the codec — including retry classification — but does not re-classify it. Protocol-specific knowledge (HTTP `Retry-After`, gRPC `RetryInfo`) lives entirely inside the codec; the exporter sees only the unified `WireResult`.

### 3.4 OTLP Encoder — `src/wire/encoder/`

**Owns:** the upb bindings, generated upb C accessors for OpenTelemetry protos, the `UpbArena` RAII wrapper used during encode, the `IOtlpEncoder` C++ wrapper.

**Does not own:** anything about HTTP, gRPC, framing, headers, or transport. The encoder produces protobuf bytes; downstream layers wrap them.

**Containment rule (non-negotiable).** `src/wire/encoder/` is the **only** place in the codebase that includes upb headers or references upb symbols. No upb type appears in any header outside this directory. Violating this is rejected at review.

**Output type.** `EncodedPayload` — owned bytes plus size, shaped as `std::unique_ptr<std::byte[]>` + `std::size_t`. Move-only. The encoder allocates the buffer on every call; the upb arena is destroyed when `Encode()` returns. See `memory-model.md` for the per-encode arena rule and the rationale.

**Provides:** `IOtlpEncoder`.

**Consumes:** the SDK's `Resource` and the batch of completed spans handed in by the exporter — both as plain C++ values, no upb leakage.

### 3.5 Wire codecs — `src/wire/http/`, `src/wire/grpc/`

**Owns:** for each codec: HTTP/2 header construction (request and response), framing (HTTP body for the HTTP codec, 5-byte gRPC length-prefix for the gRPC codec), trailer parsing where applicable, status interpretation, retry classification, response-body capture for diagnostics.

**Does not own:** the bytes of the OTLP payload (encoder owns those), the socket or nghttp2 session (transport owns those), retry orchestration (exporter owns that).

**Provides:** `IWireCodec` — **one** interface with two implementations. Both implementations expose the same `Send(EncodedPayload, Deadline) -> WireResult` shape.

`WireResult` carries: success / failure status, retryable / non-retryable classification, `retry_after` (sourced from `Retry-After` for HTTP and `RetryInfo` for gRPC; the exporter sees neither field name — only `retry_after`), partial-success rejected count, raw response body for diagnostics (capped at `max_response_bytes`).

**Consumes:** `ITransport`.

The decision to keep this as a single interface, including the rule that the codec — not the exporter — owns retry classification (notably: `RESOURCE_EXHAUSTED` without `RetryInfo` is non-retryable, and the codec returns it pre-classified), is recorded in `interfaces.md`. Adding a protocol-specific method to `IWireCodec` later is an ICP, not a punch-through.

The deeper implementation notes for the gRPC codec — state machine, byte-level edge cases, `RetryInfo` decoding — live in `grpc-wire-protocol.md`.

### 3.6 Transport — `src/transport/`

**Owns:** the OpenSSL `SslCtx` and `SslSession`, the nghttp2 session, the socket file descriptor, the I/O loop (epoll on Linux, kqueue on BSD/macOS), the per-endpoint connection state machine (open / connecting / reconnecting / closed), reconnect with backoff and jitter on socket-level failures.

**Does not own:** any OTLP semantics. The transport sends bytes and surfaces response bytes; it does not parse OTLP responses.

**Provides:** `ITransport` — `Connect`, `Send`, `Close`, plus a callback path for `OnResponse`. The interface is the seam where an `nghttp3`-based HTTP/3 transport could drop in for v1.5+; no HTTP/3 work in v1.

**Consumes:** `IReactor` (epoll/kqueue abstraction, present primarily as a test seam), the RAII wrappers in `src/common/raii/` (`Socket`, `SslCtx`, `SslSession`, `Nghttp2Session`).

**Connection policy.** One HTTP/2 connection per `(endpoint, protocol)` tuple by default. Optional experimental coalescing for shared HTTP+gRPC endpoints is gated behind explicit config and a startup preflight; off by default. See `microtel-spec.md` §5.2.

### 3.7 Common — `src/common/`

A small set of shared, layer-independent services:

- **`src/common/raii/`** — `Socket`, `SslCtx`, `SslSession`, `Nghttp2Session`, `UpbArena`. Each is move-only, has a `noexcept` destructor, and exposes `release()` for explicit ownership transfer.
- **`src/common/config/`** (Track E) — `microtel.toml` parser, env-var resolution, validation. Returns a frozen, validated `Config` value to the SDK.
- **Logging** — spdlog-by-default (header-only, `SPDLOG_USE_STD_FORMAT`), with a minimal stderr fallback when `MICROTEL_USE_SPDLOG=OFF`. Sink injection via `LogSink` (public). Internal diagnostics are **never** routed back through microtel's own OTLP exporter — see `error-model.md` §9.
- **Errors and limits** — `microtel::Error`, `ConfigError`, the lifecycle `Status` enum, the byte / record / response / trailer / decompression budget constants from `microtel-spec.md` §5.5.
- **Clocks** — `IClock` and `ISteadyClock`. Tests inject fakes; production uses `std::chrono::system_clock` and `std::chrono::steady_clock`.
- **Diagnostics** — `IDiagnosticsSink` collects per-reason drop counters, batch counters, last-error timestamps, and is the backing store behind `Provider::GetExporterHealth()`.

---

## 4. Two walkthroughs

The full set of normative flows lives in `sequences/*.md`. Two abbreviated walkthroughs are reproduced here so this document stands alone for an introductory read.

### 4.1 Happy path — sampled span, successful export

1. Application calls `tracer->StartSpan("handle_request")`. The API consults the configured `ISampler`; the result is `SAMPLED`. A `Span` object is allocated and returned.
2. Application calls `span->SetAttribute(...)`, `span->AddEvent(...)`. These are `noexcept`; failures (limits exceeded) are dropped and counted.
3. Application calls `span->End()` (or the RAII close fires). The completed span record is enqueued on the MPSC queue read by the exporter worker thread. The caller thread does not block.
4. Exporter worker thread wakes (queue notification or batch deadline), drains up to `max_export_batch_size` records, builds a batch.
5. Exporter calls `IOtlpEncoder::Encode(batch)`. The encoder allocates an `UpbArena`, fills upb messages from the C++ batch, serialises into a freshly-allocated buffer, returns an `EncodedPayload`. The arena is destroyed before `Encode()` returns.
6. Exporter calls `IWireCodec::Send(payload, deadline)`. The codec constructs HTTP/2 headers and (for gRPC) the 5-byte length-prefixed framing, hands off to `ITransport`.
7. Transport submits the request on its nghttp2 session on the I/O thread. nghttp2 writes to the socket; OpenSSL handles TLS.
8. Response arrives. The wire codec parses status / trailers, optionally captures partial-success rejected counts, returns a `WireResult` to the exporter.
9. Exporter records success in `IDiagnosticsSink`. The `EncodedPayload` and `Span` records are released.

### 4.2 Error path — retryable failure with `Retry-After`

Steps 1–7 as above. At step 8, the response is `503 Service Unavailable` with `Retry-After: 2`.

8. The HTTP wire codec classifies the response: `success=false`, `retryable=true`, `retry_after=2s`. Returns `WireResult` to the exporter. The codec also captures the (capped) response body for diagnostics.
9. Exporter records the failure in `IDiagnosticsSink` with the retryable-classification reason.
10. Exporter sleeps until `retry_after` elapses (jitter applied per `microtel-spec.md` §5).
11. Exporter calls `IOtlpEncoder::Encode(batch)` **again**. A fresh arena, a fresh `EncodedPayload`. The original `EncodedPayload` was released after step 8 — encoded bytes do not survive across retries by design (`memory-model.md` §3 records the rationale).
12. Send succeeds on the retry. Drop counter for retryable-recovered increments.

If the retry budget is exhausted before success, the batch is dropped with an explicit `retry_budget_exhausted` reason on the drop counter, and a rate-limited diagnostic is emitted.

A non-retryable failure (e.g., 415 Unsupported Media Type, or gRPC `RESOURCE_EXHAUSTED` without `RetryInfo`) skips the retry loop entirely; the codec returns `retryable=false` and the exporter drops the batch with the corresponding reason.

---

## 5. Cross-cutting concerns

**Diagnostics.** Every component reports drop reasons, batch counts, and last-error timestamps to a single `IDiagnosticsSink`. The sink is the backing store behind `Provider::GetExporterHealth()` (spec §6.4). Internal diagnostic logs are emitted via spdlog (or the stderr fallback) and are **never** recursively exported through microtel's own OTLP exporter — preventing the failure loop where a broken exporter generates more telemetry it can't ship.

**Drop accounting.** Reasons are explicit: `queue_full`, `record_too_large`, `span_attribute_limit`, `span_event_limit`, `span_link_limit`, `response_too_large`, `partial_success_rejection`, `retry_budget_exhausted`, `non_retryable_failure`, `post_shutdown`. Counters are per-reason, exposed in health and rate-limited in logs. Definitions and which layer increments each are pinned in `error-model.md`.

**Limits.** Byte budgets (`max_total_queue_bytes`, `max_record_bytes`, `max_response_bytes`, `max_trailer_bytes`, `max_decompressed_bytes`) and span structural limits (`attribute_count_limit`, `event_count_limit`, `link_count_limit`, `attribute_value_length_limit`, etc.) are enforced at well-defined layer boundaries. The full table and per-layer responsibility is in `memory-model.md`.

**Time.** All time-dependent code consumes `IClock` (wall) or `ISteadyClock` (monotonic). Tests inject fakes; production uses `std::chrono::system_clock` / `std::chrono::steady_clock`. No layer reads the clock directly.

**Fork.** After `fork()`, the child starts with exporter workers **disabled** until explicitly reinitialized. Parent I/O state is not safe to share across fork. The fork-survival sequence diagram is the normative reference.

**Configuration.** Resolved precedence is code > environment > `microtel.toml` > built-in defaults. The validated `Config` value is frozen at `Build()` time; no runtime mutation in v1. Per-setting precedence is documented in `configuration.md`.

---

## 6. Out of scope for v1

The architecture deliberately accommodates these without implementing them:

- **HTTP/3.** The `ITransport` seam is the drop-in point; nghttp3-based transport is v1.5+ if pursued.
- **Metrics, logs.** The encoder, exporter, transport, and codec layers are signal-agnostic in shape but only the trace path is wired in v1. Metrics design is `M11` per the roadmap; metrics implementation is `M12`.
- **Control plane / hot reload.** No long-running socket, no `microtelctl`, no JSON wire in v1. The frozen-`Config`-at-`Build` rule above is what makes v1 simple; v1.1 introduces a documented control-plane threat model.
- **Leaf / concentrator.** v2.0 adds a pure-C leaf library and a `Receiver` interface; v1 leaves space for the latter without implementing it.
- **Auto-instrumentation.** Manual only in v1.
- **Sugar layer.** `microtel::sugar` arrives in v1.1; v1's API is the direct OTel-style surface.

See `microtel-roadmap.md` for the full forward view.

---

## 7. Track ↔ component map

After M2 (Skeleton & contracts) lands, the following tracks have disjoint file ownership and may proceed in parallel.

| Track | Directory | Provides | Consumes (via mocks) |
|---|---|---|---|
| **A — Trace SDK** | `src/api/`, `src/sdk/`, `src/exporter/` | `ISpanProcessor`, `ISampler`, `IResourceDetector`, `IExporter`, the public API | `IOtlpEncoder`, `IWireCodec`, `IClock`, `IDiagnosticsSink` |
| **B — OTLP/HTTP wire** | `src/wire/http/` | `IWireCodec` (HTTP impl) | `IOtlpEncoder` (output type only), `ITransport` |
| **C — OTLP/gRPC wire** | `src/wire/grpc/` | `IWireCodec` (gRPC impl) | `IOtlpEncoder` (output type only), `ITransport` |
| **D — Transport** | `src/transport/`, `src/common/raii/` | `ITransport`, `IReactor`, RAII wrappers | OpenSSL, nghttp2 |
| **E — Config** | `src/common/config/` | validated `Config` value, `IAuthProvider` callback path | TOML parser, env vars |
| **F — Encoder** | `src/wire/encoder/` | `IOtlpEncoder`, `EncodedPayload` | upb (vendored), generated proto accessors |

D and F are foundational and finish first. Then A, B, C, E proceed concurrently. Per-track gtest suites run against mocks of every cross-track dependency. The full atlas — including tests/* ownership — is in `development.md`.

---

## 8. Glossary

- **Caller thread** — any application thread that calls `StartSpan` / `SetAttribute` / `AddEvent` / `End`. Never blocks on the export pipeline. Plural; many threads share this role.
- **Exporter worker thread** — single thread per process that drains the processor queue, builds batches, encodes, and hands payloads to the wire codec.
- **I/O thread** — single thread per process that owns the nghttp2 session and the socket, runs the epoll/kqueue loop.
- **Batch** — a bounded, time- or size-triggered group of completed spans handed to the encoder as a single unit.
- **Sampled span / unsampled span** — a span for which the `ISampler` returned `SAMPLED` / `NOT_SAMPLED`. Unsampled spans take the no-allocation fast path and are never enqueued.
- **`EncodedPayload`** — bytes plus size, owned by `std::unique_ptr<std::byte[]>` + `std::size_t`. Move-only. Produced by `IOtlpEncoder`, consumed by `IWireCodec`.
- **`WireResult`** — the unified value type returned by every `IWireCodec` implementation. Carries success / retryable classification, `retry_after`, partial-success rejected count, capped response body for diagnostics.
- **Partial success** — an OTLP response indicating some items were rejected. Recorded in diagnostics and **not** retried (retry would re-send accepted items too).
- **Retry budget** — the maximum elapsed time across all retries of a single batch. Exhaustion drops the batch with the `retry_budget_exhausted` reason.
- **Drop counter** — a per-reason counter incremented by the layer that drops a record / batch. Exposed via `Provider::GetExporterHealth()`.
- **RAII wrapper** — a move-only C++ type whose destructor releases an underlying C resource (socket, OpenSSL handle, nghttp2 session, upb arena). All in `src/common/raii/`.
- **Track A–F** — the parallel implementation tracks unblocked after M2 lands. See §7.
- **ICP** — Interface Change Proposal. A short markdown document recording a breaking change to a locked interface or to a durable architecture document. See `docs/icps/README.md`.
- **Sequence diagram** — a normative flow diagram in `docs/sequences/`. Eight in v1: connection establishment, retry-after-failure, GOAWAY handling, shutdown drain, fork survival, backpressure-and-drop, partial-success handling, gRPC trailer-only and multi-frame parsing.
