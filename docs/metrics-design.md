# microtel Metrics Design

**Status:** Draft — M11 skeleton. **Not yet signed off.** No implementation
until this document is complete and a reviewer has approved it (per
`microtel-roadmap.md` §1.2 and `microtel-spec.md` §M11). Implementation is M12.

This document settles the semantic decisions that v1.0 deliberately deferred so
that the metrics signal (v1.2) can be implemented against a locked design, the
same way `docs/interfaces.md` locked the trace contracts before M3. It is a
design document, not an interface header: it decides *what* and *why*; the
concrete `include/microtel/` headers it implies are drafted during M12 and, if
they touch any locked surface, follow the ICP process.

## Scope

In scope for v1.2 (this doc must decide all of these):

- Synchronous instruments: `Counter`, `UpDownCounter`, `Gauge`, `Histogram`.
- Asynchronous instruments: `ObservableCounter`, `ObservableUpDownCounter`,
  `ObservableGauge`.
- `MeterProvider` / `Meter` surface and lifecycle.
- `MetricReader` / `MetricExporter` pipeline reusing the existing OTLP encoder
  (`src/wire/encoder/`) and nghttp2 transport (`src/transport/`).
- Aggregation temporality (delta / cumulative), per-metric configurable.
- Cardinality limits with the OTel overflow attribute and drop accounting.
- Views (basic: rename, attribute filter, aggregation override).
- Exemplars linked to active span context.
- `mt::Timer` sugar wired into histograms (completes the v1.1-deferred piece).

Explicitly **out of scope** for v1.2 (anti-goals; do not design here):

- Logs (v1.3).
- Advanced exemplar storage/formats beyond span-context linkage.
- Metric backfill / persistence.
- Full Views (regex matchers, full selector grammar) — deferred to v1.4.

## Sign-off checklist

Each item below must reach a concrete **Decision** (not "TBD") before sign-off.
Reviewer approves by checking every box.

- [ ] §1 Aggregation temporality (delta vs cumulative; default; per-metric override)
- [ ] §2 Cardinality limits (limit, overflow attribute, drop accounting)
- [ ] §3 Histogram bucket configuration (default boundaries; per-view override; exponential?)
- [ ] §4 Async-instrument callback semantics (registration, timing, threading, re-entrancy)
- [ ] §5 MetricReader / MetricExporter interaction (collect cadence, push vs pull, flush/shutdown)
- [ ] §6 Views (selector surface, allowed transforms, conflict rules)
- [ ] §7 Exemplars (reservoir, span-context linkage, sampling interaction)
- [ ] §8 Instrument API surface (the seven instruments + MeterProvider/Meter)
- [ ] §9 Memory & threading model (hot-path noexcept, allocation budget, lock strategy)
- [ ] §10 Wire mapping (OTLP metrics protobuf; encoder/transport reuse)
- [ ] §11 Configuration & compatibility (SdkBuilder additions, env vars, tier impact)

---

## §1 Aggregation temporality

**Context.** OTLP supports DELTA and CUMULATIVE temporality. The choice affects
collector compatibility, memory (cumulative retains state), and counter-reset
semantics.

**Decisions needed.**
- Default temporality, and whether it differs per instrument kind (e.g.
  cumulative for monotonic counters, delta for histograms).
- Per-metric / per-reader override mechanism.
- Interaction with the `OTEL_EXPORTER_OTLP_METRICS_TEMPORALITY_PREFERENCE` env var.

**Options considered.** _TBD._

**Decision.** _TBD._

## §2 Cardinality limits

**Context.** Unbounded attribute combinations are the dominant metrics
failure mode. OTel defines an overflow attribute (`otel.metric.overflow=true`)
once a limit is hit.

**Decisions needed.**
- Default per-instrument cardinality limit and how it is configured.
- Overflow attribute behaviour and when aggregation collapses into it.
- Drop accounting: which `DropReason` enumerator(s) — note `provider.hpp`'s
  `DropReason` is an ICP-gated public enum, so new metric drop reasons are an ICP.

