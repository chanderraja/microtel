# microtel: A Lightweight OpenTelemetry-Compatible Trace Runtime and OTLP Exporter Built on nghttp2

**Status:** Draft v0.13 (SonarQube Cloud OSS tier, single-reviewer, tooling-enforced TDD)
**Owner:** TBD
**License:** Apache 2.0 (matches OpenTelemetry upstream)

---

## 1. Summary

`microtel` is an open-source, lightweight OpenTelemetry-compatible trace runtime focused on OTLP export with a small dependency and runtime footprint. It replaces the heavyweight gRPC client stack with a thin HTTP/2 transport built directly on **nghttp2**, supporting both **OTLP/HTTP-protobuf** and **OTLP/gRPC** on the wire without linking against the gRPC library. The gRPC path is implemented as a small unary-RPC protocol layer over the same nghttp2 transport.

**v1 is exporter-first with traces as the only signal.** It includes the minimum trace SDK surface required to produce and export spans manually — `Tracer`, `Span`, context, W3C propagation, samplers, batch processor, resource handling, lifecycle methods — but it is not a full OpenTelemetry SDK implementation. The release proves wire compatibility, low runtime overhead, and a small dependency closure on Linux. Metrics, logs, control plane, hot reload, sugar APIs, and broader SDK coverage are staged as follow-on releases after the core transport and exporter are stable.

The deliverable is a **C++20 core** with optional **Python bindings**, packaged for Linux first (RHEL / Debian / Fedora), with a long-term path to other POSIX platforms. RHEL 8 default toolchain users need `devtoolset-11` or newer; if locked-down enterprise adoption requires it, a C++17 compatibility mode will be evaluated after the transport prototype lands.

### Why not upstream this into `opentelemetry-cpp`?

`microtel` is an experiment in a smaller dependency and runtime model than the default SDK architecture. If the nghttp2 transport proves stable and useful, parts of the work may be proposed upstream. For now, an independent project lets us move faster, keep a stricter scope, and optimize for constrained deployments without requiring upstream SDK-wide tradeoffs.

---

## 2. Goals & Compatibility

### 2.1 Goals

1. **Prove the wedge.** Ship a production-usable OTLP exporter for traces that demonstrates nghttp2-native OTLP/HTTP and OTLP/gRPC compatibility and validates the dependency-footprint claim.
2. **Documented OpenTelemetry compatibility tiers** (see §2.2). v1 guarantees Tier 1 and Tier 2 for traces.
3. **Lower CPU & memory at emit time** than `opentelemetry-cpp` with either of its OTLP exporters, measured on a fixed workload (see §10).
4. **Smaller binary and dependency footprint.** No gRPC library, no abseil-cpp transitive sprawl, no protoc-gen-grpc-cpp, no Bazel.
5. **Production correctness.** Partial-success parsing, retry with backoff, trailer-only and multi-frame gRPC parsing, GOAWAY/RST_STREAM handling, explicit timeout taxonomy, deterministic shutdown, defined backpressure semantics.
6. **Operational deployability.** Static config file with OTel env-var fallback, TLS via system trust or explicit CA, basic auth via static headers or callback, internal diagnostic logging.

### 2.2 Compatibility tiers

Compatibility is defined in tiers so the promise is testable, not aspirational:

| Tier | Promise | v1 status |
|---|---|---|
| **Tier 1: OTLP wire compatibility** | Exported payloads are accepted by receivers implementing the pinned OTLP specification version, over both OTLP/HTTP-protobuf and OTLP/gRPC. The client correctly parses success, failure, retryable failure, and partial-success responses. Compatibility is measured against a pinned `opentelemetry-proto` tag and an interop matrix of collector and backend versions (§14). | ✅ for traces |
| **Tier 2: OpenTelemetry data-model compatibility** | Traces, attributes, resources, and instrumentation scope map to the OpenTelemetry protobuf schemas. | ✅ for traces |
| **Tier 3: API-adapter compatibility** | Optional shims support common `opentelemetry-cpp` and `opentelemetry-python` use cases. The supported subset is documented in §14. | experimental for traces |
| **Tier 4: Full SDK conformance** | Every requirement in the OpenTelemetry SDK specification is met. | explicitly out of scope for v1 |

## 3. Non-Goals

- **Metrics, logs, control plane, hot reload, sugar APIs, full SDK conformance, full Python parity.** All deferred to follow-on releases.
- **gRPC features beyond OTLP needs:** streaming RPCs, name resolution, xDS, load balancing, interceptors, channel state machines, health checking, reflection. OTLP is unary-only.
- **Windows support in v1.**
- **Vendor-specific exporters** (Datadog, New Relic, etc.). The collector handles that.
- **Auto-instrumentation.** v1 is manual instrumentation only.
- **Connection coalescing across protocols** as a default (see §5.2).

---

## 4. Motivation

`opentelemetry-cpp` paired with the OTLP/gRPC exporter pulls in **gRPC, abseil, c-ares, re2, protobuf, OpenSSL, zlib** as a baseline. On a constrained or air-gapped target this is hundreds of MB of source, long build times, and a large runtime resident set.

The architectural insight: gRPC at the wire level is a thin protocol on top of HTTP/2 — a 5-byte length-prefixed framing per message, a handful of specific HTTP/2 headers, and an HTTP/2 trailer carrying `grpc-status`. The complexity that makes the gRPC *library* heavy lives almost entirely in features OTLP doesn't use. Implementing only the gRPC wire protocol on top of nghttp2 keeps wire compatibility while shedding the entire gRPC library closure.

`nghttp2` is a mature, MIT-licensed, ~30k-line C library that implements HTTP/2 framing, HPACK, and flow control without imposing a programming model. Driving it directly we keep:

- HTTP/2 multiplexing (one TCP connection serves all batches over a single endpoint)
- TLS via OpenSSL (already required transitively almost everywhere)
- Protobuf serialization via **upb**, Google's lightweight C protobuf runtime

The estimated runtime dependency closure for the v1 core is: **nghttp2, OpenSSL, upb (vendored), zlib**, plus optional **spdlog** (header-only, std::format mode). Same closure regardless of whether the user picks HTTP or gRPC at runtime.

---

## 5. Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│             Application (C++; Python optional)                  │
└─────────────────────────────────────────────────────────────────┘
                                │
                                ▼
┌─────────────────────────────────────────────────────────────────┐
│              OpenTelemetry Trace API (v1)                       │
│        Tracer · Span · Context · W3C Propagators                │
└─────────────────────────────────────────────────────────────────┘
                                │
                                ▼
┌─────────────────────────────────────────────────────────────────┐
│                     SDK (minimal, v1)                           │
│  Resource · AlwaysOn / AlwaysOff / TraceIdRatio / ParentBased   │
│  BatchSpanProcessor · ForceFlush · Shutdown                     │
└─────────────────────────────────────────────────────────────────┘
                                │
                                ▼
┌─────────────────────────────────────────────────────────────────┐
│                       OTLP Wire Encoder                         │
│      upb (vendored, pinned) + OTel .proto definitions           │
│           C accessors wrapped behind a thin C++ API             │
└─────────────────────────────────────────────────────────────────┘
                  │                           │
                  ▼                           ▼
┌─────────────────────────┐   ┌─────────────────────────────────┐
│  OTLP/HTTP Codec        │   │  OTLP/gRPC Codec                │
│  - application/         │   │  - 5-byte length-prefix framing │
│    x-protobuf           │   │  - :path: /<svc>/<method>       │
│  - POST /v1/traces      │   │  - te: trailers                 │
│  - retry/partial-success│   │  - parses grpc-status trailer   │
└──────────┬──────────────┘   └─────────────────┬───────────────┘
           │                                    │
           └──────────────┬─────────────────────┘
                          ▼
┌─────────────────────────────────────────────────────────────────┐
│                  Transport: HTTP/2 (nghttp2)                    │
│       One connection per endpoint/protocol tuple (default)      │
│           TLS via OpenSSL · epoll / kqueue I/O loop             │
└─────────────────────────────────────────────────────────────────┘
                                │
                                ▼
                      OTel Collector (any)
                or any OTLP-compatible backend
