# `tests/integration/`

Multi-component flows: real production components wired together, with
fakes or an in-process server at the outer edges. Tests here are
registered with the ctest label `integration`, and unlike unit tests
they may include headers from `src/` directly.

## What goes here and what goes in `unit/`

- `unit/` tests one type against mocked dependencies, in under 1 ms.
- `integration/` tests two or more types from `src/` together, with
  fakes only at the system boundary (sockets, the collector, the
  clock). The time budget is looser: most tests finish in well under a
  second, and a few that open real sockets take several.

If a test wires the real `src/sdk/`, `src/exporter/` and
`src/wire/encoder/` against a `FakeTransport`, it's an integration
test. If it wires `src/exporter/` against a `MockWireCodec`, it's a
unit test and belongs in `tests/unit/exporter/`.

## Layout

| Subdirectory | What it covers |
|---|---|
| `transport/` | `Http2Transport` over a real loopback socket against an in-process nghttp2 server: connect, send, TLS certificate verification (ICP 0022), and the frame-level gRPC cases (GOAWAY, RST_STREAM, split DATA frames) that `FakeTransport` cannot express. |
| `sdk/` | The export pipeline built by a real `SdkBuilder`: exporter health counters, the retry budget, auth-failure blast radius, and exemplar and log trace-correlation wiring. |
| `sugar/` | The single integration test for the v1.1 sugar layer (ICP 0028 §3): a sugar-only call tree driven through `SdkTracer` and `BatchSpanProcessor` to an exporter. |
| `otelcpp_shim/` | All three signals driven through the opentelemetry-cpp API onto a real microtel `Provider`, over loopback HTTP/2, with the captured OTLP bytes decoded and checked. |

## Rules

- Use real components with fakes at the edges. Reach for
  `FakeTransport`, `FakeReactor` and `FakeClock` rather than mocking
  individual methods.
- Every test here must be clean under ASan, TSan and UBSan (a CI gate,
  spec §13.5 and §14.2).
- Keep tests deterministic. Don't use a fixed sleep to wait for
  something to happen. Where a test does have to wait on a real socket
  or worker, it polls for the condition with a deadline.

## Collector

No test here talks to a real collector. The docker compose stack the M1
spike used was removed, and byte-level checks against a live collector
now live in [`tests/conformance/`](../conformance/).
