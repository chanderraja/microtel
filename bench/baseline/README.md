# Benchmark Baseline

`results.json` is the reference document for the perf-gate regression check
in `benchmark.yml`. When the file exists, the weekly benchmark job runs
`--regression-check baseline/results.json --threshold 0.05` and exits non-zero
if any SUT regresses by more than 5%.

## What is gated now

The current placeholder file has `latency_*.median = 0`, which the regression
check skips (`b_val <= 0 → continue`). Only `drop_rate_pct` is actively gated:
any SUT that increases its span drop rate by more than 5 percentage points
relative to the baseline (0.0%) will fail the benchmark job.

## How to update the latency baseline

Run the weekly benchmark on the designated reference machine (ideally a
bare-metal CI runner with a fixed CPU governor), then replace this file:

```bash
# 1. Trigger benchmark.yml on the reference runner.
# 2. Download the bench-results-<sha> artifact.
# 3. Extract results.json into this directory:
cp /path/to/downloaded/results.json bench/baseline/results.json
git add bench/baseline/results.json
git commit -m "bench: update perf baseline from run <sha>"
```

Or use the helper script:

```bash
ci/scripts/baseline-update.sh <sha>
```

## Thresholds

| Metric | Gate | Mode |
|---|---|---|
| `latency_p50_ns` median | +5% relative | Active once baseline populated |
| `latency_p95_ns` median | +5% relative | Active once baseline populated |
| `latency_p99_ns` median | +5% relative | Active once baseline populated |
| `drop_rate_pct`  | +5 pp absolute | **Active now** |
