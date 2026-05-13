# microtel bench/: Benchmark Harness Spec

**Status:** Draft v0.2
**Companion to:** `microtel` core spec
**License (proposed):** Apache 2.0

---

## 1. Purpose

A benchmark harness that produces **fair, reproducible, environment-portable** comparisons between **microtel** and stock **opentelemetry-cpp** (with both gRPC and HTTP exporters). Users — internal teams, prospective adopters, skeptics — can clone the repo, run a single command, and get a credible report of how the two libraries perform **on their own hardware** for **their own workload shape**.

Two audiences, one harness:
- **microtel CI:** runs on every PR to detect performance regressions.
- **End users:** run on their environment to make an evidence-based adoption decision.

---

## 2. Design Principles

1. **Apples-to-apples.** Same workload code, same compiler flags, same hardware, same collector, same network path. The *only* variable across runs is the OTel library.
2. **Reproducible.** Containerized by default with pinned versions (compiler, OS image, library SHA, collector version). Deterministic workload generation.
3. **Portable.** Runs on a developer laptop, a bare-metal server, or a constrained VM. No cloud-only assumptions, no proprietary dependencies.
4. **Honest.** Reports methodology, environment fingerprint, variance, and confidence intervals. Refuses to print a single "winner" number — always shows the distribution.
5. **Trivial to run.** `./bench.sh` from a clean clone, no manual setup. A single Markdown + JSON report drops into `results/`.
6. **Defensible.** Methodology is documented and reviewable. Anyone should be able to point at a number and trace exactly how it was produced.

---

## 3. Architecture

```
┌──────────────────────────────────────────────────────────────────┐
│                         Bench Driver (Python)                    │
│  - Parses scenario config                                        │
│  - Spawns SUT and sink containers                                │
│  - Records environment fingerprint                               │
│  - Drives workload via control socket                            │
│  - Collects measurements, computes stats, writes report          │
└──────────────────────────────────────────────────────────────────┘
              │                      │                      │
              ▼                      ▼                      ▼
   ┌─────────────────┐    ┌─────────────────┐    ┌─────────────────┐
   │   SUT-A:        │    │   SUT-B:        │    │   SUT-C:        │
   │   microtel      │    │   otel-cpp +    │    │   otel-cpp +    │
   │   (HTTP+pb)     │    │   gRPC exporter │    │   HTTP exporter │
   │                 │    │                 │    │                 │
   │   emit_app      │    │   emit_app      │    │   emit_app      │
   │   (same C++)    │    │   (same C++)    │    │   (same C++)    │
   └────────┬────────┘    └────────┬────────┘    └────────┬────────┘
            │                      │                      │
            └──────────────────────┼──────────────────────┘
                                   │
                                   ▼
                  ┌───────────────────────────────┐
                  │        Sink (selectable)      │
                  │  ┌─────────────────────────┐  │
                  │  │ blackhole-sink (default)│  │  ← minimal, low-variance
                  │  │  HTTP/2 + gRPC, parses, │  │
                  │  │  acks, counts bytes     │  │
                  │  └─────────────────────────┘  │
                  │  ┌─────────────────────────┐  │
                  │  │ otel-collector (optional)│ │  ← realistic, higher variance
                  │  │  pinned version         │  │
                  │  └─────────────────────────┘  │
                  └───────────────────────────────┘
```

### 3.1 The SUT (system-under-test)

Three SUT images, one per library/exporter combination. All share the **same workload shape** — the same profile YAML drives the same workload logic — with a **compile-time-selected backend**: `bench/emit-app/` contains backend-specific adapter files, one per library, selected at build time so the microtel and otel-cpp APIs can differ while the workload logic stays identical. Compiler, flags (`-O2 -g -fno-omit-frame-pointer`), and the rest of the toolchain are identical, enforced via a shared base Dockerfile.

The emit-app does three things:
1. Listens on a TCP socket (port 9090) for control messages from the driver (`start`, `stop`, `flush`, `report`). TCP is used rather than a Unix domain socket so the control channel works across Docker container boundaries without volume mounts.
2. Runs the configured workload profile.
3. Exposes a metrics endpoint (process-internal counters: items emitted, items dropped, queue high-water mark, wallclock spent in `StartSpan`/`End`, etc.).

### 3.2 The sink

Two sink modes:

- **`blackhole-sink` (default):** A tiny purpose-built server in Go (~500 LOC) that speaks both **HTTP/2 + protobuf** and **gRPC**, accepts OTLP, parses just enough to count items and bytes per signal, returns `200 OK` immediately, and exposes counters over a control socket. Eliminates collector-side variance — the cost we measure is purely the lib + transport.

