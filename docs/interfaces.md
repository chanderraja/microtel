# microtel Internal Interfaces

**Status:** M0 deliverable. **Sign-off required before M1 begins.** Per spec §13.2, every interface here is reviewed from the perspective of every downstream track that consumes it; the reviewer surfaces consumer/producer misalignment before code is written.
**Companion documents:** `architecture.md`, `threading-model.md`, `memory-model.md`, `error-model.md`. Detail in those documents is referenced rather than restated.
**Source of truth for rationale:** `microtel-spec.md` §13.2.
**Change control:** after M0 sign-off, breaking changes go through the ICP process (`docs/icps/README.md`).

---

## 1. Purpose and authority

This document is the locked contract for every internal interface in microtel v1. The corresponding headers (`include/microtel/internal/*.hpp`) are the operational form; if a header and this document disagree, **this document wins** and the header is the bug.

The audience is two groups:

- **The M0 reviewer**, who signs off on each interface from the perspective of every consumer track.
- **Implementation authors in M3+**, who write code against these contracts and against mocks at every dependency boundary.

After sign-off, the contracts are stable for the duration of v1. Breaking changes require an ICP.

### 1.1 ICPs that shape these interfaces

Interfaces locked here have several supporting ICPs already resolved. The
sign-off table in §7 reflects the result; this cross-reference exists so a
reader landing in §4 can trace each decision back to the rationale.

| ICP | Subject | Status |
|---|---|---|
| [0001](icps/0001-m0-deliverables-clarification.md) | M0 deliverables clarification: public headers in M0; eight sequence diagrams; `configuration.md` and `development.md` added. | Accepted |
| [0002](icps/0002-vendor-tl-expected.md) | Vendor `tl::expected` as `microtel::Expected` to keep the C++20 floor (RHEL 8 / devtoolset-11). Affects every `Expected`-returning signature here. | Accepted |
| [0003](icps/0003-m0-deferred-decisions.md) | `SslCtx` per-`Transport` (§4.1); `SpanHandle` shape for `Tracer::StartSpan` (cross-track A); MPSC queue stays an M3 implementation choice (default: bounded ring). | Accepted |

No M0 deferred decisions remain open. M1 may surface new ICPs from spike
findings; those land as numbered entries under `docs/icps/`.

## 2. How to read this document

Every interface is documented along **six axes** (per spec §13.2):

1. **Purpose** — one or two sentences.
2. **Contract** — preconditions, postconditions, invariants.
3. **Lifetime** — who creates, who destroys, when.
4. **Threading** — which methods may be called from which thread; reentrancy.
5. **Error model** — how failure is reported. References `error-model.md`.
6. **Allocation behavior** — none, per-call, per-batch, arena-backed.
7. **Mock availability** — location of the dumb mock under `tests/mocks/`; fake under `tests/fakes/` if logic is required.

Method shapes below are **structural pseudocode**, not the literal header text. Headers in `include/microtel/internal/*.hpp` are written in conforming C++20 and may differ in cosmetic ways (parameter names, attribute ordering, default arguments). Where the difference matters, this document is canonical.

## 3. Cross-cutting value types

Several interfaces refer to the same value types. Defined here once.

### 3.1 `EncodedPayload` — encoder output

Defined in detail in `memory-model.md` §3.2. Recap:

```
class EncodedPayload {
    std::unique_ptr<std::byte[]> bytes;   // owned
    std::size_t                  size;
public:
    // move-only; no copy; release() returns the unique_ptr
};
```

Move-only. No upb type appears in this header. (LOCKED)

### 3.2 `WireResult` — wire codec output

Returned from `IWireCodec::Send`. Carries everything the exporter needs to act, with no protocol-specific knowledge required upstream.

```
struct WireResult
{
    bool                        success                   = false;
    bool                        retryable                 = false;
    std::optional<Duration>     retry_after               = std::nullopt;
    std::uint32_t               partial_success_rejected  = 0;
    std::optional<Error>        error                     = std::nullopt;
    BoundedString               response_excerpt;           // capped at max_response_bytes; for diagnostics
};
```

Where `Duration` is `std::chrono::milliseconds`, and `BoundedString` is a small helper holding up to a configured byte cap (alias of `std::string` plus a length invariant in v1; can become a `pmr::string` in v1.5).

`success=true` and `partial_success_rejected>0` is the partial-success case (`error-model.md` §6). The exporter must not retry. (LOCKED)

The codec — not the exporter — owns retry classification (`error-model.md` §7). The exporter uses `retryable` and `retry_after` directly.

### 3.3 `BatchHandle` — exporter input

A move-only owning type holding a contiguous group of completed `SpanRecord`s plus the `Resource` and `InstrumentationScope` they share. Constructed by `BatchSpanProcessor` and consumed by `IExporter::Export`.

```
class BatchHandle
{
    std::span<const SpanRecord>      spans;       // borrowed view; backed by m_records below
    std::vector<SpanRecord>          m_records;   // actual ownership
    std::shared_ptr<const Resource>  resource;    // shared, immutable
    InstrumentationScope             scope;
public:
    // move-only
};
```

