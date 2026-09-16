# `src/api/`

## Purpose

Implements the `Tracer` and `Span` types as seen by application code, the
W3C Trace Context propagator, and the `Context` carrier. This is the
hot-path entry point — `StartSpan`, `SetAttribute`, `AddEvent`, `End` are
all surfaced here.

The headers live in [`include/microtel/`](../../include/microtel/); this
directory holds the implementation behind the abstract base classes
declared there.

## Owner

Track A — Trace SDK (per [`docs/development.md`](../../docs/development.md) §3).

## Implements

- `microtel::TraceId::ToHex` / `microtel::SpanId::ToHex`
  ([`trace.cpp`](trace.cpp), declared in
  [`include/microtel/trace.hpp`](../../include/microtel/trace.hpp)).
- `microtel::W3CTraceContextPropagator::Inject` / `Extract`
  ([`propagator.cpp`](propagator.cpp), declared in
  [`include/microtel/propagator.hpp`](../../include/microtel/propagator.hpp)) —
  issue #188. Both `traceparent` and `tracestate` are complete.
- `microtel::TraceState` — storage, the W3C Trace Context §3.3 grammar, and
  copy-on-write `Get` / `Set` / `Erase`
  ([`trace_state.cpp`](trace_state.cpp), declared in
  [`include/microtel/trace.hpp`](../../include/microtel/trace.hpp)) — issue
  #208, [ICP 0025](../../docs/icps/0025-propagation-core.md) §1. The entry
  list is immutable behind a `shared_ptr`, which is what keeps `SpanContext`'s
  copy `noexcept`; `internal::TraceStateImpl` is defined in that one
  translation unit and nowhere else, so its layout is not part of the ABI.
- `microtel::Baggage` — the W3C Baggage grammar, percent-encoding, the opaque
  `;`-property tail, the three grammar limits, and copy-on-write
  `Get` / `Set` / `Erase`
  ([`baggage.cpp`](baggage.cpp), declared in
  [`include/microtel/baggage.hpp`](../../include/microtel/baggage.hpp)) —
  [ICP 0025](../../docs/icps/0025-propagation-core.md) §2. Same shape as
  `TraceState`: the entry list is immutable behind a `shared_ptr`, which is what
  keeps `Context`'s copy `noexcept`, and `internal::BaggageImpl` is defined in
  that one translation unit so its layout is not part of the ABI. Baggage rides
  `Context`, **never** `SpanContext` — it is per-context rather than per-span,
  and a growable member on `SpanContext` would break
  `Span::GetContext() const noexcept`.
- `microtel::CurrentContext` and `microtel::ScopedContext`
  ([`context.cpp`](context.cpp), declared in
  [`include/microtel/context.hpp`](../../include/microtel/context.hpp)) —
  issue #221, [ICP 0025](../../docs/icps/0025-propagation-core.md) §3. The one
  `thread_local Context` per thread lives in that translation unit alone, which
  is why `CurrentContext()` is deliberately **not** inline: a process that links
  `microtel_api` once cannot end up with two slots. Nesting is the C++ stack —
  each `ScopedContext` holds the value it displaced — so restore is positional
  and scopes must be destroyed in reverse order.
- `microtel::Tracer` (declared in [`include/microtel/tracer.hpp`](../../include/microtel/tracer.hpp))
- `microtel::Span`   (declared in [`include/microtel/span.hpp`](../../include/microtel/span.hpp))
- The unsampled-`Span` no-op singleton + `internal::SpanDeleter` per
  [ICP 0003 §3.2](../../docs/icps/0003-m0-deferred-decisions.md#32-unsampled-span-shape--unique_ptrspan-to-no-op-singleton)

## Depends on

- `ISpanProcessor` (mock at [`tests/mocks/mock_span_processor.hpp`](../../tests/mocks/))
- `ISampler`       (mock at [`tests/mocks/mock_sampler.hpp`](../../tests/mocks/))
- `IDiagnosticsSink` (fake at [`tests/fakes/fake_diagnostics_sink.hpp`](../../tests/fakes/))
- `IClock`         (fake at [`tests/fakes/fake_clock.hpp`](../../tests/fakes/))

## Test entry points

- `tests/unit/api/`   — gtest unit tests, one file per public type.
- `tests/integration/sdk/` — flows that drive the SDK end-to-end against fakes.

## Style notes

- **Hot path is `noexcept`** (LOCKED — `docs/threading-model.md` §8).
  Every method on `Tracer` and `Span` listed in the public header must be
  declared and implemented `noexcept`. Anything that would unwind is
  caught at the boundary and converted to drop-and-count.
- **Zero allocation on the unsampled path** (LOCKED — `memory-model.md` §8.1).
  The unsampled `Span` is a static singleton; the `SpanDeleter`'s no-op
  branch must not invoke `operator delete`. Verify with a sanitizer
  build.
- **No I/O** in any caller-thread method. No syscalls beyond
  `clock_gettime(CLOCK_MONOTONIC)` (treated as effectively non-blocking).
- **Threading category:** `Tracer` is thread-safe; `Span` is externally
  synchronised per-instance. Doxygen tags must say so.
