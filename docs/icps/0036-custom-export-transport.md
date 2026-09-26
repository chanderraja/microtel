# ICP 0036: an application-supplied export transport for full-SDK nodes

**Status:** Draft.
**Affected interfaces / docs:**
- `include/microtel/export_transport.hpp` (new public header)
- `include/microtel/sdk_builder.hpp` (new `WithExportTransport`)
- `include/microtel/leaf_receiver.hpp` (`LeafTimeMode` gains `Unix = 3`)
- `src/sdk/sdk_builder.cpp` (`BuildExporters`, `BuildWireCodec`, the
  `CreateTransport` call), a new internal codec under `src/wire/custom/`,
  `src/sdk/leaf_resource.cpp`, `src/sdk/leaf_receiver.cpp`,
  `src/common/config/` (the `"unix"` time-mode value)
- `docs/interfaces.md` §4.3 (a third `IWireCodec` implementation)
- `docs/threading-model.md` §2 and §10 (no I/O thread; user code on the
  exporter workers)
- `docs/error-model.md` §7 (a new §7.3 matrix)
- `docs/leaf-concentrator-design.md` §3.4, §4.2, §5 (undeclared payloads and
  the `unix` mode)
- `docs/architecture.md` §3.5, §3.6; `docs/configuration.md`

**Affected tracks:** SDK, exporter / wire, concentrator. No encoder change, no
change to `DropReason`.

## Summary

Let a full C++ microtel `Provider` hand its encoded OTLP requests to an
`ExportTransport` object supplied by the application instead of to microtel's
HTTP/2 transport, and let the concentrator accept those requests from a node
it is configured to trust as already holding Unix time.

## Motivation

A leaf deployment usually has more than one class of device on the link. A
Cortex-M sensor runs the C leaf. The Linux board next to it, on the same
UART, CAN bus, BLE link or local UDP segment, can run the full C++ runtime
with sampling, batching, retries, propagation and the sugar layer, but today
it can only export over HTTP/2. If it has no route to the collector, or the
link is the only one it has, it cannot report at all. The C leaf can't fill
the gap: it has no sampler, no batching and no context propagation, and a
C++ service would lose the whole Tracer API by switching to it.

The pieces are already there. The exporter produces a complete
`ExportTraceServiceRequest` per request, with retries and accounting around
it. The concentrator decodes exactly that message. What is missing is a seam
between the encoder and the wire, and a way for the concentrator to accept a
payload that carries no leaf wire header. This is additive to v1.2, where the
concentrator ships as experimental.

## Proposed change

### Decision 1: public API — an abstract class, owned by the Provider

```cpp
// include/microtel/export_transport.hpp (new)
namespace microtel
{

enum class ExportSignal : std::uint8_t
{
    Traces = 0,   ///< an ExportTraceServiceRequest
    Metrics = 1,  ///< an ExportMetricsServiceRequest
    Logs = 2,     ///< an ExportLogsServiceRequest
};

/// @brief One encoded OTLP request, handed to ExportTransport::Send.
struct ExportRequest
{
    ExportSignal signal = ExportSignal::Traces;
    /// Uncompressed protobuf bytes of the signal's Export*ServiceRequest.
    /// Borrowed for the duration of Send only; copy them to keep them.
    std::span<const std::byte> bytes;
    /// Send must return by this time (Decision 2).
    std::chrono::steady_clock::time_point deadline;
    /// 0 for the first attempt, n for the nth retry of the same request.
    std::uint32_t attempt = 0;
};

enum class SendOutcome : std::uint8_t
{
    Success = 0,       ///< delivered, or handed to the link
    Retryable = 1,     ///< try again later; the retry engine decides when
    NonRetryable = 2,  ///< drop this request
};

struct SendResult
{
    SendOutcome outcome = SendOutcome::NonRetryable;
    /// Retryable only: the earliest time to retry, like HTTP Retry-After.
    std::optional<std::chrono::milliseconds> retry_after;
    /// Success only: items the far end rejected (OTLP partial success).
    std::uint32_t rejected = 0;
    /// Failures only: recorded, capped, as HealthSnapshot::last_error_message.
    std::string message;
};

/// @brief Carries encoded OTLP requests over a link the application owns.
///
/// @threadsafety Send may be called concurrently from one exporter worker
///   per enabled signal (at most three), never concurrently for one signal.
///   Cancel may be called from any thread, concurrently with Send.
class ExportTransport
{
public:
    virtual ~ExportTransport() noexcept = default;

    /// Called on an exporter worker thread. May block, until request.deadline.
    [[nodiscard]] virtual SendResult Send(const ExportRequest& request) = 0;

    /// Called once, from the thread running Provider::Shutdown, if the
    /// shutdown timeout expires while a Send is in flight. Send must then
    /// return promptly. The default does nothing.
    virtual void Cancel() noexcept {}
};

struct ExportTransportOptions
{
    bool traces = true;
    bool metrics = false;  ///< Decision 3
    bool logs = false;     ///< Decision 3
};

}  // namespace microtel
```

