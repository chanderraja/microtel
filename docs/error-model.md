# microtel Error Model

**Status:** M0 deliverable. Normative for the three error regimes, the drop-counter enum, the retry-classification matrix, and the diagnostic surface in v1.
**Companion documents:** `architecture.md`, `threading-model.md` (no-exceptions-across-threads), `memory-model.md` (which budget enforcement triggers which drop), `interfaces.md` (per-method error annotation).
**Source of truth for rationale:** `microtel-spec.md` §6.1, §6.4, §7.2, §7.3.

---

## 1. Purpose and authority

This document is the canonical answer to:

1. **For every public method, how does it report failure?** (return `microtel::Expected`, return a structured `Status`, drop-and-count, log-only.)
2. **What are the named drop reasons, and which layer increments which counter?**
3. **For both wire protocols, which response is retryable, which is non-retryable, which is malformed?**
4. **What does a caller see when something fails?**

Some rules are non-negotiable in v1; they are flagged **(LOCKED)**. Changing a (LOCKED) rule requires an ICP.

---

## 2. The three error regimes

Every microtel method falls into one of three regimes. The choice is structural — never mixed.

### 2.1 Initialisation regime — `microtel::Expected<T, E>` (LOCKED)

`SdkBuilder::Build()` and any other one-shot configuration entry point that can fail validates eagerly and returns `microtel::Expected<std::shared_ptr<Provider>, ConfigError>`. No exceptions on init paths.

`microtel::Expected` is the project-local alias defined in `include/microtel/expected.hpp` per [ICP 0002](icps/0002-vendor-tl-expected.md). It resolves to `std::expected<T, E>` when the floor moves to C++23; on C++20 it aliases the vendored `tl::expected` polyfill. Callers see the same shape regardless of the active backend.

Why expected and not exceptions:

- Init failures are programmer-actionable. The application should branch on them, log them, and either retry with adjusted config or exit. Exceptions encourage swallowing.
- Init failures often happen during static-init of larger systems where exceptions are awkward.
- `microtel::Expected` composes naturally with structured logging.

### 2.2 Hot-path regime — `noexcept` and drop-and-count (LOCKED)

Every method on `Tracer` and `Span` listed in `threading-model.md` §2.1 is `noexcept`. On any internal failure (queue full, span limit exceeded, post-shutdown call, malformed input), the method:

1. Drops the record / field / event.
2. Increments the appropriate counter on `IDiagnosticsSink` (§3 below).
3. Returns silently.

The application sees no error. This is intentional — telemetry instrumentation must never disturb the host application's control flow.

### 2.3 Lifecycle regime — structured `Status` (LOCKED)

`Provider::ForceFlush(timeout)` and `Provider::Shutdown(timeout)` return a single small enum:

```
enum class Status : std::uint8_t
{
    Completed       = 0,    // operation finished within the timeout
    TimedOut        = 1,    // partial work; some data may not have been flushed
    AlreadyShutDown = 2,    // idempotent re-call after Shutdown
    Failed          = 3,    // unrecoverable internal error; see GetExporterHealth()
};
```

The `Status` is `[[nodiscard]]`. Callers must inspect it. Detail beyond the four values lives in `Provider::GetExporterHealth()` and the diagnostic log; the lifecycle return is intentionally coarse.

---

## 3. Drop-counter enum (LOCKED)

Each drop reason maps to exactly one counter. The counter is incremented exactly once per drop event by the layer that decides the drop. Counters are exposed via `Provider::GetExporterHealth()` and rate-limited in the diagnostic log.

