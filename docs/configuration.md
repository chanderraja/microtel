# microtel Configuration

**Status:** M0 deliverable. Documents precedence rules and per-setting resolution.
**Companion:** `microtel-spec.md` §12 (canonical for setting names and semantics), `error-model.md` §8 (init-failure taxonomy).
**Maintenance:** the precedence rules in §1 are stable; the per-setting tables in §3 are appended to as new settings land in M3+. New settings without a row here are a documentation bug.
**Last verified against source:** the v1.0 release cut — every setter name below checked against [`include/microtel/sdk_builder.hpp`](../include/microtel/sdk_builder.hpp), every TOML key against [`src/common/config/toml_loader.cpp`](../src/common/config/toml_loader.cpp), and every environment variable against [`src/common/config/env_resolver.cpp`](../src/common/config/env_resolver.cpp) (issues #194, #196). Sections that describe an *intended* surface rather than a built one now say so explicitly.

---

## 1. Precedence

Resolved precedence, **highest to lowest** (LOCKED — spec §12.1):

```
1. Explicit code options (e.g., SdkBuilder::WithEndpoint)
2. Environment variables (OTEL_* and MICROTEL_*)
3. microtel.toml entries
4. Built-in defaults
```

**Resolution is per-setting**, not per-source. If `service.name` is set in code and `service.version` is set in env, both win in their respective slots; neither shadows the other.

**OTel-standard `OTEL_*` env vars are honoured alongside microtel-specific `MICROTEL_*` ones.** Where both an OTEL and a MICROTEL env var name the same setting, MICROTEL wins (it is more specific to this implementation). v1 does not currently define any such overlap; if one is added later it must be called out here.

**Strict-by-default unknown keys.** Unknown keys in `microtel.toml` raise `ConfigError::Kind::UnknownKey` at `Build()` time. Mixed-version deployments may relax via:

```toml
[config]
unknown_keys = "error"   # error (default) | warn | ignore
```

---

## 2. Sources

### 2.1 Code

Method calls on `SdkBuilder`. Highest precedence. Each `WithXxx` call records the setting; `Build()` resolves precedence and validates.

### 2.2 Environment variables

Read at `Build()` time. Both `OTEL_*` and `MICROTEL_*` namespaces. Per-signal env vars that v1 does not implement (e.g., `OTEL_EXPORTER_OTLP_METRICS_ENDPOINT`) are **ignored, not rejected** — they belong to a future signal and should not break trace-only deployments.

### 2.3 `microtel.toml`

**v1 loads a config file only when you name one.** The single path is the one
passed to `SdkBuilder::FromFile(path)`, read at `Build()` time. If it does not
exist, `Build()` fails with `ConfigError::Kind::FileNotFound` — a file you asked
for and did not get is an error, not a fallback.

There is no search path. `MICROTEL_CONFIG_FILE`, `./microtel.toml` in the
working directory and `/etc/microtel/microtel.toml` are **not consulted**;
earlier revisions of this section listed all three as tiers 2–4 and none was
ever implemented (`src/sdk/sdk_builder.cpp` has exactly one `LoadToml` call
site, guarded by `FromFile`). Without `FromFile`, microtel resolves from env
plus defaults and never touches the filesystem.

Discovery is deferred, not rejected; if it lands it belongs in this section with
the precedence between tiers spelled out.

**Top-level tables recognised in the file:** `[config]`, `[exporter]`,
`[service]`, `[resource]`, `[tls]`, `[sdk]`, `[timeouts]`. Any other top-level
table is an unknown key and, under the default policy, fails `Build()` — so the
§3 tables below are the whole TOML surface, not a subset of it.

### 2.4 Built-in defaults

Compiled into the library. Lowest precedence; the only source guaranteed to be present.

---

## 3. Per-setting precedence tables

Each row covers one setting. Columns: TOML key, equivalent code call (where applicable), OTEL env var (if any), MICROTEL env var (if any), default, validation, owning section in spec.

Rows are alphabetised within each subsection.

### 3.1 Service identity

| TOML | Code | OTEL env | MICROTEL env | Default | Notes |
|---|---|---|---|---|---|
| `service.name` | `WithServiceName(s)` | `OTEL_SERVICE_NAME` | — | `"unknown_service"` | spec §12.7 |
| `service.version` | `WithServiceVersion(s)` | (in `OTEL_RESOURCE_ATTRIBUTES`) | — | empty | spec §12.7 |

The `service.name` resource attribute is always present. When nothing supplies
a value, `config::Validate` resolves it to `unknown_service`, the placeholder
the OTel resource semantic conventions specify so that a backend always has
something to group on. microtel does not append an executable name — the OTel
conventions permit `unknown_service:<process name>`, but v1 has no process-name
or resource-detector machinery to derive one from, so the plain form is what it
emits. `service.version` has no such requirement and stays absent when unset.

(Before #203 landed, no fallback existed and the attribute was omitted
entirely.)

### 3.2 Resource attributes

| TOML | Code | OTEL env | MICROTEL env | Default | Notes |
|---|---|---|---|---|---|
| `[resource]` table | `WithResource({...})` | `OTEL_RESOURCE_ATTRIBUTES` (csv `k=v,k=v`) | — | empty | Detector contributions merge per spec §12.7 (detectors first, then env, then user). |

### 3.3 Exporter — endpoint and protocol

| TOML | Code | OTEL env | MICROTEL env | Default | Notes |
|---|---|---|---|---|---|
| `exporter.endpoint` | `WithEndpoint(s)` | `OTEL_EXPORTER_OTLP_ENDPOINT` | — | none (required) | If unset, `Build` fails with `ConfigError::EndpointMalformed`. |
| `exporter.protocol` | `WithProtocol(p)` | `OTEL_EXPORTER_OTLP_PROTOCOL` | — | **`http`**, or `grpc` for a `grpc://` / `grpcs://` endpoint | `http` or `grpc`. See "Endpoint scheme and protocol" below. |
| `exporter.compression` | `WithCompressionGzip(b)` | `OTEL_EXPORTER_OTLP_COMPRESSION` | — | off | TOML/env value is `gzip` to enable; anything else is off. The code setter is a `bool`, not a codec name — gzip is the only compression v1 implements. Controls **requests**: gzip request bodies with `content-encoding: gzip` (HTTP) or frame flag `0x01` with `grpc-encoding: gzip` (gRPC). Responses are independent — `accept-encoding` / `grpc-accept-encoding: gzip` is advertised whatever this is set to, and a compressed response is inflated under `MemoryLimitOptions::max_decompressed_bytes`. |
| `[exporter.headers]` table | `WithHeaders({...})` | `OTEL_EXPORTER_OTLP_HEADERS` (csv `k=v,k=v`) | — | empty | Static headers; runtime auth via `WithAuthProvider` is separate. |

**Endpoint scheme and protocol.** Four schemes are accepted. Two of them are
microtel shorthand that carries a protocol; two say nothing about it.

| Endpoint scheme | Transport | Effect on `protocol` |
|---|---|---|
| `https://` | TLS | none — `protocol` keeps its configured value (default `http`) |
| `http://` | plaintext h2c | none — `protocol` keeps its configured value (default `http`) |
| `grpcs://` | TLS | selects `grpc` unless `protocol` was set explicitly |
| `grpc://` | plaintext h2c | selects `grpc` unless `protocol` was set explicitly |

So `WithEndpoint("grpc://collector:4317")` alone speaks OTLP/gRPC, and picks up
the gRPC default port 4317 when the URL omits a port. "Set explicitly" means any
of `WithProtocol(p)`, the `exporter.protocol` TOML key, or
`OTEL_EXPORTER_OTLP_PROTOCOL` — not the built-in default.

**A `grpc://` or `grpcs://` endpoint combined with an explicit `protocol =
"http"` is rejected** at `Build()` with `ConfigError::Kind::ProtocolMismatch`
and `field = "exporter.protocol"`. The two statements contradict each other and
microtel resolves the contradiction in neither direction. Use an `http://` or
`https://` endpoint for OTLP/HTTP, or drop the explicit protocol.

Spec §12.2 still calls `https://` plus an explicit `protocol` the canonical
form, and it remains the unambiguous spelling; the shorthand is a convenience,
not a replacement. (Before #203 landed, the shorthand was normalised for TLS
only and left `protocol` at its default, so `grpc://` silently spoke OTLP/HTTP
at a gRPC port.)

**No per-signal env vars.** `OTEL_EXPORTER_OTLP_TRACES_ENDPOINT` and
`OTEL_EXPORTER_OTLP_TRACES_PROTOCOL` were listed here and are read by nothing —
`src/common/config/env_resolver.cpp` handles the unsuffixed forms only. Setting
a `_TRACES_` variable changes no behaviour and produces no warning. Per §2.2
that silence is deliberate for signals microtel does not implement; for traces
it is a gap, not a policy.

### 3.4 Exporter — timeouts (six independent, spec §7.3)

All six are set in code through the single `WithTimeouts(TimeoutOptions)` setter
— there are no per-axis `With…Timeout` methods — and in TOML under one
`[timeouts]` table, whose keys are integer milliseconds.

| TOML (`[timeouts]`) | Code (`WithTimeouts({…})`) | OTEL env | MICROTEL env | Default |
|---|---|---|---|---|
| `connect_ms` | `.connect = d` | — | — | 10s |
| `tls_ms` | `.tls_handshake = d` | — | — | 10s |
| `per_export_ms` | `.per_export = d` | `OTEL_EXPORTER_OTLP_TIMEOUT` (ms) | — | 10s |
| `retry_budget_ms` | `.retry_budget = d` | — | — | 60s |
| `flush_ms` | `.flush = d` (default for `ForceFlush`) | — | — | 5s |
| `shutdown_ms` | `.shutdown = d` (default for `Shutdown`) | — | — | 5s |

```toml
[timeouts]
retry_budget_ms = 5000
```

Caller-provided timeouts to `ForceFlush(timeout)` and `Shutdown(timeout)` override the configured default.

`retry_budget` caps the total elapsed time across all retry attempts for a
single batch; the remaining retry parameters (attempt count, backoff shape,
jitter) are not configurable in v1 and keep the OTLP-recommended defaults in
`RetryPolicyConfig`. The budget is checked between attempts, not enforced
against one in flight: before each backoff the loop looks ahead and stops if
that sleep would reach or pass the budget, so no backoff ever runs beyond it
(issue #195). An attempt already in flight still can — it is bounded by
`per_export`, not by `retry_budget`. Treat `retry_budget` as the point at which
microtel stops starting new attempts, not as a hard deadline.

### 3.5 Exporter — TLS

All five fields are set in code through the single `WithTls(TlsOptions)` setter
— there are no per-field `With…` methods — and in TOML under a **top-level
`[tls]` table**, not under `[exporter]`.

| TOML (`[tls]`) | Code (`WithTls({…})`) | OTEL env | MICROTEL env | Default |
|---|---|---|---|---|
| `insecure` | `.insecure = b` | — | — | `false` |
| `ca_bundle` | `.ca_bundle = p` | `OTEL_EXPORTER_OTLP_CERTIFICATE` | — | (system trust) |
| `client_cert` | `.client_cert = p` | — | — | (none) |
| `client_key` | `.client_key = p` | — | — | (none) |
| `sni_override` | `.sni_override = s` | — | — | (host from endpoint) |

```toml
[tls]
ca_bundle = "/etc/pki/tls/certs/collector-ca.pem"
```

`insecure=true` emits a `Warn`-level log line from `Build()`. There is no way to
suppress it short of installing a filtering `SetLogSink`.

**`MICROTEL_FORBID_INSECURE_TLS=ON` refuses `insecure=true` at initialisation.**
The CMake option compiles `MICROTEL_FORBID_INSECURE_TLS=1` into
`microtel_config`, and `config::Validate` fails a configuration carrying
`tls.insecure = true` with `ConfigError::Kind::InsecureDisallowed` and
`field = "tls.insecure"` — so `SdkBuilder::Build()` returns the error instead of
a `Provider`, and there is no runtime way to turn verification back off. The
option is a property of the build, not of the configuration: a library compiled
with it OFF cannot be made to refuse, and one compiled with it ON cannot be made
to accept. Default builds (`OFF`) keep the `Warn` line described above.

Two corrections against what this section used to claim (#196): the TOML paths
were given as `exporter.tls.*`, which is an **unknown key** — `[exporter]`
accepts only `endpoint`, `protocol`, `compression` and `headers`, so that
spelling fails `Build()` under the default policy rather than configuring TLS.
And `OTEL_EXPORTER_OTLP_CLIENT_CERTIFICATE` / `OTEL_EXPORTER_OTLP_CLIENT_KEY`
are read by nothing: mTLS material is code or TOML only. `OTEL_EXPORTER_OTLP_CERTIFICATE`
is the one TLS env var that works.

### 3.6 Exporter — proxy

**Not implemented in v1.** microtel connects directly to the configured
endpoint. There is no TOML or code surface, and the environment variables below
are **reserved names, read by nothing** — setting one changes no behaviour and
produces no warning. Deferred; see [`microtel-roadmap.md`](../microtel-roadmap.md)
and [`compatibility-matrix.md`](compatibility-matrix.md) §3. (Spec §12.4.)

| Variable | Effect in v1 | Intended effect |
|---|---|---|
| `https_proxy` / `HTTPS_PROXY` | none | proxy `https://` endpoints via `CONNECT` |
| `http_proxy` / `HTTP_PROXY` | none | proxy `http://` endpoints |
| `no_proxy` / `NO_PROXY` | none | comma-separated bypass list, leading-dot subdomain matching |

### 3.7 Batch span processor

All four are set in code through the single `WithBatch(BatchOptions)` setter and
in TOML under the **`[sdk]` table** — not `[batch]`, which is an unknown key.

| TOML (`[sdk]`) | Code (`WithBatch({…})`) | OTEL env | MICROTEL env | Default |
|---|---|---|---|---|
| `max_queue_size` | `.max_queue_size = N` | — | — | 8192 |
| `max_export_batch_size` | `.max_export_batch_size = N` | — | — | 512 |
| `schedule_delay_ms` | `.schedule_delay = d` | — | — | 5000 ms |
| `drop_policy` | `.drop_policy = p` | — | — | `"newest"` / `DropPolicy::DropNewest` |

```toml
[sdk]
max_queue_size = 16384
drop_policy = "oldest"
```

`drop_policy` takes `"newest"` or `"oldest"` in TOML (any other value is
`ConfigError::InvalidValue` on field `sdk.drop_policy`) and
`DropPolicy::DropNewest` / `DropPolicy::DropOldest` in code. Spec §5.4.

Three corrections (#196): the TOML table is `[sdk]`, not `[batch]`; the delay
key is `schedule_delay_ms` (integer milliseconds), not `schedule_delay`; and
there is no `WithDropPolicy` setter — it is a `BatchOptions` field. The
`OTEL_BSP_*` env vars listed here are read by nothing.

### 3.8 Memory budgets (spec §5.5; `memory-model.md` §6)

**Code-only in v1.** All five are fields of `MemoryLimitOptions`, set through
`WithMemoryLimits(MemoryLimitOptions)`. There is no `[limits]` TOML table and no
environment variable; the values are plain integer bytes.

| Code (`WithMemoryLimits({…})`) | TOML | Env | Default | Enforced at |
|---|---|---|---|---|
| `.max_total_queue_bytes = n` | — | — | 16 MiB | `BatchSpanProcessor::OnEnd`, against the summed estimate of everything queued (counter `queue_full`) |
| `.max_record_bytes = n` | — | — | 64 KiB | `BatchSpanProcessor::OnEnd`, before the record is queued (counter `record_too_large`) |
| `.max_response_bytes = n` | — | — | 1 MiB | the transport, as the response body is accumulated (counter `response_too_large`) |
| `.max_trailer_bytes = n` | — | — | 64 KiB | the transport, as the trailers are accumulated (also counter `response_too_large`); also advertised as `SETTINGS_MAX_HEADER_LIST_SIZE`, which is what bounds the response headers (issue #213) |
| `.max_decompressed_bytes = n` | — | — | 4 MiB | the wire codec, as a gzipped response inflates (counter `decompression_too_large`) |

Corrections (#196): the five `WithMax…Bytes(n)` setters this section named do
not exist, the `limits.*` TOML keys are unknown keys that fail `Build()`, and
the SI-suffix parsing (`"16MiB"`, `"4MB"`) was never implemented. A TOML surface
for these is deferred; adding one means a `[limits]` entry in the top-level
known-key list in `src/common/config/toml_loader.cpp` and a row in this table.

### 3.9 Span structural limits (spec §5.6; `memory-model.md` §7)

**Code-only in v1**, through `WithSpanLimits(SpanLimitOptions)`. No TOML table,
no environment variables.

| Code (`WithSpanLimits({…})`) | TOML | OTEL env | Default | Enforced at |
|---|---|---|---|---|
| `.attribute_count_limit = N` | — | — | 128 | `SdkSpan::SetAttribute` (counter `span_attribute_limit`) |
| `.event_count_limit = N` | — | — | 128 | `SdkSpan::AddEvent` (counter `span_event_limit`) |
| `.link_count_limit = N` | — | — | 128 | `SdkSpan::AddLink` (counter `span_link_limit`) |
| `.attribute_value_length_limit = N` | — | — | 4096 | every string attribute copy — `SetAttribute` and the event / link attribute copies. **Bytes**, cut back to a UTF-8 code point boundary; the attribute is truncated, not dropped (counter `attribute_value_truncated`) |
| `.event_attribute_count_limit = N` | — | — | 128 | `SdkSpan::AddEvent` (counter `event_attribute_limit`) |
| `.link_attribute_count_limit = N` | — | — | 128 | `SdkSpan::AddLink` (counter `link_attribute_limit`) |

Correction (#196): the `span_limits.*` TOML keys and the six `OTEL_SPAN_*` /
`OTEL_ATTRIBUTE_VALUE_LENGTH_LIMIT` / `OTEL_EVENT_ATTRIBUTE_COUNT_LIMIT` /
`OTEL_LINK_ATTRIBUTE_COUNT_LIMIT` env vars this table listed are read by
nothing. The limits themselves are enforced; only the two configuration
surfaces were fictional.

### 3.10 Sampling

**Code-only in v1**, through `WithSampler(SamplerHandle)`. The handle comes from
a factory in [`microtel/sampler.hpp`](../include/microtel/sampler.hpp):

| Factory | Behaviour |
|---|---|
| `MakeAlwaysOnSampler()` | sample every span (the builder's default) |
| `MakeAlwaysOffSampler()` | sample nothing |
| `MakeTraceIdRatioSampler(r)` | sample a deterministic fraction `r` ∈ [0.0, 1.0] of trace IDs |
| `MakeParentBasedSampler(root)` | follow the parent's decision; use `root` when there is no parent |

Corrections (#196): there is no `[sampling]` TOML table, and `OTEL_TRACES_SAMPLER`
/ `OTEL_TRACES_SAMPLER_ARG` are read by nothing — so the sampler **names**
(`always_on`, `parentbased_traceidratio`, …) this section listed as "accepted"
are accepted nowhere. Nothing in the tree parses a sampler from a string.
`parentbased_always_on` was also given as the default; the builder's default is
a bare always-on sampler, not a parent-based one.

### 3.11 Logging

**No configuration surface in v1.** `SetLogSink(LogSink)` is a runtime
injection, not a setting — the application installs a callback receiving
`(LogLevel, std::string_view)` and routes microtel's internal logs wherever it
likes (`error-model.md` §9.3). Level filtering, sink selection and log-file
paths are the application's side of that callback.

Correction (#196): the `logging.level` / `logging.sink` / `logging.file` TOML
keys and the `MICROTEL_LOG_LEVEL` / `MICROTEL_LOG_SINK` / `MICROTEL_LOG_FILE`
env vars are read by nothing, and neither the `journald` nor the `syslog` sink
named here exists. Spec §9.4 describes the intended surface; this section now
describes the built one.

### 3.12 Configuration meta

| TOML | Code | OTEL env | MICROTEL env | Default |
|---|---|---|---|---|
| `config.unknown_keys` | — | — | — | `error` |

Values: `error` (default), `warn`, `ignore`. Correction (#196): there is no
`WithUnknownKeysPolicy` setter — the policy is TOML-only, which follows from
what it governs (it is parsed first, out of `[config]`, to establish the mode
every later section's unknown-key check runs under).

### 3.13 Metrics pipeline

Metrics are implemented but not claimed until v1.2 (see the root `README.md`
status table). The configuration surface that exists today is code plus three
environment variables; there is no TOML table.

| Code | OTEL env | MICROTEL env | Default |
|---|---|---|---|
| `WithMetricInterval(d)` | `OTEL_METRIC_EXPORT_INTERVAL` (ms) | — | 60 s |
| `WithMetricTemporality(p)` | `OTEL_EXPORTER_OTLP_METRICS_TEMPORALITY_PREFERENCE` (`cumulative` \| `delta` \| `lowmemory`) | — | `Cumulative` |
| `WithMetricLimits({.max_cardinality = n})` | — | `MICROTEL_METRIC_CARDINALITY_LIMIT` (decimal integer) | 2000 |
| `WithView(ViewConfig)` | — | — | no views |

---

## 4. Build-time options

Distinct from runtime configuration. Set via CMake at compile time. (Spec §9.2.)

| CMake option | Default | Effect |
|---|---|---|
| `MICROTEL_USE_SPDLOG` | `ON` | When `OFF`, microtel uses a minimal stderr logger instead of spdlog. Sink injection still works. |
| `MICROTEL_FORBID_INSECURE_TLS` | `OFF` | When `ON`, a configuration with `tls.insecure = true` fails `Build()` with `ConfigError::Kind::InsecureDisallowed`. Default builds warn instead. See §3.5. |
| `MICROTEL_BUILD_OTELCPP_SHIM` | `OFF` | Builds the experimental opentelemetry-cpp adapter (ICP 0014). |
| `MICROTEL_BUILD_TESTS` | `ON` | Builds the test tree. Set `OFF` for cross-compilation. |
| `MICROTEL_BUILD_HEADER_CHECK` | `ON` | Builds the header compile check that includes every public and internal header. |
| `MICROTEL_BUILD_EXAMPLES` | `OFF` | Builds the standalone API examples under `examples/`. |
| `MICROTEL_BUILD_BENCH` | `OFF` | Builds the benchmark harness under `bench/` (needs Podman/Docker). |
| `MICROTEL_BUILD_FUZZ` | `OFF` | Builds the libFuzzer harnesses (clang only). |
| `MICROTEL_COVERAGE` | `OFF` | Builds instrumented for coverage: clang source-based (`-fprofile-instr-generate -fcoverage-mapping`), which is what `ci/scripts/coverage.sh` gates on, or gcov `--coverage` under gcc. |
| `MICROTEL_SANITIZER` | *(empty)* | One of `asan`, `tsan`, `ubsan`. A cache string, not a boolean option. |

Correction (#196): `MICROTEL_BUILD_PYTHON` does not exist — there is no Python
extension in the tree — and the shim option is named
`MICROTEL_BUILD_OTELCPP_SHIM`, not `MICROTEL_BUILD_COMPAT_SHIMS`.

Build-time options never appear in `microtel.toml` and have no environment-variable equivalents. They are properties of the binary, not of the runtime configuration.

---

## 5. Resolved-config dump and secrets

**Not implemented in v1.** `Build()` logs no configuration dump at all — neither
redacted nor otherwise. The two `Warn` lines it can emit (plaintext OTLP/HTTP,
and `insecure=true`) are the whole of what `Build()` says about the resolved
configuration, and neither carries a header value, a credential or a path.

Spec §12.6 describes the intended design: an `info`-level dump at `Build()`
success with `Authorization` headers and client-secret-shaped values redacted,
private-key *paths* preserved, token-provider outputs never logged, and an
opt-in `--show-secrets` CLI flag gated by a build-time
`MICROTEL_ALLOW_SHOW_SECRETS=ON`. Correction (#196): this section described that
design in the present tense. None of it exists — there is no redaction code, no
`--show-secrets` flag (the `microtel-preflight` CLI accepts only
`--preflight=…`), and no `MICROTEL_ALLOW_SHOW_SECRETS` option.

The practical consequence is the reassuring one: microtel cannot leak a secret
through a config dump it does not emit. The gap is the diagnostic value of the
dump, not a disclosure risk. When it lands it belongs here in the present tense,
with tests over the redaction rules.

---

## 6. Adding a new setting

When a new setting lands in M3+, the change includes:

1. A row in the appropriate §3 subsection.
2. The default in `src/common/config/`.
3. Validation logic in the same place, mapping failures to a `ConfigError::Kind`.
4. A `WithXxx` method on `SdkBuilder` if there is a code surface.
5. Env-var read in `src/common/config/env_resolver.cpp`, if there is one.
6. Tests for: TOML happy path, TOML invalid value, env-var precedence, code precedence, default fallback.

If the new setting overlaps with an existing OTel env var, this document calls out the relationship and the precedence (per §1, MICROTEL wins on overlap).

If the new setting is hot-reloadable in a future release, that is a v1.1+ concern and is recorded in the v1.1 control-plane design doc when written.