```

The `Transport` layer is abstract behind an interface (`Connect / Send / Close / OnResponse`). The v1 implementation is nghttp2-over-OpenSSL; the seam exists so a future nghttp3-based HTTP/3 transport can drop in without touching exporters or the SDK. No HTTP/3 work in v1.

### 5.1 Threading model

- **Caller thread:** API calls (`StartSpan`, `End`) are non-blocking and never wait on I/O. Span records are written to an MPSC queue.
- **Exporter worker thread (one per process):** Drains the queue, batches by size and time deadline, encodes to protobuf via the OTLP encoder, hands to the wire codec (HTTP or gRPC), then to the HTTP/2 transport.
- **HTTP/2 I/O thread (one per process):** Owns the nghttp2 session and the socket. Uses `epoll` (Linux) or `kqueue` (BSD/macOS). Receives serialized payloads from the exporter worker via a small lock-protected request queue.

Caller threads never block on the export pipeline by default.

### 5.2 Connection management

Default is **one HTTP/2 connection per endpoint/protocol tuple**. The OTLP spec allows servers to accept both HTTP and gRPC on the same port, but real-world infrastructure (collectors, gateways, reverse proxies, service meshes, load balancers) frequently routes 4317 and 4318 differently. The default does not assume otherwise.

**Optional experimental coalescing** can reuse a single connection for OTLP/HTTP and OTLP/gRPC only when (a) the endpoint is identical and (b) a preflight probe at startup confirms the receiver accepts both protocols on that endpoint. Off by default; gated behind explicit config.

Reconnect uses exponential backoff with jitter on socket-level failures. The transport honors HTTP/2 `GOAWAY`, `Retry-After`, HTTP `429/502/503/504`, and gRPC retryable status codes per the OTLP spec.

### 5.3 Shutdown and fork

- `ForceFlush(timeout)` flushes queued telemetry up to the timeout. Returns whether the flush completed.
- `Shutdown(timeout)` stops accepting new records, drains queues up to the timeout, closes the transport, and joins worker threads. Idempotent.
- Destructors must not block indefinitely; they invoke `Shutdown` with a small finite timeout if not already shut down.
- `std::atexit` integration is opt-in.
- After `fork()`, the child process starts with exporter workers **disabled** until explicitly reinitialized. The parent's I/O state is not safe to share across fork.

### 5.4 Backpressure and drop accounting

- Caller threads never block by default.
- On queue overflow, the SDK drops **newest** records by default (configurable to drop-oldest). Drop-newest preserves FIFO ordering of already-queued batches and avoids starving older in-flight traces; operators focused on current-state diagnostics can switch to drop-oldest.
- Drops happen at `End()` time, not `StartSpan()`. If the queue is full when a span ends, the completed span is dropped — `StartSpan()` always returns a usable span object that the application can populate with attributes and events; whether the data ultimately ships is decided when the span enters the queue.
- Per-signal, per-reason drop counters are exposed via `GetExporterHealth()` and through internal logs.
- A rate-limited diagnostic is emitted on first drop and periodically while dropping continues.

### 5.5 Memory budgets

Bounded queue size alone is insufficient — a single record with many large attributes can blow memory. v1 enforces:

```toml
[limits]
max_total_queue_bytes  = "16MiB"
max_record_bytes       = "64KiB"
max_response_bytes     = "1MiB"
max_trailer_bytes      = "64KiB"
max_decompressed_bytes = "4MiB"   # decompression-bomb protection
```

Records exceeding `max_record_bytes` are dropped with a diagnostic. Responses exceeding `max_response_bytes` cause the request to be treated as failed.

### 5.6 Span limits

Span structural limits are enforced per the OTel SDK specification. Defaults:

```toml
[span_limits]
attribute_count_limit        = 128
event_count_limit            = 128
link_count_limit             = 128
attribute_value_length_limit = 4096
event_attribute_count_limit  = 128
link_attribute_count_limit   = 128
```

When a limit is exceeded, additional attributes / events / links are **dropped and counted**. Existing values are not evicted in v1 (no LRU). Limit drops are visible through `GetExporterHealth()` and rate-limited internal diagnostics.

---

## 6. API Surface (v1)

The native `microtel::*` API is the supported v1 API. Compatibility shims (§6.3) are experimental migration aids and are not required to use microtel. Production users should prefer the native API unless they are specifically evaluating migration from existing OpenTelemetry SDK code.

### 6.1 C++

```cpp
#include <microtel/tracer.hpp>

auto provider = microtel::SdkBuilder()
    .WithResource({{"service.name", "my-service"}, {"service.version", "1.2.3"}})
    .WithEndpoint("https://collector.internal:4317")
    .WithProtocol(microtel::Protocol::Grpc)        // or Http
    .WithBatch({.max_queue_size       = 8192,
                .max_export_batch_size = 512,
                .schedule_delay        = std::chrono::milliseconds(5000)})
    .Build();

auto tracer = provider->GetTracer("my.component");
{
    auto span = tracer->StartSpan("handle_request");
    span->SetAttribute("http.route", "/users/:id");
    // ... work ...
}  // RAII close

provider->ForceFlush(std::chrono::seconds(5));
provider->Shutdown(std::chrono::seconds(5));
```

The public C++ API is exposed through lightweight headers; implementation lives in `libmicrotel`. Not header-only.

**Style:** Allman braces, `m_` prefix on members, PascalCase types. Public API uses OTel naming where conventions dictate. Full coding standards in `docs/coding-standards.md` (M0 deliverable).

**Error model:**

- `SdkBuilder::Build()` validates eagerly and returns `std::expected<std::shared_ptr<Provider>, microtel::ConfigError>`. Initialization failures are surfaced explicitly, not via exceptions.
- Hot-path span operations (`StartSpan`, `SetAttribute`, `AddEvent`, `End`) are `noexcept`. On failure (queue full, limit exceeded, post-shutdown call) microtel drops the record or field, increments diagnostics, and returns. No exceptions on the data path.
- Destructors are `noexcept` and must not block indefinitely; they invoke `Shutdown` with a small finite timeout if not already shut down.
- `ForceFlush(timeout)` and `Shutdown(timeout)` return a structured status (`Completed` / `TimedOut` / `AlreadyShutDown` / `Failed`). The exact status type is locked during M0 and documented in `docs/interfaces.md`.

### 6.2 Python (post-v1.0; M18, all three signals)

```python
from microtel import SdkBuilder

provider = (SdkBuilder()
    .with_resource(service_name="my-service", service_version="1.2.3")
    .with_endpoint("https://collector.internal:4317")
    .with_protocol("grpc")
    .build())

tracer = provider.get_tracer("my.component")
with tracer.start_as_current_span("handle_request") as span:
    span.set_attribute("http.route", "/users/:id")