`SpanRecord` is the worker-thread shape of a completed span — owned attributes, events, links, status. Defined alongside `BatchHandle` in `include/microtel/internal/batch.hpp`.

### 3.4 `SamplingResult`

```
enum class SamplingDecision : std::uint8_t
{
    Drop                 = 0,
    RecordOnly           = 1,
    RecordAndSample      = 2,
};

struct SamplingResult
{
    SamplingDecision                            decision = SamplingDecision::Drop;
    std::vector<KeyValue>                       additional_attributes;   // appended to span if sampled
    std::optional<TraceState>                   trace_state;             // overrides parent state if set
};
```

Spec uses `RECORD`, `RECORD_AND_SAMPLE`, `DROP` per the OTel SDK spec; we use the locally-cased variants but the semantics match.

### 3.5 `HealthSnapshot`

The shape returned from `Provider::GetExporterHealth()`. Built by `IDiagnosticsSink::Snapshot()`.

```
struct HealthSnapshot
{
    std::array<std::uint64_t, kDropReasonCount>  drop_counters;     // indexed by DropReason
    std::uint64_t                                batches_sent       = 0;
    std::uint64_t                                batches_failed     = 0;
    std::uint64_t                                queue_depth_now    = 0;
    std::optional<TimePoint>                     last_error_time    = std::nullopt;
    BoundedString                                last_error_message;          // capped, redacted
    ConnectionState                              connection_state   = ConnectionState::Disconnected;
};
```

`DropReason` is the enum mirroring the table in `error-model.md` §3. Adding a new reason is an ICP.

### 3.6 Time and clock

`TimePoint` aliases `std::chrono::system_clock::time_point` for wall-clock-bearing fields, and `std::chrono::steady_clock::time_point` for monotonic. `Duration` aliases `std::chrono::milliseconds` unless otherwise stated.

---

## 4. The interfaces

Twelve interfaces. Tagged by track per spec §13.1.

| # | Interface | Header | Track |
|---|---|---|---|
| 1 | `ITransport` | `transport.hpp` | D |
| 2 | `IOtlpEncoder` | `otlp_encoder.hpp` | F |
| 3 | `IWireCodec` | `wire_codec.hpp` | B / C |
| 4 | `IExporter` | `exporter.hpp` | A → B / C |
| 5 | `ISampler` | `sampler.hpp` | A |
| 6 | `ISpanProcessor` | `processor.hpp` | A |
| 7 | `IClock` / `ISteadyClock` | `clock.hpp` | common |
| 8 | `IReactor` | `reactor.hpp` | D |
| 9 | `IAuthProvider` | `auth_provider.hpp` | E |
| 10 | `IResourceDetector` | `resource_detector.hpp` | A |
| 11 | `IDiagnosticsSink` | `diagnostics_sink.hpp` | common |
| 12 | `ILogSink` (public) | `include/microtel/log_sink.hpp` | common |

---

### 4.1 `ITransport`

#### Purpose

A connection to a single OTLP endpoint over HTTP/2. Sends opaque request bytes and surfaces opaque response bytes; knows nothing about OTLP semantics. Implementations: nghttp2-over-OpenSSL in v1; the seam is shaped to allow nghttp3 in v1.5+ without changing this header.

#### Contract

```
class ITransport {
public:
    virtual ~ITransport() noexcept = default;

    [[nodiscard]] virtual microtel::Expected<void, Error>
        Connect(const ConnectOptions& opts) = 0;

    /// Submit a request. The transport writes the bytes and signals completion
    /// via the per-request future returned. The bytes pointed to by `payload`
    /// MUST remain valid until the future completes or until Cancel() is called
    /// for the same request handle.
    [[nodiscard]] virtual RequestHandle
        Send(const RequestSpec& spec) noexcept = 0;

    virtual void Cancel(RequestHandle handle) noexcept = 0;

    [[nodiscard]] virtual ConnectionState GetState() const noexcept = 0;

    /// Initiate orderly shutdown. Sends GOAWAY, drains in-flight up to timeout,
    /// closes the socket. Idempotent.
    [[nodiscard]] virtual Status Close(Duration timeout) noexcept = 0;
};
```

Where `ConnectOptions` carries endpoint, TLS material, ALPN preference, timeout taxonomy (six timeouts per spec §7.3); `RequestSpec` carries HTTP/2 headers, the borrowed payload span, and a per-request deadline; `RequestHandle` is a small move-only token paired with a future-like completion observable by the codec.

**Preconditions.** `Connect` must be called and return success before `Send` is called. After `Close` returns `Completed` or `TimedOut`, no further `Send` is permitted.

**Postconditions.** `Send` returns immediately with a handle; the underlying request runs asynchronously on the I/O thread. The bytes referenced in `RequestSpec.payload` are read by the I/O thread; the **caller (the wire codec) retains ownership** of those bytes and must not free them until the completion fires.

**Invariants.** One transport instance manages exactly one socket and one nghttp2 session. Reconnect is internal: there is no client-initiated reconnect call, and no reconnect policy to configure. Clients observe it only indirectly — through `ConnectionState::Reconnecting` in `GetExporterHealth()`, and through the retryable export failure that triggers it.

