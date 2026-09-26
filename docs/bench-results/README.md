# Committed benchmark snapshot

These files are one benchmark run, committed verbatim so the numbers in the
root [`README.md`](../../README.md) can be checked from a clone. The build
doesn't produce them. A local `cd bench && ./bench.sh` writes to
`bench/results/`, which is gitignored, so the README used to link a path that
only existed for people who had run the harness themselves.

The snapshot was refreshed on 2026-09-25 for the v1.1.1 tag, from a clean
checkout of the release commit (`683b237`), on the same host as the previous
snapshot (Ryzen 5 5600G, 12 cores, SMT on, podman 5.8.4, kernel
7.1.13-200.fc44). Host load was 0.28 at the start. Before committing, the
environment block was diffed against the previous snapshot as
[`RELEASING.md`](../../RELEASING.md) §5 requires.

## Changes since the previous snapshot

**One identity field changed: the CPU governor is `performance`, where the
v1.1.0 snapshot used `powersave`.** Every other field matches (CPU model, core
count, SMT, kernel, container engine). A governor change moves numbers for code
that hasn't changed, so don't read the deltas below as microtel getting faster
or slower. Compare SUTs within this run instead: they all ran under the same
governor, minutes apart.

Against v1.1.0, the unchanged otelcpp SUTs moved −3.7% (gRPC) and −2.0% (HTTP)
in spans/sec; microtel moved +2.5% (HTTP) and +0.8% (gRPC). All of it is inside
this host's run-to-run spread. StartSpan p50 is 236 ns for microtel against
812 ns for otelcpp-gRPC, a 3.4× ratio (3.5× in the previous snapshot, within
the same spread).

The microtel binary grew from **15,359,952 to 15,952,688 bytes (+3.9%)**
because of v1.1.1 code: the shared retry engine for all three signals, per-key
merging of table-valued settings, and the resolved-Resource log line, which
brings in `std::format`. The unchanged otelcpp-gRPC binary is byte-identical
across the rebuild.

The other two profiles in the same session: `realistic-request` shows 100%
delivery for both SUTs, and `compression` shows 342.2 B/span uncompressed
against 39.2 gzip'd (8.7×), both unchanged from v1.1.0.

## Files

| File | What it is |
|---|---|
| [`results.md`](results.md) | Generated summary: environment, profile, warnings, results table. Start here. |
| [`results.json`](results.json) | Raw per-sample data, schema version 1.0. What the plots and the summary are rendered from. |
| [`plots.html`](plots.html) | Interactive charts. Loads plotly from a CDN, so it needs network to render. |
| [`leaf-footprint.md`](leaf-footprint.md) | Not part of the benchmark run: the experimental leaf's flash and RAM for both encoder backends, as the `leaf-footprint` CI job measures them. Refreshed per release. |

## This snapshot

| Field | Value |
|---|---|
| Profile | `hot-loop-traces` — 10 000 spans/sample, 10 samples, 1 000 warmup spans, blackhole sink |
| SUTs | `microtel`, `microtel-grpc`, `otelcpp-grpc`, `otelcpp-http` |
| Generated | 2026-09-25 (release commit `683b237`) |
| Host | AMD Ryzen 5 5600G, 12 physical cores, Fedora, podman 5.8.4 |

The run carries one warning, left in `results.md` and `results.json`: SMT
was enabled. It widens the spread but doesn't change the order-of-magnitude
comparisons in the README. The governor warning the previous snapshot carried
is gone because this run used `performance`.

Two other profiles, `realistic-request` and `compression`, were run on the same
host in the same session. They aren't committed here, since this directory
holds only the profile the root README quotes, but the delivery and
compression figures above come from them. Each profile started with host
load below 0.5 and the governor at `performance`, checked before and after.

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