provider.force_flush(timeout_s=5)
provider.shutdown(timeout_s=5)
```

Bindings via **nanobind**, covering traces, metrics, and logs. `microtel.__capabilities__` exposes the binding scope at runtime; any surface not yet bound raises `NotImplementedError` with the version when it's expected.

> **The example above is the M18 target shape, not a description of today.**
> `start_as_current_span` requires a thread-local current-span slot, which the
> SDK does not yet have — `SdkTracer::StartAsCurrentSpan` currently delegates to
> `StartSpan`, so the `with` block ends the span correctly but child spans are
> not implicitly parented. The same missing slot is why log↔trace correlation
> does not fire (`ICurrentSpanSource` is wired to `nullptr`). Both are
> prerequisites for M18. See [ICP 0013](docs/icps/0013-rescope-defer-python-bindings.md).

### 6.3 Compatibility shims (experimental; not yet implemented)

Optional adapter packages let existing OTel API code call into microtel:

- `microtel_otelcpp_shim`: implements the `opentelemetry-cpp` API over microtel's SDK for traces, metrics, and logs. Scheduled as **M17** — see [ICP 0014](docs/icps/0014-otelcpp-shim-and-rule-13.md).
- `microtel_python_shim`: registers as the global provider for `opentelemetry-api` consumers. Follows the Python bindings (M18); not separately scheduled.

**Neither exists yet.** `microtel-roadmap.md` §3's tier table previously showed Tier 3 as "experimental: traces" from v1.0; that was a forward-looking target, not a description of shipped code, and the table has been corrected to reflect it.

**These are released as experimental packages.** They are not v1.0 load-bearing, the supported subset is partial, and the compatibility matrix records exactly what's covered. Compatibility tests target the OTel-like API and wire output, not convenience wrappers.

`microtel_otelcpp_shim` is **source-only** — it is never shipped as a prebuilt binary, for any configuration. The opentelemetry-cpp API encodes its `nostd::` types and ABI version into every signature, so a prebuilt shim's ABI would depend on build choices made by the consumer. Building from source inside the consumer's own build is what makes exactly one configuration apply. Full reasoning in ICP 0014.

A migration guide (`docs/migration-from-otel-cpp.md`) covers: dependency-graph diff, build-system changes, API import changes, OTel env-var → `microtel.toml` translation, and a checklist of behavioral differences operators should sanity-check before promoting to production. It is a v1.0 release deliverable (M10) and is validated against the working shim (M17). Milestone numbers in this project are as-built labels rather than an execution sequence — M11–M14 shipped while M10 has not — so M17 preceding M10 in practice is expected. If v1.0 ships first, the guide's shim-validated sections are the part that must wait.

### 6.4 Operational surface (v1)

Distinct from the deferred control plane (§17), v1 ships a minimal operational surface:

- `microtel --preflight=connect <config>`: validates DNS / TCP / TLS / HTTP/2 only. No telemetry sent.
- `microtel --preflight=export <config>`: sends a single synthetic span to each configured endpoint and prints success or precise failure. Synthetic spans are tagged so collector rules can drop them:
  - `service.name = "microtel-preflight"`
  - span name = `"microtel.preflight"`
  - attributes: `microtel.preflight = true`, `microtel.version = "<version>"`, `microtel.protocol = "grpc"|"http"`
- `provider->GetExporterHealth()` C++/Python API: structured exporter stats (batches sent/failed, queue depth, last error timestamp, drop counters by reason — including queue-overflow, span-limit, record-too-large, partial-success-rejection).
- Internal diagnostic logging via spdlog (or the minimal stderr logger if `MICROTEL_USE_SPDLOG=OFF`); see §9.4. **Internal diagnostics are never recursively exported through microtel's own OTLP exporter in v1** — preventing failure loops where a broken exporter generates more telemetry it can't ship.
- `ForceFlush()` and `Shutdown()` lifecycle methods (§5.3).

No long-running Unix socket server, no `microtelctl`, no JSON wire protocol, no hot reload in v1.

---

## 7. Wire Protocols

### 7.1 OTLP/HTTP (protobuf)

- **Endpoint paths** (per OTLP/HTTP spec): `POST /v1/traces`. (`/v1/metrics`, `/v1/logs` are protocol-supported but unused in v1.)
- **Default port:** 4318.
- **Content-Type:** `application/x-protobuf`.
- **Content-Encoding:** `gzip` for request compression (configurable, off by default for low-CPU profile).
- **Accept-Encoding:** `gzip` when response decompression is supported.
- **Retry policy:** retry on 429, 502, 503, 504; respect `Retry-After`; exponential backoff with jitter; drop after configurable max attempts.
- **Response handling:** 2xx responses with a body are parsed as `ExportTraceServiceResponse`. Non-2xx responses are classified by HTTP status; response bodies are captured up to `max_response_bytes` for diagnostics only and are not interpreted for retry except where the OTLP spec requires `Retry-After`. 415 (Unsupported Media Type) and 404 (Not Found) are non-retryable misconfiguration errors and surfaced explicitly in diagnostics.

### 7.2 OTLP/gRPC (over nghttp2, no gRPC library)

- **Endpoint path:** `POST /opentelemetry.proto.collector.trace.v1.TraceService/Export`.
- **Default port:** 4317.
- **Required request headers:** `te: trailers`, `content-type: application/grpc+proto`, `:method: POST`, `:scheme: https`, `user-agent: microtel-cpp/<version>` (or `microtel-python/<version>`).
- **Per-message framing:** 5-byte prefix per message (1 byte compression flag + 4-byte big-endian length) followed by protobuf bytes. OTLP unary requests carry exactly one message, but the **parser must not assume a gRPC message corresponds to a single HTTP/2 DATA frame** — it treats DATA as a byte stream, supports message prefixes split across frames and multiple messages in one frame.
- **Compression:** request compression via `grpc-encoding: gzip` (per-message, distinct from HTTP `Content-Encoding`); `grpc-accept-encoding: gzip` for response decompression support.
- **Status:** parsed from the HTTP/2 trailer (second HEADERS frame after END_STREAM). `grpc-status: 0` is success; non-zero is failure regardless of `:status: 200`. The codec also handles **trailer-only responses** — a response may contain final status in the initial HEADERS frame without DATA frames.
- **Missing `grpc-status` (malformed servers / proxies):** if a response lacks `grpc-status`, microtel maps the HTTP `:status` to a transport error for diagnostics. Retry classification follows the HTTP status only for known transient values (429, 502, 503, 504); otherwise the batch fails non-retryably. Common in proxy-heavy environments where intermediaries terminate the stream without trailers.
- **Retry policy:** retry on `CANCELLED (1)`, `DEADLINE_EXCEEDED (4)`, `ABORTED (10)`, `OUT_OF_RANGE (11)`, `UNAVAILABLE (14)`, `DATA_LOSS (15)` per OTLP spec. **`RESOURCE_EXHAUSTED (8)` is retryable only when status details include `RetryInfo` or another configured signal that recovery is possible.** Without that signal, it is treated as non-retryable and the batch is dropped with diagnostics. This requires the codec to parse `google.rpc.Status` and `RetryInfo` — known status detail types are generated and decoded explicitly through upb. Unknown `Any` details are preserved only for diagnostics and are not interpreted.
- **Cancellation:** HTTP/2 `RST_STREAM` with code `CANCEL`.

**Explicitly not implemented** (and not needed for OTLP unary): server-streaming, client-streaming, bidi-streaming RPCs; service config / retry policy JSON; name resolution / xDS / load balancing; health checking service; server reflection; channel-state machinery beyond "open or reconnecting"; interceptors / middleware.

### 7.3 Shared

- **Protobuf schemas:** vendored from `open-telemetry/opentelemetry-proto` at a pinned tag. Code generation uses upb's `protoc` plugin (`protoc-gen-upb`); generated code is committed.
- **HTTP/2 settings:** sensible defaults for `SETTINGS_MAX_CONCURRENT_STREAMS` and `INITIAL_WINDOW_SIZE`; tunable via config.
- **Encoder:** a single OTLP encoder produces protobuf bytes via upb; the HTTP and gRPC codecs differ only in framing and headers. No double-encoding. upb's C API is hidden behind a thin internal `OtlpEncoder` C++ wrapper so the rest of the SDK never sees upb symbols directly.
- **Partial success.** microtel parses every OTLP response. If `partial_success` is present, rejected item counts and error messages are recorded in exporter diagnostics and the request is **not retried** (retry would re-send accepted items too). Drop counters distinguish partial-success rejections from full-failure drops.
- **Timeouts.** Six independent timeouts, each separately configurable: `connect`, `tls_handshake`, `per_export`, `retry_budget` (max elapsed time for retries on a single batch), `flush`, `shutdown`. A timed-out gRPC request is cancelled with `RST_STREAM` where possible; retry depends on the configured policy.
- **User-Agent.** `microtel-cpp/<version>` for C++ and `microtel-python/<version>` for the Python binding.
- **Semantic conventions.** microtel does not ship semantic-convention helper constants in v1. Users set attributes directly using string keys (e.g., `"http.method"`, `"http.route"`). Helper packages may be added later; v1 stays out of the semconv-versioning treadmill.

---

## 8. SDK Features (v1 scope)

| Feature | v1 | Notes |
|---|---|---|
| Traces (spans, events, links, status) | ✅ | first stable signal |
| OTLP/HTTP-protobuf exporter | ✅ | primary path |
| OTLP/gRPC exporter (nghttp2-native, no gRPC lib) | ✅ | core differentiator |
| W3C Trace Context propagation | ✅ | inject + extract |
| W3C Baggage propagation | ❌ | v1.1; Trace Context only in v1 |
| Samplers: AlwaysOn, AlwaysOff, TraceIdRatio, ParentBased | ✅ | composable Chain in v1.1 |
| Resource attributes: explicit config, `OTEL_RESOURCE_ATTRIBUTES`, `OTEL_SERVICE_NAME` | ✅ | basic |
| Resource detectors (process, host, k8s, cloud) | ❌ | v1.1+ |
| Batch span processor (bounded queue) | ✅ | required |
| Simple (non-batched) processor | ✅ | for tests/debug |
| Span limits enforcement | ✅ | concrete defaults in §5.6 |
| `ForceFlush` / `Shutdown` | ✅ | production requirement |
| Partial-success parsing | ✅ | required for correctness |
| Retry / backoff / jitter | ✅ | both wire protocols |
| Static config + OTel env-var fallback | ✅ | no hot reload in v1 |
| Preflight CLI flag | ✅ | high-value, low-cost |
| Internal diagnostic logging | ✅ | spdlog or minimal stderr |
| **Metrics** | ❌ | v1.1 / v1.2, after a metrics design doc |
| **Logs** | ❌ | after metrics |
| **Control plane** (UDS, microtelctl, hot reload) | ❌ | v1.1 / v2 |
| **Sugar layer** (`microtel::sugar`) | ❌ | v1.1 |
| **Compat shims** (otel-cpp / otel-python) | experimental | not v1 load-bearing |
| **Auth providers** beyond static + callback | ❌ | OAuth2 / SigV4 / mTLS-rotation are adapter packages or v1.x |

---

## 9. Build & Dependencies

### 9.1 Dependencies (runtime)

| Library | Version | License | Notes |
|---|---|---|---|
| nghttp2 | ≥ 1.50 | MIT | HTTP/2 (substrate for both wire protocols) |
| OpenSSL | ≥ 1.1.1 | Apache 2.0 | TLS |
| upb | vendored, pinned commit | BSD-3 | wire encoding |
| zlib | ≥ 1.2.11 | zlib | gzip (HTTP and gRPC compression) |
| spdlog | optional, ≥ 1.11, header-only | MIT | internal diagnostic logging (`SPDLOG_USE_STD_FORMAT`, no fmt dep) |

### 9.2 Build options

- `MICROTEL_USE_SPDLOG=ON|OFF` (default ON). When OFF, microtel uses a minimal internal stderr logger and the sink-injection API remains available. Lets embedded/constrained users shave further dependency and compile-time cost.
- `MICROTEL_BUILD_PYTHON=ON|OFF` (default OFF for the C++ build; Python wheel build is a separate target).
- `MICROTEL_BUILD_COMPAT_SHIMS=ON|OFF` (default OFF; ships as separate experimental packages).
- `MICROTEL_FORBID_INSECURE_TLS=ON|OFF` (default OFF). When ON, the build refuses to honor `insecure = true` at runtime regardless of config. Default-OFF builds emit a prominent runtime warning when `insecure = true` is configured.

### 9.3 Build system

- **CMake ≥ 3.20** as the primary build system. Out-of-source builds; presets for Debug / Release / RelWithDebInfo.
- `find_package` for system deps; `FetchContent` fallback opt-in.
- Generated upb code (OTel protos) is committed under `gen/`. **CI verifies that `make regen-protos` from the pinned upb + opentelemetry-proto versions produces a zero-diff result.** Prevents generated-code drift.
- Optional Bazel rules contributed but not first-class.

### 9.4 Internal logging

microtel uses spdlog (header-only, `SPDLOG_USE_STD_FORMAT`) by default for its own diagnostic logging. This is **distinct from OTel logs the SDK exports**: microtel's internal logs are independent of the export pipeline so diagnostic output keeps flowing even when the exporter is the failing component.

Default sink is `stderr`. The microtel.toml configures sink (file, rotating, journald, syslog) and level (`error`/`warn`/`info`/`debug`/`trace`).

A sink-injection hook lets users redirect microtel's internal logs into their own logging system:

```cpp
microtel::SetLogSink([](microtel::LogLevel lvl, std::string_view msg) {
    my_app_logger.Log(static_cast<int>(lvl), msg);
});
```

The sink-injection API is available in both `MICROTEL_USE_SPDLOG=ON` and `=OFF` builds.

### 9.5 Packaging

- **C++:** `.deb`, `.rpm`, plus a CMake-installable tree. pkg-config files included.
- **Python wheels:** built via `cibuildwheel` for manylinux_2_28 (RHEL 9-compatible). **nghttp2 and zlib bundled** where manylinux permits; **OpenSSL is dynamically linked to system OpenSSL** (not bundled) for distro and FIPS compatibility. Hardened environments build from source. `auditwheel` behavior is documented per release.

### 9.6 Vendored dependency policy

```
- store upstream commit SHA and license in third_party/<dep>/README.md
- include generated-source provenance
- update through a dedicated PR with API/ABI diff notes
- run collector interop and fuzz suites before merge
```

Applies to upb and to opentelemetry-proto.

---

## 10. Performance Targets

Measured against `opentelemetry-cpp` with both its OTLP/gRPC and OTLP/HTTP exporters on identical workloads, same hardware, OTel collector pinned to the same version. Workload: traces only, 100k spans/sec sustained, 200-byte average span, 5 attributes per span. Full methodology in `docs/bench-spec.md`.

### 10.1 Hot-path metrics

For trace SDKs, sampler behavior dominates production cost — most spans in production are unsampled and the unsampled path must be near-zero overhead. Benchmarks cover both:

- p50 / p95 / p99 nanoseconds for span creation, **sampled** and **unsampled**
- p50 / p95 / p99 for parent-sampled vs parent-not-sampled paths
- attributes added before the sampling decision vs after
- allocations per span on caller thread (sampled and unsampled)
- bytes allocated per span on caller thread
- caller-thread CPU cycles per span
- queue push contention under 1, 4, 16 application threads

### 10.2 Exporter metrics

- batches/sec sustained
- spans/sec sustained
- encoder CPU
- transport CPU
- queue depth under collector outage
- spans dropped under sustained outage
- reconnect behavior under `GOAWAY` and `RST_STREAM`

### 10.3 Footprint metrics

- stripped shared object size
- package install size
- loaded RSS after init
- RSS under steady export
- transitive dynamic dependencies via `lddtree`

### 10.4 Cold-start metric

Defined precisely: time from SDK initialization start to successful receipt of the first export response from a local collector, with DNS disabled, collector warmed, TLS mode specified, and batch delay forced to zero.

### 10.5 Footprint targets (v1, stretch)

Component-separated to keep claims defensible:

| Component | Target (stretch, pending prototype) |
|---|---|
| `libmicrotel-exporter.so` (stripped) | < 800 KB |
| `libmicrotel-sdk.so` (stripped, full v1 surface) | < 1.5 MB |
| Total transitive dynamic closure | < 3 MB |
| Python extension | measured separately |
| Control-plane component | excluded from core size target (deferred to v1.1) |

Benchmarks report both **dynamic-link** and **mostly-static** configurations. Dependency closure is measured with `lddtree` for dynamic and package artifact size for static. Realistic floors will be set after M0 and M2; the table above is stretch.

| Metric | vs otel-cpp+gRPC | vs otel-cpp+HTTP |
|---|---|---|
| Caller-thread CPU per span | ≥ 30% lower | ≥ 15% lower |
| RSS, steady state | ≥ 50% lower | ≥ 25% lower |
| Wire bytes per span (gzip on) | within ±5% | within ±5% |
| Wire bytes per span (gzip off) | within ±2% | within ±2% |
| Library binary size (stripped, dyn) | ≥ 70% smaller | ≥ 50% smaller |
| Cold-start to first export | ≥ 40% faster | ≥ 20% faster |

---

## 11. Project Structure

```
microtel/
├── CMakeLists.txt
├── LICENSE                       (Apache 2.0)
├── README.md
├── SECURITY.md
├── CODEOWNERS
├── docs/
│   ├── architecture.md
│   ├── migration-from-otel-cpp.md
│   ├── grpc-wire-protocol.md     (gRPC-on-nghttp2 implementation notes)
│   ├── interfaces.md             (M1 contract document)
│   ├── development.md            (track-to-directory atlas)
│   ├── compatibility-matrix.md
│   ├── interop-matrix.md
│   └── icps/                     (Interface Change Proposals, post-M1)
├── third_party/
│   └── upb/                      (vendored at pinned commit; SHA + license in README.md)
├── proto/                        (vendored from opentelemetry-proto, pinned)
├── gen/                          (generated upb C accessors, committed)
├── include/microtel/             (public C++ headers)
├── src/
│   ├── api/                      (Tracer, Span surfaces)
│   ├── sdk/                      (Resource, Samplers, BatchSpanProcessor)
│   ├── exporter/                 (Batching, OTLP encoding — protocol-agnostic)
│   ├── wire/
│   │   ├── encoder/              (OtlpEncoder C++ wrapper; only file that touches upb directly)
│   │   ├── http/                 (OTLP/HTTP-protobuf codec)
│   │   └── grpc/                 (OTLP/gRPC codec on nghttp2: framing, trailers, status, RetryInfo)
│   ├── transport/                (Transport interface; nghttp2 + OpenSSL impl)
│   └── common/                   (logging, config, errors, limits)
├── python/
│   ├── pyproject.toml
│   ├── src/microtel/             (Python package)
│   └── bindings/                 (nanobind C++)
├── tests/
│   ├── unit/                     (gtest)
│   ├── integration/              (collector-in-a-container, both protocols)
│   ├── interop/                  (collector versions × backends matrix)
│   └── grpc-wire/                (validates gRPC framing/trailer parsing)
├── examples/
├── ci/                           (shared scripts, Dockerfiles, sonar config)
└── .github/workflows/            (GitHub Actions CI)
```

Every top-level subdirectory under `src/` carries a one-screen `README.md` describing what lives there, which track owns it, which interfaces it implements, what it depends on (with mock locations), test entry points, and any directory-local style notes. This convention exists for both human contributors and AI coding agents — a freshly-opened agent context can ingest the directory README plus the relevant interface header and have everything it needs to make a non-trivial change without reading the entire codebase.

---

## 12. Configuration

The canonical configuration source is `microtel.toml`. Strict by default — unknown keys raise errors at SDK init. Mixed-version deployments can relax this:

```toml
[config]
unknown_keys = "error"  # error (default) | warn | ignore
```

### 12.1 Precedence

```
1. Explicit code options (highest)
2. Environment variables (OTEL_* and MICROTEL_*)
3. microtel.toml
4. Built-in defaults (lowest)
```

This matches conventional expectations for containerized deployments: environment variables override image-baked config files, allowing operators to tune behavior at deploy time without rebuilding images. OTel-standard `OTEL_*` env vars are honored alongside microtel-specific `MICROTEL_*` ones; the resolved precedence per setting is documented in `docs/configuration.md`.

### 12.2 Endpoint conventions

Canonical:

```toml
[exporter]
endpoint    = "https://collector.internal:4317"
protocol    = "grpc"             # http | grpc
compression = "gzip"             # off | gzip
```

```cpp
auto provider = microtel::SdkBuilder()
    .WithEndpoint("https://collector.internal:4317")
    .WithProtocol(microtel::Protocol::Grpc)
    .Build();
