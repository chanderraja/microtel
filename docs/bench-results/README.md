# Committed benchmark snapshot

These files are one benchmark run, committed verbatim so the numbers in the
root [`README.md`](../../README.md) can be checked from a clone. The build
doesn't produce them. A local `cd bench && ./bench.sh` writes to
`bench/results/`, which is gitignored, so the README used to link a path that
only existed for people who had run the harness themselves.

The snapshot was refreshed on 2026-09-16 for the v1.1.0 tag, on the same host
as the v1.0 snapshot (Ryzen 5 5600G, 12 cores, `powersave` governor, SMT on,
podman 5.8.4, kernel 7.1.13-200.fc44). Host load was 0.12 at the start. Before
committing, the environment block was diffed against the previous snapshot as
[`RELEASING.md`](../../RELEASING.md) §5 requires, and every identity field
matched. Without that, the deltas below would mean nothing.

## Changes since v1.0 that come from the harness

Three numbers moved because the harness was fixed. Read this before comparing
against the v1.0 snapshot.

1. **Latency percentiles are rank-interpolated**
   ([#261](https://github.com/chanderraja/microtel/issues/261)/[#262](https://github.com/chanderraja/microtel/pull/262)).
   `Percentile()` used to return log2-bucket midpoints, so every percentile
   ≥128 ns was a multiple of 192 ns. That is where v1.0's `192`/`384`/`768 ns`
   came from. Values are now interpolated within the bucket: microtel p50
   `192 → 229`, p95 `384 → 486.5`; otelcpp p50 `768 → 807.5`. Both sides were
   quantised, so the old 4.0× p50 ratio was partly a bucketing artifact, and
   the same-host ratio is 3.5×. v1.1 did not get slower: throughput over the
   same interval is within noise (microtel −4.0%, microtel-grpc +0.1%, and the
   unchanged otelcpp SUTs +6.0% / +8.0%).
2. **Delivery and drop percentages use `spans_expected`**
   ([#215](https://github.com/chanderraja/microtel/issues/215)/[#230](https://github.com/chanderraja/microtel/pull/230),
   [#229](https://github.com/chanderraja/microtel/issues/229)/[#275](https://github.com/chanderraja/microtel/pull/275)).
   The blackhole sink couldn't inflate gzip'd bodies and divided by the wrong
   denominator, which reported 300% delivery on the multi-span
   `realistic-request` profile. It now reports 100.00%. The `hot-loop-traces`
   profile committed here doesn't show it, since nothing drops and 0 over
   either denominator is 0.
3. **gRPC `bytes_received` counts wire bytes**
   ([#228](https://github.com/chanderraja/microtel/issues/228)/[#275](https://github.com/chanderraja/microtel/pull/275)),
   including the 5-byte length-prefix frame header, and counts the compressed
   body instead of the inflated proto size. Uncompressed gRPC gains 5 bytes
   per message: in the `compression` profile gRPC is `342.1724` B/span against
   `342.1619` for HTTP, a `0.0105 × 10 000 = 105` byte difference over 21
   messages, which is 21 × 5. Here it shows as `wire_bytes_per_span` going
   62.16 → 62.17 on `microtel-grpc` while `microtel` (HTTP) is unchanged.

The `compression` profile now also shows gzip's real saving, which the old
accounting hid: 342.16 B/span uncompressed against 39.15 gzip'd (8.7×), with
the gzip SUTs going from a meaningless 0% delivery to a measured 100%.

The microtel binary did grow, from **14,229,216 to 15,359,952 bytes (+7.9%)**,
because of v1.1 feature code (sampler chains, propagation core, baggage, sugar,
the four setters, multi-profile). For comparison, the unchanged otelcpp-gRPC
binary moved 2 088 bytes across the same rebuild.

## Files

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

The run carries two warnings, left in `results.md` and `results.json`: the
CPU governor was `powersave` and SMT was enabled. They widen the spread but
don't change the order-of-magnitude comparisons in the README. The v1.0
snapshot had a third warning for host load (0.81); this run started at 0.12.

Two other profiles, `realistic-request` and `compression`, were run on the same
host in the same session. They aren't committed here, since this directory
holds only the profile the root README quotes, but the delivery and
compression figures above come from them.

## Refreshing it

```bash
cd bench && ./bench.sh                     # writes bench/results/
cp bench/results/{plots.html,results.json,results.md} docs/bench-results/
```

Then update the table in the root `README.md` so the two agree.

**Diff the `environment` block against the outgoing snapshot before you
commit** ([`RELEASING.md`](../../RELEASING.md) §5). A run from a different
machine makes every delta unreadable. The v1.1.0 attempt from CI's 4-core
`ubuntu-24.04` runner showed a ~45% throughput drop on SUTs whose code hadn't
changed; an unchanged SUT that moves is the giveaway. Until
[#277](https://github.com/chanderraja/microtel/issues/277) settles on a
reference host, refresh this directory from a local run on the host above,
not from a `benchmark.yml` artifact.

This snapshot is documentation only. The weekly perf-regression check reads a
separate file, `bench/baseline/results.json` (see
[`bench/baseline/README.md`](../../bench/baseline/README.md)).