**Decision.** _TBD._

## §3 Histogram bucket configuration

**Decisions needed.**
- Default explicit bucket boundaries.
- Per-view boundary override.
- Whether exponential (base-2) histograms ship in v1.2 or defer.

**Decision.** _TBD._

## §4 Async-instrument callback semantics

**Context.** Observable instruments report via callbacks invoked at collection
time. Threading and re-entrancy must be pinned down given the project's
strict threading model (`docs/threading-model.md`).

**Decisions needed.**
- Callback registration / unregistration API and lifetime.
- Which thread invokes callbacks; timeout/budget; behaviour on a slow callback.
- Re-entrancy and exception/`noexcept` policy (hot-path methods are `noexcept`).
- Duplicate-measurement and missing-measurement handling.

**Decision.** _TBD._

## §5 MetricReader / MetricExporter interaction

**Decisions needed.**
- Periodic (push) reader cadence vs pull; default interval; config.
- How `ForceFlush` / `Shutdown` propagate through reader → exporter (reuse the
  `Status` taxonomy in `include/microtel/status.hpp`).
- Reuse of the OTLP encoder (`src/wire/encoder/`) and transport
  (`src/transport/`); per-signal vs shared batch pipeline.

**Decision.** _TBD._

## §6 Views

**Decisions needed.**
- Selector surface for v1.2 (instrument name / kind / meter) — full grammar
  deferred to v1.4.
- Allowed transforms: rename, attribute-key filter, aggregation override.
- Conflict resolution when multiple views match one instrument.

**Decision.** _TBD._

## §7 Exemplars

**Decisions needed.**
- Reservoir strategy and size.
- Linkage to active span context (trace_id / span_id) when present.
- Interaction with trace sampling and with cardinality overflow.

**Decision.** _TBD._

## §8 Instrument API surface

**Decisions needed.**
- Public signatures for the four sync and three async instruments.
- `MeterProvider` / `Meter` acquisition (mirrors `Provider::GetTracer`?).
- Value types (int64 / double) and measurement attribute passing.
- `mt::Timer` sugar shape and how it binds to a histogram.

**Decision.** _TBD._

## §9 Memory & threading model

**Decisions needed.**
- Hot-path `noexcept` and allocation policy for `Add`/`Record` (cf.
  `docs/memory-model.md`, `docs/threading-model.md`).
- Aggregation state storage and lock strategy (per-instrument? striped?).
- Per-metric memory budget and its place in `MemoryLimitOptions`.

**Decision.** _TBD._

## §10 Wire mapping

**Decisions needed.**
- OTLP metrics protobuf message mapping via upb (encoder additions).
- Resource / scope reuse from the trace path.
- gRPC vs HTTP path parity (no new transport surface expected).

**Decision.** _TBD._

## §11 Configuration & compatibility

**Decisions needed.**
- `SdkBuilder` additions (e.g. `WithMetricReader`, temporality, cardinality).
- Metrics-relevant `OTEL_*` env vars honoured.
- Compatibility-tier impact (Tier 1/2 advance to metrics; Tier 3 shim
  experimental) and the interop-matrix additions needed (`§14` / `interop.yml`).

**Decision.** _TBD._

---

## References

- `microtel-roadmap.md` §1.2 — v1.2 scope; M11 prerequisite.
- `microtel-spec.md` — M11/M12 milestone table; "one signal at a time" rule;
  compatibility tiers (§2.2).
- `docs/interfaces.md` — the locked trace contracts this doc parallels for metrics.
- `docs/error-model.md` — `DropReason` / `Status` taxonomy to extend.
- `docs/memory-model.md`, `docs/threading-model.md` — hot-path and allocation rules.
- OpenTelemetry Metrics SDK & data-model specification (pinned version: _TBD_).