| Reason (counter name) | Where incremented | Triggered by |
|---|---|---|
| `queue_full` | `BatchSpanProcessor`, on `End()` enqueue | span queue at capacity (default `drop_newest`) |
| `record_too_large` | `BatchSpanProcessor`, on `End()` enqueue | record's encoded-size estimate exceeds `max_record_bytes` |
| `span_attribute_limit` | API layer, in `SetAttribute` | per-span `attribute_count_limit` reached |
| `span_event_limit` | API layer, in `AddEvent` | per-span `event_count_limit` reached |
| `span_link_limit` | API layer, in `AddLink` | per-span `link_count_limit` reached |
| `event_attribute_limit` | API layer, in `AddEvent` (per-event attributes) | per-event `event_attribute_count_limit` reached |
| `link_attribute_limit` | API layer, in `AddLink` (per-link attributes) | per-link `link_attribute_count_limit` reached |
| `attribute_value_truncated` | API layer, on attribute set | string value exceeded `attribute_value_length_limit`; truncation, not a full drop, but counted here |
| `post_shutdown` | API layer (caller path) and `BatchSpanProcessor` | call after `Shutdown` returned |
| `response_too_large` | wire codec | response body exceeded `max_response_bytes` |
| `decompression_too_large` | wire codec | decompressed body exceeded `max_decompressed_bytes` |
| `malformed_response` | wire codec | response could not be parsed (missing trailers, bad framing, unparseable proto) |
| `partial_success_rejection` | wire codec, on partial-success parse | rejected items count from the response — see §6 |
| `non_retryable_failure` | wire codec | request returned a non-retryable status (415, gRPC `INVALID_ARGUMENT`, etc.) |
| `retryable_failure_recovered` | exporter | a retryable failure that subsequently succeeded — counted for visibility, not a drop |
| `retry_budget_exhausted` | exporter | retry-budget elapsed before success |
| `transport_busy` | wire codec, propagated | transport request queue full (§3.2 of `threading-model.md`) |
| `connect_failure` | transport | TCP / TLS / ALPN handshake failed during initial connect or reconnect |
| `force_flush_timeout` | exporter | `ForceFlush` deadline elapsed with records still queued |
| `shutdown_timeout` | exporter / transport | `Shutdown` deadline elapsed with in-flight work |
| `cardinality_overflow` | SDK, metric aggregation store | attribute set exceeded the per-instrument cardinality limit; the measurement is folded into the `otel.metric.overflow` series, not lost (ICP 0008, `metrics-design.md` §2) |
| `metric_callback_timeout` | SDK, metric collection | async instrument callback exceeded the per-collection deadline; its measurements were dropped for that cycle (ICP 0008, `metrics-design.md` §4) |
| `non_finite_value` | SDK, instrument record path | NaN / ±Inf measurement dropped, as the OTel spec requires (ICP 0008) |
| `log_attribute_limit` | SDK, `SdkLogger::Emit` | a `LogRecord`'s attribute set exceeded the per-record attribute limit; surplus attributes dropped and `dropped_attributes_count` incremented (ICP 0011, `logs-design.md` §5) |

**Counters are `std::atomic<uint64_t>`** (LOCKED). The increment path is lock-free and fits the leaf-lock rule in `threading-model.md` §4.

**Adding a new counter is an ICP** because every counter is part of `GetExporterHealth()`'s public surface. Renaming a counter is an ICP.

---

## 4. Error types

Two value types and one enum cover the regimes in §2.

### 4.1 `microtel::Error` — runtime error description

A small value type used as the `E` in any non-init `microtel::Expected` path, and as the carried payload in `WireResult::error` for non-success cases. Defined in `include/microtel/error.hpp`.

```
class Error
{
public:
    enum class Kind : std::uint8_t
    {
        Unspecified           = 0,
        Network               = 1,   // socket / TLS / nghttp2 transport error
        Protocol              = 2,   // OTLP wire failure (status interpretation)
        ResourceExhausted     = 3,   // peer signalled overload; retryable depending on RetryInfo
        Cancelled             = 4,   // local cancel (timeout, shutdown)
        Malformed             = 5,   // unparseable response or trailer
        InternalFailure       = 6,   // microtel bug; should never happen in production
    };

    Kind        kind = Kind::Unspecified;
    std::string message;          // short, redacted, safe to log
    int         os_errno = 0;     // optional OS errno or library code
};
```