```

Aligns with OTel exporter conventions. `grpc://` and `grpcs://` schemes are accepted as microtel-specific shorthand but the canonical form uses `https://` plus an explicit `protocol` field.

**Path semantics:**

- For OTLP/HTTP using the generic `[exporter].endpoint`: an empty path or `/` causes microtel to append `/v1/traces`. A non-empty path is treated as a base path and `/v1/traces` is appended (so `https://collector:4318/foo` becomes `https://collector:4318/foo/v1/traces`). Per-signal endpoints — when introduced — are used exactly as provided with no append.
- For OTLP/gRPC: the RPC path is fixed to `/opentelemetry.proto.collector.trace.v1.TraceService/Export`. **User-provided URL paths in gRPC endpoints are rejected at config-load time in v1** to avoid ambiguous routing behavior. A future `grpc_path_prefix` option may be added if real deployments require it.

### 12.3 TLS

- System trust store by default.
- `ca_bundle = "/path/to/ca.pem"` for an explicit CA.
- `insecure = true` is permitted but emits a prominent runtime warning. Deployments that want a hard ban can compile with `MICROTEL_FORBID_INSECURE_TLS=ON`, in which case `insecure = true` causes initialization to fail.
- mTLS via `client_cert` + `client_key` (paths).
- SNI is taken from the endpoint host; explicit `sni_override` available.
- ALPN selects `h2` for HTTP/2.
- FIPS users link microtel against a FIPS-validated OpenSSL provider; behavior is determined by the linked OpenSSL.

