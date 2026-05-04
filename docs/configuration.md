# microtel Configuration

**Status:** M0 deliverable. Documents precedence rules and per-setting resolution.
**Companion:** `microtel-spec.md` §12 (canonical for setting names and semantics), `error-model.md` §8 (init-failure taxonomy).
**Maintenance:** the precedence rules in §1 are stable; the per-setting tables in §3 are appended to as new settings land in M3+. New settings without a row here are a documentation bug.

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

Path resolution at `Build()` time:

1. Explicit path passed to `SdkBuilder::FromFile(path)` if used.
2. `MICROTEL_CONFIG_FILE` environment variable if set.
3. `./microtel.toml` in the process working directory.
4. `/etc/microtel/microtel.toml` (Linux only; ignored if missing).

Missing file at any tier is not an error — microtel proceeds with env + defaults. **Only the file the user explicitly named** (option 1 or 2) is required to exist; if it doesn't, `ConfigError::Kind::FileNotFound`.

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

### 3.2 Resource attributes

| TOML | Code | OTEL env | MICROTEL env | Default | Notes |
|---|---|---|---|---|---|
| `[resource]` table | `WithResource({...})` | `OTEL_RESOURCE_ATTRIBUTES` (csv `k=v,k=v`) | — | empty | Detector contributions merge per spec §12.7 (detectors first, then env, then user). |

### 3.3 Exporter — endpoint and protocol

| TOML | Code | OTEL env | MICROTEL env | Default | Notes |
|---|---|---|---|---|---|
| `exporter.endpoint` | `WithEndpoint(s)` | `OTEL_EXPORTER_OTLP_ENDPOINT`, `OTEL_EXPORTER_OTLP_TRACES_ENDPOINT` | — | none (required) | If unset, `Build` fails with `ConfigError::EndpointMalformed`. Per-signal `_TRACES_` overrides the unsigned form. |
| `exporter.protocol` | `WithProtocol(p)` | `OTEL_EXPORTER_OTLP_PROTOCOL`, `OTEL_EXPORTER_OTLP_TRACES_PROTOCOL` | — | derived from URL scheme; otherwise `grpc` | `http` or `grpc`. URL schemes `grpc://` / `grpcs://` are accepted shorthand (spec §12.2). |
| `exporter.compression` | `WithCompression(c)` | `OTEL_EXPORTER_OTLP_COMPRESSION` | — | `off` | `off` or `gzip`. |
| `[exporter.headers]` table | `WithHeaders({...})` | `OTEL_EXPORTER_OTLP_HEADERS` (csv `k=v,k=v`) | — | empty | Static headers; runtime auth via `WithAuthProvider` is separate. |

### 3.4 Exporter — timeouts (six independent, spec §7.3)

| TOML | Code | OTEL env | MICROTEL env | Default |
|---|---|---|---|---|
| `exporter.timeouts.connect` | `WithConnectTimeout(d)` | — | — | 10s |
| `exporter.timeouts.tls_handshake` | `WithTlsHandshakeTimeout(d)` | — | — | 10s |
| `exporter.timeouts.per_export` | `WithPerExportTimeout(d)` | `OTEL_EXPORTER_OTLP_TIMEOUT` (ms) | — | 10s |
| `exporter.timeouts.retry_budget` | `WithRetryBudget(d)` | — | — | 60s |
| `exporter.timeouts.flush` | `WithFlushTimeout(d)` (default for `ForceFlush`) | — | — | 5s |
| `exporter.timeouts.shutdown` | `WithShutdownTimeout(d)` (default for `Shutdown`) | — | — | 5s |

Caller-provided timeouts to `ForceFlush(timeout)` and `Shutdown(timeout)` override the configured default.

### 3.5 Exporter — TLS