`message` is short and pre-redacted. It is safe to log at any level. It does not include user-supplied payload bytes.

### 4.2 `microtel::ConfigError` — init-time error description

Used only in the `microtel::Expected<Provider, ConfigError>` returned from `SdkBuilder::Build()` and similar init paths. Defined in `include/microtel/error.hpp`.

```
class ConfigError
{
public:
    enum class Kind : std::uint8_t
    {
        Unspecified                = 0,
        InvalidValue               = 1,    // a setting parsed but failed validation
        UnknownKey                 = 2,    // strict-mode unknown TOML key
        EnvParseFailure            = 3,    // OTEL_* / MICROTEL_* env var malformed
        FileNotFound               = 4,
        FileParseFailure           = 5,    // TOML syntax error
        TlsMaterialUnreadable      = 6,    // CA bundle / client cert / key not openable or invalid
        EndpointMalformed          = 7,
        ProtocolMismatch           = 8,    // explicit protocol disagrees with URL scheme
        InsecureDisallowed         = 9,    // MICROTEL_FORBID_INSECURE_TLS=ON and insecure=true
        BuildAlreadyConsumed       = 10,   // SdkBuilder::Build() called twice
    };

    Kind        kind = Kind::Unspecified;
    std::string field;              // dotted path to the offending setting; empty if not field-bound
    std::string message;            // human-readable, safe to log
};
```

`field` carries a dotted path like `"exporter.endpoint"` so applications can surface a precise diagnostic to the operator.

### 4.3 `microtel::Status` — lifecycle status

The four-value enum from §2.3.

---

## 5. Cross-thread rules

**No exception ever crosses a thread boundary** (LOCKED).

Three concrete consequences:

1. **The exporter worker never throws.** Internal failures on the worker (encoder bug, allocation failure, parse error) are caught at the worker's top-level loop, recorded as a diagnostic, and the worker continues. The caller of `ForceFlush` / `Shutdown` observes a `Status` of `Failed`; detail is in `GetExporterHealth()`.
2. **The I/O thread never throws.** Same pattern. Internal failures complete the in-flight request with `Error::Kind::InternalFailure` and a short message; the worker observes a failed `WireResult` and proceeds.
3. **Caller-thread methods are `noexcept`.** Anything that would unwind is caught at the API boundary; the path drops-and-counts.

**No `std::exception_ptr` is moved between threads.** Errors are recorded as diagnostics on the producing thread; the consuming thread observes a *result* (a counter increment, a failed `WireResult`, a `Status`) and acts on it.

---

## 6. Partial success (LOCKED — never retried)

Per `microtel-spec.md` §7.3:

When the wire codec parses an OTLP response that includes `partial_success` with a non-zero rejected count:

1. The codec records `partial_success_rejection` with the rejected count on `IDiagnosticsSink`.
2. The codec returns a `WireResult` with `success=true` and `partial_success_rejected=N`.
3. **The exporter does not retry the request.** Retrying would re-send the items the receiver already accepted.

This is the most counterintuitive rule in the error model, which is exactly why it has its own sequence diagram (`docs/sequences/partial-success.md`) and its own counter.

The rejected-items error message from the response is captured (capped at `max_response_bytes`) and surfaced via `GetExporterHealth()` so operators can investigate. Partial-success responses are still logged, at `warn`, with rate limiting.

---

## 7. Retry classification matrix

The wire codec — not the exporter — owns retry classification (per ICP 0001 and `interfaces.md`). The exporter respects the `WireResult::retryable` flag without reinterpretation.

### 7.1 OTLP/HTTP

