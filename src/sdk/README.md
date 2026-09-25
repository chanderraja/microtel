# `src/sdk/`

## What lives here

The SDK behind the public API, built as `microtel_sdk`: `SdkBuilder` and the
`Provider` lifecycle (`ForceFlush`, `Shutdown`, the v1.1 setters), `Resource`
resolution, the samplers, the tracer and span, the span and log-record
processors, the metrics pipeline, and the multi-profile registry. It is the
largest directory in `src/`; the list below is grouped by signal.

## Owner

Track A — Trace SDK.

## What it implements

Provider and builder:

- `microtel::Provider` and `SdkBuilder` (declared in
  [`provider.hpp`](../../include/microtel/provider.hpp) and
  [`sdk_builder.hpp`](../../include/microtel/sdk_builder.hpp)), as
  `SdkProvider` in [`sdk_provider.cpp`](sdk_provider.cpp) and
  [`sdk_builder.cpp`](sdk_builder.cpp).
- The four hot-reload setters on `Provider`: `SetBatchOptions`,
  `SetMetricInterval`, `SetSamplerRatio`, `SetLogLevel`. They live in
  [`sdk_provider.cpp`](sdk_provider.cpp), with the concrete `SetOptions` /
  `SetInterval` they drive on the batch processors and the periodic reader.
- The multi-profile registry in
  [`provider_registry.{hpp,cpp}`](provider_registry.hpp), which also defines
  the public free function `microtel::GetProvider`. It is a fixed array of
  `kMaxProfiles = 8` atomic slots, claimed by `SdkBuilder::Build` after
  construction and released at the top of `~SdkProvider`. Every operation is
  lock-free and allocation-free, because the `pthread_atfork` child handler is
  registered here and walks the array, calling
  `SdkProvider::MarkForkedChild` on each live provider
  ([ICP 0027](../../docs/icps/0027-multi-profile-threading.md)).
- `internal::IDiagnosticsSink` as `DiagnosticsCounters`
  ([`diagnostics_counters.hpp`](diagnostics_counters.hpp)): atomic per-reason
  counters plus a mutex-guarded last-error slot, and the `HealthSnapshot`
  behind `GetExporterHealth()`.

Resource:

- `microtel::Resource` and the resource-merging pipeline (spec §12.7).
- `internal::IResourceDetector`: the `process` and `host` detectors in
  [`resource_detectors.cpp`](resource_detectors.cpp), behind the public
  `MakeProcessDetector` / `MakeHostDetector` factories. The spec §12.7
  composition that merges them with the config lives in
  [`resource_builder.cpp`](resource_builder.cpp), which also logs the
  resolved Resource once per `Build()` at `Info` (sorted, escaped, redacted, capped;
  issue #284). k8s and cloud detectors are future work.

Traces:

- `microtel::Tracer` and `microtel::Span` as `SdkTracer` and `SdkSpan`
  ([`sdk_tracer.cpp`](sdk_tracer.cpp), [`sdk_span.cpp`](sdk_span.cpp)).
- The unsampled-`Span` no-op singleton and the out-of-line
  `internal::SpanDeleter::operator()` in [`noop_span.cpp`](noop_span.cpp), per
  [ICP 0003 §3.2](../../docs/icps/0003-m0-deferred-decisions.md#32-unsampled-span-shape--unique_ptrspan-to-no-op-singleton).
- `internal::ISampler` realisations: `AlwaysOnSampler`, `AlwaysOffSampler`,
  `TraceIdRatioSampler` and `ParentBasedSampler` (all in
  [`sampler_factories.cpp`](sampler_factories.cpp)), plus the v1.1 rule
  combinators and chain composition modes `AttributeRuleSampler`,
  `SpanNameRuleSampler`, `SpanKindRuleSampler` and `ChainSampler` (in
  [`sampler_chains.cpp`](sampler_chains.cpp)). They all use the
  `DescriptionSlot` in [`sampler_description.hpp`](sampler_description.hpp)
  for the one thing `ISampler::TrySetRatio` makes hard: publishing a new
  `Description()` without invalidating a `string_view` already handed out
  ([ICP 0026](../../docs/icps/0026-provider-setters.md) §5).
- `internal::ISpanProcessor` realisations: `BatchSpanProcessor` and
  `SimpleSpanProcessor`.
- `internal::ICurrentSpanSource` as `CurrentSpanSource` in
  [`current_span_source.hpp`](current_span_source.hpp). It reads the API's
  thread-local context slot and applies the `trace_based` filter (valid and
  sampled, otherwise an invalid context). `SdkProvider` owns one and lends it
  to every `SdkMeter` (as `StorageOptions::span_source`, which turns on the
  exemplar reservoirs) and every `SdkLogger` (log/trace correlation). See
  issue #221 and [ICP 0025](../../docs/icps/0025-propagation-core.md) §3.

Metrics ([`docs/metrics-design.md`](../../docs/metrics-design.md)):

- `SdkMeter` and the instrument types (`meter_instruments.hpp`,
  `metric_observable_instruments.hpp`).
- Per-aggregation storage: `SumStorage`, `GaugeStorage`, `HistogramStorage`,
  `ExponentialHistogramStorage`, keyed by `AttributeSet`.
- `MetricProducer`, `ViewRegistry`, and the two readers,
  `PeriodicExportingMetricReader` and `SynchronousMetricReader`.

Logs ([`docs/logs-design.md`](../../docs/logs-design.md)):

- `SdkLogger` and `NoopLogger`.
- `internal::ILogRecordProcessor` realisations: `BatchLogRecordProcessor` and
  `SimpleLogRecordProcessor`.

## Dependencies and test doubles

- `IExporter`, `IMetricExporter`, `ILogExporter`: mocks
  `mock_exporter.hpp`, `mock_metric_exporter.hpp`, `mock_log_exporter.hpp` in
  [`tests/mocks/`](../../tests/mocks/); fakes `fake_exporter.hpp` and
  `fake_log_exporter.hpp` in [`tests/fakes/`](../../tests/fakes/).
- `ISpanProcessor` and `ISampler`: `mock_span_processor.hpp`,
  `mock_sampler.hpp`, and `fake_span_processor.hpp`.
- `IDiagnosticsSink`: `fake_diagnostics_sink.hpp`.
- `IResourceDetector`: `fake_resource_detector.hpp`.
- The `Config` value from [`src/common/config/`](../common/config/), built
  directly in tests; there is no interface.
- `SdkBuilder` pulls in the exporter, encoder, wire codec and transport
  directories to assemble the real pipeline.

The span queue in `BatchSpanProcessor` is a `std::deque` guarded by one mutex.
[ICP 0003 §3.3](../../docs/icps/0003-m0-deferred-decisions.md#33-mpsc-queue-shape--stays-deferred)
left the structure as an M3 implementation choice.

## Tests

- `tests/unit/sdk/`: one file per type. The samplers, processors, metric
  storages and readers, logger, registry and builder each have their own.
- `tests/unit/sdk/provider_registry_test.cpp` and
  `provider_registry_race_test.cpp` cover the registry below the line and
  `WithProfileName` / `GetProvider` above it, plus the TSAN hammer.
  `tests/unit/sdk/fork_safety_test.cpp` covers the child handler, including
  the multi-provider case the old single slot could not serve.
- `tests/unit/sdk/hot_reload_hammer_test.cpp` and `provider_setters_test.cpp`
  cover the setters; `tests/fuzz/provider_setters_fuzz.cpp` fuzzes their
  validation.
- `tests/integration/sdk/exemplar_wiring_test.cpp`: the two
  `ICurrentSpanSource` seams driven through a real `SdkProvider`.
- `tests/integration/sdk/`: `retry_budget_test.cpp`,
  `auth_failure_export_test.cpp` and `exporter_health_test.cpp` run the SDK
  end-to-end against fakes.
- `tests/conformance/`: against a real OpenTelemetry Collector (shared
  with `src/exporter/`).

## Style notes

The public hot-path contract for `Tracer` and `Span` (`noexcept`, zero
allocation on the unsampled path, no I/O on the caller thread) is written down
in [`src/api/README.md`](../api/README.md); `SdkTracer`, `SdkSpan` and the
no-op span here are what have to honour it.

- **Worker thread is owned here** (`BatchSpanProcessor` per
  `docs/threading-model.md` §2.2). `Shutdown` joins it, and the destructor
  calls `Shutdown` with a small finite timeout if that has not happened yet.
- **`OnEnd` is `noexcept`**. It drops the record on a full queue and never
  throws.
- **Sampler hot path must not allocate** in the default case (LOCKED —
  `memory-model.md` §8.1, ICP 0003 §3.2). The chain combinators keep that
  promise by fixing everything at construction: the child vector is sized
  once, each child's dynamic type is resolved once, and the description is
  formatted once. `tests/unit/sdk/sampler_chain_alloc_test.cpp` counts
  allocations around `ShouldSample` to keep it that way. `TrySetRatio` keeps
  the same promise from the other side. The ratio sampler's decision is one
  relaxed atomic load, with the always-sample case folded into the threshold
  sentinel instead of a second, separately readable field.
- **A setter takes at most one non-leaf lock, and never across a call-out**
  (`docs/threading-model.md` §4 rule 2). That is why `SetBatchOptions` runs in
  two phases rather than one nested one, and why a composite sampler
  recomposes its description after forwarding to its delegates. The TSAN
  hammer in `tests/unit/sdk/hot_reload_hammer_test.cpp` checks it;
  `MICROTEL_HAMMER_SECONDS` extends its default budget for a local run.
- **An instrument must not be used after the provider that owns it is
  destroyed.** `SdkMeter` borrows the diagnostics sink and the
  `ICurrentSpanSource` from the provider by raw pointer
  ([`sdk_meter.hpp`](sdk_meter.hpp)).
- **Provider holds a `unique_ptr<SslCtx>` indirectly via `Transport`**
  (ICP 0003 §3.1), so there is no shared ownership of TLS state. There is one
  per transport, which means a process running several named profiles has one
  per profile.
- **The registry must stay lock-free**, everywhere, because the fork child
  handler walks it. A mutex held at `fork()` time by a thread that does not
  exist in the child would hang the child's re-`Build()`, the one recovery
  `docs/threading-model.md` §7 supports. A slot holds a bare `SdkProvider*` and
  nothing else. The profile name lives in the provider, so the handler never
  touches a `std::string` and the registry never allocates.
  `tests/unit/sdk/provider_registry_race_test.cpp` is the TSAN hammer that
  checks it.
