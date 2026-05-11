# microtel Roadmap: From Exporter-First v1 to Full OpenTelemetry Coverage

**Companion to:** `microtel-spec.md` (v0.10, the v1 spec)
**Status:** Draft v0.1
**Scope:** Multi-year evolution from v1.0 (traces-only exporter) through full OTel SDK conformance and embedded deployments.

---

## 1. How to read this document

This is a **theme-based roadmap**, not a dated commitment. Each minor release advances one cohesive theme; major versions bump when public C++ API or wire-level promises break. Effort estimates from the v1 spec are realistic ranges; v1.x and beyond are intentionally less precise because real signals from M0 and M1 will reshape them.

The roadmap is the source of truth for **what's deferred from v1**. Anything called out as "v1.1" or "v2.0" in the main spec is described here in detail. If a feature isn't on this roadmap, it isn't on the project's plan; new ideas land in §11 first and graduate to a milestone if they survive review.

The spec (`microtel-spec.md`) covers v1.0 in full. This document is brief on v1.0 and detailed on everything after.

---

## 2. Versioning and maintenance policy

- **Semantic versioning** for the public C++ API and `microtel.toml` schema.
- **Wire compatibility** is tracked against a pinned OTel spec version, called out per release.
- **One LTS line per major version.** When v2.0 ships, the v1.x line gets security fixes and OTel-spec updates for **18 months**. After that, v1.x is end-of-life. The deprecation calendar lives in `docs/release-policy.md`.
- **Pre-1.0 ABI is unstable**, period. Post-1.0 source compat within major; binary best-effort within minor (§18 of spec).
- **Compat shims** version independently from microtel core. A shim `0.x` may target microtel `1.5` and remain experimental even as microtel itself is stable.

---

## 3. Compatibility tier progression

Each release advances along the four-tier model from spec §2.2. The progression below is the planned path; tier promotions require the gates listed in §10 to pass first.

| Release | Tier 1 (wire) | Tier 2 (data-model) | Tier 3 (API-adapter) | Tier 4 (SDK conformance) |
|---|---|---|---|---|
| **v1.0** | traces | traces | experimental: traces | — |
| **v1.1** | traces | traces | experimental: traces | — |
| **v1.2** | traces, metrics | traces, metrics | experimental: traces, metrics | — |
| **v1.3** | all three signals | all three signals | experimental: all three | — |
| **v1.4** | all three | all three | **beta: all three** | partial |
| **v1.5** | all three | all three | beta: all three | partial |
| **v2.0** | all three + leaf | all three + leaf | **stable: all three** | partial+ |
| **v2.1** | + MCU leaf | + MCU leaf | stable | partial+ |
| **v2.2** | + concentrator HA | stable | stable | partial+ |
| **v3.0** | all + profiles | all + profiles | stable | **full conformance claim** |

"Profiles" refers to OpenTelemetry's continuous-profiling signal, which is still stabilizing upstream as of this writing. It graduates onto the roadmap once upstream marks it stable.

---

## 4. Release themes

### v1.0 — Trace Runtime + OTLP Exporter (release point)

**Theme:** Prove the wedge. Smallest credible OTel-compat trace runtime over OTLP/HTTP and OTLP/gRPC.

Covered in detail in `microtel-spec.md` §13. Brief recap:

- C++20 trace SDK (Tracer, Span, W3C Trace Context, AlwaysOn / AlwaysOff / TraceIdRatio / ParentBased samplers, BatchSpanProcessor)
- OTLP/HTTP and OTLP/gRPC over nghttp2, no gRPC library
- Production correctness: partial-success, retry policies, GOAWAY/RST_STREAM, fork-safety, deterministic shutdown
- Static config + OTel env-var fallback, no hot reload
- Preflight CLI flag, exporter-health API, internal logging
- Python bindings (target, not blocker)
- Experimental compat shims released as separate packages

**v1.0 release gates** are listed in spec §13.5.

---

### v1.1 — Operational Polish