| Response | `success` | `retryable` | `retry_after` | Counter |
|---|---|---|---|---|
| 2xx, no body | true | n/a | n/a | (success) |
| 2xx, partial-success body, rejected = 0 | true | n/a | n/a | (success) |
| 2xx, partial-success body, rejected > 0 | true | **false** (never retried) | n/a | `partial_success_rejection` |
| 429 | false | true | from `Retry-After` if present, else jittered backoff | (counted on retry outcome) |
| 502, 503, 504 | false | true | from `Retry-After` if present, else jittered backoff | (counted on retry outcome) |
| 404 | false | false | n/a | `non_retryable_failure` |
| 415 | false | false | n/a | `non_retryable_failure` |
| Other 4xx | false | false | n/a | `non_retryable_failure` |
| Other 5xx (not in retryable list) | false | false | n/a | `non_retryable_failure` |
| Connection failure / TLS failure / read timeout | false | true (limited attempts) | jittered backoff | `connect_failure` if pre-request |
| Response > `max_response_bytes` | false | false | n/a | `response_too_large` |
| Decompressed body > `max_decompressed_bytes` | false | false | n/a | `decompression_too_large` |
| Body unparseable as protobuf | false | false | n/a | `malformed_response` |

### 7.2 OTLP/gRPC

| `grpc-status` | `success` | `retryable` | `retry_after` | Counter |
|---|---|---|---|---|
| `OK (0)` | true | n/a | n/a | (success) |
| `OK` with partial-success rejected > 0 | true | **false** (never retried) | n/a | `partial_success_rejection` |
| `CANCELLED (1)` | false | true | jittered backoff | |
| `INVALID_ARGUMENT (3)` | false | false | n/a | `non_retryable_failure` |
| `DEADLINE_EXCEEDED (4)` | false | true | jittered backoff | |
| `NOT_FOUND (5)` | false | false | n/a | `non_retryable_failure` |
| `PERMISSION_DENIED (7)` | false | false | n/a | `non_retryable_failure` |
| `RESOURCE_EXHAUSTED (8)`, with `RetryInfo` in details | false | true | from `RetryInfo.retry_delay` | |
| `RESOURCE_EXHAUSTED (8)`, **without `RetryInfo`** | false | **false** | n/a | `non_retryable_failure` |
| `FAILED_PRECONDITION (9)` | false | false | n/a | `non_retryable_failure` |
| `ABORTED (10)` | false | true | jittered backoff | |
| `OUT_OF_RANGE (11)` | false | true | jittered backoff | |
| `UNIMPLEMENTED (12)` | false | false | n/a | `non_retryable_failure` |
| `INTERNAL (13)` | false | false | n/a | `non_retryable_failure` |
| `UNAVAILABLE (14)` | false | true | jittered backoff | |
| `DATA_LOSS (15)` | false | true | jittered backoff | |
| `UNAUTHENTICATED (16)` | false | false | n/a | `non_retryable_failure` |
| Trailer-only response without `grpc-status`, HTTP `:status` 429/502/503/504 | false | true | jittered backoff | |
| Trailer-only response without `grpc-status`, other HTTP `:status` | false | false | n/a | `malformed_response` |
| Multi-frame parse failure / truncated message | false | false | n/a | `malformed_response` |
| Decoded body > `max_decompressed_bytes` | false | false | n/a | `decompression_too_large` |

The `RESOURCE_EXHAUSTED` row is the most important non-obvious entry — it is documented separately in `microtel-spec.md` §7.2 and has acceptance test coverage requirements per the M4 milestone in spec §13.

---

## 8. Init-failure taxonomy

`SdkBuilder::Build()` returns `microtel::Expected<std::shared_ptr<Provider>, ConfigError>` and validates eagerly. Every documented init failure maps to a `ConfigError::Kind`:

