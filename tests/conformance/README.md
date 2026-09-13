# `tests/conformance/`

End-to-end against a real OpenTelemetry Collector. Nine test binaries,
27 enabled tests and 2 deliberately disabled tripwires. Every test
skips unless [`ci/scripts/conformance.sh`](../../ci/scripts/conformance.sh)
has started a collector and exported the environment contract below.

## What this proves

Tier 1 of the four-tier compatibility model in `microtel-spec.md` §2.2:
payloads emitted by microtel are accepted by receivers implementing the
pinned OTLP specification version, over both OTLP/HTTP-protobuf and
OTLP/gRPC.

This is the gate for the v1.0 release per spec §13.5:

> OTLP/HTTP trace export passes integration tests against the pinned
> OpenTelemetry Collector matrix.
> OTLP/gRPC trace export passes integration tests against the pinned
> OpenTelemetry Collector matrix.

## Boundary vs. the other tiers

A real receiver is the only thing that proves acceptance. Every other
tier has microtel — or a peer microtel's authors wrote — on both sides
of the wire, so its green says the client agrees with itself.

- **`unit/`** — one type against mocks. Includes the byte-level wire
  corpus under [`tests/unit/wire/`](../unit/wire/): what microtel
  *writes*, asserted against hand-encoded byte fixtures in the test
  sources.
- **`integration/`** — real components wired together against fakes at
  the system boundary (sockets, the clock).
- **`conformance/`** — the bytes leave the process and a receiver
  written by the OpenTelemetry and gRPC projects decodes them. The
  assertions read the collector's own output, which is downstream of
  its protobuf decode.

Consequence, enforced in [`CMakeLists.txt`](CMakeLists.txt):
`${CMAKE_SOURCE_DIR}/src` is **not** on the include path here. A test
that needs an internal header to make its point belongs in the
integration tier.

## Layout

### `http/` — OTLP/HTTP-protobuf, 5 binaries

| Binary | Source | Tests |
|---|---|---|
| `conformance_http_basic_test` | `basic_export_test.cpp` | `Connect()` preflight; one span's full round trip |
| `conformance_http_batching_test` | `batching_test.cpp` | multi-batch exactly-once, gzip, multi-DATA-frame body |
| `conformance_http_tls_test` | `tls_test.cpp` | custom CA, mTLS, SNI override, and two negative cases |
| `conformance_http_auth_test` | `auth_test.cpp` | static bearer header, auth callback, wrong-token 401 |
| `conformance_http_plaintext_gap_test` | `plaintext_gap_test.cpp` | pins the plaintext gap as a failure — issue #166 |

### `grpc/` — OTLP/gRPC, 4 binaries

| Binary | Source | Tests |
|---|---|---|
| `conformance_grpc_basic_test` | `basic_export_test.cpp` | `Connect()` preflight; one span's full round trip |
| `conformance_grpc_batching_test` | `batching_test.cpp` | multi-RPC exactly-once, `grpc-encoding: gzip`, message spanning DATA frames |
| `conformance_grpc_tls_test` | `tls_test.cpp` | custom CA, mTLS, SNI override, and two negative cases |
| `conformance_grpc_auth_test` | `auth_test.cpp` | static bearer header, auth callback, wrong-token `grpc-status: 16` |

There is no gRPC mirror of `plaintext_gap_test.cpp`: the gRPC suite
runs over `http://` except for `tls_test.cpp`, and that is the answer
to it.

One binary per theme rather than one per directory. A conformance
binary is bounded by a ctest `TIMEOUT` covering a flush plus a poll of
the collector's output file; a single binary would make that budget the
sum of every theme's worst case.

### `support/` — 3 headers, included as `conformance/support/<name>.hpp`

| Header | What |
|---|---|
| [`conformance_env.hpp`](support/conformance_env.hpp) | `GetEnv`, `UniqueMarker` (a per-run needle), and `ConformanceEnabled` — the skip-or-fail contract below. |
| [`collector_output.hpp`](support/collector_output.hpp) | Reads back what the collector wrote: `PollForLineContaining` and `CountOccurrences`. |
| [`provider_builder.hpp`](support/provider_builder.hpp) | `ConfigureConformanceBuilder` — endpoint, protocol, and timeouts short enough to fail fast instead of becoming a ctest timeout. |

### `collector/config.yaml`

Four named `otlp` receivers, each on its own port pair, so a test picks
a transport-security posture by picking an endpoint rather than by
restarting the collector. All four feed one traces pipeline —
`batch` processor, then the `file` exporter — so every test asserts
against one output file whichever port it used.

| Receiver | gRPC | HTTP | Posture |
|---|---|---|---|
| `otlp/plain` | 4317 | 4318 | no TLS |
| `otlp/tls` | 4327 | 4328 | server cert |
| `otlp/mtls` | 4337 | 4338 | server cert + client CA |
| `otlp/auth` | 4347 | 4348 | `bearertokenauth` extension |

Also exposed: the `health_check` extension on 13133 (the runner's
readiness signal) and Prometheus telemetry on 8888 (dumped as a
diagnostic when the gate fails).

