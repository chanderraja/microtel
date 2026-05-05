# `tests/integration/`

Multi-component flows. Real production components wired together,
talking to fakes (and sometimes a containerised collector) at the
outer edges.

## What goes here vs. unit/

- **`unit/`** — one type, mocked dependencies, < 1 ms.
- **`integration/`** — two or more types from `src/`, with fakes only
  at the system boundary (sockets, the collector, the clock).
  Per-test budget is forgiving (up to ~1 s).

If the test wires real `src/sdk/` + real `src/exporter/` + real
`src/wire/encoder/` against a `FakeTransport`, it's integration. If it
wires `src/exporter/` against `MockWireCodec`, it's unit (in
`tests/unit/exporter/`).

## Suggested subdirectories

These are conventional names; create as the work lands:

| Subdirectory | Theme |
|---|---|
| `sdk_export_pipeline/` | Span end-to-end through real SDK + exporter against fakes. |
| `transport_loopback/`  | Real transport against an in-process server. |
| `transport_goaway/`    | GOAWAY mid-batch, RST_STREAM, reconnect. |
| `lifecycle/`           | `ForceFlush` / `Shutdown` timeout, idempotency, destructor-safety. |
| `fork/`                | `fork()` boundary: child observes `m_state = Closed`, re-init works. |
| `backpressure/`        | Multi-producer queue overflow, drop-newest vs drop-oldest. |
| `partial_success/`     | Partial-success response with rejected items, never retried. |

## Bar

- **Real components, fakes at the edges.** Use `FakeTransport`,
  `FakeReactor`, `FakeClock` rather than mocking individual methods.
- **Sanitizer-clean.** ASan + TSan + UBSan all green on every test
  here (CI gate per spec §13.5 / §14.2).
- **Deterministic.** Use fakes that advance on demand; no sleeps.
  Sleeps in tests are a code smell.

## Local collector

Some integration tests need a real collector for byte-level
verification. The collector spins up via the docker compose stack the
M1 spike used (now removed); reintroduce a similar
`tests/integration/docker/` if needed during M3.