| TOML | Code | OTEL env | MICROTEL env | Default |
|---|---|---|---|---|
| `exporter.tls.insecure` | `WithInsecure(b)` | — | — | `false` |
| `exporter.tls.ca_bundle` | `WithCaBundle(p)` | `OTEL_EXPORTER_OTLP_CERTIFICATE` | — | (system trust) |
| `exporter.tls.client_cert` | `WithClientCert(p)` | `OTEL_EXPORTER_OTLP_CLIENT_CERTIFICATE` | — | (none) |
| `exporter.tls.client_key` | `WithClientKey(p)` | `OTEL_EXPORTER_OTLP_CLIENT_KEY` | — | (none) |
| `exporter.tls.sni_override` | `WithSniOverride(s)` | — | — | (host from endpoint) |

`insecure=true` emits a runtime warning unless suppressed; if compiled with `MICROTEL_FORBID_INSECURE_TLS=ON`, `insecure=true` causes `Build` to fail with `ConfigError::InsecureDisallowed` (`error-model.md` §8).

### 3.6 Exporter — proxy

Driven entirely by environment variables; no TOML or code surface in v1. (Spec §12.4.)

| Variable | Effect |
|---|---|
| `https_proxy` / `HTTPS_PROXY` | Honoured for `https://` endpoints. |
| `http_proxy` / `HTTP_PROXY` | Honoured for `http://` endpoints (only meaningful with `insecure=true`). |
| `no_proxy` / `NO_PROXY` | Honoured; standard comma-separated host list with leading-dot subdomain matching. |

### 3.7 Batch span processor

| TOML | Code | OTEL env | MICROTEL env | Default |
|---|---|---|---|---|
| `batch.max_queue_size` | `WithBatch({.max_queue_size = N})` | `OTEL_BSP_MAX_QUEUE_SIZE` | — | 8192 |
| `batch.max_export_batch_size` | `WithBatch({.max_export_batch_size = N})` | `OTEL_BSP_MAX_EXPORT_BATCH_SIZE` | — | 512 |
| `batch.schedule_delay` | `WithBatch({.schedule_delay = d})` | `OTEL_BSP_SCHEDULE_DELAY` (ms) | — | 5000 ms |
| `batch.drop_policy` | `WithDropPolicy(p)` | — | — | `drop_newest` (alternate: `drop_oldest`; spec §5.4) |

### 3.8 Memory budgets (spec §5.5; `memory-model.md` §6)

| TOML | Code | Default |
|---|---|---|
| `limits.max_total_queue_bytes` | `WithMaxTotalQueueBytes(n)` | 16 MiB |
| `limits.max_record_bytes` | `WithMaxRecordBytes(n)` | 64 KiB |
| `limits.max_response_bytes` | `WithMaxResponseBytes(n)` | 1 MiB |
| `limits.max_trailer_bytes` | `WithMaxTrailerBytes(n)` | 64 KiB |
| `limits.max_decompressed_bytes` | `WithMaxDecompressedBytes(n)` | 4 MiB |

Each accepts a SI suffix in TOML (`"16MiB"`, `"4MB"`).

### 3.9 Span structural limits (spec §5.6; `memory-model.md` §7)

| TOML | Code | OTEL env | Default |
|---|---|---|---|
| `span_limits.attribute_count_limit` | `WithSpanLimits({.attribute_count_limit = N})` | `OTEL_SPAN_ATTRIBUTE_COUNT_LIMIT` | 128 |
| `span_limits.event_count_limit` | `WithSpanLimits({.event_count_limit = N})` | `OTEL_SPAN_EVENT_COUNT_LIMIT` | 128 |
| `span_limits.link_count_limit` | `WithSpanLimits({.link_count_limit = N})` | `OTEL_SPAN_LINK_COUNT_LIMIT` | 128 |
| `span_limits.attribute_value_length_limit` | `WithSpanLimits({.attribute_value_length_limit = N})` | `OTEL_ATTRIBUTE_VALUE_LENGTH_LIMIT` | 4096 |
| `span_limits.event_attribute_count_limit` | `WithSpanLimits({.event_attribute_count_limit = N})` | `OTEL_EVENT_ATTRIBUTE_COUNT_LIMIT` | 128 |
| `span_limits.link_attribute_count_limit` | `WithSpanLimits({.link_attribute_count_limit = N})` | `OTEL_LINK_ATTRIBUTE_COUNT_LIMIT` | 128 |

