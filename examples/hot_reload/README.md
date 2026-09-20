# `hot_reload/` — retuning a live pipeline

Four knobs on `Provider` change configuration without restarting the process
(ICP [0026](../../docs/icps/0026-provider-setters.md)):

```cpp
Status SetBatchOptions(const BatchOptions& opts) noexcept;
Status SetMetricInterval(std::chrono::milliseconds interval) noexcept;
Status SetSamplerRatio(double ratio) noexcept;
Status SetLogLevel(LogLevel level) noexcept;
```

This example emits one root span every 50 ms for a minute and applies a
schedule of changes to the running provider, printing the `Status` each one
returned. It is a transcript of the contract, including the calls that are
**refused**.

```bash
examples/stack/up.sh
cmake -S . -B build -DMICROTEL_BUILD_EXAMPLES=ON && cmake --build build
./build/examples/microtel_example_hot_reload        # ~60s, then exits cleanly
```

The endpoint defaults to `http://localhost:4317`; pass another as `argv[1]`.

---

## What it does, and when

| t | Call | Returns |
|---|---|---|
| 10 s | `SetSamplerRatio(0.1)` | `Completed` — the sampled rate drops to about a tenth |
| 20 s | `SetBatchOptions({.max_queue_size = 0, …})` | `InvalidArgument`, with the reason in the internal log |
| 30 s | `SetLogLevel(Error)` | `Completed` |
| 40 s | the same invalid `SetBatchOptions` | `InvalidArgument` — same outcome, **no explanation**: the log floor is above `Warn` now |
| 45 s | `SetLogLevel(Warn)` | `Completed` |
| 50 s | `SetBatchOptions({4096, 128, 1s})` | `Completed` |
| after `Shutdown` | `SetSamplerRatio(0.1)` | `AlreadyShutDown` |
| on a provider with an `AlwaysOn` sampler | `SetSamplerRatio(0.5)` | `Unsupported` |

The last two run after the emitter has finished, so all four outcomes a setter
can produce appear in one run.

The example installs a `microtel::SetLogSink` that prints microtel's internal
diagnostics inline. That is where the *reason* for a rejection lives: `Status`
says `InvalidArgument` and the log says which field and what range.

---

## The setters' contract

**Validate first, then change nothing on rejection.** A rejected call is not a
partial application. `SetBatchOptions({.max_queue_size = 0,
.max_export_batch_size = 512, .schedule_delay = 1s})` leaves the queue size,
the batch size *and* the delay exactly as they were, even though only one field
was wrong. Validation also runs **before** the support check, so a bad value is
reported as bad whatever pipelines the provider happens to own.

What each setter rejects with `InvalidArgument`:

| Setter | Rejected |
|---|---|
| `SetBatchOptions` | `max_queue_size == 0`; `max_export_batch_size == 0`; `max_export_batch_size > max_queue_size`; `schedule_delay <= 0ms` |
| `SetMetricInterval` | `interval <= 0ms` |
| `SetSamplerRatio` | NaN; `< 0.0`; `> 1.0` |
| `SetLogLevel` | a value outside the declared `LogLevel` enumerators |

Note that **the setters are stricter than the builder**: `SdkBuilder` accepts a
zero `max_queue_size` today and produces a processor that never drains. That
divergence is recorded in ICP 0026's Discrepancies, not hidden.

**`AlreadyShutDown` after `Shutdown`.** Every setter checks the shutdown flag
*before* it takes any mutex — the pattern `GetMeter` and `GetLogger` already
use, so a forked child cannot deadlock on a lock held by a thread that no
longer exists.

**`Unsupported` means "this provider has no such knob"**, and is not an error
condition of the pipeline:

- `SetSamplerRatio` on a sampler with no ratio. An `AlwaysOn` or `AlwaysOff`
  sampler is **never converted** into one that has a ratio — the sampler
  object's identity is fixed for the provider's life, which is what makes
  retuning safe at all (every `Tracer` caches a raw pointer to it). A
  `ParentBased` sampler forwards to the root it owns, so
  `parentbased_traceidratio` retunes; a `false` from the root is a `false` from
  the chain.
- `SetMetricInterval` with no metrics pipeline. A provider built by
  `SdkBuilder` always has one, so this example cannot demonstrate that half —
  it demonstrates the sampler half instead.

