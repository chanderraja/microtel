# Compatibility Matrix

## 1. Purpose

[`microtel-spec.md`](../microtel-spec.md) §15.1 in operational form, and the
"explicitly marked unsupported in the compatibility matrix" escape hatch that
§13.5's release gates point at. Two questions, answered in one place:

- **Does microtel do this?** — and if so, what test says so.
- **What does *not* work**, stated plainly, so nobody discovers it against a
  production collector.

Sibling of [`interop-matrix.md`](interop-matrix.md), which records the pinned
*versions* the conformance gates run against. This file records the *features*.
Nothing reads either mechanically.

---

## 2. Signals and protocols (spec §15.1)

| Area | v1 status | Evidence |
|---|---|---|
| OTLP/HTTP traces | Supported over TLS; **unsupported over plaintext against HTTP/1.1-only receivers** — see §4 | `tests/conformance/http/` |
| OTLP/gRPC traces | Supported, plaintext and TLS | `tests/conformance/grpc/` |
| OTLP metrics, both protocols | Implemented, **not** conformance-tested | unit tests only; collector conformance is deferred to v1.2 ([`interop-matrix.md`](interop-matrix.md) §6) |
| OTLP logs, both protocols | Implemented, **not** conformance-tested | unit tests only; `Provider::GetLogger` per [ICP 0012](icps/0012-provider-get-logger.md) |
| OTel SDK env vars (subset) | Partial | `tests/unit/common/config/`, [`configuration.md`](configuration.md) §3 |
| `opentelemetry-cpp` API shim | Experimental; source-only, adds nothing to the consumer's link closure | `src/adapters/otelcpp/`, `tests/unit/adapters/`, `tests/integration/otelcpp_shim/`; [ICP 0014](icps/0014-otelcpp-shim-and-rule-13.md) |
| `opentelemetry-python` API shim | Not in v1 — Python ships post-v1.0 as M18 | [ICP 0013](icps/0013-rescope-defer-python-bindings.md) |
| Collector versions | Pinned matrix | [`interop-matrix.md`](interop-matrix.md) §2 |

The metrics and logs rows are where this file is deliberately at odds with the
spec's "Planned": the code exists and is unit-tested, but no test has ever
asked a real collector to decode a microtel metric or log payload, so nothing
here claims one will.

---

## 3. Transport and security ledger (spec §13.5)

Spec §13.5 gates v1.0 on TLS system trust, custom CA, mTLS, static headers,
auth callback, and proxy behaviour each either passing integration tests **or
being explicitly marked unsupported here**. This is that ledger.

| Capability | Status | Evidence |
|---|---|---|
| TLS with the system trust store | Supported | `SSL_CTX_set_default_verify_paths` when no `ca_bundle` is set; verification itself is covered by `tests/integration/transport/http2_tls_connect_test.cpp` (`TrustedCa_ConnectSucceeds`, `UntrustedCert_ConnectFails`). No test pins the *host's* trust store — that would assert on the CI image, not on microtel. |
| TLS with a custom CA (`ca_bundle`) | Supported | `TlsCustomCa`, `UntrustedCaFails` in `tests/conformance/{http,grpc}/tls_test.cpp` |
| mTLS (`client_cert` + `client_key`) | Supported | `MutualTls`, `MutualTlsWithoutClientCertFails`, both suites |
| SNI override | Supported | `SniOverride`, both suites; [ICP 0022](icps/0022-tls-peer-verification.md) |
| Static auth headers | Supported | `StaticBearerHeader`, both suites |
| Auth callback (`WithAuthProvider`) | Supported | `AuthCallback` + `WrongTokenRejected`, both suites; TTL caching in `tests/unit/common/auth/auth_providers_test.cpp` |
| `insecure = true` | Supported, and **warns** at `Build()` | `SdkBuilderTest.Build_InsecureTls_Warns`. A hard ban is `MICROTEL_FORBID_INSECURE_TLS=ON`, which turns it into `ConfigError::InsecureDisallowed`. |
| **HTTP proxy (`https_proxy` / `http_proxy` / `no_proxy`, `CONNECT`)** | **UNSUPPORTED in v1** | Not implemented — zero occurrences in `src/`. The variable names are reserved; setting them changes nothing. Deferred, see [`microtel-roadmap.md`](../microtel-roadmap.md). |
| **Plaintext OTLP/HTTP against an HTTP/1.1-only receiver** | **UNSUPPORTED** | §4 below; [`interop-matrix.md`](interop-matrix.md) §4; issue #166 |
| A peer that hangs up under an in-flight write | Survivable: the export fails, the host process does not | `SIGPIPE` is suppressed per write — `MSG_NOSIGNAL` on plaintext sends, and a custom `BIO` carrying the same flag for TLS, covering `SSL_write` and the handshake writes inside `SSL_connect`. The host needs no `signal(SIGPIPE, SIG_IGN)` of its own, and microtel installs no handler and changes no process-wide disposition (`threading-model.md` §7.1). Evidence: `NoSignalIoTest` / `NoSignalBioTest` in `tests/unit/transport/nosignal_io_test.cpp`, plus the peer-reset tests in `tests/integration/transport/`. Issue #177. |
| gzip request compression | Supported | conformance suites, both protocols |
| Response decompression (`grpc-encoding` / `content-encoding: gzip`) | Supported, bounded by `max_decompressed_bytes` | `tests/unit/wire/grpc/`, `tests/unit/wire/http/`, `tests/fuzz/response_decompression_fuzz.cpp` |
| HTTP/3 | Out of scope for v1 | spec §17; v1.5 experiment per roadmap |
| Windows | Out of scope | spec §3 |