```cpp
// include/microtel/sdk_builder.hpp
SdkBuilder& WithExportTransport(std::unique_ptr<ExportTransport> transport,
                                ExportTransportOptions opts = {});
```

**Where it plugs in.** At `IWireCodec`, not `ITransport`. `ITransport` is the
HTTP/2 layer; the wire codecs above it add framing, headers, gzip and the
HTTP or gRPC status matrix, none of which means anything on a UART. A new
internal `ExportTransportCodec : IWireCodec` (in `src/wire/custom/`) wraps the
user's object, one instance per enabled signal, and `BuildExporters` uses it
in place of `BuildWireCodec`. The encoder, `OtlpExporter`, the fan-in of
design §3.6.1, `RetryEngine` and every counter stay as they are. When a custom
transport is set, `Build()` does not call `CreateTransport()`: there is no
`Http2Transport`, no reactor and no I/O thread.

**Mapping to `WireResult`.** `Success` becomes `success = true` with
`partial_success_rejected = rejected`. `Retryable` becomes `retryable = true`
with `retry_after`. `NonRetryable` becomes a non-retryable failure. Failures
carry `Error{Kind::Network, message}`. So `batches_sent`, `batches_failed`,
`partial_success_rejection`, `non_retryable_failure`,
`retryable_failure_recovered` and `retry_budget_exhausted` mean what they mean
for HTTP. `connect_failure`, `response_too_large`, `malformed_response`,
`decompression_too_large` and `transport_busy` never fire.
`error-model.md` gets this as a §7.3 matrix.

