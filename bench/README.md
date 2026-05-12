# microtel bench/

Fair, reproducible benchmark harness comparing microtel against
opentelemetry-cpp (gRPC and HTTP exporters) on identical hardware.

Full spec: [`docs/bench-spec.md`](../docs/bench-spec.md)

---

## Quick start

```bash
# Requires: Docker (or Podman), Python 3.11+, cmake

# Build bench targets (from repo root)
cmake -S . -B build -DMICROTEL_BUILD_BENCH=ON -DMICROTEL_BUILD_TESTS=OFF
cmake --build build --target bench

# Run the harness
./bench/bench.sh
```

Results land in `bench/results/` (gitignored).

---

## Layout

| Path | Purpose |
|---|---|
| `bench.sh` | Entry point — builds images, runs driver, writes results |
| `driver/` | Python orchestration (env fingerprint, runner, stats, report) |
| `emit-app/` | C++ workload app — compile-time-selected backend (microtel / otel-cpp) |
| `sut/` | SUT Dockerfiles (one per library/exporter combination) |
| `sink/` | blackhole-sink (Go) and otel-collector config |
| `profiles/` | Workload profile YAMLs |
| `versions.lock` | Pinned versions for reproducibility |
| `docs/` | Methodology, interpreting results, adding a library |

---

## Milestones

| Milestone | Scope |
|---|---|
| **B0** | Driver scaffolding, env fingerprint, blackhole sink, microtel + otel-cpp-gRPC SUTs, `hot-loop-traces` profile only, JSON output |
| **B1** | All v1 profiles, Markdown report, otel-cpp-HTTP SUT |
| **B2** | otel-collector sink mode, regression-check mode, CI templates |
| **B3** | Flamegraph integration, Plotly plots, Rust/Go SUT examples |
| **B4** | Public publish with reference results |

---

## Status

**Scaffold** (B0 in progress). Directory structure is in place; driver,
emit-app, and SUT Dockerfiles are added in the B0 implementation PR.

---

## Design notes

- The harness never picks a winner — it shows the data distribution.
- Every report embeds the full environment fingerprint.
- Results are not comparable across machines; see `docs/methodology.md`.
- `bench/results/` is gitignored. To archive a run, copy `results.json`
  and `results.md` to a dated subdirectory and PR them to `bench/docs/`.