---

## 4. Plaintext OTLP/HTTP cannot reach a stock collector

microtel's transport is HTTP/2-only (nghttp2), so a plaintext `http://`
endpoint means **h2c with prior knowledge**. The OpenTelemetry Collector's
plaintext OTLP/HTTP receiver serves HTTP/1.1 only — it does not wrap its
handler in `h2c` — so it answers the HTTP/2 connection preface with an
HTTP/1.1 response and the SETTINGS exchange never completes.

**This is a documented limitation, not a bug being fixed.** microtel has no
HTTP/1.1 mode and none is planned for v1; whether to add one is a roadmap item
(see [`microtel-roadmap.md`](../microtel-roadmap.md) §12) and issue #166.

**What works instead**, in order of preference:

| Configuration | Works? |
|---|---|
| `https://collector:4318` + `Protocol::Http` | Yes — the same collector negotiates `h2` through ALPN |
| `http://collector:4317` + `Protocol::Grpc` | Yes — gRPC is h2c by definition |
| `https://collector:4317` + `Protocol::Grpc` | Yes |
| `http://collector:4318` + `Protocol::Http` | **No** — unless the receiver speaks h2c |
| `http://proxy:8080` + `Protocol::Http`, where the proxy is h2c-capable | Yes — this is why the configuration is not rejected outright |

**How it reports itself.** Since the transport learned to name this case, all
three of these surfaces say so:

- `Build()` logs a warning through the internal log channel
  (`microtel::SetLogSink` to capture it):

  > plaintext OTLP/HTTP (http:// with protocol=http) is HTTP/2 with prior
  > knowledge and cannot reach an HTTP/1.1-only OTLP receiver such as a stock
  > OpenTelemetry Collector - use https:// or OTLP/gRPC; see
  > docs/compatibility-matrix.md

- `Connect()` fails with `Error::Kind::Protocol`:

  > peer answered the HTTP/2 preface with an HTTP/1.1 response - endpoint
  > appears to be HTTP/1.1-only; use https:// (ALPN h2) or OTLP/gRPC; see
  > docs/compatibility-matrix.md

- The same message reaches `HealthSnapshot::last_error_message`, and the
  failure is **not retried**: a protocol mismatch is permanent, and a retry
  budget spent on it only buries the message.

The neighbouring case — a TLS endpoint that never agrees to `h2` — is caught
by an ALPN check right after the handshake and reports `Error::Kind::Protocol`
naming what was negotiated.

One boundary is worth knowing, because it moves with the linked OpenSSL. If
such a server answers ALPN with a protocol microtel never offered (microtel
offers only `h2`), OpenSSL **3.2 and later** enforce RFC 7301 themselves and
fail the handshake, so the refusal arrives as a TLS error rather than the ALPN
message; **3.0 and 3.1** let it through and microtel's check is what refuses
it. Either way the connection is refused — which is what
`Http2TlsConnectTest.AlpnAnswersHttp11_ConnectFails` asserts.

---

## 5. Open-issue caveats

Claims above are true as written; these are the known gaps behind them, all
open at the time of writing.

- **Instrumentation scope on the wire is the service name** (issue #167).
  `GetTracer(name, version)` does not reach the collector, so a backend groups
  spans by service rather than by instrumentation library. Affects both
  protocols identically. No conformance test asserts on `ScopeSpans.scope` —
  that would enshrine the bug.
- **`TraceState` and `W3CTraceContextPropagator` are declared and not
  defined** (issue #188): `TraceState::FromHeader` / `ToHeader` / `Size` /
  `Empty` and `W3CTraceContextPropagator::Inject` / `Extract` have no
  definition in any shipped translation unit, so calling one from consumer
  code is a link error.
  Same defect class as issue #168, which is fixed — `TraceId::ToHex()` and
  `SpanId::ToHex()` are now defined in `src/api/` (`microtel_api`).
- **`HealthSnapshot::drop_counters` is nearly all dead for traces** (issue
  #169). Only 2 of 24 `DropReason` counters are ever incremented, so the
  negative conformance tests assert `batches_failed` and collector output
  instead. Do not build alerting on a counter without checking it is wired.
- **gRPC failures name the status** (issue #171, fixed). Both the
  `grpc-status` name and number and the collector's percent-decoded
  `grpc-message` reach `HealthSnapshot::last_error_message` — e.g.
  `UNAUTHENTICATED (16): provided authorization does not match expected scheme
  or token`. Before this, every non-zero status read `"grpc error"` and a
  rejected credential was indistinguishable from a malformed payload.
- **Response decompression shipped** (issue #161). Both codecs advertise
  `gzip` and inflate a compressed response under `max_decompressed_bytes`.
  Before it shipped, a collector that compressed its response body was not
  interoperable on the gRPC path — and the pinned collector does exactly that
  whenever the request is compressed, so `compression = "gzip"` deployments
  were silently discarding partial-success counts.

---

## 6. Update procedure

A row changes in the same PR as the behaviour it describes. When a row moves
from unsupported to supported, the test that proves it moves with it — a
status without evidence in the third column is not a status.

§4 in particular is load-bearing: `tests/conformance/http/plaintext_gap_test.cpp`
asserts the limitation *and* its error message, and the sibling HTTP
conformance tests point at TLS endpoints because of it. If that test starts
failing, this section and [`interop-matrix.md`](interop-matrix.md) §4 are part
of the change.