**`SetSamplerRatio` rejects rather than clamps.** `MakeTraceIdRatioSampler`
clamps at build time, where it is a documented convenience. At reload time a
caller asking for `1.5` has a bug in its administrative surface, and clamping
would hide it from the operator reading the return value.

**`SetLogLevel` is process-wide.** The internal log path is a free function
with no provider in scope, so providers built from different profiles share
this knob and the last writer wins.

**`SetBatchOptions` is not atomic across pipelines.** It retunes the span
processor, then the log processor, never holding two locks at once. For the
duration of one call a span batch may be cut under the new options while a log
batch is still under the old.

---

## What to watch in Grafana while it runs

Open <http://localhost:3000> before starting the run. Every tick is a **root**
span, so one tick is one trace and the tick rate and the trace rate are the
same number.

1. **First ten seconds:** about 20 traces a second arrive — the dashboard's
   table fills as fast as it refreshes.
2. **After `SetSamplerRatio(0.1)` at t=10s:** the rate falls to roughly 2 a
   second. On the **microtel — recent traces** dashboard that is a visible
   thinning; in **Explore → Tempo** with
   `{resource.service.name="microtel-hot-reload-example"}` it is a shorter
   list per refresh. The console's own `(last 5s: 100 emitted, 9 sampled)`
   line is the same fact measured at the source.
3. **Nothing changes at t=30s or t=45s.** The log-level knob is about
   microtel's own diagnostics, not about telemetry — the trace rate is
   unaffected, which is the point of watching both.
4. **After the valid `SetBatchOptions` at t=50s:** batches are cut every
   second instead of every five, so traces arrive in smaller, more frequent
   groups. Tempo makes this hard to see directly; the collector does —
   `podman-compose -f examples/stack/compose.yaml -p microtel-stack logs
   otel-collector` shows a line per batch.

The run prints the last sampled trace ID at the end. Paste it into **Explore →
Tempo**, or:

```bash
curl -s http://localhost:3200/api/traces/<trace-id> | head -c 200
```

Then the process flushes, reports health, and shuts down — about 60 seconds of
emitting plus a second of teardown.

---

## Sample run

```
endpoint: http://localhost:4317
sampler:  TraceIdRatio(1)
running for 60s, one root span every 50ms

t=+5s  emitted=101 sampled=101  (last 5s: 101 emitted, 101 sampled)  trace_id=dad391af…
t=+10s  SetSamplerRatio(0.1) -> Completed
t=+15s  emitted=300 sampled=207  (last 5s: 99 emitted, 6 sampled)  trace_id=8ed77ef8…
         [microtel warn] SetBatchOptions rejected: max_queue_size and max_export_batch_size
                         must both be non-zero, max_export_batch_size must not exceed
                         max_queue_size, and schedule_delay must be greater than zero
t=+20s  SetBatchOptions({max_queue_size=0}) [invalid] -> InvalidArgument
t=+30s  SetLogLevel(Error) -> Completed
t=+40s  SetBatchOptions({max_queue_size=0}) [invalid, and now unexplained] -> InvalidArgument
t=+45s  SetLogLevel(Warn) -> Completed
t=+50s  SetBatchOptions({4096, 128, 1s}) [valid] -> Completed
t=+60s  emitted=1197 sampled=291  (last 5s: 99 emitted, 8 sampled)  trace_id=9bf0b8aa…

ForceFlush: Completed
batches_sent=15 batches_failed=0 queue_depth=0
Shutdown: Completed

after Shutdown, SetSamplerRatio(0.1) -> AlreadyShutDown
         [microtel warn] SetSamplerRatio: the configured sampler has no ratio to retune
                         - nothing changed
SetSamplerRatio(0.5) on an AlwaysOn sampler -> Unsupported
```

Elided for width: full trace IDs, the per-5s report lines between the steps,
and the second warn line's wrapping.

---

## What is *not* hot-reloadable

The endpoint, the protocol, TLS material, the resource, and the sampler
*object* are all fixed at `Build()`. Changing any of them means building
another `Provider` — which is what
[`health_and_backpressure/`](../health_and_backpressure/) does when it moves
off a dead collector. Swapping the sampler object is deferred with a reason
(ICP 0026, "Deferred"): every `Tracer` caches a raw pointer to it, so a live
swap is a data race plus a use-after-free, and fixing that would put a
refcount round-trip inside `StartSpan`.