**Theme:** Make v1 actually nice to operate, layer on the ergonomics that v1.0 deliberately deferred.

- **Sugar layer** (`microtel::sugar`): function-scoped spans via `std::source_location`, RAII scoped spans with inline attributes, traced-lambda helpers, exception recording, scoped timers (active once metrics arrive in v1.2), pre-bound `AttrKey` for hot paths. Python equivalents using decorators and context managers. See §5 below for full sugar evolution.
- **Control plane** — Unix-domain-socket server with length-prefixed JSON wire (per spec §13 in earlier drafts), `microtelctl` Go binary with REPL and tab completion. Hot-reloadable settings: sampler ratio, batch sizes, internal log level. Endpoint, protocol, TLS material, service.name, and resource attributes are explicitly **not** hot-reloadable in v1.1.
- **W3C Baggage propagation.** Inject + extract.
- **Composable sampler chains.** `microtel::sampler::Chain({...})` with built-in rule-based combinators (sample-on-attribute, sample-on-duration, etc.).
- **Multi-profile within one process.** Named providers with independent endpoints, samplers, and Resources.
- **Built-in auth providers.** OAuth2 client credentials, AWS SigV4. mTLS rotation likely v1.2 (more involved).
- **Resource detectors:** process (`process.pid`, `process.executable.name`, `process.command_line`) and host (`host.name`, `host.id`).
- **`microtelctl` packaging:** standalone Go binary in `.deb`/`.rpm`/`.tar.gz`, separate from the core runtime package — reconciles the v1.0 "single shared library + Python wheel" claim.

**Ships when:** sugar layer has a stable API, control plane has a documented threat model, hot reload is fuzz-tested.

**Anti-goals in v1.1:** still no metrics, no logs, no full SDK conformance claim, no Windows.

---

### v1.2 — Metrics

**Theme:** Second signal lands.

**Prerequisite milestone (M11 from spec):** `docs/metrics-design.md` lands first, with reviewer sign-off. Covers the semantic decisions that v1.0 deliberately deferred: aggregation temporality (delta vs cumulative), cardinality limits, histogram bucket configuration, async-instrument callback semantics, reader/exporter interaction, views, exemplars roadmap.

Then implementation:

- **Sync instruments:** Counter, UpDownCounter, Gauge, Histogram.
- **Async instruments:** ObservableCounter, ObservableUpDownCounter, ObservableGauge with callback semantics defined in the design doc.
- **MetricReader / MetricExporter pipeline** sharing the existing OTLP encoder and transport infrastructure.
- **Aggregation temporality** with delta and cumulative paths; per-metric configuration.
- **Cardinality limits** with explicit overflow attribute (per OTel spec) and drop accounting.
- **Views API** (basic — rename, attribute filter, aggregation override). Full views deferred to v1.4.
- **Exemplars** linked to active span context where present.
- **`mt::Timer` sugar wired into histograms** — completes the sugar layer's deferred-during-v1.1 piece.

**Compatibility tier:** Tier 1 and Tier 2 advance to include metrics. Tier 3 shim adds metric instruments, still experimental.

**Anti-goals in v1.2:** no logs yet, no advanced exemplar formats, no metric backfill / persistence.

---

### v1.3 — Logs

**Theme:** Third signal lands. All OTLP signal coverage.

- **OTel Logs API:** Logger, LogRecord, severity levels, attribute schema.
- **OTLP/logs export** on both wire protocols.
- **Trace context correlation:** logs emitted within an active span carry the `trace_id` and `span_id` automatically.
- **Bridge adapters as separate packages:**
  - `microtel-bridge-spdlog`: a spdlog sink that converts spdlog records to OTel LogRecords. The natural pairing given microtel's internal logging dependency.
  - `microtel-bridge-glog`: same for `glog`.
  - `microtel-bridge-log4cxx`: same for `log4cxx`.
  - Bridges are independently versioned packages.

**Compatibility tier:** Tier 1 and Tier 2 reach all three signals. Tier 3 shim becomes "experimental: all three."

