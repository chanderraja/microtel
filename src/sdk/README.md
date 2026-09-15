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
  [`sampler_chains.cpp`](sampler_chains.cpp))
- `internal::ISpanProcessor` realisations: `BatchSpanProcessor`,
  `SimpleSpanProcessor`
- `internal::IResourceDetector` — the `process` and `host` detectors in
  [`resource_detectors.cpp`](resource_detectors.cpp), behind the public
  `MakeProcessDetector` / `MakeHostDetector` factories. The spec §12.7
  composition that merges them with the config lives in
  [`resource_builder.cpp`](resource_builder.cpp); k8s and cloud
  detectors are future work
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
  `ShouldSample` to prove it stays that way.
- **Provider holds a `unique_ptr<SslCtx>` indirectly via `Transport`**
  per ICP 0003 §3.1 — no shared ownership of TLS state.