### 12.4 Proxy

- `https_proxy` honored from environment.
- `no_proxy` honored.
- HTTP `CONNECT` for proxied HTTPS endpoints.
- TLS interception by intermediate proxies is the operator's problem to resolve via custom `ca_bundle`.

### 12.5 Auth

v1 supports two auth surfaces:

- **Static headers:** `[exporter.headers]` table in TOML or `WithHeaders({...})` in code.
- **Callback:** `WithAuthProvider(callable)` returning the current `Authorization` header value, called per-export-batch with results cached for a configurable TTL.

Built-in OAuth2 client credentials, AWS SigV4, and mTLS rotation providers are deferred to adapter packages or v1.x.

### 12.6 Secret redaction

Resolved config views (e.g., the `--print-config` CLI flag) redact `Authorization` headers, client secrets, private-key paths if configured as sensitive, and token-provider outputs by default. Showing secrets requires an explicit `--show-secrets` flag and is disabled in production builds unless explicitly enabled.

### 12.7 Resource attributes

```toml
[service]
name    = "my-service"
version = "1.2.3"

[resource]
"deployment.environment" = "prod"
"host.id"                = "auto"   # detect at startup
```

Conflicts between code-set, file-set, env-set, and detector-set Resource follow OTel precedence: detectors run first, then environment, then user-supplied (file or code). The resolved Resource is logged at init.

### 12.8 What's not in v1

- Hot reload of any setting.
- Multi-profile within one process.
- Composable sampler chains (single sampler only in v1).
- Long-running control-plane socket.

These are v1.1+ (§17).

---

## 13. Roadmap

The roadmap is structured for incremental delivery and parallel execution. Three principles drive the structure:

1. **Architecture-first.** M0 produces `docs/architecture.md`, `docs/interfaces.md`, sequence diagrams, and stub interface headers — no source code. The spike (M1) validates those decisions with throwaway code. Only after M2 (Skeleton & contracts) locks the interfaces in code with mocks does parallel implementation work begin. See §14.1.
2. **TDD from M3 onward.** Every implementation milestone follows test-first per §14.2.
3. **One signal at a time.** Each signal lands as its own milestone sequence, with an explicit design-doc milestone and reviewer sign-off before implementation. Traces first (M3–M7), then metrics (M11 design → M12 implementation → M13 views), then logs (M14). **All three have shipped.** The *v1.0 release* (M10) is still scoped to traces; do not read "v1 is traces only" as a statement about what is implemented — that phrasing described the release cut, and the implementation has run ahead of it.

The "Depends on" column gates start; "Parallel with" lists milestones whose file ownership and contract surfaces are disjoint and can therefore run concurrently.

This section covers v1.0 in detail. The path from v1.0 through full OpenTelemetry SDK conformance — release themes, sugar layer evolution, compatibility-tier progression, decision log — is in the companion document `microtel-roadmap.md`.

| Milestone | Scope | Effort | Depends on | Parallel with |
|---|---|---|---|---|
| **M0 – Architecture & Design** | Produce `docs/architecture.md`, `docs/threading-model.md`, `docs/memory-model.md`, `docs/error-model.md`, `docs/interfaces.md`, `docs/coding-standards.md`, sequence diagrams under `docs/sequences/`, and stub interface headers in `include/microtel/internal/` (compile, no implementations). **No source code beyond compilable interface stubs.** Reviewer sign-off on architecture as a whole and per-interface (§14.1). | 3–4 wk | — | — |
| **M1 – Spike** | nghttp2 client posting hand-crafted protobuf to a real collector via OTLP/HTTP, validating M0's architecture decisions with throwaway code. No SDK. Proves the architecture is buildable. Findings flow back into M0 docs as ICPs. | 1–2 wk | M0 | — |
| **M2 – Skeleton & contracts** | Lock all internal interfaces and their mocks (`Transport`, `OtlpEncoder`, `Exporter`, `Sampler`, `Processor`) per `docs/interfaces.md`. CMake layout, CI scaffolding (build, sanitizers, clang-tidy, coverage, SonarQube), gtest skeletons, spdlog wiring, public header tree. After M2 closes, parallel implementation tracks unblock. | 3–4 wk | M1 | — |
| **M3 – OTLP encoder + OTLP/HTTP traces E2E** | upb encoder, OTLP/HTTP transport, BatchSpanProcessor, Tracer/Span minimal API, AlwaysOn sampler, basic Resource. First end-to-end trace export to a real collector. **TDD from this milestone forward**: tests-first per §14.2. | 4 wk | M2 | M4 |
| **M4 – OTLP/gRPC traces over nghttp2** | gRPC framing on nghttp2 (5-byte length prefix, `te: trailers`, `:path` construction), trailer parsing including trailer-only responses, multi-frame stream parsing, status code mapping with `RetryInfo` decoding for `RESOURCE_EXHAUSTED`. Validated against real grpc-server. **Acceptance test:** a fake gRPC OTLP receiver returns `RESOURCE_EXHAUSTED` with and without `google.rpc.RetryInfo`; microtel retries only the `RetryInfo` case and treats the other as non-retryable. Runtime-selectable protocol per endpoint. | 3 wk | M2 | M3, M5 |
| **M5 – Production correctness** | Retry / backoff / jitter (HTTP and gRPC), explicit timeout taxonomy (connect / TLS / per-export / retry-budget / flush / shutdown), partial-success parsing, `GOAWAY` and `RST_STREAM` handling, ForceFlush / Shutdown, fork-safety, drop accounting. | 3 wk | M3 | M4, M6 |
| **M6 – Config & deployability** | Strict `microtel.toml` validation, OTel env-var fallback, endpoint URL parsing, TLS / proxy / auth (static + callback), preflight CLI flag, secret redaction. | 3 wk | M2 | M3, M4, M5 |
| **M7 – Performance harness & footprint proof** | Benchmark harness in `bench/`, opt-in via `cmake -DMICROTEL_BUILD_BENCH=ON`, measuring all hot-path / exporter / footprint metrics defined in §10. Validates the size and CPU claims that justify the project's existence. | 3 wk | M5 | — |
| **M9 – Hardening** | Fuzzing (gRPC framing/trailer paths, TOML parser, response-size limits), soak tests, perf gates in CI, collector interop matrix CI (Collector / Jaeger / Tempo where supported), SonarQube clean run. | 4 wk | M7 | — |
| **M10 – v1.0 traces release** | Apache 2.0 OSS release, `docs/migration-from-otel-cpp.md` finalized, experimental compat shims released as separate packages, conf-talk submission. | — | M9 | — |
| **M11 – Metrics design doc** | `docs/metrics-design.md`: aggregation temporality (delta vs cumulative), cardinality limits, histogram bucket configuration, async-instrument callbacks, reader/exporter interaction, views, exemplars roadmap. **Reviewer sign-off.** No implementation in this milestone. | 2 wk | M10 | — |
| **M12 – Metrics implementation** | Instruments, aggregations, MetricReader, async observables, Resource detectors expanded. | 6–8 wk | M11 | — |
| **M13 – Views & attribute filtering** | OTel Views: per-instrument stream selection via `ViewRegistry` selector matching, and per-view `attribute_allowlist` filtering for synchronous and observable instruments. | 2 wk | M12 | — |
| **M14 – Logs** | OTel logs API, OTLP/logs export, bridge to spdlog as adapter. | 3 wk | M12 | — |
| **M15 – Control plane + hot reload** | Unix-socket server, JSON wire, `microtelctl` Go binary, REPL, hot-reloadable settings, multi-profile. | 4 wk | M10 | M12, M13, M14 |
| **M16 – Sugar layer** | `microtel::sugar` C++ helpers and Python decorator/context-manager equivalents. | 3 wk | M10 | — |
| **M17 – opentelemetry-cpp API-adapter shim** | `microtel_otelcpp_shim`: implements the `opentelemetry-cpp` API over microtel's SDK for traces, metrics, and logs. **Source-only distribution** — no prebuilt binaries, for any configuration. Opt-in via `MICROTEL_BUILD_OTELCPP_SHIM=ON`. `docs/migration-from-otel-cpp.md` written against the working shim. Experimental tier. | 4 wk | M14 | M15, M16 |
| **M18 – Python bindings (all signals)** | nanobind layer over traces, metrics, and logs; `logging.Handler` bridge; wheels via `cibuildwheel` for manylinux_2_28; PyPI publish; examples. `microtel.__capabilities__` reports `{"traces": True, "metrics": True, "logs": True}`. | 4 wk | M10; thread-local current-span slot | M15, M16, M17 |

