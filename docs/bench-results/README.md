# Committed benchmark snapshot

**Snapshot refreshed 2026-09-16 for the v1.1.0 tag** — same host as the v1.0 snapshot (Ryzen 5 5600G, 12 cores, `powersave` governor, SMT on, podman 5.8.4, kernel 7.1.13-200.fc44), host load 0.12 at start, so this run carries two warnings where v1.0's carried three: the load warning is gone. The environment block was diffed against the previous snapshot before committing, per [`RELEASING.md`](../../RELEASING.md) §5 — every identity field matches, which is what makes the deltas below readable at all.

> ### Three numbers moved because the harness was fixed, not the code
>
> Read these before comparing against the v1.0 snapshot.
>
> 1. **Latency percentiles are rank-interpolated** ([#261](https://github.com/chanderraja/microtel/issues/261)/[#262](https://github.com/chanderraja/microtel/pull/262)). `Percentile()` used to return log2-bucket **midpoints**, so every percentile ≥128 ns was a multiple of 192 ns — that is where v1.0's stamped `192`/`384`/`768 ns` came from. They are interpolated within the bucket now: microtel p50 `192 → 229`, p95 `384 → 486.5`; otelcpp p50 `768 → 807.5`. **Both sides were quantised, so the old 4.0× p50 ratio was partly an artifact of the bucketing.** The honest same-host ratio is **3.5×**. This is the fix landing, not a v1.1 slowdown — throughput over the same interval is within noise (microtel −4.0%, microtel-grpc +0.1%, and the *unchanged* otelcpp SUTs +6.0% / +8.0%).
> 2. **Delivery and drop percentages use `spans_expected`** ([#215](https://github.com/chanderraja/microtel/issues/215)/[#230](https://github.com/chanderraja/microtel/pull/230), [#229](https://github.com/chanderraja/microtel/issues/229)/[#275](https://github.com/chanderraja/microtel/pull/275)). The blackhole sink could not inflate gzip'd bodies and divided by the wrong denominator, which reported **300%** delivery on the multi-span `realistic-request` profile. It now reports a true **100.00%**. Not visible in the `hot-loop-traces` profile committed here — nothing drops, and 0 over either denominator is 0.
> 3. **gRPC `bytes_received` counts wire bytes** ([#228](https://github.com/chanderraja/microtel/issues/228)/[#275](https://github.com/chanderraja/microtel/pull/275)), including the 5-byte length-prefix frame header, and counts the compressed body rather than the inflated proto size. Uncompressed gRPC shifts **+5 bytes per message**: in the `compression` profile, `342.1724` B/span for gRPC against `342.1619` for HTTP — a `0.0105 × 10 000 = 105` byte difference over 21 messages, exactly 21 × 5. Here it shows as `wire_bytes_per_span` 62.16 → 62.17 on `microtel-grpc` while `microtel` (HTTP) is unchanged.
>
> The `compression` profile also now shows gzip's real saving, which the old accounting hid: **342.16 B/span identity vs 39.15 gzip'd, 8.7×**, with the gzip SUTs going from a meaningless 0% delivery to a measured 100%.

The binary grew **14,229,216 → 15,359,952 bytes (+7.9%)** — v1.1 feature code (sampler chains, propagation core, baggage, sugar, the four setters, multi-profile). For scale, the unchanged otelcpp-gRPC binary moved 2 088 bytes across the same rebuild.

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
| Generated | 2026-09-16 |
| Host | AMD Ryzen 5 5600G, 12 physical cores, Fedora, podman 5.8.4 |

The run carries two warnings, recorded in `results.md` and `results.json` rather
than edited out: the CPU governor was `powersave`, and SMT was enabled. They
widen the spread; they do not move the order-of-magnitude comparisons the README
draws. The v1.0 snapshot carried a third — host load 0.81 — which this run does
not: load was 0.12.

It was taken alongside two other profiles on the same host in the same session,
`realistic-request` and `compression`. Those are not committed here (this
directory holds one profile, the one the root README quotes), but they are what
the delivery-denominator and compression annotations above are measured from.

## Refreshing it

```bash
cd bench && ./bench.sh                     # writes bench/results/
cp bench/results/{plots.html,results.json,results.md} docs/bench-results/
```

Then update the table in the root `README.md` to match — the point of this
directory is that the two agree.

**Diff the `environment` block against the outgoing snapshot before you commit**
([`RELEASING.md`](../../RELEASING.md) §5). A refresh from a different machine is
not a refresh, it is a machine swap, and it makes every delta unreadable: the
v1.1.0 attempt from CI's 4-core `ubuntu-24.04` runner showed a ~45% throughput
drop on SUTs whose code had not changed. The tell is an unchanged SUT that
moved. See [#277](https://github.com/chanderraja/microtel/issues/277) — until it
settles on a reference host, refresh this directory from a local run on the host
above, not from a `benchmark.yml` artifact.

This snapshot is documentation, not a gate: `bench/baseline/results.json` is the
separate file the weekly perf-regression check reads (see
[`bench/baseline/README.md`](../../bench/baseline/README.md)).