**Exceptions.** `Send` is not `noexcept`. The codec contains a throw at the
boundary exactly as `CallbackAuthProvider::InvokeCallback` contains one from
an `AuthCallback`, including its commented non-`std` case: the request becomes
a `NonRetryable` failure carrying `Error{Kind::InternalFailure, what}`.
Letting it reach the exporter's `DrainQueue` handler instead would lose every
request in the drain, not just this one (issue #251), and a non-`std` throw
would reach the `noexcept` worker and terminate.

**The bytes.** Always uncompressed, since gzip is an HTTP content-coding and
the link's own framing is the application's business. Borrowed, not owned: a
retry re-encodes the request anyway (`memory-model.md` §3.1), so there is
nothing to hand over, and a transport that queues copies. Framing,
fragmentation, link-level acknowledgement and flow control belong to the
application, as they do for the leaf.

**Other settings.** Endpoint, protocol, headers, TLS, auth and compression
have no meaning with a custom transport:

- Set **in code** together with `WithExportTransport`, they fail `Build()`
  with `ConfigError::Kind::InvalidValue`, `field = "exporter.transport"`.
  Both were asked for on purpose and one would be silently ignored.
- Set by **environment or file** (`OTEL_EXPORTER_OTLP_*`, `[exporter]`), they
  are ignored and one `Warn` goes to the `LogSink` naming them. Containers
  commonly carry `OTEL_EXPORTER_OTLP_ENDPOINT` for other processes, and failing
  on it would be hostile.
- Of `TimeoutOptions`, `per_export` becomes the `Send` deadline and
  `retry_budget`, `flush` and `shutdown` apply as today. `connect` and
  `tls_handshake` are unused. `max_response_bytes`, `max_trailer_bytes` and
  `max_decompressed_bytes` are unused.
- `Provider::Connect()` returns success and does nothing.
- `HealthSnapshot::connection_state` is kept by the codec: `Disconnected`
  until the first `Success`, `Connected` after one, `Reconnecting` after a
  failure that follows a success, `Closed` after `Shutdown`. That matches what
  the states tell an operator (`provider.hpp`): configuration before first
  contact, the peer or link afterwards.

### Decision 2: threading

- **Caller.** `Send` runs on the exporter worker of its signal
  (`OtlpExporter::m_worker`, and the metric and log equivalents), the thread
  that calls `IWireCodec::Send` today. It is never called on an application
  thread and never on the hot path. `SendAll`'s default loop applies, so one
  worker sends its requests one after another.
- **Blocking** is allowed, up to `ExportRequest::deadline`: now plus
  `per_export`, clamped to the shutdown deadline once `Shutdown` has begun.
  A slow `Send` stalls its own signal's pipeline, and the queue in front of it
  then drops by its policy, counted `queue_full`. It stalls no other signal.
- **Concurrency.** With the default options (traces only) there is one caller.
  Each enabled signal adds one worker, so `Send` must be thread-safe when
  metrics or logs are on. This is the same promise ICP 0009 made for
  `ITransport` and ICP 0029 recorded for `AuthCallback`, and it goes in the
  header.
- **Shutdown.** `RetryEngine::Abort` already ends backoff sleeps. If an
  exporter's shutdown wait expires while a `Send` is in flight, microtel calls
  `Cancel()` once, from the thread running `Shutdown`, before joining the
  worker. A `Send` that ignores both its deadline and `Cancel` blocks
  `Shutdown`, and so the `Provider` destructor. That is the one place where
  CLAUDE.md rule 15 depends on application code, and the header says so. The
  alternative, detaching the worker, would leave a thread inside an
  `ExportTransport` that is about to be destroyed.
- **Lifetime.** The `Provider` owns the transport and destroys it after every
  exporter worker has been joined.

### Decision 3: signals — traces by default, metrics and logs opt-in

The concentrator ingests traces only in v1.2 (design §1.3), so traces-only is
the default. A signal left off gets the no-op `Meter` or `Logger` from
`GetMeter` / `GetLogger`, and its pipeline and worker are never built. One
`Warn` goes to the `LogSink` the first time. There is no drop counter, because
nothing was produced to drop; ICP 0030's `SignalNotCompiled` is named for a
different condition and is not reused. If ICP 0030 has not landed, this ICP's
implementing packet adds the no-op meter that 0030 also needs.

Metrics and logs can be switched on for applications that route them
somewhere else: to their own collector connection, a file, or a concentrator
that grows metric or log ingest later. `ExportRequest::signal` tells the
application which is which, and it has to carry that across the link itself.
It must not pass a metrics or logs payload to `LeafReceiver::Ingest`. Field 1
of all three Export requests has the same wire type, so a metrics request can
partly decode as a trace request. Validation should reject it as `Malformed`,
but that would be luck rather than a guarantee.

### Decision 4: concentrator compatibility

**What arrives.** An ordinary `ExportTraceServiceRequest`, one `ResourceSpans`
per `(Resource, scope)` in the request, all carrying the node's own Resource,
and no `microtel.leaf.*` attributes. Today `Ingest` rejects it as `Malformed`,
because design §3.4 requires `microtel.leaf.proto` and deliberately left open
"what timestamps mean when no mode is declared". This ICP decides that.

**Decision.** `LeafTimeMode` gains `Unix = 3` (TOML `time_mode = "unix"`). It
is a configuration value only and never appears on the wire. A payload with
no `microtel.leaf.*` key at all is accepted **only** when the sender's
effective configured mode, its own `time_mode` or else `default_time_mode`, is
`Unix`. Its timestamps are taken as Unix time unchanged (`t' = t`, clamped as
design §5.1 clamps). Every other check in §3.4 applies: span ids, kinds,
`end >= start`, and the §3.7 limits.

- `auto` still rejects an undeclared payload, so the receiver does not become
  a general OTLP intake by default. An operator trusts a node's clock by
  naming it, in `[concentrator.leaves."<id>"]`, from a resolver, or for the
  whole fleet with `default_time_mode = "unix"`.
- A payload that declares a mode is checked as today. A leaf configured
  `unix` may still send `concentrator_stamped`, the universal fallback of
  §5.1, and nothing else.
- A payload carrying some `microtel.leaf.*` keys but not `microtel.leaf.proto`
  is `Malformed`, as today.
- On the wire, `microtel.leaf.time_mode` stays 0 to 2 and the wire version
  stays 1. The C leaf is unchanged.

**The SDK emits nothing leaf-specific.** A custom transport is a generic seam
(a file, a message bus, the application's own HTTP client), and stamping
`microtel.leaf.*` on it would leak reserved keys to anything that is not a
concentrator. The concentrator treats a trusted full node as a collector
would: its clock is its own responsibility.

**Identity** is design §4.1 unchanged. The transport-derived id (for UDP, the
source `address:port`) keys the leaf table and is written as
`leaf_id_attribute`. With no transport id, the node's own `device.id` is the
fallback. The node's Resource is layer 2 of §4.4, so its `service.name` and
the rest survive unless the operator's per-leaf config overrides them.

**Fan-in** is unchanged. The node is one entry in the leaf table. Its spans
join the concentrator's batches and leave in shared requests, one
`ResourceSpans` per `(Resource, scope)`.

**Operational notes** for the deployment guide, with no code change:

- **Sampling happens twice.** The concentrator samples every ingested span as
  a root (§3.6). A trace-id-ratio sampler at the same ratio on both sides is
  idempotent. A lower ratio at the concentrator thins the node's traces
  further.
- **Sizes.** A default node request (512 spans) can exceed the concentrator's
  `max_payload_bytes` (64 KiB) and a UDP datagram. Size the node's
  `max_export_batch_size` to the link. A node Resource with process and host
  detectors can exceed `max_leaf_resource_bytes` (2 KiB); the excess is
  dropped and counted in `resource_attributes_dropped`.
- **Mapping `IngestResult` back**, on a link that carries an answer:
  `Accepted` → `Success`; `PartiallyAccepted` → `Success` with `rejected =
  spans_dropped`; `Malformed`, `TooLarge`, `UnknownLeaf`, `Disabled` →
  `NonRetryable`; `ShutDown`, `OutOfMemory` → `Retryable`. A fire-and-forget
  link such as UDP returns `Success` once the datagram is sent.

**Concentrator-side changes, all additive:**

1. `LeafTimeMode::Unix = 3` in `include/microtel/leaf_receiver.hpp`.
2. `CheckWireInfo` / `SdkLeafReceiver::Admit`: an undeclared `ResourceSpans`
   is admitted provisionally, and `ModesAllowed` accepts it only under a
   configured `Unix`.
3. `CorrectionFor`: `Unix` is the identity correction.
4. The config parser and the validator accept `"unix"` for `time_mode` and
   `default_time_mode`, and `MICROTEL_CONCENTRATOR_DEFAULT_TIME_MODE`.
5. No new `IngestStatus`, `DropReason` or `LeafReceiverStats` field.

### Decision 5: rules 12 and 13

No new runtime dependency and nothing new in the link closure. The exporter
already depends on nothing below `IWireCodec`. A Provider built with a custom
transport still links nghttp2, OpenSSL and zlib; it just never constructs
the HTTP/2 transport. Compiling them out for a custom-transport-only build is
**out of scope**. It belongs to ICP 0030's feature selection, as a possible
`MICROTEL_WITH_HTTP2_TRANSPORT`. 0030 rejected compiling out TLS because the
result could not verify peers; a build with no built-in transport has no
peers, so that reasoning would need revisiting there, not here.

### Decision 6: example and tests

- **Example.** `examples/leaf/udp_full_node.cpp`: a C++ program using
  `SdkBuilder().WithServiceName(...).WithExportTransport(...)`, whose
  `UdpExportTransport` sends each trace request as one datagram to
  `udp_concentrator`. `examples/leaf/microtel.toml` gains a
  `[concentrator.leaves."127.0.0.1:<port>"]` entry with `time_mode = "unix"`,
  and the README a third terminal line. The same concentrator then shows C
  leaves and a C++ node side by side.
- **Unit tests.** `tests/mocks/mock_export_transport.hpp` (returns what it is
  told) and `tests/fakes/fake_export_transport.hpp` (a scripted result
  sequence, with an optional blocking `Send` released by `Cancel`). Covered:
  the `SendResult` → `WireResult` mapping and counters; retry with and without
  `retry_after`; partial success; a `Send` that throws, `std` and non-`std`;
  the deadline clamp; `Cancel` on shutdown timeout; the `connection_state`
  transitions; the `Build()` conflicts and warnings; no I/O
  thread constructed; traces-only default giving the no-op meter and logger.
  A TSAN test runs all three signals through one transport.
- **Concentrator tests.** An undeclared payload is accepted under `unix` and
  rejected under `auto` and every other mode; its timestamps are unchanged;
  the transport id still wins; a declared payload under `unix` follows the
  §5.1 rules. The ingest fuzz target (design §7.3) gets undeclared seeds with
  `default_time_mode = unix`.
- **Integration.** An in-process loopback `ExportTransport` that calls another
  Provider's `LeafReceiver::Ingest` directly, asserting that one request from
  the node arrives as its spans, with the node's Resource and `device.id`.
- **Conformance.** The design §7.5 end-to-end job adds the full node over UDP
  through the concentrator to the collector, for both concentrator protocols.
  It checks the node's spans reach the collector with unchanged timestamps and
  `service.name`, share export requests with the C leaves' spans, and carry the
  transport-derived `device.id`.

### Decision 7: versioning and follow-up docs

Additive in the v1.2 minor: a new header, a new non-virtual `SdkBuilder`
method (the builder is pimpl), and a trailing `LeafTimeMode` enumerator. No
vtable, `HealthSnapshot` or `DropReason` change. The concentrator has not been
released, so the enumerator breaks no user. An exhaustive `switch` over
`LeafTimeMode` needs one more arm.

After acceptance, in the implementing packets:

- `docs/interfaces.md` §4.3: `IWireCodec` has three implementations.
- `docs/threading-model.md` §2: the I/O thread is absent with a custom
  transport, and user code runs on the exporter workers. §10: a row for
  `ExportTransport`.
- `docs/error-model.md` §7.3: the custom-transport matrix.
- `docs/architecture.md` §3.5 and §3.6: the seam.
- `docs/configuration.md`: which settings are ignored and which conflict.
- `docs/leaf-concentrator-design.md` §3.4, §4.2, §4.3 and a new §5.6 for
  `unix`, plus the §5.5 table.
- `microtel-spec.md` §18.4 and `microtel-roadmap.md` v1.2: a line each.
- `examples/leaf/README.md`, a `README.md` for `src/wire/custom/`, and the
  CHANGELOG.

## Migration

Nothing for existing users: without `WithExportTransport` the build and every
behaviour are unchanged. Concentrator operators who add a full node configure
it with `time_mode = "unix"`. Code that switches exhaustively over
`LeafTimeMode` adds an arm.

## Rationale & alternatives

- **A callback, `std::function<SendResult(const ExportRequest&)>`.** Simpler
  for a one-liner, but there is no clean place for `Cancel`, and a
  `std::function` must be copyable, which rules out capturing a move-only
  socket or port. Rejected.
- **`Send` declared `noexcept`.** A throw in application I/O code would then
  terminate the process from a microtel worker. Containment at the boundary is
  what `AuthCallback` already does. Rejected.
- **`std::shared_ptr<ExportTransport>`.** Lets the application keep a handle,
  but rule 8 needs a reason and there is none: an application that shares its
  link keeps the link object itself and gives the transport a reference to it.
- **Plugging in at `ITransport`.** It would keep the HTTP or gRPC framing,
  headers and status matrix on a link that has no HTTP, so the application
  would have to fake HTTP/2 responses. Rejected.
- **An asynchronous `Send` with a completion callback.** It avoids blocking a
  worker, but the worker has nothing else to do, and every retry, deadline and
  accounting path is synchronous today. Could be added later beside `Send`.
- **Handing the bytes over by ownership.** Saves one copy for a queueing
  transport, but a retry re-encodes anyway, and borrowing matches
  `IngestRequest::payload` on the other side of the link.
- **The SDK stamps `microtel.leaf.*` as sync-relative** (`sync_age = 0`,
  `encode_time` per attempt), needing no concentrator change. Rejected:
  `sync_age` would be invented, since a full node doesn't know when its NTP
  last synced; the per-attempt `encode_time` would need a per-request Resource
  in the exporter; and reserved keys would reach any non-concentrator
  destination.
- **A wire-declared `Unix` mode (`microtel.leaf.time_mode = 3`)** stamped by
  the SDK. Rejected: the same key leak, plus a wire contract change for the C
  leaf, which doesn't need it.
- **Treating every undeclared payload as Unix time**, making the receiver a
  general OTLP intake. Simpler to configure, but a misconfigured producer's
  clock would pass unchecked, reversing design §3.4's default. Opt-in per
  sender keeps that default.
- **Disabling metrics and logs outright with a custom transport.** Simpler,
  but it would block the applications that route those signals themselves.
  Opt-in costs two booleans.
- **A byte cap on coalesced requests** (`max_request_bytes`), so a request
  always fits the link. Useful, but the exporter joins handles by span count
  today, and cutting one oversized handle would need encoder changes. Left out
  of this ICP; the batch size is the knob for now.