| Failure | `ConfigError::Kind` | `field` example |
|---|---|---|
| Unknown key in `microtel.toml`, strict mode | `UnknownKey` | `exporter.unknownsetting` |
| TOML syntax error | `FileParseFailure` | (empty) |
| Endpoint URL malformed | `EndpointMalformed` | `exporter.endpoint` |
| Endpoint scheme conflicts with explicit `protocol` | `ProtocolMismatch` | `exporter.protocol` |
| TLS CA bundle missing or unreadable | `TlsMaterialUnreadable` | `exporter.tls.ca_bundle` |
| Client cert / key path unreadable | `TlsMaterialUnreadable` | `exporter.tls.client_cert` |
| `insecure=true` while compiled with `MICROTEL_FORBID_INSECURE_TLS=ON` | `InsecureDisallowed` | `exporter.tls.insecure` |
| Out-of-range numeric value (negative timeout, etc.) | `InvalidValue` | `exporter.timeouts.per_export` |
| `OTEL_EXPORTER_OTLP_ENDPOINT` malformed | `EnvParseFailure` | `OTEL_EXPORTER_OTLP_ENDPOINT` |
| Second call to `SdkBuilder::Build()` | `BuildAlreadyConsumed` | (empty) |

**Network preflight is not part of `Build()`.** `Build()` does not open sockets. Network reachability is validated by `microtel --preflight=connect` / `--preflight=export` (spec §6.4) — never as a side effect of constructing a `Provider`.

---

## 9. Diagnostic surface

Three layers of visibility, in increasing detail.

### 9.1 `Provider::GetExporterHealth()`

Returns a structured snapshot. The shape is locked in `interfaces.md` against the `IDiagnosticsSink` interface; for v1 the snapshot includes:

- All counters in §3 by name, as `uint64_t`.
- Total batches sent, total batches failed.
- Current queue depth.
- Last-error timestamp and last-error short message (capped).
- Connection state (one of `Disconnected`, `Connecting`, `Connected`, `Reconnecting`, `Closed`).

The snapshot is consistent at a moment in time but not transactionally consistent across counters — it is a read of `std::atomic<uint64_t>` values and a borrowed view into the last-error slot.

### 9.2 Internal diagnostic log

Routed via spdlog (`MICROTEL_USE_SPDLOG=ON`, the default) or the minimal stderr fallback (`=OFF`). Levels:

- `error` — non-retryable failures, init failures, internal-failure recovery, `connect_failure` after reconnect-budget elapsed.
- `warn` — retryable failures, `partial_success_rejection`, `force_flush_timeout`, `shutdown_timeout`.
- `info` — `Build()` resolved-config dump (with secrets redacted per §6.6 of spec), connect / disconnect transitions.
- `debug` — per-batch send / receive summary, per-stream lifecycle.
- `trace` — per-frame nghttp2 events (rare; primarily for development).

**Rate limiting (LOCKED).** Diagnostic emissions for repeating events are rate-limited with a token-bucket limiter per `(level, reason)` pair. Defaults: 1 burst of 10, 1/sec sustained. The first occurrence of a new reason is always emitted; subsequent ones are suppressed but counted separately so operators can see "10 emitted, 4231 suppressed" in `GetExporterHealth()`.

### 9.3 Sink injection

Applications may redirect microtel's internal logs into their own logging system via `microtel::SetLogSink`:

```
microtel::SetLogSink([](microtel::LogLevel lvl, std::string_view msg) {
    my_app_logger.Log(static_cast<int>(lvl), msg);
});
```

Sink injection is available in both `MICROTEL_USE_SPDLOG=ON` and `=OFF` builds (spec §9.4).

### 9.4 Never-recursive-export rule (LOCKED)

**microtel's internal diagnostic logs are never routed back through microtel's own OTLP exporter** in v1. Recursive export creates a failure loop where a broken exporter generates more telemetry it cannot ship, amplifying the original problem.

If an application wants its OTel-Logs pipeline to receive microtel's internal logs, it does so explicitly via `SetLogSink` pointing at a separate logger. v1 does not provide a built-in bridge.

---

## 10. What this document does not cover

- The exact wire-level byte sequences that produce each row in §7 — see `grpc-wire-protocol.md` for the gRPC side and the conformance tests for both.
- The retry timing algorithm (jitter formula, backoff multiplier) — pinned in M5; covered in `docs/sequences/retry-after-failure.md` once written.
- The exact spdlog pattern strings — implementation detail of `src/common/logging`.
- Per-method error annotations — see `interfaces.md`.