- **`otel-collector`:** The real upstream `otel/opentelemetry-collector` at a pinned version, configured with a `debug` exporter (or `file` exporter for archival). Higher variance, but useful for end-to-end realism.

The driver picks the sink mode from config; default is `blackhole-sink` for low-noise comparisons, with `otel-collector` runs done separately.

### 3.3 The driver

Python 3.11+, single dependency: `docker` (or `podman`) on the host. No PyPI dependencies in the hot path — the driver itself is stdlib + a handful of vendored helpers, so it runs in air-gapped envs.

Responsibilities:
- Build SUT images (cached on a base layer hash).
- Capture **environment fingerprint** (CPU model, microcode, kernel, governor, hyperthreading, NUMA, container runtime, lib versions, compiler version, host load average at start). Embedded in every report.
- Pin SUT and sink containers to specific cores via `cpuset` (configurable; off by default).
- Run a configurable warm-up phase, then a measurement window.
- Record per-SUT measurements via control socket and `/proc/<pid>/status`.
- Repeat the run **N times** (default 5) with the order randomized across SUTs to wash out drift.
- Compute median, p25/p75, IQR, min/max. **Never reports just a single mean.**
- Emit `results.json`, `results.md`, optional `flamegraph-<sut>.svg`.

---

## 4. Workload Profiles

Profiles live in `bench/profiles/*.yaml`. v1 ships these:

### 4.1 `hot-loop-traces`
Tight loop emitting spans as fast as the lib will accept them. Measures **library overhead per span and steady-state max throughput.**

```yaml
profile: hot-loop-traces
duration_seconds: 60
warmup_seconds: 10
threads: 1                 # also runs configured at 4, 16
spans_per_iteration: 1
attributes_per_span: 5
attribute_value_mean_bytes: 24
events_per_span: 0
links_per_span: 0
target_rate_hz: max        # also runs at 1k, 10k, 100k
```

### 4.2 `realistic-request`
Simulated request handler: per "request," emit one span with two child spans, one log record, one histogram observation, one counter increment. Fixed RPS, sustained.

```yaml
profile: realistic-request
duration_seconds: 120
warmup_seconds: 20
threads: 4
target_rate_hz: 5000
attributes_per_span: 8
log_message_mean_bytes: 80
```

### 4.3 `cold-start`
Process starts → first byte received at sink. Measures **library init cost + first-export latency.** Run 50 times; report median + p95.

### 4.4 `bursty`
Idle baseline punctuated by 1-second bursts at 50k spans/sec every 10s for 5 minutes. Measures **queue overflow behavior, drop rate, and recovery.**

### 4.5 `large-attributes`
Spans with 20 attributes, mean 256 bytes each, including some 4 KB string attributes. Measures **encoding cost on big payloads.** This profile intentionally exercises the `attribute_value_length_limit` truncation path (spec §5.6); truncation behavior and its effect on encoding cost are part of what is measured.

### 4.6 `binary-size` (static, no execution)
Build all three SUTs, measure stripped shared library size, statically-linked dep closure size, and `.text` / `.rodata` section sizes via `size` and `bloaty`. Pure static measurement; no runs.

Users add their own profile by dropping a YAML file into `bench/profiles/`.

---

## 5. Metrics Recorded

Per SUT, per run, per profile:

| Metric | Source | Units |
|---|---|---|
| Caller-thread CPU time on emit | `getrusage(RUSAGE_THREAD)` deltas, integrated over the measurement window | μs / 1k spans |
| Process CPU time | `getrusage(RUSAGE_SELF)` | seconds |
| Peak RSS | `/proc/self/status` `VmHWM`, polled every 100 ms | MB |
| Steady-state RSS (median) | same, median over window | MB |
| Throughput (sink-observed) | sink counters | items/sec |
| Items emitted | SUT counter | count |
| Items dropped — total | SUT counter (sum of all `DropReason` buckets via `IDiagnosticsSink`) | count |
| Items dropped — `QueueFull` | `RecordDrop(DropReason::QueueFull)` counter | count |
| Items dropped — `RecordTooLarge` | `RecordDrop(DropReason::RecordTooLarge)` counter | count |
| Items dropped — `SpanAttributeLimit` | `RecordDrop(DropReason::SpanAttributeLimit)` counter | count |
| Items dropped — `AttributeValueTruncated` | `RecordDrop(DropReason::AttributeValueTruncated)` counter | count |
| Drop rate | computed (total drops / items emitted) | % |
| StartSpan overhead | per-call wallclock histogram in SUT | ns p50/p95/p99 |
| Span lifecycle overhead | StartSpan→End wallclock | ns p50/p95/p99 |
| Encoding cost per batch | timing around protobuf serialize | μs |
| Wire bytes per signal | sink-observed, gzip on/off | bytes/span |
| Cold-start to first export | driver-observed | ms |
| Binary size (stripped) | static measurement | bytes |
| Dep closure size | `ldd` + file sizes | bytes |