This sentence previously read "Reconnect is internal — clients do not see it", which was both imprecise and, until ICP 0018, untrue in the other direction: no reconnect existed at all. The vague phrasing is part of why that went unnoticed — "clients do not see it" reads as a property nobody can test.

#### Lifetime

Created by the `Provider` at `Build()` time. Owned by the `Provider`. Destroyed when the `Provider` is destroyed. `Close` is invoked by the `Provider` during `Shutdown`.

#### Threading

- `Close` is caller-thread-safe; called once from the `Provider` lifecycle path.
- `Connect` is caller-thread-safe. It is called once from the `Provider`
  lifecycle path when the application calls `Provider::Connect()` explicitly;
  otherwise `IWireCodec::Send` calls it from the exporter worker thread on
  first use (ICP 0017). Either way it may be attempted more than once if
  multiple codecs share one transport — `Http2Transport::Connect`'s
  `Disconnected → Connecting` compare-and-swap guard ensures exactly one
  caller performs the handshake; the rest receive an "already connecting"
  error, surfaced like any other retryable transport failure.
- `Send` is **safe for concurrent callers** (LOCKED, relaxed by ICP 0009). Any number of threads may call it; the transport serialises submissions onto its single I/O thread via `m_pending_queue` under `m_pending_mu`, with atomically-allocated handle ids. Each caller still owns its own `IWireCodec` — codecs and `IOtlpEncoder` remain single-caller.

  This line previously read *"single-threaded — only the exporter worker calls it (LOCKED). Concurrent `Send` calls are a contract violation."* That was false from M12 onward: `SdkBuilder::Build` constructs three codecs over one transport (traces, metrics, logs), each driven by its own exporter worker, so `Send` has had three concurrent callers in every build since. ICP 0009 proposed this relaxation for exactly that reason and was never applied, leaving the contract asserting the opposite of what shipped. See `docs/icps/README.md`.
- `Cancel` is callable from any thread but in practice from the exporter worker and from a shutdown-driving caller thread.
- `GetState` is thread-safe.

The internal I/O thread is owned by the transport and is not visible through this interface.

#### Error model

- `Connect` returns `microtel::Expected<void, Error>`. `Error::Kind` is `Network` or `InternalFailure`.
- `Send` and `Cancel` are `noexcept`; failure is delivered through the request handle's future. The future yields a `TransportResult` with `Error` populated on failure.
- `Close` returns the lifecycle `Status` (`error-model.md` §2.3).

The transport increments `connect_failure` directly. All other counters are incremented by the wire codec or the exporter based on the result the transport surfaces.

#### Allocation behavior

