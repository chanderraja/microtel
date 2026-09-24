# microtel bench/

A reproducible benchmark harness that compares microtel against
opentelemetry-cpp (gRPC and HTTP exporters) on the same hardware. The
full spec is [`docs/bench-spec.md`](../docs/bench-spec.md).

## Quick start

```bash
# Requires: Docker or Podman, Python 3.11+
./bench/bench.sh
```

`bench.sh` checks the prerequisites and hands off to the Python driver
(`python3 -m driver`; pass `-h` for the options). The driver builds each
system-under-test (SUT) image itself: the microtel SUT compiles microtel
and `emit-app` together from source inside its Dockerfile. You don't
need a host build to run the harness.

To build `emit-app` on the host while working on it:

```bash
cmake -S . -B build -DMICROTEL_BUILD_BENCH=ON -DMICROTEL_BUILD_TESTS=OFF
cmake --build build --target emit_app
```

Results land in `bench/results/`, which is gitignored.

## Layout

| Path | Purpose |
|---|---|
| `bench.sh` | Entry point: checks prerequisites, then runs the driver |
| `driver/` | Python orchestration: env fingerprint, runner, stats, report, regression check, plots, flamegraphs |
| `emit-app/` | C++ workload app with a compile-time-selected backend (microtel or otel-cpp) |
| `sut/` | SUT Dockerfiles, one per library/exporter combination, plus `registry.yaml` |
| `sink/` | `blackhole/` (Go OTLP receiver) and `collector/` (otel-collector sink image and config) |
| `collector-configs/` | `otelcol.yaml`, a standalone collector config for interop testing (the collector sink image uses `sink/collector/config.yaml`) |
| `profiles/` | Workload profile YAMLs |
| `baseline/` | Reference `results.json` for the weekly perf-gate regression check |
| `scripts/` | Auxiliary scripts (`jaeger_interop.py`) |
| `versions.lock` | Pinned versions for reproducibility |
| `docs/` | `methodology.md` |

## Milestones

| Milestone | Scope |
|---|---|
| **B0** | Driver scaffolding, env fingerprint, blackhole sink, microtel + otel-cpp-gRPC SUTs, `hot-loop-traces` profile only, JSON output |
| **B1** | All v1 profiles, Markdown report, otel-cpp-HTTP SUT |
| **B2** | otel-collector sink mode, regression-check mode, CI templates |
| **B3** | Flamegraph integration, Plotly plots, Rust/Go SUT examples |
| **B4** | Public publish with reference results |

B0 through B3 are in the tree: the driver has collector sink mode,
`--regression-check`, Plotly output and `--flamegraph`, the `rust-otel`
and `go-otel` SUTs exist, and `.github/workflows/benchmark.yml` runs
the harness weekly. B4 is not done.

## Design notes

- The harness never picks a winner. It shows the distribution of the
  data.
- Every report embeds the full environment fingerprint.
- Results are not comparable across machines; see
  [`docs/methodology.md`](docs/methodology.md).
- `bench/results/` is gitignored. The run the root README quotes is
  committed separately, verbatim, under
  [`docs/bench-results/`](../docs/bench-results/); its README explains
  how that snapshot is refreshed.