> **Numbering note.** M13 (Views & attribute filtering) was split out from M12 during implementation and tracked under its own milestone number; Logs, control plane, and sugar renumbered to M14/M15/M16 to keep the spec aligned with the as-built commit labels. See [ICP 0010](docs/icps/0010-milestone-renumber-views.md).
>
> **M8 is a retired number.** It formerly held "Python bindings (traces only)". That scope was overtaken by M12/M13/M14 — all three signals ship — and the slot never corresponded to any implementation (history runs M7 → M9). Python bindings are now **M18**; M8 is not reused, so every as-built commit label M9–M16 keeps its meaning. See [ICP 0013](docs/icps/0013-rescope-defer-python-bindings.md). The opentelemetry-cpp shim takes **M17** per [ICP 0014](docs/icps/0014-otelcpp-shim-and-rule-13.md), ahead of Python, because it has no SDK prerequisites while Python requires the thread-local current-span slot.

### 13.1 Parallel work tracks after M2

Once M2 (Skeleton & contracts) lands, the following tracks have disjoint file ownership and can each be assigned to a separate agent or developer:

- **Track A — Trace SDK** (`src/sdk/`, `src/api/`)
- **Track B — OTLP/HTTP wire** (`src/wire/http/`)
- **Track C — OTLP/gRPC wire** (`src/wire/grpc/`)
- **Track D — Transport** (`src/transport/`)
- **Track E — Config** (`src/common/config/`)
- **Track F — Encoder** (`src/wire/encoder/`)

Tracks D and F are foundational and finish first. Then A/B/C/E proceed concurrently. Per-track gtest suites run against mocks of every cross-track dependency.

### 13.2 Interface design review (M0 sign-off)

M0 produces `docs/interfaces.md` documenting every locked interface. Per interface: **purpose**, **contract** (preconditions, postconditions, invariants), **lifetime**, **threading model**, **error model**, **allocation behavior**, **mock availability**.

Each interface needs **reviewer sign-off** before M1 (the spike) begins. The reviewer reads each interface from the perspective of all downstream tracks that consume it, surfacing consumer/producer misalignment before code is written. (In a larger team this would be a two-reviewer process with consumer-track owners; for a small team or solo project, the single reviewer wears all the hats.)

After M0 closes, breaking changes to any locked interface require an **Interface Change Proposal** PR'd into `docs/icps/`, identifying affected tracks and the migration path. Lightweight, but visible.

### 13.3 File ownership

Three layered mechanisms keep parallel tracks from colliding:

1. **CODEOWNERS** as authoritative source of truth.
2. **Per-directory `README.md`** as the local map for any contributor or agent.
3. **`docs/development.md`** as the human-readable track-to-directory atlas.

A CI check that fails PRs touching files outside the author's claimed track is deferred until collision incidents prove the lighter tools insufficient.

### 13.4 Python binding cadence

Python wheels ship **post-v1.0**, as M18, covering all three signals — traces, metrics, and logs — because all three are live public C++ surfaces. `microtel.__capabilities__` exposes the binding scope at runtime; any surface not yet bound raises `NotImplementedError` with the version target. No orphan C++ surfaces — if a public C++ method exists for more than one release without a Python equivalent, that's a bug, not a deferred feature. That rule is why a traces-only wheel is no longer an acceptable first release: it would ship the bug the rule forbids. See [ICP 0013](docs/icps/0013-rescope-defer-python-bindings.md).

### 13.5 v1.0 Release Gates

microtel v1.0 cannot ship until **all** of the following are true:

- M0 architecture documents exist, are signed off by the reviewer per interface, and reflect any changes from spike findings (M1 ICPs).
- OTLP/HTTP trace export passes integration tests against the pinned OpenTelemetry Collector matrix.
- OTLP/gRPC trace export passes integration tests against the pinned OpenTelemetry Collector matrix.
- Partial-success responses are parsed, recorded in diagnostics, and never retried.
- Retryable and non-retryable HTTP/gRPC failures match the documented policy matrix, including `RESOURCE_EXHAUSTED` with and without `RetryInfo`.
- gRPC trailer-only responses, split DATA-frame messages, multi-frame messages, `GOAWAY`, and `RST_STREAM` are covered by tests.
- `ForceFlush()` and `Shutdown()` pass timeout, idempotency, and destructor-safety tests.
- Queue count limits, byte limits, response limits, trailer limits, and decompression limits are enforced with drop / failure accounting.
- TLS system trust, custom CA, mTLS, static headers, auth callback, and proxy behavior either pass integration tests or are explicitly marked unsupported in the compatibility matrix.
- Initial benchmark results against `opentelemetry-cpp` are published, even if the stretch targets are not yet met.
- **Test coverage thresholds met** per §14.2: ≥ 90% line / ≥ 85% branch on SDK and encoder code; ≥ 80% on transport and exporter paths.
- **AddressSanitizer, ThreadSanitizer, UndefinedBehaviorSanitizer clean** on the full test suite.
- **No SonarQube critical or blocker issues** open (per §14.4).
- **No quarantined tests** open for more than two weeks.
- `SECURITY.md`, `CODEOWNERS`, `compatibility-matrix.md`, `interop-matrix.md`, and third-party notices exist and are current.
- `make regen-protos` produces a zero-diff result against the pinned upb + opentelemetry-proto versions.

Python bindings are **not part of the v1.0 target set** and are not a gate. They ship post-v1.0 as **M18**, together with the PyPI publish. See [ICP 0013](docs/icps/0013-rescope-defer-python-bindings.md).

---

## 14. Engineering Practices

microtel is intended to be developed by a mix of human contributors and AI coding agents (Claude Code, Copilot Agent, etc.). The practices below scale across that mix because explicit contracts beat implicit norms — agents and humans both work better against documented expectations than inferred ones.

### 14.1 Architecture-First Development

**No source code is written for v1 until the architecture is signed off.** M0 (the first milestone) is dedicated to design — no implementation, no spike code, only documents and interface headers. M1 (the spike) validates those documents with throwaway code; M2 builds the project skeleton; M3 onwards is implementation work driven by TDD against the locked interfaces.

M0 deliverables:

- `docs/architecture.md` — system overview, component responsibilities, data flow.
- `docs/threading-model.md` — explicit per-component threading rules: which methods are thread-safe, which are thread-affine to a specific thread (caller / exporter worker / I/O), where synchronization happens, lock-ordering rules.
- `docs/memory-model.md` — ownership rules per resource type (heap memory, file descriptors, sockets, OpenSSL contexts, nghttp2 sessions, upb arenas, threads). Who allocates, who frees, lifetime semantics, RAII wrappers.
- `docs/error-model.md` — exception policy (per §6.1), `std::expected<>` vs error codes, error propagation across thread boundaries, drop-vs-fail decisions.
- `docs/interfaces.md` — every internal interface fully specified per the §13.2 design-review process: purpose, contract, lifetime, threading, error model, allocation behavior, mock availability.
- `docs/configuration.md` — per-setting precedence table (this section §12.1) and resolution rules. New settings appended as they land in M3+.
- `docs/development.md` — track-to-directory atlas (§13.1, §13.3); answers "which track owns this code?" for both contributors and AI agents.
- `include/microtel/*.hpp` — public API headers with full Doxygen and method signatures, no method bodies.
- `include/microtel/internal/*.hpp` — internal interface headers with full Doxygen comments and pure-virtual / abstract declarations.
- Both header trees compile under an `INTERFACE` CMake target with `-Werror`. No implementations.
- Sequence diagrams in `docs/sequences/` for the trickiest flows: connection establishment, retry-after-failure, GOAWAY handling, shutdown drain, fork survival, backpressure-and-drop, partial-success handling, gRPC trailer-only and multi-frame parsing.

