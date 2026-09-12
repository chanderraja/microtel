# ICP 0023: `ISpanProcessor::OnEnd` carries the `InstrumentationScope`

**Status:** Accepted — signed off 2026-09-12; applied in the PR that follows
this ICP.
**Affected interfaces / docs:** [`include/microtel/internal/processor.hpp`](../../include/microtel/internal/processor.hpp)
(`ISpanProcessor::OnEnd`, LOCKED in [`docs/interfaces.md`](../interfaces.md)
§4.6), §4.6's contract block and prose, and the `End()` allocation bullet in
[`docs/memory-model.md`](../memory-model.md) §8.2. No public API changes;
`BatchHandle`, `IExporter`, and the encoder are untouched.
**Affected tracks:** Track A (`src/sdk/`, `tests/mocks/`, `tests/fakes/`).

## Summary

`OnEnd` gains a second parameter — the `InstrumentationScope` of the tracer
that produced the span — so the scope the caller passed to
`Provider::GetTracer(name, version)` reaches the wire instead of the service
name from builder config.

## Motivation

Issue #167. `SdkProvider::GetTracer` builds the correct
`InstrumentationScope{name, version}` and hands it to `SdkTracer`, which hands
it to every `SdkSpan`. `SdkSpan::m_scope` is then **never read**:
`internal::SpanRecord` has no scope field and `OnEnd` takes only the record.
Meanwhile `SdkBuilder::Build` constructs a *different* scope from
`cfg.service_name` / `cfg.service_version` and passes it to the
`BatchSpanProcessor` constructor, which stamps it on every batch it exports.

The result is a data-model error, not a cosmetic one: `ScopeSpans.scope`
carries the service identity, which per the OTel data model belongs only in the
`Resource`, and the instrumentation scope is unrecoverable downstream. The
conformance suite observed `GetTracer("microtel.conformance.http", "1.0")`
arriving at a real collector as `"scope":{"name":"microtel-conformance"}`, with
no version at all. Spans from different tracers are also merged into one
`ScopeSpans` entry, which the model forbids.

The same defect does not exist on the other two signals. Logs already pass the
scope through `ILogRecordProcessor::OnEmit(LogRecord&&, const
InstrumentationScope&)` and group by scope at drain; metrics group per scope in
`MetricProducer`. Traces are the odd one out, and the fix is to make traces
look like logs.

## Proposed change

1. **`include/microtel/internal/processor.hpp`** — `OnEnd` becomes
   `virtual void OnEnd(SpanRecord&& record, const InstrumentationScope& scope) noexcept = 0;`,
   mirroring `ILogRecordProcessor::OnEmit` exactly.

2. **`src/sdk/sdk_span.cpp`** — `End()` passes `m_scope`. The dead field
   becomes live; nothing else about `SdkSpan` changes.

3. **`src/sdk/batch_span_processor.{hpp,cpp}`** — the queue element becomes
   `QueuedSpan { SpanRecord record; InstrumentationScope scope; }`, the
   `m_scope` member and the constructor's scope parameter are removed, and
   `ExportBatch` groups the drained records by `(name, version)` in first-seen
   order, emitting one `BatchHandle` per group. This is a transcription of
   `BatchLogRecordProcessor::ExportBatch`. The exporter already accepts N
   batches per drain (`OtlpExporter::FanOutAndProcess`).

4. **`src/sdk/simple_span_processor.{hpp,cpp}`** — same signature change; the
   scope for the one-span batch comes from the parameter. Its `m_scope` member
   and constructor parameter go away.

5. **`src/sdk/sdk_builder.cpp`** — the `InstrumentationScope` built from
   `cfg.service_name` / `cfg.service_version` is deleted outright.
   `cfg.service_name` continues to reach the `Resource` through
   `BuildResource`, which is where it belongs.

6. **`docs/interfaces.md`** §4.6 — contract block and prose updated in the same
   commit as this ICP. §3.3's claim that a `BatchHandle` holds records "plus the
   `Resource` and `InstrumentationScope` they share" stops being aspirational
   and becomes enforced by the grouping in step 3.

### Allocation note

`OnEnd`'s documented allocation behaviour is "queue push only"
(`memory-model.md` §8.2). A queued span now costs one extra
`InstrumentationScope` — two `std::string` copies — taken on the caller thread
immediately before the push, so the contract holds in spirit: the copy is
bounded, it is not a batch-shaped allocation, and it happens in the same
critical section the push already occupied. Scope names and versions are short
and almost always fit `std::string`'s SSO buffer, and the worst case is bounded
by `max_queue_size` (default 8192) × 2 strings. `QueuedLog` made the identical
trade in the logs pipeline; accepting it here keeps the two pipelines
comparable rather than buying a marginal allocation saving with a divergent
design.

## Migration

`ISpanProcessor` is locked, so every implementation must change signature.
In-tree that is `BatchSpanProcessor`, `SimpleSpanProcessor`,
`tests/mocks/mock_span_processor.hpp`, and
`tests/fakes/fake_span_processor.hpp` (the fake records the scope of each span
it receives, so tests can assert on it). The two processors' constructors lose
their `InstrumentationScope` parameter, which is a source-breaking change for
any test that constructs one directly.

Nothing outside the SDK is affected: the interface is internal, the public API
is unchanged, and the otel-cpp shim (ICP 0014) goes through `Provider` /
`Tracer`, not through `ISpanProcessor`.

Observable behaviour changes for every user: exported spans now carry the
`GetTracer` name and version, and spans from different tracers land in separate
`ScopeSpans` entries (and, at the exporter, separate batches — one export
request per scope per drain, where previously one drain was always one
request). Anything downstream that keyed off the scope name being the service
name must read `service.name` from the `Resource` instead.

### Deferred: `schema_url`

`ScopeSpans.schema_url` and `InstrumentationScope.attributes` stay unset.
`Provider::GetTracer` has no `schema_url` parameter and
`internal::InstrumentationScope` has no field to hold one, so supporting them
is a public-API change with its own encoder work. **Out of scope for this ICP**
— a future one can add the parameter and the field together. This ICP only
moves the scope that already exists to where the data model says it goes.

## Rationale & alternatives

- **Put the scope on `SpanRecord`** — rejected. It puts a per-tracer constant
  on a per-span struct, paying the two string copies whether or not the
  processor batches, and it diverges from the logs design for no gain. The
  `QueuedSpan` pairing keeps the cost inside the one processor that needs a
  queue.
- **Have the tracer hand the processor an interned scope handle** — rejected as
  premature. It is the right answer if scope copying ever shows up in a
  profile, and it can be added later without touching this signature, since
  `OnEnd` takes the scope by `const&` either way.
- **Group by scope in the exporter instead of the processor** — rejected. The
  exporter's input is a `BatchHandle`, which is defined as records sharing one
  scope (§3.3); moving grouping downstream would mean weakening that invariant
  rather than finally enforcing it.
- **Keep the builder's scope as a fallback when `GetTracer` is given an empty
  name** — rejected. An empty instrumentation-scope name is a legitimate,
  representable value in the data model; substituting the service name for it
  is the bug this ICP is fixing, in miniature.