All histograms ship as raw HDR-histogram data alongside the summary, so users can re-derive any percentile.

---

## 6. Reporting

### 6.1 `results.json`
Full machine-readable record: environment fingerprint, profile config, every metric, every percentile, every run's raw data. Schema versioned. Designed to feed downstream dashboards or regression-detection tooling.

### 6.2 `results.md`
Human-readable report. Layout:

1. **Environment fingerprint** (CPU, kernel, lib SHAs, etc.) — first, so the reader knows what they're looking at.
2. **TL;DR table.** Median values, side by side, with relative deltas. Includes the IQR so deltas inside the noise band are visible as such.
3. **Per-profile sections.** Detailed stats, with notes on anomalies (e.g., "SUT-B saw 0.3% drop rate; SUT-A saw 0%").
4. **Methodology footer.** What was measured, how, what was held constant, what wasn't.

Example TL;DR row format:

```
| Metric                       | microtel        | otel-cpp+gRPC   | otel-cpp+HTTP   | Δ (microtel vs gRPC) |
|------------------------------|-----------------|-----------------|-----------------|----------------------|
| StartSpan p50 (ns)           |  82  [76–89]    | 148  [140–161]  | 121  [115–129]  | -45%                 |
| Peak RSS (MB)                |  18.4           |  62.1           |  41.7           | -70%                 |
| Wire bytes/span (gzip off)   | 214             | 214             | 214             |  0%                  |
| Binary size stripped (KB)    | 1380            | 4920            | 4220            | -72%                 |
```

Every cell with a square-bracket range is `[p25–p75]`. Cells without ranges are deterministic measurements (sizes, byte counts on identical payloads).

### 6.3 Optional plots
`results.html` with Plotly charts (latency CDFs, RSS over time, throughput over time). Off by default to keep the harness dependency-light; enable with `--with-plots`.

### 6.4 Optional flame graphs
`--flamegraph <sut>` runs `perf record` against the named SUT during the measurement window and renders a flame graph SVG via Brendan Gregg's scripts (vendored). Requires `perf` on the host and the `SYS_ADMIN` capability for the container. Off by default.

---

## 7. Reproducibility Guarantees

- **Pinned versions.** Every external dep (compiler image, OS base, otel-cpp release tag, microtel commit, collector release) is pinned in `bench/versions.lock`. Updates are a deliberate PR.
- **Deterministic workload.** Workload generation uses a seeded PRNG; the same seed + profile yields the same sequence of spans/metrics/logs.
- **Order randomization.** SUT-run order is shuffled per repetition to wash out time-ordered drift.
- **Cold cache, warm process.** Each repetition starts a fresh SUT process, runs `warmup_seconds` of traffic that is discarded, then measures.
- **Environment guards.** Driver refuses to run (with a warning override flag) if it detects:
  - CPU governor is not `performance`
  - System load average > 0.5 at start
  - SMT/hyperthreading is enabled and `--allow-smt` not passed
  - Available memory below a threshold
- **Known-noise floor.** A `null-vs-null` calibration run (same SUT vs itself across two containers) is included in the harness; it reports the noise floor of the measurement environment so users can tell the difference between "real win" and "in the noise."

---

## 8. Extensibility

- **Adding a library.** Drop a new SUT Dockerfile in `bench/sut/<name>/`, add it to `bench/sut/registry.yaml`, rebuild. The harness is library-agnostic by design — `otel-rust`, `otel-go`, or future microtel siblings can be benchmarked side-by-side. The library doesn't need to be C++.
- **Adding a profile.** Drop a YAML in `bench/profiles/`. No driver changes.
- **Adding a metric.** Two changes: SUT counter exposure + driver schema update. Backward compatible — old reports stay readable.
- **CI integration.** A `--regression-check baseline.json` mode compares the current run against a stored baseline and exits non-zero on regression beyond a configurable threshold (default 5% on key metrics). Drop-in for Jenkins, GitHub Actions, GitLab CI.

---

## 9. Project Layout

Lives inside the main `microtel` repo at `bench/`, built behind
`cmake -DMICROTEL_BUILD_BENCH=ON`. Benchmarks are atomically versioned with
the library; the directory can be extracted to its own repo later if needed
(`git filter-repo --subdirectory-filter bench/`). See ICP 0005.