### 3.10 Sampling

| TOML | Code | OTEL env | Default |
|---|---|---|---|
| `sampling.sampler` | `WithSampler(s)` | `OTEL_TRACES_SAMPLER` | `parentbased_always_on` |
| `sampling.argument` | (sampler-specific) | `OTEL_TRACES_SAMPLER_ARG` | (sampler-specific) |

Sampler names accepted: `always_on`, `always_off`, `traceidratio` (with `argument` as the ratio in `[0.0, 1.0]`), `parentbased_always_on`, `parentbased_always_off`, `parentbased_traceidratio`.

### 3.11 Logging

| TOML | Code | OTEL env | MICROTEL env | Default |
|---|---|---|---|---|
| `logging.level` | (`SetLogSink` for redirection only) | — | `MICROTEL_LOG_LEVEL` | `info` |
| `logging.sink` | — | — | `MICROTEL_LOG_SINK` | `stderr` |
| `logging.file` | — | — | `MICROTEL_LOG_FILE` | (none; required if `sink="file"`) |

`logging.level` accepts `error`, `warn`, `info`, `debug`, `trace`. `logging.sink` accepts `stderr`, `file`, `journald` (Linux), `syslog`. Spec §9.4.

`SetLogSink` is a runtime injection, not a configuration setting — it overrides the resolved sink at any point during the process lifetime (`error-model.md` §9.3).

### 3.12 Configuration meta

| TOML | Code | OTEL env | MICROTEL env | Default |
|---|---|---|---|---|
| `config.unknown_keys` | `WithUnknownKeysPolicy(p)` | — | — | `error` |

---

## 4. Build-time options

Distinct from runtime configuration. Set via CMake at compile time. (Spec §9.2.)

| CMake option | Default | Effect |
|---|---|---|
| `MICROTEL_USE_SPDLOG` | `ON` | When `OFF`, microtel uses a minimal stderr logger instead of spdlog. Sink injection still works. |
| `MICROTEL_BUILD_PYTHON` | `OFF` | Builds the nanobind Python extension. |
| `MICROTEL_BUILD_COMPAT_SHIMS` | `OFF` | Builds the experimental otel-cpp / otel-python shims as separate libraries. |
| `MICROTEL_FORBID_INSECURE_TLS` | `OFF` | Hardens the build to refuse `insecure=true` at runtime. |
| `MICROTEL_BUILD_TESTS` | `ON` | Builds the test tree. Set `OFF` for cross-compilation. |

Build-time options never appear in `microtel.toml` and have no environment-variable equivalents. They are properties of the binary, not of the runtime configuration.

---

## 5. Resolved-config dump and secrets

At `Build()` success, microtel logs the resolved configuration at `info` level with secrets redacted by default (spec §12.6):

- `Authorization` headers → `<redacted>`.
- Client-secret-shaped values → `<redacted>`.
- Private-key paths → preserved (paths are not secret).
- Token-provider outputs → never logged.

Showing secrets requires the explicit `--show-secrets` flag on the `microtel` CLI, gated again by build-time `MICROTEL_ALLOW_SHOW_SECRETS=ON`. Production builds default the build flag to `OFF`.

The resolved-config dump is an `info`-level emission and respects `SetLogSink`. Applications that capture microtel's logs must be aware that the dump includes the (redacted) configuration on startup.

---

## 6. Adding a new setting

When a new setting lands in M3+, the change includes:

1. A row in the appropriate §3 subsection.
2. The default in `src/common/config/`.
3. Validation logic in the same place, mapping failures to a `ConfigError::Kind`.
4. A `WithXxx` method on `SdkBuilder` if there is a code surface.
5. Env-var read in `src/common/config/env.cpp`.
6. Tests for: TOML happy path, TOML invalid value, env-var precedence, code precedence, default fallback.

If the new setting overlaps with an existing OTel env var, this document calls out the relationship and the precedence (per §1, MICROTEL wins on overlap).

If the new setting is hot-reloadable in a future release, that is a v1.1+ concern and is recorded in the v1.1 control-plane design doc when written.