**Reviewer sign-off** on the architecture as a whole, plus per-interface sign-off as in §13.2. The reviewer reads each interface from the perspective of every downstream track that will consume it. After sign-off, breaking changes go through the ICP process.

M0 ends when an agent or developer can pick up M1 (the spike) without architecture questions. Architecture questions surfacing during M1 or later are escalated as ICPs.

### 14.2 Test-Driven Development

Implementation milestones (M3 onwards) follow a strict TDD cadence:

1. **Write a failing test** for the behavior the change will introduce.
2. **Write minimal code** to make the test pass.
3. **Refactor** without changing observed behavior; tests remain green.

**Enforced by tooling**, not just review. Three CI gates make TDD compliance mechanical:

- **Diff coverage gate.** Every line of source code added or modified in a PR must be covered by a test in the same PR. Threshold: 90% diff line coverage on SDK and encoder paths, 80% on transport and exporter paths. Implemented via `diff-cover` (or equivalent) running against the lcov report from the existing coverage build.
- **Test-presence gate.** A custom CI check fails any PR that modifies `src/**/*.{cpp,hpp}` without a corresponding modification in `tests/**/*.{cpp,hpp}`. Documented exceptions: pure refactors with the `[refactor]` PR label (review still required), formatting/comment-only changes, deleting code with its tests removed.
- **Mutation testing as a quality signal.** Periodic CI job runs mutation testing (e.g., `mull` for C++) over recently-changed code; mutation-survival above a configurable threshold opens an issue for follow-up. Not a blocking gate (mutation testing is slow and noisy), but visible.

These gates run on every PR including AI-coding-agent PRs — agents and humans face the same mechanical bar. Reviewers focus on whether tests are *good* (meaningful, well-targeted, not over-specified); the tooling handles whether tests *exist*.

**Required test types:**

| Type | Tool | Scope | Required for |
|---|---|---|---|
| Unit | gtest | One file/class, mocked dependencies, < 1 ms | every public function and every non-trivial private one |
| Integration | gtest + collector container | Multiple components wired together | every cross-component flow |
| Conformance | OTel test harness adapter | Against a real OpenTelemetry Collector | wire-protocol compliance |
| Wire | bespoke gtest | Protocol byte-level validation | OTLP/HTTP and OTLP/gRPC codecs |
| Fuzz | libFuzzer | Adversarial inputs | TOML parser, gRPC trailer parser, response decompression, OTLP response parser |

**Aggregate coverage targets** (independent of diff coverage):
- Unit: ≥ 90% line, ≥ 85% branch on SDK and encoder code.
- Integration: ≥ 80% on transport and exporter paths.
- Lower targets on I/O-heavy code (network failure paths) require explicit justification in the PR description.

**Mocking discipline.** Every internal interface has a mock under `tests/mocks/`. Components consume mocks of their dependencies; integration tests wire real implementations together. Mocks are dumb — they return what they're configured to return, no logic. "Smart mocks" that recompute behavior are a code smell; use a fake instead.

**Test layout:** `tests/<unit|integration|conformance|wire|fuzz>/<component>/`.

**Build option:** `MICROTEL_BUILD_TESTS=ON|OFF` (default ON). When OFF, the entire test tree is skipped — useful for cross-compilation, constrained-environment builds, and embedded leaf builds in v2.0+.

**CI gates** (all must pass on every PR):
- Diff coverage threshold met (above).
- Test-presence check passes (above).
- All test categories pass on Linux x86_64 and Linux ARM64.
- Aggregate coverage thresholds met per category.
- AddressSanitizer, ThreadSanitizer, and UndefinedBehaviorSanitizer clean.
- No flaky tests — flaky tests get a `quarantine` label and stop blocking CI but remain visible until fixed (an open `quarantine`-labelled test for more than two weeks blocks v1.0 release).

### 14.3 RAII Discipline

Every resource — heap memory, file descriptors, sockets, mutexes, threads, OpenSSL contexts, nghttp2 sessions, upb arenas, callback registrations, child processes — is owned by an RAII type whose destructor releases it. **No exceptions in production code.** Tests may use raw resources but only in code clearly named with a `_test_only` suffix and with a comment explaining why.

**Pointer rules:**
- `std::unique_ptr<T>` for unique ownership of heap objects.
- `std::shared_ptr<T>` only when shared ownership is structurally required; justify in code review.
- `std::weak_ptr<T>` for breaking cycles.
- Raw pointers are **non-owning by definition**. A function taking `T*` does not delete; a function returning `T*` returns a borrowed reference whose lifetime is documented in Doxygen.
- No raw `new` / `delete` in production code (linter-enforced). Use `std::make_unique` and `std::make_shared`.
- No homegrown smart pointers; no `std::auto_ptr`; no `boost::scoped_ptr`.

**Custom RAII wrappers for C resources** live in `src/common/raii/`:

- `Socket` — owns a file descriptor, closes on destruction.
- `SslCtx`, `SslSession` — own OpenSSL context and session pointers.
- `Nghttp2Session` — owns the nghttp2 session.
- `UpbArena` — owns an upb arena.

Each wrapper is move-only, has a `release()` method for explicit ownership transfer across API boundaries, and a `noexcept` destructor.

**Move semantics:**
- Resource-owning types are move-only by default. Copy semantics added only with explicit justification.
- Rule of zero where possible (`= default` everything). Rule of five when custom resource handling is required — all five members specified explicitly, no compiler-generated mix.

A repository-wide RAII pattern enforcement runs in CI via clang-tidy with custom checks plus a project-specific scanner. Violations block merge.

### 14.4 Static Analysis (SonarQube-Aligned)

CI runs static analysis on every PR. The configuration aligns with SonarQube's C/C++ rule set so violations would also fail in environments running SonarQube against the codebase — relevant for users embedding microtel into projects where SonarQube is already a release gate.

**Tooling:**
- **clang-tidy** with `.clang-tidy` curated to align with SonarQube's C++ rule set.
- **clang-format** with `.clang-format` for layout. Auto-applied; PRs with formatting drift fail CI.
- **Sanitizers** (asan, tsan, ubsan) in dedicated CI build configurations.
- **cppcheck** as a secondary check.
- **SonarQube Cloud** on the project's OSS tier (free for public/open-source repositories, no LOC cap, full feature set including the C++ analyzer and PR decoration). Runs on every PR; surfaces inline findings in PR comments. v1.0 cannot ship with critical or blocker issues open.

**Enforced rules** (non-exhaustive; full set in `docs/coding-standards.md`, produced in M0):

- No `goto`.
- No commented-out code in committed source.
- **Cognitive complexity ≤ 15** per function.
- **Cyclomatic complexity ≤ 10** per function.
- No magic numbers — named constants required (`constexpr` preferred over `#define`).
- No naked `catch(...)` blocks.
- No unsafe C functions: `strcpy`, `strcat`, `sprintf`, `gets`, unbounded `scanf`. Use `snprintf`, `strncpy_s`-equivalents, or C++ alternatives.
- All non-static class members initialized at declaration or in every constructor (no UB-prone uninitialized state).
- Const correctness — methods, parameters, local variables marked `const` where applicable.
- Maximum 3 levels of indentation per function (refactor required at 4+).
- No nested ternaries.
- Maximum 7 parameters per function (use a struct beyond that).
- No empty `if`/`else`/`catch` blocks.
- All non-trivial public APIs documented in Doxygen.
- No unnamed namespaces in header files.
- Header include order: own header first, then project headers, then system headers (LLVM convention).
- No `using namespace` at file scope in headers.

### 14.5 Code Review

- **Reviewer sign-off** for non-trivial changes (anything beyond pure docs, formatting, or trivial test additions).
- Reviewers focus on: API design, RAII compliance, test coverage, complexity, observable behavior, security implications.
- AI-coding-agent PRs follow the same review discipline. Authors are responsible for the quality of the code they submit, regardless of who wrote it.
- Interface Change Proposals (ICPs) follow the lighter-weight process from §13.2 — markdown PR'd into `docs/icps/` identifying affected tracks and migration path.

---

## 15. Compatibility & Interop Matrix

### 15.1 OTel API/SDK compatibility (Tier 3)

| Area | v1 status | Test method |
|---|---|---|
| OTLP/HTTP traces | Supported | collector integration test |
| OTLP/gRPC traces | Supported | collector integration test |
| OTLP/HTTP metrics | Planned | conformance TBD |
| OTLP/gRPC metrics | Planned | conformance TBD |
| OTLP/HTTP logs | Planned | conformance TBD |
| OTel SDK env vars (subset) | Partial | config test matrix |
| `opentelemetry-cpp` API shim | Experimental | sample-app compile tests |
| `opentelemetry-python` API shim | Experimental | sample-app runtime tests |
| Collector versions | Pinned matrix | CI containers |

### 15.2 Collector / backend interop