- `Connect`: allocates `SslCtx`, `SslSession`, `Nghttp2Session`, and per-stream state lazily. `SslCtx` is owned by this `Transport` instance — not process-shared (see [ICP 0003 §3.1](icps/0003-m0-deferred-decisions.md#31-sslctx-ownership--per-transport)). Bounded.
- `Send`: allocates one in-flight request record. No payload copy.
- I/O loop: nghttp2 owns its buffers; per-frame allocations are nghttp2's responsibility.

#### Mock and fake

- **Mock** at `tests/mocks/mock_transport.hpp`. Returns the configured `TransportResult`; no logic.
- **Fake** at `tests/fakes/fake_transport.hpp`. In-memory loopback: sends a request record into a `FakeServer` implementing simple OTLP/HTTP and OTLP/gRPC handlers configurable by tests. Used by exporter and codec integration tests that don't need a real socket.

#### Consumers

`IWireCodec` (HTTP and gRPC implementations). The exporter does not depend on `ITransport` directly — it goes through the codec.

---

### 4.2 `IOtlpEncoder`

#### Purpose

Encodes a `BatchHandle` into OTLP protobuf bytes. The only place in the codebase that includes upb headers (LOCKED — `memory-model.md` §3.1).

#### Contract

```
class IOtlpEncoder {
public:
    virtual ~IOtlpEncoder() noexcept = default;

    [[nodiscard]] virtual EncodedPayload
        Encode(const BatchHandle& batch) = 0;
};
```

**Preconditions.** `batch` is non-empty and well-formed; the `Resource` and `InstrumentationScope` are present.

**Postconditions.** The returned `EncodedPayload` carries the protobuf-encoded bytes for an `ExportTraceServiceRequest`. The internal upb arena is destroyed before the call returns. (LOCKED)

**Invariants.** Stateless across calls — `Encode` is a pure function of its input. Multiple encoders may exist; in v1 there is one shared encoder per `Provider`.

#### Lifetime

Created by the `Provider` at `Build()` time. Owned by the `Provider`. Stateless, so destruction is trivial.

#### Threading

- `Encode` is called only by the exporter worker (LOCKED).
- The interface is **not** thread-safe — concurrent `Encode` calls would race on internal scratch state if any exists. Restricting calls to a single thread keeps the implementation simple.

#### Error model

- `Encode` is **not** `noexcept`. It may throw `std::bad_alloc` if heap exhaustion occurs during buffer allocation. The exporter worker catches at its top-level loop and records `non_retryable_failure` (the batch is dropped; not retried because the bug is local).
- Other failures are not contemplated — input validation occurs upstream in the SDK.

#### Allocation behavior

- One `UpbArena` per call (LOCKED — `memory-model.md` §3.1).
- One `std::byte[]` allocation per call for the output buffer.
- Inside the arena: bump-pointer; freed `O(1)` at arena destruction.

#### Mock and fake

- **Mock** at `tests/mocks/mock_otlp_encoder.hpp`. Returns a configured `EncodedPayload` (typically a small canned byte sequence). Used by exporter and codec tests.
- **Fake** at `tests/fakes/fake_otlp_encoder.hpp`. Implements the encoder using a hand-rolled protobuf writer (no upb dependency in tests). Used by integration tests that want byte-level fidelity without bringing in the full upb generated code in test fixtures.

#### Consumers

`IExporter` (only). The wire codecs receive `EncodedPayload` from the exporter; they do not call the encoder.

---

### 4.3 `IWireCodec`

#### Purpose

Serializes / deserializes the OTLP-over-HTTP/2 wire shape — framing, headers, status interpretation, retry classification — for one of the two wire protocols. Two implementations: HTTP-protobuf (`src/wire/http/`) and gRPC-on-nghttp2 (`src/wire/grpc/`). One interface (LOCKED — ICP 0001 / spec ICP discussion).

#### Contract

```
class IWireCodec {
public:
    virtual ~IWireCodec() noexcept = default;

    [[nodiscard]] virtual WireResult
        Send(EncodedPayload&& payload, Duration deadline) = 0;
};
```

**Preconditions.** None. If the underlying `ITransport` is not connected,
`Send` connects it first (ICP 0017). A failed connect attempt is reported as
an ordinary `retryable` `WireResult`, not a distinct error shape.

**Postconditions.** Returns a fully-classified `WireResult`. The codec has parsed any response, populated `partial_success_rejected` if applicable, and applied the protocol's retry rules. The exporter must respect `retryable` and `retry_after` without reinterpretation.

**Invariants.** A single codec instance is bound to one `ITransport` instance and one wire protocol. The bound protocol is a construction-time property; not switchable.

#### Lifetime

Created by the `Provider` at `Build()` time, paired 1:1 with the transport for that endpoint. Owned by the `Provider`.

#### Threading

- `Send` is called only by the exporter worker (LOCKED).
- The codec is **not** thread-safe. Concurrent `Send` calls are a contract violation.
- Internally the codec waits on a per-request completion future signalled by the I/O thread (`threading-model.md` §3.3).

#### Error model

- `Send` is **not** `noexcept` (it allocates and may throw `std::bad_alloc`); failures other than allocation are reported via `WireResult.success=false` and `WireResult.error`.
- The codec increments these counters: `partial_success_rejection`, `non_retryable_failure`, `response_too_large`, `decompression_too_large`, `malformed_response`, `transport_busy`. The exporter increments `retryable_failure_recovered` and `retry_budget_exhausted` based on the codec's classification.
- Retry classification follows the matrix in `error-model.md` §7. (LOCKED)

#### Allocation behavior

- One in-flight request record per call.
- Response buffer pre-sized to a small initial capacity, grown to at most `max_response_bytes`.
- Compression buffers (when `gzip` is used) bounded by `max_decompressed_bytes`.

#### Mock and fake

- **Mock** at `tests/mocks/mock_wire_codec.hpp`. Returns a configured `WireResult`. The mock is the most-used cross-track seam — every exporter test uses it.
- **Fake** at `tests/fakes/fake_wire_codec.hpp`. Drives a configurable scripted response sequence so the exporter can be tested against retry / partial-success / non-retryable flows without bringing in a real transport or server.

The actual protocol-specific tests live in `tests/wire/` and `tests/grpc-wire/` and exercise real codec implementations against a fake transport.

#### Consumers

`IExporter`.

---

### 4.4 `IExporter`

#### Purpose

The protocol-agnostic export pipeline: drain batches from the processor, encode, send, classify, retry, account for drops. Owns the exporter worker thread.

#### Contract

```
class IExporter {
public:
    virtual ~IExporter() noexcept = default;

    [[nodiscard]] virtual ExportResult
        Export(BatchHandle&& batch) noexcept = 0;

    [[nodiscard]] virtual Status
        ForceFlush(Duration timeout) noexcept = 0;

    [[nodiscard]] virtual Status
        Shutdown(Duration timeout) noexcept = 0;
};

enum class ExportResult : std::uint8_t
{
    Success         = 0,
    Failure         = 1,
    Dropped         = 2,
    AlreadyShutDown = 3,
};
```

**Preconditions.** `BatchSpanProcessor` calls `Export` with batches whose size and shape conform to the configured limits.

**Postconditions.** `Export` is non-blocking — it hands the batch to the exporter worker via the worker's task queue and returns immediately. Successful return does **not** mean the batch was sent — only that it was accepted into the pipeline.

**Invariants.** One exporter per `Provider`. The exporter owns its worker thread and its associated synchronisation.

#### Lifetime

Created by the `Provider` at `Build()`. Owned by the `Provider`. `Shutdown` joins the worker thread; the destructor calls `Shutdown(small_finite_timeout)` if not already shut down.

#### Threading

- `Export` is callable from any thread but in practice only the `BatchSpanProcessor` calls it.
- `ForceFlush` and `Shutdown` are caller-thread-safe; idempotent. Multiple concurrent `Shutdown` calls are serialised under `m_shutdown` (`threading-model.md` §4).
- The worker thread is internal.

#### Error model

- `Export`: `noexcept`. Failures are recorded on `IDiagnosticsSink` and the result is returned as `ExportResult`. The processor uses the result for its own accounting; the application sees nothing.
- `ForceFlush` / `Shutdown`: `noexcept`, return the lifecycle `Status`.

The exporter increments: `retryable_failure_recovered`, `retry_budget_exhausted`, `force_flush_timeout`, `shutdown_timeout`, and forwards the codec's drop reasons.

#### Allocation behavior

- Worker thread is allocated at construction, joined at `Shutdown`.
- Per-batch retry state is stack-allocated on the worker.
- No allocation on the `Export` enqueue path beyond the task-queue node.

#### Mock and fake

- **Mock** at `tests/mocks/mock_exporter.hpp`. Records calls to `Export`; configurable to return any `ExportResult`. Used by `BatchSpanProcessor` tests.
- **Fake** at `tests/fakes/fake_exporter.hpp`. Records the batches in memory and exposes them for inspection. Used by SDK integration tests verifying that a span flow ends in an export call without engaging the wire stack.

#### Consumers

`ISpanProcessor` (specifically `BatchSpanProcessor`). The `Provider` calls `ForceFlush` / `Shutdown`.

---

### 4.5 `ISampler`

#### Purpose

Decides whether a span is sampled and exposes additional attributes / `TraceState` to apply if so. Realised by `AlwaysOn`, `AlwaysOff`, `TraceIdRatio`, and `ParentBased` in v1.

#### Contract

```
class ISampler {
public:
    virtual ~ISampler() noexcept = default;

    [[nodiscard]] virtual SamplingResult
        ShouldSample(const SamplingContext& ctx) const noexcept = 0;

    [[nodiscard]] virtual std::string_view Description() const noexcept = 0;
};
```

`SamplingContext` carries: parent context (TraceId, SpanId, sampled flag, TraceState), span kind, span name, initial attributes, links.

**Preconditions.** `ctx` is well-formed.

**Postconditions.** Returns a `SamplingResult` with a definite decision. The sampler does not mutate any state observable to other callers.

**Invariants.** `ShouldSample` is a pure function of its input plus optional sampler-internal state (e.g., `TraceIdRatio`'s threshold). Concurrent calls are safe.

#### Lifetime

Created by `SdkBuilder::Build()`. Owned by the `Provider`. Replaceable in v1.1's hot-reload path; in v1, immutable after `Build`.

#### Threading

- `ShouldSample` is called from the **caller thread**, on the hot path. Must be thread-safe and `noexcept`. (LOCKED)
- `Description` is thread-safe.

#### Error model

- `ShouldSample` is `noexcept`. It must always return a definite decision; "fall back to drop" is the recovery for any internal anomaly.

#### Allocation behavior

- `ShouldSample` **must not allocate on the hot path** (LOCKED).  Returning `SamplingResult` via NRVO with empty `additional_attributes` and absent `trace_state` is the unsampled / default-sampled fast path.
- For samplers that produce additional attributes (rare in v1), allocation is permitted but should be bounded.

#### Mock and fake

- **Mock** at `tests/mocks/mock_sampler.hpp`. Returns a configured `SamplingResult`.
- The four built-in samplers each have unit tests; no general fake required.

#### Consumers

`Tracer::StartSpan` (in the API layer). Replaceable from `SdkBuilder`.

---

### 4.6 `ISpanProcessor`

#### Purpose

Consumes completed spans and routes them onward. v1 ships `BatchSpanProcessor` (queue + worker + batch) and `SimpleSpanProcessor` (synchronous, for tests).

#### Contract

```
class ISpanProcessor {
public:
    virtual ~ISpanProcessor() noexcept = default;

    virtual void OnStart(Span& span, const Context& parent) noexcept = 0;

    virtual void OnEnd(SpanRecord&& record) noexcept = 0;

    [[nodiscard]] virtual Status ForceFlush(Duration timeout) noexcept = 0;

    [[nodiscard]] virtual Status Shutdown(Duration timeout) noexcept = 0;
};
```

**Preconditions.** `OnEnd` is called exactly once per `Span`, on the caller thread that ended the span.

**Postconditions.** `OnEnd` either accepts the record into its pipeline or drops with the appropriate reason (`queue_full`, `record_too_large`, `post_shutdown`).

**Invariants.** `OnStart` and `OnEnd` are pair-balanced per span.

#### Lifetime

Created by `SdkBuilder::Build()`. Owned by the `Provider`. `Shutdown` is invoked during `Provider::Shutdown`.

#### Threading

- `OnStart`, `OnEnd` callable from any caller thread; thread-safe; `noexcept`. (LOCKED)
- `ForceFlush`, `Shutdown` callable from any caller thread; idempotent; `noexcept`.
- `BatchSpanProcessor` owns the exporter worker thread (it embeds `IExporter`).

#### Error model

- All four methods are `noexcept`. Drops are reported through `IDiagnosticsSink`.

#### Allocation behavior

- `OnStart`: typically zero work in v1 (no in-process span enrichment hooks until v1.4). Implementations may keep it as a no-op.
- `OnEnd`: queue push only (`memory-model.md` §8.2).

#### Mock and fake

- **Mock** at `tests/mocks/mock_span_processor.hpp`. Records calls; no logic.
- **Fake** at `tests/fakes/fake_span_processor.hpp`. Stores spans in a vector for inspection.

#### Consumers

The SDK's `Tracer` calls `OnStart` and `OnEnd`. The `Provider` calls `ForceFlush` / `Shutdown`.

---

### 4.7 `IClock` and `ISteadyClock`

#### Purpose

Test seam for time. Production injects `std::chrono::system_clock` and `std::chrono::steady_clock`; tests inject fakes that advance on demand.

#### Contract

```
class IClock {
public:
    virtual ~IClock() noexcept = default;
    [[nodiscard]] virtual TimePointWall Now() const noexcept = 0;
};

class ISteadyClock {
public:
    virtual ~ISteadyClock() noexcept = default;
    [[nodiscard]] virtual TimePointSteady Now() const noexcept = 0;
};
```

**Postconditions.** Each `Now()` returns a value monotonically non-decreasing within the same instance (`ISteadyClock`) or convertible to wall-clock (`IClock`).

#### Lifetime

Created by `SdkBuilder::Build()`; owned by the `Provider`. Components borrow non-owning references via `T*` parameters; lifetimes are documented at use sites.

#### Threading

- `Now()` is thread-safe. (LOCKED)
- May be called from any thread.

#### Error model

- `noexcept`. Time always succeeds.

#### Allocation behavior

- None. Both methods return value types.

#### Mock and fake

- **Fake** at `tests/fakes/fake_clock.hpp` and `fake_steady_clock.hpp`. Manual advancement: tests call `Advance(Duration)` to move the clock forward. Used pervasively in retry, batch-deadline, and shutdown-timeout tests.
- No mock — the fake serves both purposes.

#### Consumers

`BatchSpanProcessor` (batch deadlines), `IExporter` (retry timing, timeouts), `IDiagnosticsSink` (last-error timestamp).

---

### 4.8 `IReactor`

#### Purpose

Test seam behind `ITransport`'s I/O loop. Wraps epoll on Linux and kqueue on BSD/macOS. Tests inject a fake reactor that delivers events on test-driven schedules — useful for verifying GOAWAY mid-batch, RST_STREAM mid-stream, and partial-frame edge cases without an actual socket.

#### Contract

```
class IReactor {
public:
    virtual ~IReactor() noexcept = default;

    [[nodiscard]] virtual microtel::Expected<void, Error>
        Register(int fd, EventMask mask, EventCallback cb) = 0;

    virtual void Modify(int fd, EventMask mask) = 0;
    virtual void Unregister(int fd) noexcept = 0;

    /// Block up to `deadline` waiting for events, then dispatch each via its
    /// registered callback. Returns the number of events dispatched.
    virtual std::size_t WaitAndDispatch(TimePointSteady deadline) = 0;

    /// Wake the loop if it is in WaitAndDispatch on another thread.
    virtual void Wake() noexcept = 0;
};
```

**Preconditions.** `Register` precedes any `Modify` / `Unregister` for the same `fd`.

**Postconditions.** `WaitAndDispatch` invokes callbacks before returning. Callbacks run on the I/O thread.

**Invariants.** One reactor instance per transport; not shared across transports.

#### Lifetime

Created by `ITransport` at `Connect`. Owned by the transport. Destroyed at `Close`.

#### Threading

- All methods other than `Wake` are I/O-thread-only. (LOCKED)
- `Wake` is the explicit cross-thread wakeup primitive; thread-safe; `noexcept`.

#### Error model

- `Register` returns `microtel::Expected<void, Error>` for syscall failures.
- `Modify` and `Unregister` are not expected to fail except on programming error; if they do, an internal-failure diagnostic is recorded and the loop continues. (`Unregister` is `noexcept` because the transport must always be able to clean up.)

#### Allocation behavior

- `Register` allocates one entry per fd. Bounded by the transport's outstanding fd count (in v1: one socket plus two eventfds = three).

#### Mock and fake

- **Fake** at `tests/fakes/fake_reactor.hpp`. Tests script event timelines; the fake dispatches them to registered callbacks on demand.
- No mock — the fake is the primary test seam.

#### Consumers

`ITransport` only.

---

### 4.9 `IAuthProvider`

#### Purpose

Per spec §12.5, supplies the `Authorization` header value for each export batch. v1 supports two implementations: `StaticHeadersAuthProvider` (returns a constant) and `CallbackAuthProvider` (calls user code with a TTL cache).

#### Contract

```
class IAuthProvider {
public:
    virtual ~IAuthProvider() noexcept = default;

    /// Returns the value of the `Authorization` header to send on the next
    /// batch. Empty optional means: no header.
    [[nodiscard]] virtual microtel::Expected<std::optional<std::string>, Error>
        GetAuthorization(TimePointSteady now) = 0;
};
```

**Preconditions.** Called by the wire codec immediately before constructing the request HTTP/2 headers.

**Postconditions.** Returns the current header value or `std::nullopt`. Cache management (TTL) is internal.

**Invariants.** Implementations may cache; the cache is invalidated on TTL or on demand via an internal method not in this interface.

#### Lifetime

Created by `SdkBuilder::Build()`. Owned by the `Provider`. The `CallbackAuthProvider` wraps a user-supplied `std::function`; the user's function is owned by the provider and outlives every batch invocation.

#### Threading

- `GetAuthorization` callable from the exporter worker. Must be thread-safe (the cache update path) but in practice only one caller. **The user-supplied callback may be invoked on the exporter worker thread**; this is documented in `auth_provider.hpp` and the public `WithAuthProvider` API. (LOCKED — affects user code.)

#### Error model

- Returns `microtel::Expected<std::optional<std::string>, Error>`. On error the wire codec records `non_retryable_failure` (the batch is dropped — sending without auth is worse than not sending) and proceeds to the next batch.
- The user-supplied callback may throw; the provider catches at the boundary and converts to `Error::Kind::InternalFailure`. (No exceptions cross thread boundaries — `error-model.md` §5.)

#### Allocation behavior

- `StaticHeadersAuthProvider`: zero allocation per call.
- `CallbackAuthProvider`: one cache lookup, optional one user-callback invocation, one string copy on cache miss.

#### Mock and fake

- **Fake** at `tests/fakes/fake_auth_provider.hpp`. Returns a configured value or sequence; supports simulating cache TTL expiration.

#### Consumers

`IWireCodec` (both HTTP and gRPC implementations).

---

### 4.10 `IResourceDetector`

#### Purpose

Produces a partial `Resource` at SDK initialisation. v1 ships a minimal env-var detector and an explicit-config "detector"; full detectors (process, host, k8s, cloud) arrive in v1.1+. The interface is locked in M0 so v1.1 detectors do not break the contract.

#### Contract

```
class IResourceDetector {
public:
    virtual ~IResourceDetector() noexcept = default;

    [[nodiscard]] virtual microtel::Expected<Resource, ConfigError>
        Detect() = 0;

    [[nodiscard]] virtual std::string_view Name() const noexcept = 0;
};
```

**Preconditions.** Called at most once per detector instance, during `SdkBuilder::Build()`.

**Postconditions.** Returns a partial `Resource`. Conflicts between detectors are resolved by the precedence in spec §12.7.

**Invariants.** Detection is one-shot; no live updates in v1.

#### Lifetime

Created by `SdkBuilder` (or by user code passed to it). Owned by the builder until `Build` consumes them; the produced `Resource` is moved into the `Provider`.

#### Threading

- Not thread-safe; called once on the caller thread that invokes `Build`.

#### Error model

- `microtel::Expected<Resource, ConfigError>`. A failed detector yields a `ConfigError` if strict mode is configured; otherwise the detector's contribution is empty and a diagnostic is logged.

#### Allocation behavior

- Per-detector; small.

#### Mock and fake

- **Fake** at `tests/fakes/fake_resource_detector.hpp`. Returns a configured `Resource`. Used in SDK init tests.

#### Consumers

`SdkBuilder::Build`.

---

### 4.11 `IDiagnosticsSink`

#### Purpose

The single sink for drop counters, batch counters, last-error data, and connection state. Backing store for `Provider::GetExporterHealth()`.

#### Contract

```
class IDiagnosticsSink {
public:
    virtual ~IDiagnosticsSink() noexcept = default;

    virtual void RecordDrop(DropReason reason, std::uint64_t n = 1) noexcept = 0;
    virtual void RecordBatchSent() noexcept = 0;
    virtual void RecordBatchFailed(const Error& err) noexcept = 0;
    virtual void SetQueueDepth(std::uint64_t depth) noexcept = 0;
    virtual void SetConnectionState(ConnectionState state) noexcept = 0;
    [[nodiscard]] virtual HealthSnapshot Snapshot() const noexcept = 0;
};
```

**Preconditions.** None — every method is callable at any point in the lifecycle.

**Postconditions.** Counters are monotonically non-decreasing. `Snapshot` returns a consistent-at-a-moment view.

**Invariants.** All counter increments are atomic (`std::atomic<uint64_t>`).

#### Lifetime

Created by `SdkBuilder::Build()`. Owned by the `Provider`. Components hold non-owning `IDiagnosticsSink*`.

#### Threading

- All methods are thread-safe (LOCKED). The sink is the leaf-lock of the system (`threading-model.md` §4).
- Called from caller threads, exporter worker, and I/O thread.

#### Error model

- `noexcept`. Recording a diagnostic must never fail in a way that fails the caller's operation.

#### Allocation behavior

- Counter increments are atomic — zero allocation.
- `RecordBatchFailed` may copy a short error message into a small ring buffer; bounded.
- `Snapshot` allocates one `HealthSnapshot` value.

#### Mock and fake

- **Fake** at `tests/fakes/fake_diagnostics_sink.hpp`. Exposes counters as plain values for assertions in tests.

#### Consumers

Every component records here. `Provider::GetExporterHealth` is the only reader in v1.

---

### 4.12 `ILogSink` (public)

#### Purpose

Caller-injectable hook for redirecting microtel's internal diagnostic logs into the application's logger. Public — defined in `include/microtel/log_sink.hpp`, not the `internal/` tree.

#### Contract

```
namespace microtel
{
    enum class LogLevel : std::uint8_t
    {
        Trace = 0, Debug = 1, Info = 2, Warn = 3, Error = 4,
    };

    using LogSink = std::function<void(LogLevel lvl, std::string_view msg)>;

    void SetLogSink(LogSink sink) noexcept;
    void ResetLogSink() noexcept;        // restore default (spdlog or stderr)
}
```

**Preconditions.** `SetLogSink` may be called at any point; it replaces the current sink atomically.

**Postconditions.** Subsequent log emissions are routed through the new sink.

**Invariants.** Exactly one sink is active per process at any time. Multi-threaded emission is serialised by the routing layer; the sink itself is invoked under no lock and must be self-synchronising if it touches shared state.

#### Lifetime

The application owns the lifetime of any captured state in the lambda. The sink may be invoked from any internal thread (exporter worker, I/O thread). The application must ensure the sink remains valid until `ResetLogSink` is called or the process exits.

#### Threading

- `SetLogSink`, `ResetLogSink` thread-safe.
- The sink itself may be called from **any internal thread**; the application is responsible for the sink's thread-safety.

#### Error model

- `SetLogSink` and `ResetLogSink` are `noexcept`.
- A sink that throws is caught and the exception is dropped silently (with a one-shot internal diagnostic emitted to the *previous* sink if available). Never recursive.

#### Allocation behavior

- `SetLogSink` may allocate during `std::function` construction. Outside of registration, zero allocation.

#### Mock and fake

- Tests use a captured-vector lambda directly — no separate mock or fake.

#### Consumers

The internal logging layer (`src/common/logging/`) and any application that wishes to capture microtel's diagnostics.

---

## 5. Mock and fake conventions

Restating the rule from `CLAUDE.md` rule §4 and spec §14.2:

- **Mocks are dumb.** They return what they're configured to return. No logic. Live in `tests/mocks/`.
- **Fakes have logic.** When a test requires non-trivial behaviour (a fake server, a fake clock that advances, a fake reactor that scripts events), it's a fake, not a "smart mock". Live in `tests/fakes/`.

Every interface in this document has a mock or a fake (sometimes both). The selection per interface is recorded in the §4 entries above. Mocks and fakes are written in M2 alongside the project skeleton.

## 6. What this document does not cover

- Public-API headers (`include/microtel/`) — see the headers themselves; the interfaces here are internal.
- Implementation choices left to M3 (queue data structure, exact backoff formula) — see the relevant model document.
- Auth providers beyond static + callback (OAuth2, SigV4) — v1.1+; will gain new sub-interfaces under `IAuthProvider` if needed.
- The `IReceiver` interface from spec §17.4 (v2.0 leaf/concentrator) — out of v1 scope. The seam exists architecturally but is not realised in v1.

## 7. Sign-off log

Each row is locked once a reviewer signs and dates it. Per spec §13.2,
breaking changes after a row is locked require an ICP. The "Locked"
column records the M0 sign-off date.

| Interface | Reviewer | Locked | Status | Cross-reference |
|---|---|---|---|---|
| `ITransport` | Chander Raja | 2026-05-04 | Accepted | `SslCtx` per-`Transport` per [ICP 0003 §3.1](icps/0003-m0-deferred-decisions.md#31-sslctx-ownership--per-transport). |
| `IOtlpEncoder` | Chander Raja | 2026-05-04 | Accepted | Per-encode arena (LOCKED — `memory-model.md` §3.1). |
| `IWireCodec` | Chander Raja | 2026-05-04 | Accepted | One interface, two impls per [ICP 0001](icps/0001-m0-deliverables-clarification.md). |
| `IExporter` | Chander Raja | 2026-05-04 | Accepted | — |
| `ISampler` | Chander Raja | 2026-05-04 | Accepted | Hot-path `noexcept`, no allocation on default path. |
| `ISpanProcessor` | Chander Raja | 2026-05-04 | Accepted | — |
| `IClock` / `ISteadyClock` | Chander Raja | 2026-05-04 | Accepted | — |
| `IReactor` | Chander Raja | 2026-05-04 | Accepted | — |
| `IAuthProvider` | Chander Raja | 2026-05-04 | Accepted | User callback may run on exporter worker (LOCKED). |
| `IResourceDetector` | Chander Raja | 2026-05-04 | Accepted | Locked in M0 even though full detectors arrive in v1.1+. |
| `IDiagnosticsSink` | Chander Raja | 2026-05-04 | Accepted | Atomic counters; leaf-lock. |
| `ILogSink` | Chander Raja | 2026-05-04 | Accepted | Public; `include/microtel/log_sink.hpp`. |

All twelve rows locked; no pending interfaces. M1 (the spike) is unblocked.