**The auth receiver is asymmetric on purpose.** Its gRPC port (4347) is
plaintext, which is the original intent: no TLS means an auth failure
cannot possibly be a trust failure. Its HTTP port (4348) carries the
same server certificate as `otlp/tls`, because microtel cannot reach a
plaintext collector HTTP receiver at all — microtel is HTTP/2-only and
that receiver is HTTP/1.1-only (issue #166,
[`docs/interop-matrix.md`](../../docs/interop-matrix.md) §4). The HTTP
auth tests buy the isolation back by pinning the correct CA in every
test, so the negative test differs from the positive ones in exactly
one field: the token.

The same gap is why every OTLP/HTTP test here except
`plaintext_gap_test.cpp` points at a TLS endpoint. Those endpoints were
not chosen to test TLS; TLS is where ALPN negotiates `h2`, which is the
only OTLP/HTTP path that reaches a stock collector today.

## Running it locally

```bash
cmake -S . -B build -DMICROTEL_BUILD_TESTS=ON
cmake --build build
ci/scripts/conformance.sh build
```

The build-dir argument defaults to `build`. The script generates a
throwaway ECDSA CA, server, client and wrong-CA certificate set under
`<build-dir>/conformance/certs`, starts the pinned collector with
`<build-dir>/conformance/out` bind-mounted, waits up to 60 s on the
health endpoint, and then runs `ctest -L conformance`.

| Exit code | Meaning |
|---|---|
| 0 | every conformance test passed |
| 1 | at least one conformance test failed |
| 2 | the gate could not run — no container engine, collector never healthy, or no test carries the `conformance` label |

Engine selection: `podman` if present, else `docker`. Two overrides:

| Variable | Effect |
|---|---|
| `MICROTEL_CONTAINER_ENGINE` | force `podman` or `docker` |
| `MICROTEL_COLLECTOR_IMAGE` | override the pinned collector image |

On a failing run the script dumps the collector's logs and its
Prometheus metrics to stderr before removing the container.

**Plain `ctest` skips this tier, by design.** The binaries are
registered like any other test, so `ctest --test-dir build` runs them —
and every test skips, because none of the environment variables below
is set. That is what keeps a developer box, and the `compile`,
`sanitizers` and `coverage` CI jobs, green without a collector.

## Environment contract

Exported by [`ci/scripts/conformance.sh`](../../ci/scripts/conformance.sh),
consumed through [`support/conformance_env.hpp`](support/conformance_env.hpp).

| Variable | Meaning |
|---|---|
| `MICROTEL_CONFORMANCE_REQUIRE` | set once the collector is healthy; turns a missing variable from a skip into a failure |
| `MICROTEL_CONFORMANCE_HTTP_ENDPOINT` | `http://127.0.0.1:4318` — plaintext, used only by the negative test |
| `MICROTEL_CONFORMANCE_GRPC_ENDPOINT` | `http://127.0.0.1:4317` — plaintext, the gRPC default |
| `MICROTEL_CONFORMANCE_HTTP_TLS_ENDPOINT` | `https://localhost:4328` |
| `MICROTEL_CONFORMANCE_GRPC_TLS_ENDPOINT` | `https://localhost:4327` |
| `MICROTEL_CONFORMANCE_HTTP_MTLS_ENDPOINT` | `https://localhost:4338` |
| `MICROTEL_CONFORMANCE_GRPC_MTLS_ENDPOINT` | `https://localhost:4337` |
| `MICROTEL_CONFORMANCE_HTTP_AUTH_ENDPOINT` | `https://localhost:4348` — `https`, per the asymmetry above |
| `MICROTEL_CONFORMANCE_GRPC_AUTH_ENDPOINT` | `http://127.0.0.1:4347` |
| `MICROTEL_CONFORMANCE_CA` | the run's CA, for `TlsOptions::ca_bundle` |
| `MICROTEL_CONFORMANCE_WRONG_CA` | an unrelated CA — the negative control for CA pinning |
| `MICROTEL_CONFORMANCE_CLIENT_CERT` | client certificate for the mTLS receivers |
| `MICROTEL_CONFORMANCE_CLIENT_KEY` | matching client key |
| `MICROTEL_CONFORMANCE_AUTH_TOKEN` | the bearer token `bearertokenauth` accepts |
| `MICROTEL_CONFORMANCE_OUTPUT_FILE` | `<build-dir>/conformance/out/traces.jsonl` |

### Why an all-skip run cannot masquerade as green

`ConformanceEnabled(env_name, out_value)` resolves one variable or
explains why the test cannot run:

```cpp
std::string endpoint;
if (!ConformanceEnabled(kHttpEndpointEnv, endpoint))
{
    GTEST_SKIP() << "collector not configured";
}
```

Outside the gate it returns false and the test skips. Inside the gate
`MICROTEL_CONFORMANCE_REQUIRE` is set, so a missing variable records an
`ADD_FAILURE()` first — broken plumbing in the runner fails loudly
instead of silently skipping everything. The runner's second guard is
independent: before exporting anything it counts
`ctest -N -L conformance` and exits 2 if the answer is zero, so a
build that registered no conformance tests cannot report success
either.

