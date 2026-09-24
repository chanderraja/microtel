# `src/api/`

## What lives here

The out-of-line parts of the public API that do not need an SDK behind them:
trace and span IDs, `TraceState`, W3C Baggage, the per-thread `Context`
carrier, and the W3C Trace Context propagator. The headers are in
[`include/microtel/`](../../include/microtel/); this directory builds them into
`microtel_api`, which links nothing but `microtel_headers`.

`Tracer` and `Span` are abstract classes in the public headers. Their
implementations (`SdkTracer`, `SdkSpan`, and the unsampled no-op span with
the out-of-line `internal::SpanDeleter`, per
[ICP 0003 §3.2](../../docs/icps/0003-m0-deferred-decisions.md#32-unsampled-span-shape--unique_ptrspan-to-no-op-singleton))
live in [`src/sdk/`](../sdk/). The hot-path rules they must follow are listed
under style notes below.

## Owner

Track A — Trace SDK ([`docs/development.md`](../../docs/development.md) §3).

## What it implements

- `microtel::TraceId::ToHex` and `microtel::SpanId::ToHex` in
  [`trace.cpp`](trace.cpp), declared in
  [`include/microtel/trace.hpp`](../../include/microtel/trace.hpp).
- `microtel::W3CTraceContextPropagator::Inject` / `Extract` in
  [`propagator.cpp`](propagator.cpp), declared in
  [`include/microtel/propagator.hpp`](../../include/microtel/propagator.hpp)
  (issue #188). Both `traceparent` and `tracestate` are complete.
- `microtel::TraceState` in [`trace_state.cpp`](trace_state.cpp), declared in
  [`include/microtel/trace.hpp`](../../include/microtel/trace.hpp): storage,
  the W3C Trace Context §3.3 grammar, and copy-on-write `Get` / `Set` / `Erase`
  (issue #208, [ICP 0025](../../docs/icps/0025-propagation-core.md) §1). The
  entry list is immutable behind a `shared_ptr`, which is what keeps
  `SpanContext`'s copy `noexcept`. `internal::TraceStateImpl` is defined in that
  one translation unit and nowhere else, so its layout is not part of the ABI.
- `microtel::Baggage` in [`baggage.cpp`](baggage.cpp), declared in
  [`include/microtel/baggage.hpp`](../../include/microtel/baggage.hpp): the W3C
  Baggage grammar, percent-encoding, the opaque `;`-property tail, the three
  grammar limits, and copy-on-write `Get` / `Set` / `Erase`
  ([ICP 0025](../../docs/icps/0025-propagation-core.md) §2). It has the same
  shape as `TraceState`. The entry list is immutable behind a `shared_ptr`,
  which keeps `Context`'s copy `noexcept`, and `internal::BaggageImpl` stays in
  its one translation unit so its layout is not part of the ABI. Baggage rides
  on `Context` and never on `SpanContext`: it belongs to a context rather than
  a span, and a growable member on `SpanContext` would break
  `Span::GetContext() const noexcept`.
- `microtel::CurrentContext` and `microtel::ScopedContext` in
  [`context.cpp`](context.cpp), declared in
  [`include/microtel/context.hpp`](../../include/microtel/context.hpp)
  (issue #221, [ICP 0025](../../docs/icps/0025-propagation-core.md) §3). The
  single `thread_local Context` per thread lives in that translation unit
  alone, which is why `CurrentContext()` is deliberately not inline: a process
  that links `microtel_api` once cannot end up with two slots. Nesting follows
  the C++ stack. Each `ScopedContext` holds the value it displaced, so restore
  is positional and scopes must be destroyed in reverse order.

## Dependencies and test doubles

Only the public headers in `include/microtel/`. None of the internal
interfaces are used here, so there is nothing to mock for this directory's own
code. `tests/unit/api/meter_api_test.cpp` is the exception among the tests: it
exercises the meter API through a real `SdkProvider` built on
`mock_exporter.hpp`, `mock_span_processor.hpp` and `mock_transport.hpp` from
[`tests/mocks/`](../../tests/mocks/).

## Tests

- `tests/unit/api/`: gtest unit tests, one file per public type
  (`trace_id_test.cpp`, `trace_state_test.cpp`, `baggage_test.cpp`,
  `baggage_propagator_test.cpp`, `propagator_test.cpp`, `context_test.cpp`,
  and others).
- `tests/fuzz/baggage_fuzz.cpp`: the W3C `baggage` header parser.

## Style notes

The first four rules are the contract of the public `Tracer` / `Span` API.
They bind the implementations in [`src/sdk/`](../sdk/), and other hot-path
APIs (the metrics record calls, for one) point back here for them.

- **Hot path is `noexcept`** (LOCKED — `docs/threading-model.md` §8).
  Every method on `Tracer` and `Span` listed in the public header must be
  declared and implemented `noexcept`. Anything that would unwind is caught
  at the boundary and converted to drop-and-count.
- **Zero allocation on the unsampled path** (LOCKED — `memory-model.md` §8.1).
  The unsampled `Span` is a static singleton
  ([`src/sdk/noop_span.cpp`](../sdk/noop_span.cpp)), and the no-op branch of
  `internal::SpanDeleter` must not invoke `operator delete`. Verify with a
  sanitizer build.
- No I/O in any caller-thread method, and no syscalls beyond
  `clock_gettime(CLOCK_MONOTONIC)`, which is treated as effectively
  non-blocking.
- `Tracer` is thread-safe; `Span` is externally synchronised per instance.
  The Doxygen threading tags must say so.
- Copying `SpanContext` and `Context` must stay `noexcept` and allocation-free.
  That is why `TraceState` and `Baggage` share an immutable entry list rather
  than owning a growable container.
- The implementation types (`TraceStateImpl`, `BaggageImpl`) and the
  thread-local context slot stay inside their own `.cpp` files. Moving any of
  them into a header puts their layout, or a second slot, into the ABI.
