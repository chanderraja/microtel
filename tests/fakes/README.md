# `tests/fakes/`

**Logic-bearing** test doubles. Used when a mock isn't enough.

## What lives here

| Fake | Interface | Why a fake (not a mock) |
|---|---|---|
| `fake_clock.hpp`              | `internal::IClock`           | Tests advance time on demand; the clock must respond consistently to `Now()` calls between advances. |
| `fake_steady_clock.hpp`       | `internal::ISteadyClock`     | Same. |
| `fake_transport.hpp`          | `internal::ITransport`       | In-memory loopback against a `FakeServer` that scripts protocol-level responses. |
| `fake_reactor.hpp`            | `internal::IReactor`         | Tests script event timelines; the fake dispatches them on registered callbacks deterministically. |
| `fake_diagnostics_sink.hpp`   | `internal::IDiagnosticsSink` | Stores counters as plain `uint64_t` and exposes them for assertions. |
| `fake_auth_provider.hpp`      | `internal::IAuthProvider`    | TTL-cache simulation: tests configure cache lifetime + miss/hit sequences. |
| `fake_resource_detector.hpp`  | `internal::IResourceDetector` | Returns a configured `Resource`; trivial but logically distinct from a mock. |
| `fake_span_processor.hpp`     | `internal::ISpanProcessor`   | Stores received spans in a vector for inspection. |
| `fake_exporter.hpp`           | `internal::IExporter`        | Records batches in memory and exposes them. |

## Bar

- **Logic is OK.** That's what distinguishes a fake from a mock.
- **Deterministic.** No real clocks, no real I/O, no sleeps. Tests
  drive time, events, and responses explicitly.
- **No threads.** The fake clock and reactor exist precisely so tests
  can avoid real concurrency. If a fake needs to spawn a thread, the
  design is wrong.
- **Test-only by name.** Lives in `microtel::testing` namespace; never
  exposed in production headers.

## Naming

`Fake<InterfaceName>` — `FakeClock`, `FakeTransport`, `FakeReactor`.
Same namespace + naming as `tests/mocks/`.

## Promotion path

Sometimes a mock starts simple, then a test needs scripted behaviour,
then another needs more, and the mock accretes logic. When that
happens, **promote to a fake**. Don't grow logic on a mock; the mock
class becomes a fake and the file moves from `tests/mocks/` to
`tests/fakes/`.

The reverse promotion (fake → mock) doesn't happen — once logic exists
in a test double, it stays a fake even if some tests configure the
fake to be a no-op.