**Anti-goals in v1.3:** no log-side sampling (collector handles it), no structured-log search features (not microtel's job).

---

### v1.4 — Conformance Push

**Theme:** Move compat shims from experimental to beta; first partial Tier 4 claim.

- **Compat shims to beta.** Tested against ≥10 representative real-world applications drawn from the OTel community demos and contributed adopters. Shim API surface frozen at OTel spec version X.Y. Documented deprecation policy.
- **Auto-instrumentation for select libraries:** database clients (libpq, mysqlclient), HTTP clients (libcurl, cpp-httplib), gRPC clients via the standard interceptor mechanism. Each instrumentation is a separate package. Python auto-instrumentation follows the OTel-Python contrib pattern.
- **Resource detectors expanded:** Kubernetes (downward API), AWS (EC2 IMDS, ECS task metadata, EKS), GCP (metadata server), Azure (IMDS).
- **Span Processors as a public extension point.** Until v1.4, processors are internal-only. v1.4 publishes the `Processor` interface as stable, allowing third-party span processors (filtering, enrichment, fan-out).
- **Custom Samplers as a documented extension point.** Same pattern.
- **Persistent storage option.** Optional disk-backed retry queue for failed batches that survives process restart. Off by default, opt-in via config. Useful for satellite, edge, and intermittent-connectivity deployments.
- **Full Views API.** Custom buckets, attribute hashing, stream-level renames.

**Compatibility tier:** Tier 4 reaches "partial." Specifically: span semantics, metric instrument semantics, log record semantics conform; the "everything in the spec" surface is not yet 100%.

**Anti-goals in v1.4:** still no Windows, no auto-instrumentation for everything (just major libraries), no profiles signal.

---

### v1.5 — Performance & Footprint Refinement

**Theme:** Squeeze the last drop. The v1.0 numbers prove the wedge; v1.5 makes them luxurious.

- **Coroutine API.** Async export with `co_await` for users on coroutines-aware code. Returns `microtel::Task<ExportResult>`. Callback API stays as the supported v1.0 surface.
- **Connection pooling.** Optional multi-connection mode for very-high-throughput deployments where one HTTP/2 connection's flow-control becomes the bottleneck. Off by default.
- **Optimized hot path.** Compile-time attribute key encoding (for callers using `mt::AttrKey`), refined lock-free MPSC, possible move to a fully wait-free hot path on x86-64 / ARM64.
- **Static-link optimizations.** First-class CMake support for `-Bstatic` builds, with measured size targets. Currently mostly-static is supported; v1.5 makes it a tested release configuration.
- **HTTP/3 transport (experimental).** The `Transport` interface gains an nghttp3-based implementation. Configurable per-endpoint. Experimental in v1.5; may stabilize in v2.0 or stay experimental indefinitely depending on real-world usage signals.
- **Refined benchmarks.** The `bench/` directory gets richer scenarios: bursty traffic, high-cardinality metrics, long-tail latency under collector pressure.

**Anti-goals in v1.5:** still no Windows, no coroutine-only API (callback always supported).

---

### v2.0 — Leaf / Concentrator Architecture

**Theme:** Embedded story. Open the door for fleets of constrained devices to participate in OTel.

Covered in detail in `microtel-spec.md` §17.4. Recap:

- **microtel-leaf**, a pure-C library (`< 30 KB` flash target) for constrained embedded systems. No threading, no batching, no retries, no TLS, no HTTP. Encodes OTLP messages and hands the bytes to an application-supplied transport.
- **Concentrator role** in main microtel: ingests leaf payloads, enriches with Resource attributes from config (`device-id → service.*`), runs the standard batching / sampling / export pipeline, ships to upstream collector via existing OTLP/HTTP or OTLP/gRPC.
- **Encoder strategy:** v2.0 ships with **upb on the leaf** — covers Linux-on-ARM, OpenWrt-class, Cortex-A and beefier Cortex-R targets. Leaf API is encoder-agnostic by design; nanopb backend lands in v2.1.
- **Time handling:** three modes — concentrator-stamped, sync-relative, boot-relative — configurable per leaf in the concentrator's config.
- **Late Resource enrichment.** Promotes the v1-internal hook to a public stable API.
- **Receiver abstraction.** Public stable API. Third parties can write custom receivers (e.g., legacy proprietary protocol → OTLP).

**Compatibility tier:** Tier 3 promotes to **stable** for all three signals — the leaf-and-concentrator deployment story is enough adoption surface that the shim API can no longer be experimental.

**Major-version bump because:** the `Receiver` and Resource-enrichment hooks become public (breaking change to the previously-internal interface). v1.x line continues for 18 months.

**Anti-goals in v2.0:** no full RTOS ports, no leaf-side sampling, no PTP/NTP, no reliable delivery on the leaf-to-concentrator link (application transport's job).

---

### v2.1 — Nanopb Backend for True MCU Support

**Theme:** Reach the smallest devices.

- **nanopb encoder backend** for microtel-leaf. Same leaf API; encoder swapped at build time.
- **Static memory pools throughout** the leaf. No malloc anywhere.
- **Documented RAM/flash budgets** per leaf configuration. Target: **< 15 KB flash, < 2 KB RAM** for a minimal trace-only leaf on Cortex-M0+.
- **Concrete reference ports:** STM32 HAL, Zephyr, FreeRTOS examples in `examples/leaf/`. Not formal RTOS integrations — examples.
- **Wire-format conformance tests** add nanopb as a second backend through the same gauntlet that already covered upb in v2.0.

**Anti-goals in v2.1:** still no formal RTOS integrations as part of microtel core.

---

### v2.2 — Operational Excellence at Scale

**Theme:** Big-fleet deployments need operations features the single-process model doesn't have.

- **Concentrator clustering.** HA pairs sharing leaf state. A leaf can fail over between concentrators without losing in-flight telemetry. Built on a small consensus protocol or a shared backing store; design TBD in v2.2 design doc.
- **Concentrator-side advanced sampling.** Head-based and tail-based sampling at the concentrator, since leaves emit raw and concentrators have the budget for more sophisticated decisions.
- **Leaf authentication.** A small protocol on top of the existing leaf-to-concentrator transport for mutual authentication and integrity. Detail TBD.
- **Multi-tenant concentrator routing.** Single concentrator process serving multiple downstream collectors with per-tenant config, enrichment rules, and rate limits.

---

### v3.0 — Full SDK Conformance

**Theme:** Tier 4 claim. microtel passes the full OpenTelemetry SDK conformance test suite.

- **Pass full conformance.** Every requirement in the OTel SDK spec, with caveats explicit and minimal.
- **Stable shims.** Compat shims graduate from beta to stable; no longer "experimental migration aids" but supported peers of the native API.
- **All current OTel API surface.** Whatever the OTel spec defines as of v3.0's release, microtel implements.
- **Profiles signal**, if upstream-stable by then. Otherwise deferred again.
- **Auto-instrumentation expanded.** All major libraries with stable upstream OTel instrumentation get a microtel equivalent.
- **Decision: deprecate native API or keep both.** A real choice for v3.0 design: either native `microtel::*` API stays first-class with shims as alternative, or microtel commits fully to OTel API surface and natives become a thin convenience layer. Discussion happens in v2.x; decision lands in v3.0 design doc.

**Anti-goals in v3.0:** none meaningful at this point — the project has reached full coverage. Future work is depth (perf, ports, integrations) and breadth (more bridges, more auto-instrumentations).

---

## 5. Sugar Layer Evolution

The sugar layer (`microtel::sugar` and Python equivalents) grows continuously rather than landing at one milestone. Each release adds helpers as the underlying primitives become available.

### v1.0
None. The native API is direct OTel-style: `tracer->StartSpan(...)`, `span->SetAttribute(...)`, `span->End()`.

### v1.1 (sugar layer's introduction release)
- `MICROTEL_TRACE_FUNCTION()` — function-scoped span auto-named via `std::source_location`.
- `mt::Span("name", {attrs})` — RAII scoped span with inline attributes.
- `mt::Traced("name", lambda)` — trace a lambda; returns the lambda's value.
- `mt::RecordException(span, e)` — sets Error status + adds exception event.
- `mt::AttrKey("http.method")` — pre-bound attribute key for hot paths (skips per-call string lookup).
- Python: `@mt.trace_function` decorator, `mt.span(...)` context manager, `mt.traced(name, callable)`, `mt.record_exception(span, e)`.

### v1.2 (metrics arrive)
- `mt::Timer("histogram_name")` — RAII timer recording duration to a histogram on destruction. Was deferred from v1.1 because histograms didn't exist yet.
- `mt::Counter<T>(...)` — pre-bound counter helper for hot paths.
- Python: `@mt.timed("histogram_name")` decorator, `mt.counter("name", ...)`.

### v1.4 (conformance / extension push)
- Auto-instrumentation hooks for `std::async`, `std::thread`, coroutines.
- Span linking helpers: `mt::Linked(span)`, `mt::FollowsFrom(span)` for cross-trace relationships.
- `mt::TryCatch(span, lambda)` — wrap a lambda, auto-record exceptions to the span.
- Async-aware Tracer methods that propagate context across coroutine resumption.

### v2.0 (leaf opens new shape)
- **Leaf-side sugar in C macros**, since the leaf is C-only:
  - `MICROTEL_LEAF_TRACE(name)` — start-and-emit shorthand.
  - `MICROTEL_LEAF_ATTR(key, val)` — typed attribute setter.
  - `MICROTEL_LEAF_ERROR(msg)` — error span shorthand.
- The leaf API itself remains primitive; macros provide ergonomic shorthand.

### v3.0 (full conformance)
- Compile-time attribute set definitions (`mt::AttrSet<...>`) for high-cardinality APIs.
- `mt::Result<T>` integration — automatic span attribute capture from result types.
- Reflection-based attribute capture for structured types (using C++26 reflection if available; otherwise compile-time helpers).

---

## 6. Performance and footprint trajectory

The v1.0 footprint targets in spec §10.5 are stretch numbers pending prototype. The trajectory across releases:

| Release | Core exporter (`libmicrotel-exporter.so`) | Full SDK (`libmicrotel-sdk.so`) | Total dynamic closure |
|---|---|---|---|
| v1.0 | < 800 KB stretch | < 1.5 MB stretch | < 3 MB stretch |
| v1.1 | + control plane (separate so) | + sugar (header-only-ish) | + spdlog, Go ctl binary |
| v1.2 | unchanged | + metrics SDK | unchanged |
| v1.3 | unchanged | + logs SDK | unchanged |
| v1.4 | unchanged | + extension surface | + auto-instr packages (separate) |
| v1.5 | refined | refined | + nghttp3 (optional) |
| v2.0 | leaf: < 30 KB flash | unchanged | leaf closure: nghttp2 + OpenSSL + upb |
| v2.1 | leaf: < 15 KB flash | unchanged | leaf: nanopb only |
| v3.0 | TBD | TBD (full conformance) | TBD |

**The overarching size discipline:** every minor release publishes its full footprint matrix as part of release notes. Regressions versus the previous release require a documented justification or a fix.

---

## 7. Adoption story progression

How the pitch to potential users evolves:

- **v1.0:** *"OTLP/gRPC and OTLP/HTTP wire compat, no gRPC library, < 3 MB closure."* Best for: embedded Linux, edge, CNF, air-gapped, anyone whose pain point is the gRPC dependency closure.
- **v1.1:** *"…plus a real operational surface — preflight, hot reload, sane control plane."* Adds: ops-heavy deployments where the gRPC closure isn't the only friction.
- **v1.2:** *"…plus production-quality metrics with explicit cardinality control."* Adds: teams currently using the OTLP/HTTP exporter or Prometheus push gateway and wanting cleaner aggregation.
- **v1.3:** *"…plus logs with built-in trace correlation."* Adds: full-signal users currently running stock OTel-cpp and wanting the footprint reduction.
- **v1.4:** *"…plus auto-instrumentation for major libraries and a beta compat shim."* Adds: teams that want to migrate from stock OTel-cpp without code changes.
- **v2.0:** *"…plus a leaf library for embedded fleets."* Adds: the constrained-device fleet audience the project's embedded positioning was always aimed at.
- **v3.0:** *"Full OpenTelemetry SDK, just smaller and faster."* The general pitch.

---

## 8. Anti-goals progression

What we're explicitly **not** doing in each phase:

| Phase | Not doing |
|---|---|
| v1.x core | Windows; full SDK conformance claim; leaf/embedded; control plane (until v1.1); auto-instrumentation (until v1.4) |
| v2.x | Full SDK conformance claim; profiles signal; formal RTOS integrations as core; leaf-side sampling; concentrator-side persistence beyond optional disk queue |
| v3.0 | Nothing meaningful left as anti-goals — coverage is full |
| All phases | Vendor-specific exporters (Datadog, New Relic, etc.) — collectors handle that; semantic-convention helper packages tied to specific OTel spec versions (we stay out of the semconv treadmill in core, may ship as separate optional package) |

---

## 9. Cross-cutting threads

A few themes don't fit a single milestone but progress across releases:

### Wire-protocol freshness
- **OTel spec version:** pinned per release, called out in release notes.
- **opentelemetry-proto pin:** updated quarterly during the v1.x line; per-release after v2.0.
- **Wire-protocol conformance test corpus:** grows release-over-release as new edge cases are caught.

### Performance benchmarks
The `bench/` directory evolves alongside the project:
- **v1.0:** establishes baseline against `opentelemetry-cpp` for traces.
- **v1.2:** adds metrics workload profiles.
- **v1.3:** adds logs workload profiles.
- **v1.5:** adds high-cardinality, bursty, and outage-recovery scenarios.
- **v2.0:** adds leaf footprint measurement and concentrator throughput.

### Documentation
- **v1.0:** spec, migration guide, README, compatibility matrix, interop matrix.
- **v1.1:** control plane operator guide, threat model.
- **v1.2:** metrics design doc (M11 from v1 spec).
- **v1.3:** logs cookbook with bridge examples.
- **v1.4:** conformance matrix, extension-author guide, auto-instrumentation cookbook.
- **v2.0:** leaf programming guide, concentrator deployment guide, embedded examples.

### Community and governance
- **Pre-1.0:** small core team; CODEOWNERS for each track.
- **v1.0 → v1.4:** maintain DCO/CLA, security policy, regular release cadence (monthly pre-1.0, quarterly stable).
- **v2.0:** consider donating to a foundation (CNCF if the leaf/concentrator pattern resonates) — depends on adoption signals.
- **v3.0:** stable maintainer model with multiple organizations contributing if the project has reached real adoption.

---

## 10. Tier-promotion gates

A release cannot promote a compatibility tier without meeting the gate.

### Tier 3 experimental → beta (planned for v1.4)

- Compat shims tested against ≥10 representative real-world applications.
- Migration guide validated by ≥5 external users.
- Shim API surface frozen at a specific OTel spec version.
- Documented deprecation policy in place.
- No load-bearing user reports of "I migrated and it broke" without a documented workaround.

### Tier 3 beta → stable (planned for v2.0)

- Two release cycles in beta with no breaking changes.
- ≥3 production users (named or anonymous) using shims at scale.
- Shim API matches the targeted OTel spec version with documented deltas.

### Tier 4 partial → full conformance (planned for v3.0)

- microtel passes the full upstream OpenTelemetry SDK conformance test suite.
- Conformance test runs in CI on every PR.
- Caveats document is short and explicit.
- Sign-off from at least one OTel maintainer or CNCF reviewer (if pursued).

---

## 11. Decision log

Brief notes on decisions whose rationale spans multiple releases and influences the roadmap.

| Decision | Adopted in | Rationale | Affected releases |
|---|---|---|---|
| Exporter-first v1, traces only | v0.9 spec | Scope was too broad to be credible v1; reviewer pushed; user agreed. Made everything else achievable. | v1.0 baseline |
| Traces before metrics | v0.9 spec | Reviewer pushed against v0.8's metrics-first; trace SDK semantics are more contained than metrics aggregation. | v1.0, v1.2 ordering |
| upb over protobuf-cpp / nanopb | v0.7 spec | Order-of-magnitude smaller; pure C; aligns with project ethos. nanopb deferred to v2.1 for tighter MCU tier. | v1.0+ encoding, v2.0/v2.1 leaf |
| Implement gRPC wire on nghttp2 directly | v0.5 spec | gRPC unary is small (~500-800 LOC) on top of HTTP/2; library closure is multi-MB. Whole project rationale. | v1.0+ |
| C++20 floor, devtoolset-11 for RHEL 8 | v0.6 spec, refined v0.7 | `std::span`, concepts, designated initializers; coroutines deferred. C++17 fallback evaluated post-prototype if needed. | All releases |
| Internal logging via spdlog with `MICROTEL_USE_SPDLOG=OFF` fallback | v0.7 spec, build flag added v0.10 | Standard, header-only, std::format mode avoids fmt dep. Minimal stderr fallback for embedded/constrained. | All releases |
| Config precedence: code > env > file > defaults | v0.10 spec | Containerized-deployments standard; OTel SDK convention. | All releases |
| Compat shims experimental, separate package | v0.9 spec | Don't make shim correctness a v1.0 release blocker. Promote to beta in v1.4 once tested. | v1.0 (experimental), v1.4 (beta), v2.0 (stable) |
| Sugar in v1.1, not v1.0 | v0.9 spec | v1.0 messaging is "drop-in for OTel-cpp users"; sugar layer competes with that pitch. Sugar arrives once core is proven. | v1.1+ |
| Control plane in v1.1, not v1.0 | v0.9 spec | Adds Unix socket server, JSON wire, CLI, REPL, attack surface, threat model. Too much for v1.0 alongside transport correctness. | v1.1+ |
| Leaf encoder is upb first, nanopb later | v0.5 spec | Larger embedded targets are most of the addressable audience and reuse microtel's existing encoder closure. nanopb adds reach to true MCU class. | v2.0, v2.1 |

This log is appended to, never rewritten. When a decision is reversed (none yet), the original entry stays and a new entry records the reversal with rationale.

---

## 12. Open roadmap questions

1. **OTel Profiles signal — when?** Stabilizes upstream first; microtel adds support after that. Currently scheduled for v3.0 but could land earlier if upstream ships sooner and adoption pressure is real.
2. **Foundation donation — CNCF?** Depends on adoption. v2.0 is a natural inflection point if the leaf/concentrator pattern resonates with embedded and edge audiences.
3. **Native API vs OTel API in v3.0.** Real choice: either native `microtel::*` API stays first-class with shims as alternative, or microtel commits fully to OTel API surface and natives become a thin convenience layer. Discussion happens during v2.x; decision lands in v3.0 design doc.
4. **Windows support — ever?** Currently a hard non-goal. Could land in v3.x if there's real demand. The transport layer abstraction makes it possible; the work is in IOCP-based I/O and Windows packaging.
5. **HTTP/3 graduation.** Experimental in v1.5. Could stabilize in v2.0 or stay experimental indefinitely depending on real-world signal.

---

## 13. References

- `microtel-spec.md` (v1 spec)
- `docs/bench-spec.md` (benchmark harness)
- [OpenTelemetry Specification](https://opentelemetry.io/docs/specs/otel/)
- [OpenTelemetry SDK Specification](https://opentelemetry.io/docs/specs/otel/sdk/)
- [OpenTelemetry Collector](https://github.com/open-telemetry/opentelemetry-collector)