## Readback strategy

The collector runs the `file` exporter, which appends one compact
protojson object per `ResourceSpans` batch to `/out/traces.jsonl`. That
file is the only place a test can observe what the collector
*understood*, as opposed to what microtel claims it sent.

- **Positive assertions** poll that file for a line containing the
  run's `UniqueMarker()` — delivery is asynchronous, so `ForceFlush`
  returning only means microtel handed the batch over — and then assert
  substrings of that line. The expected fragments are the pinned
  image's protojson rendering: hex ids, camelCase keys, typed value
  envelopes, int64 as a quoted string. Every one was read off a real
  run against the pinned image rather than derived from the proto
  definitions, which is also why a pin bump is a procedure and not a
  version edit — see
  [`docs/interop-matrix.md`](../../docs/interop-matrix.md) §2 for the
  pin and §5 for how to move it.
- **Exactly-once assertions** use `CountOccurrences` rather than a
  presence check, so a retry the collector accepted twice fails instead
  of passing quietly.
- **Negative assertions** read `Provider::GetExporterHealth()` —
  `connection_state`, `batches_failed`, `last_error_message` — and then
  assert the marker never appears in the output file at all.

The two suites share one set of fragment constants because encoding is
protocol-independent, and that was measured rather than assumed: the
collector's output line for the OTLP/HTTP basic-export span and the one
for its OTLP/gRPC twin are byte-identical once ids, timestamps and the
per-run marker are normalised
([`docs/interop-matrix.md`](../../docs/interop-matrix.md) §3).

## Deliberately excluded

| Not here | Where instead | Why |
|---|---|---|
| Partial-success responses | [`tests/unit/wire/otlp_response_test.cpp`](../unit/wire/otlp_response_test.cpp) and the two codec tests, against hand-encoded byte fixtures | A collector configured to accept spans never returns a `partial_success` body, so this tier cannot elicit one. |
| Retry and backoff timing | [`tests/unit/exporter/retry_policy_test.cpp`](../unit/exporter/retry_policy_test.cpp), [`tests/integration/sdk/exporter_health_test.cpp`](../integration/sdk/exporter_health_test.cpp) against fakes | Needs a clock the test controls. Here the negative tests run a near-zero retry budget precisely to *avoid* the schedule. |
| Restart recovery (collector bounced mid-export) | deferred — [`docs/interop-matrix.md`](../../docs/interop-matrix.md) §6 | |
| Metrics conformance | deferred to v1.2 — [`docs/interop-matrix.md`](../../docs/interop-matrix.md) §6 | |
| Throughput and delivery rate at volume | the weekly [`interop.yml`](../../.github/workflows/interop.yml) workflow | Slow, and not a merge gate. |

## Known-defect tripwires

Five defects were found by building this tier. None of them is asserted
as desired behaviour. Each is handled one of three ways:

- **Pinned by a test that fails when the defect is fixed** — #166.
- **Parked as a `DISABLED_` assertion**, already written, waiting to be
  re-enabled — #169.
- **Left conspicuously unasserted**, with the reason recorded in the
  source — #167.
- **Fixed since** — #168, #171. The pinning test was tightened into a
  regression test rather than deleted.

Nothing here turns green by accident when a fix lands. When one of the
first kind fails, do not delete it — invert it.

| Issue | What guards it |
|---|---|
| #166 — plaintext OTLP/HTTP unreachable (h2c vs an HTTP/1.1-only receiver) | `http/plaintext_gap_test.cpp` → `PlaintextHttpUnreachable` asserts `Connect()` to `:4318` **fails**. Invert it into a positive delivery test against `MICROTEL_CONFORMANCE_HTTP_ENDPOINT` when either side gains the missing half. |
| #167 — `InstrumentationScope` carries the service name, not `GetTracer(name, version)` | Nothing in this tier asserts on `ScopeSpans.scope`. The absence is the tripwire: asserting the current output would enshrine the bug. |
| #168 — `TraceId::ToHex()` / `SpanId::ToHex()` declared in a public header, defined nowhere | **Fixed.** `src/api/` (`microtel_api`) now defines both, and `{http,grpc}/basic_export_test.cpp` call the public formatter directly — this tier builds against public headers only, so it is the thing that hit the link error and is now the thing that proves the encoding matches the collector's. |
| #169 — 22 of 24 `drop_counters` never written | `WrongTokenIncrementsNonRetryableDropCounter` in both `http/auth_test.cpp` and `grpc/auth_test.cpp`. Kept rather than deleted, and split out rather than weakening `WrongTokenRejected`, so the assertion is waiting when the counters are wired. |
| #171 — `grpc-status` and `grpc-message` discarded; `last_error_message` is a fixed literal | **Fixed.** `grpc/auth_test.cpp` now asserts both halves separately: `last_error_message` names the status (`"UNAUTHENTICATED (16)"`, matching what its HTTP sibling does with `"401"`) and carries a fragment of the collector's own `grpc-message`. The tripwire became the regression test. |