| Backend | Signal | Protocol | v1 CI? |
|---|---|---|---|
| OpenTelemetry Collector | traces | HTTP / gRPC | yes |
| Jaeger via OTLP | traces | HTTP / gRPC | optional |
| Tempo via OTLP | traces | HTTP / gRPC | optional |
| Mimir via OTLP | metrics | later | no (v1.1+) |
| Loki via OTel receiver | logs | later | no (v1.2+) |

The compatibility matrix is the source of truth — claims of "drop-in" beyond what's listed are not made.

---

## 16. Risks

- **OTel API surface drift.** OpenTelemetry continues to evolve. Mitigation: pin to stable spec versions, update on a quarterly cadence, declare which OTel spec version each release implements.
- **gRPC wire-protocol edge cases.** Trailer-only responses, mid-stream `RST_STREAM`, `GOAWAY` mid-batch, partial HEADERS frames, multi-frame messages. Mitigation: dedicated `tests/grpc-wire/` suite using deliberately misbehaving servers; fuzz the trailer parser and framing layer.
- **upb has no stable API/ABI.** Per upstream, upb is not offered as a standalone consumable library and there are no formal releases. Mitigation: vendor at a pinned commit under `third_party/upb/`, treat it as our own code, bump deliberately on a quarterly cadence with full test pass before promoting. The encoder is wrapped behind a small internal `OtlpEncoder` interface so swapping is a single-component change.
- **upb generates C, not C++.** Contributors landing in the encoder will see C accessors. Mitigation: the upb-touching surface is intentionally tiny (one wrapper class in `src/wire/encoder/`); the rest of the codebase sees only C++ idioms.
- **Footprint targets too aggressive.** The `< 800 KB` exporter and `< 3 MB` closure targets are stretch pending prototype measurement. Mitigation: explicitly labeled as stretch in §10.5; component-separated; realistic floors set after M2 and M6.
- **nghttp2 API churn.** Low risk historically. Pin minimum version; isolate behind the `Transport` abstraction.
- **Adoption.** "Yet another OTel client" needs a clear pitch. Mitigation: lead with binary-size, footprint, and CPU numbers from M6's reproducible benchmarks; target embedded / edge / CNF audiences first.
- **Compat-shim scope creep.** Even labeled experimental, users may rely on shims and report bugs that pull v1 toward broader API conformance. Mitigation: §6.3 makes the experimental status explicit; the compatibility matrix (§14) records exactly what's covered; shims ship as separate packages with their own version cadence.

---

## 17. Open Questions

1. **HTTP/3 in v1?** The `Transport` abstraction is in regardless. Defer the nghttp3 implementation to v2.
2. **Sampler composability in v1?** Currently a single sampler at a time; composable Chain is v1.1. Confirm before M5 closes.
3. **Funding / maintenance model.** Pure spare-time OSS, or a backed effort? Affects roadmap aggressiveness.
4. **Benchmark spec freshness.** `docs/bench-spec.md` is at v0.1 and predates spec v0.13. Needs an update pass before M7 (Performance harness) lands — should incorporate the actual baseline numbers from the prototype and realistic footprint floors set after M0 and M2.

---

## 18. Future Direction

### 18.1 v1.1 — Operational and ergonomic expansion

- **Sugar layer** (`microtel::sugar`): function tracing via `std::source_location`, scoped spans, traced lambdas, exception recording, scoped timers, pre-bound attribute keys; Python decorator/context-manager equivalents. Sugar APIs are explicitly non-goals for compatibility testing — conformance tests target the OTel-like API and wire output, not convenience wrappers.
- **Control plane:** Unix-socket server with length-prefixed JSON wire, `microtelctl` Go binary (REPL + single-shot), hot-reloadable settings (sampler, batch sizes, log level), config reload via SIGHUP. Scope and threat model finalized as part of v1.1 design.
- **Multi-profile within one process.**
- **Composable sampler chains.**

### 18.2 v1.2 — Metrics

Preceded by **M11 metrics design doc** (`docs/metrics-design.md`) covering aggregation temporality, cardinality limits, histograms, async instruments, reader/exporter interaction, views, exemplars. Reviewer sign-off before implementation.

### 18.3 v1.3 — Logs

After metrics. Includes a bridge from spdlog (and likely glog, log4cxx) to OTel logs as adapter packages.

### 18.4 v2.0 — Leaf / concentrator architecture

A common pattern in embedded deployments is fleets of constrained devices (modems, controllers, line cards, MCU-class peripherals) that cannot run microtel as designed but still need to emit telemetry. v2.0 introduces a two-component model.

**microtel-leaf:** A pure-**C** library targeted at constrained embedded systems. C, not C++, because leaf-class targets often have no C++ runtime, no exceptions, no RTTI, and many RTOSes have C-only build paths. The leaf encodes OTLP messages and hands the bytes to an application-supplied transport (UART, CAN, BLE, proprietary radio, custom UDP — bake nothing in). No threading, no batching, no retries, no TLS, no HTTP.

**microtel (concentrator role):** Existing microtel gains a leaf-receiver path that ingests leaf payloads, enriches them with Resource attributes from its config, runs the standard batching / sampling / export pipeline, and ships to the upstream collector via OTLP/HTTP or OTLP/gRPC.

**Encoder strategy:** v2.0 ships with **upb on the leaf** (covers larger embedded targets — Linux-on-ARM, OpenWrt-class, Cortex-A, beefier Cortex-R). v2.1+ adds a **nanopb** backend for true MCU-class targets (Cortex-M, no MMU, no libc allocator, sub-256 KB flash). Same leaf API, encoder swapped at build time. The leaf's public API is encoder-agnostic by design — opaque buffer types, builder functions, no `upb_*` or `pb_*` symbols leaking out. The wire format is the contract.

**Time handling:** three modes (concentrator-stamped, sync-relative, boot-relative), configurable per leaf.

**v1 commitments to keep this option open** are minimal seams only — no public APIs are guaranteed stable until v2 design is accepted:

1. Late Resource enrichment supported in the SDK pipeline (not only at SDK init).
2. The encoder design does not assume all telemetry originates from in-process SDK objects. v2 may optimize pre-encoded OTLP ingestion, but **v1 only guarantees that external telemetry can enter through the same processing pipeline after decoding** — concatenating pre-encoded OTLP bytes is not guaranteed semantically valid in general and isn't promised.
3. A `Receiver` interface sits alongside the existing exporter abstraction; v1 ships only the implicit API-instrumentation receiver.

**Out of scope for v2.0:** RTOS ports, reliable delivery on the leaf-to-concentrator link, time synchronization beyond the three documented modes, leaf-side sampling.

---

## 19. Governance

- **License:** Apache 2.0.
- **Contribution model:** DCO or CLA, decided before first public release.
- **Security policy:** `SECURITY.md` with a private disclosure email and supported-versions list; coordinated disclosure preferred.
- **Release cadence:** monthly pre-1.0; quarterly stable after 1.0.
- **Compatibility policy:** semantic versioning; the public C++ API is stable within a major version. Wire compatibility is tracked against a pinned OTel spec version, with changes called out per release.
- **ABI policy:** No stable C++ ABI guarantee before 1.0. After 1.0, public headers follow semver source compatibility. Binary ABI compatibility is best-effort within a minor release, **not** guaranteed across minor releases unless explicitly stated. Users requiring strict binary compatibility should pin to a specific minor version.
- **Maintainer model:** CODEOWNERS required for core transport, encoder, SDK, Python, and packaging directories.
- **Threat model** (initial): enumerated for the v1.1 control plane. v1 surfaces (config file parser, response decompression, gRPC framing, TOML parser) are fuzzed in M9 (Hardening).
- **License scanning:** CI runs license scanning over vendored and generated code (upb, opentelemetry-proto). Release artifacts include third-party notices auto-generated from `third_party/*/README.md` license entries.
- **CI quality gates** (per §14): test coverage thresholds, sanitizer-clean builds, clang-tidy with SonarQube-aligned rules, SonarQube Cloud OSS-tier scan with no critical/blocker issues, no flaky tests in queue beyond two weeks, RAII pattern enforcement, generated-code zero-diff verification.

---

## 20. References

- [OpenTelemetry Protocol (OTLP) Specification](https://opentelemetry.io/docs/specs/otlp/)
- [OpenTelemetry Protocol Exporter Specification](https://opentelemetry.io/docs/specs/otel/protocol/exporter/)
- [OpenTelemetry SDK Environment Variable Specification](https://opentelemetry.io/docs/specs/otel/configuration/sdk-environment-variables/)
- [opentelemetry-proto](https://github.com/open-telemetry/opentelemetry-proto)
- [opentelemetry-cpp](https://github.com/open-telemetry/opentelemetry-cpp)
- [gRPC over HTTP/2 wire protocol](https://github.com/grpc/grpc/blob/master/doc/PROTOCOL-HTTP2.md)
- [nghttp2](https://nghttp2.org/)
- [upb (now part of protocolbuffers/protobuf)](https://github.com/protocolbuffers/protobuf/tree/main/upb)
- [nanobind](https://github.com/wjakob/nanobind)