```
microtel/
└── bench/
    ├── README.md                       (one-page run instructions)
    ├── bench.sh                        (entry point)
    ├── CMakeLists.txt                  (included when MICROTEL_BUILD_BENCH=ON)
    ├── driver/                         (Python orchestration)
    │   ├── __main__.py
    │   ├── env_fingerprint.py
    │   ├── runner.py
    │   ├── stats.py
    │   └── report.py
    ├── emit-app/                       (workload, compile-time-selected backend)
    │   ├── CMakeLists.txt
    │   ├── src/main.cpp
    │   ├── src/workload_*.cpp
    │   ├── src/control_socket.cpp
    │   ├── src/backend_microtel.cpp    (microtel adapter)
    │   ├── src/backend_otelcpp.cpp     (otel-cpp adapter)
    │   └── include/backend.hpp         (common interface)
    ├── sut/
    │   ├── microtel/Dockerfile
    │   ├── otelcpp-grpc/Dockerfile
    │   ├── otelcpp-http/Dockerfile
    │   ├── base.Dockerfile
    │   └── registry.yaml
    ├── sink/
    │   ├── blackhole/                  (Go, ~500 LOC)
    │   └── collector/config.yaml
    ├── profiles/
    │   ├── hot-loop-traces.yaml
    │   ├── realistic-request.yaml
    │   ├── cold-start.yaml
    │   ├── bursty.yaml
    │   ├── large-attributes.yaml
    │   └── binary-size.yaml
    ├── versions.lock
    ├── docs/
    │   ├── methodology.md
    │   ├── interpreting-results.md
    │   └── adding-a-library.md
    └── results/                        (gitignored; user output lands here)
```

The CI workflow (`.github/workflows/benchmark.yml`) is a separate scheduled
run; it does not block PR merges.

---

## 10. Roadmap

| Milestone | Scope |
|---|---|
| **B0** | Driver scaffolding, env fingerprint, blackhole sink, microtel + otel-cpp-gRPC SUTs, `hot-loop-traces` profile only, JSON output. |
| **B1** | All v1 profiles, Markdown report, otel-cpp-HTTP SUT. |
| **B2** | otel-collector sink mode, regression-check mode, CI templates. |
| **B3** | Flamegraph integration, Plotly plots, Rust/Go SUT examples. |
| **B4** | Public publish: blog post + reproducible results on a known reference machine, inviting third-party validation. |

---

## 11. Anti-Goals (Things The Harness Will *Not* Do)

- **Pick a winner for the user.** The report shows the data; the user makes the call. We never print "microtel wins" headlines.
- **Hide variance.** Single-number summaries without IQR/CIs are forbidden in the report template.
- **Compare across machines.** Every report is single-machine. Cross-machine comparison is the user's job and the methodology doc says so explicitly.
- **Synthesize numbers.** No "scaled" or "normalized" metrics that aren't directly measured.
- **Run as root by default.** Optional capabilities (perf, cpuset) require explicit opt-in flags.

---

## 12. Open Questions

1. **Default sink:** blackhole or collector? Leaning blackhole for primary comparison (lower noise), collector available as `--sink=collector`. Confirm.
2. **Should the harness include otel-cpp+nghttp2 hybrid?** I.e., otel-cpp's HTTP exporter but built against nghttp2 instead of its current HTTP client. Would isolate "is the win from the transport, or from the SDK rewrite, or both?" Worthwhile but adds maintenance burden. Defer to v2?
3. **Languages beyond C++.** Stock OTel Python, Go, Rust comparisons would be informative but they're different implementations entirely — apples-to-oranges. Document carefully if included.
4. **Hosting reference results.** A public reference machine (GitHub-hosted runner? a known cloud VM type? a beefy local box?) for the canonical baseline numbers. Pick one, document it, run weekly.
5. **License of the workload code.** Apache 2.0 to match microtel and OTel. Confirmed?

---

## 13. References

- microtel core spec (`microtel-spec.md`) and `docs/bench-spec.md` (this document)
- [HdrHistogram](https://github.com/HdrHistogram/HdrHistogram_c) — for percentile recording in the SUT
- [bloaty](https://github.com/google/bloaty) — for binary size analysis
- [Brendan Gregg, "Systems Performance" Ch. 12](https://www.brendangregg.com/sysperfbook.html) — methodology references
- [OpenTelemetry Collector](https://github.com/open-telemetry/opentelemetry-collector) — pinned for collector-mode sink
