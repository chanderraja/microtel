# `src/sdk/`

## Purpose

The minimal v1 SDK: `Resource` resolution, the four built-in samplers
(`AlwaysOn`, `AlwaysOff`, `TraceIdRatio`, `ParentBased`),
`BatchSpanProcessor`, `SimpleSpanProcessor`, and the `Provider` lifecycle
(`ForceFlush`, `Shutdown`). `SdkBuilder`'s implementation also lives here.

## Owner

Track A — Trace SDK.

## Implements

- `microtel::Provider` and `SdkBuilder` (declared in [`provider.hpp`](../../include/microtel/provider.hpp), [`sdk_builder.hpp`](../../include/microtel/sdk_builder.hpp))
- `microtel::Resource` and the resource-merging pipeline (spec §12.7)
- `internal::ISampler` realisations: `AlwaysOnSampler`,
  `AlwaysOffSampler`, `TraceIdRatioSampler`, `ParentBasedSampler` (all in
  [`sampler_factories.cpp`](sampler_factories.cpp)), plus the v1.1 rule
  combinators and chain composition modes — `AttributeRuleSampler`,
  `SpanNameRuleSampler`, `SpanKindRuleSampler`, `ChainSampler` (in
  [`sampler_chains.cpp`](sampler_chains.cpp)). All of them share
  [`sampler_description.hpp`](sampler_description.hpp)'s `DescriptionSlot`
  for the one thing `ISampler::TrySetRatio` makes hard: publishing a new
  `Description()` without invalidating a `string_view` already handed out
  ([ICP 0026](../../docs/icps/0026-provider-setters.md) §5)
- The four hot-reload setters on `Provider` — `SetBatchOptions`,
  `SetMetricInterval`, `SetSamplerRatio`, `SetLogLevel` (in
  [`sdk_provider.cpp`](sdk_provider.cpp), with the concrete `SetOptions` /
  `SetInterval` they drive on the batch processors and the periodic reader)
- `internal::ISpanProcessor` realisations: `BatchSpanProcessor`,
  `SimpleSpanProcessor`
- `internal::IResourceDetector` — the `process` and `host` detectors in
  [`resource_detectors.cpp`](resource_detectors.cpp), behind the public
  `MakeProcessDetector` / `MakeHostDetector` factories. The spec §12.7
  composition that merges them with the config lives in
  [`resource_builder.cpp`](resource_builder.cpp); k8s and cloud
  detectors are future work
- `internal::ICurrentSpanSource` — `CurrentSpanSource` in
  [`current_span_source.hpp`](current_span_source.hpp), which reads the API's
  thread-local context slot and applies the `trace_based` filter (valid **and**
  sampled, else an invalid context). `SdkProvider` owns one and lends it to
  every `SdkMeter` (as `StorageOptions::span_source`, which turns on the
  exemplar reservoirs) and every `SdkLogger` (log↔trace correlation) —
  issue #221, [ICP 0025](../../docs/icps/0025-propagation-core.md) §3
- The diagnostics sink (`internal::IDiagnosticsSink`)

## Depends on

- `IExporter`     (mock at [`tests/mocks/mock_exporter.hpp`](../../tests/mocks/))
- `IClock` / `ISteadyClock` (fakes at [`tests/fakes/`](../../tests/fakes/))
- `Config` value (built directly in tests; no interface)
- The MPSC queue implementation chosen in M3 per
  [ICP 0003 §3.3](../../docs/icps/0003-m0-deferred-decisions.md#33-mpsc-queue-shape--stays-deferred)

## Test entry points

- `tests/unit/sdk/` — one file per type. `BatchSpanProcessor` gets
  several files (timing, drop policy, shutdown).
- `tests/integration/sdk/exemplar_wiring_test.cpp` — the two
  `ICurrentSpanSource` seams driven through a real `SdkProvider`.
- `tests/integration/sdk_export_pipeline/` — end-to-end against fakes.
- `tests/conformance/` — against a real OpenTelemetry Collector (shared
  with `src/exporter/`).

## Style notes

- **Worker thread is owned here** (`BatchSpanProcessor` per
  `docs/threading-model.md` §2.2). Joined by `Shutdown`. Destructor
  invokes `Shutdown(small_finite_timeout)` if not already shut down.
- **`OnEnd` is `noexcept`** — drops record on full queue, never throws.
- **Sampler hot path must not allocate** in the default case (LOCKED —
  `memory-model.md` §8.1, ICP 0003 §3.2). The chain combinators keep that
  by fixing everything at construction — child vector sized once, each
  child's dynamic type resolved once, description formatted once — and
  `tests/unit/sdk/sampler_chain_alloc_test.cpp` counts allocations around
  `ShouldSample` to prove it stays that way. `TrySetRatio` keeps the same
  promise from the other side: the ratio sampler's decision is one relaxed
  atomic load, with the always-sample case folded into the threshold
  sentinel rather than kept in a second, separately-readable field.
- **A setter takes at most one non-leaf lock, and never across a call-out**
  (`docs/threading-model.md` §4 rule 2). That is why `SetBatchOptions` is two
  phases rather than one nested one, and why a composite sampler recomposes
  its description *after* forwarding to its delegates. The TSAN hammer in
  `tests/unit/sdk/hot_reload_hammer_test.cpp` is what keeps it honest;
  `MICROTEL_HAMMER_SECONDS` extends its default budget for a local run.
- **Provider holds a `unique_ptr<SslCtx>` indirectly via `Transport`**
  per ICP 0003 §3.1 — no shared ownership of TLS state.
