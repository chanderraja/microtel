# Committed benchmark snapshot

The files here are one benchmark run, committed verbatim so the numbers in the
root [`README.md`](../../README.md) are checkable from a clone.

They are **not** produced by the build. `bench/results/` is where a local
`cd bench && ./bench.sh` writes its output, and that directory is gitignored —
which is why the README used to link a path that did not exist for anyone who
had not run the harness themselves.

| File | What it is |
|---|---|
| [`results.md`](results.md) | Generated summary: environment, profile, warnings, results table. Start here. |
| [`results.json`](results.json) | Raw per-sample data, schema version 1.0. What the plots and the summary are rendered from. |
| [`plots.html`](plots.html) | Interactive charts. Loads plotly from a CDN, so it needs network to render. |

## This snapshot

| Field | Value |
|---|---|
| Profile | `hot-loop-traces` — 10 000 spans/sample, 10 samples, 1 000 warmup spans, blackhole sink |
| SUTs | `microtel`, `microtel-grpc`, `otelcpp-grpc`, `otelcpp-http` |
| Generated | 2026-07-13 |
| Host | AMD Ryzen 5 5600G, 12 physical cores, Fedora, podman 5.8.2 |

The run carries three warnings, all recorded in `results.md` and `results.json`
rather than edited out: the CPU governor was `powersave`, host load average was
0.67, and SMT was enabled. They widen the spread; they do not move the
order-of-magnitude comparisons the README draws.

## Refreshing it

```bash
cd bench && ./bench.sh                     # writes bench/results/
cp bench/results/{plots.html,results.json,results.md} docs/bench-results/
```

Then update the table in the root `README.md` to match — the point of this
directory is that the two agree. A refresh from a different machine updates the
environment block here too. This snapshot is documentation, not a gate:
`bench/baseline/results.json` is the separate file the weekly perf-regression
check reads (see [`bench/baseline/README.md`](../../bench/baseline/README.md)).
