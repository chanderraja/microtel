# `hot_reload/`: retuning a live pipeline

Four setters on `Provider` change configuration without restarting the process
(ICP [0026](../../docs/icps/0026-provider-setters.md)):

```cpp
Status SetBatchOptions(const BatchOptions& opts) noexcept;
Status SetMetricInterval(std::chrono::milliseconds interval) noexcept;
Status SetSamplerRatio(double ratio) noexcept;
Status SetLogLevel(LogLevel level) noexcept;
```

This example emits one root span every 50 ms for a minute and applies a
schedule of changes to the running provider, printing the `Status` each call
returned. The output is a transcript of the setters' contract, including the
calls that get refused.

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
| 10 s | `SetSamplerRatio(0.1)` | `Completed`; the sampled rate drops to about a tenth |
| 20 s | `SetBatchOptions({.max_queue_size = 0, …})` | `InvalidArgument`, with the reason in the internal log |
| 30 s | `SetLogLevel(Error)` | `Completed` |
| 40 s | the same invalid `SetBatchOptions` | `InvalidArgument` again, but with no explanation, because the log floor is now above `Warn` |
| 45 s | `SetLogLevel(Warn)` | `Completed` |
| 50 s | `SetBatchOptions({4096, 128, 1s})` | `Completed` |
| after `Shutdown` | `SetSamplerRatio(0.1)` | `AlreadyShutDown` |
| on a provider with an `AlwaysOn` sampler | `SetSamplerRatio(0.1)` | `Unsupported` |

The last two run after the emitter has finished, so one run shows all four
outcomes a setter can produce. (The console line for the last call says
`SetSamplerRatio(0.5)`, but `main.cpp` passes `0.1`. The ratio makes no
difference, since an `AlwaysOn` sampler refuses any value.)

The example installs a `microtel::SetLogSink` that prints microtel's internal
diagnostics inline. That's where the reason for a rejection shows up: `Status`
says `InvalidArgument`, and the log says which field and what range.

---

## The setters' contract

Each setter validates first and changes nothing if it rejects. There's no
partial application. `SetBatchOptions({.max_queue_size = 0,
.max_export_batch_size = 512, .schedule_delay = 1s})` leaves the queue size,
the batch size and the delay exactly as they were, even though only one field
was wrong. Validation also runs before the support check, so a bad value is
reported as bad regardless of which pipelines the provider happens to own.

What each setter rejects with `InvalidArgument`:

| Setter | Rejected |
|---|---|
| `SetBatchOptions` | `max_queue_size == 0`; `max_export_batch_size == 0`; `max_export_batch_size > max_queue_size`; `schedule_delay <= 0ms` |
| `SetMetricInterval` | `interval <= 0ms` |
| `SetSamplerRatio` | NaN; `< 0.0`; `> 1.0` |
| `SetLogLevel` | a value outside the declared `LogLevel` enumerators |

The setters are stricter than the builder. `SdkBuilder` only checks that
`max_export_batch_size` doesn't exceed `max_queue_size`, so a zero queue size
(paired with a zero batch size), a zero batch size or a zero `schedule_delay`
all pass `Build()` today and produce a processor that never drains or spins.
ICP 0026 records this under Discrepancies.

After `Shutdown`, every setter returns `AlreadyShutDown`. The shutdown flag is
checked before any mutex is taken, the same pattern `GetMeter` and `GetLogger`
already use, so a forked child can't deadlock on a lock held by a thread that
no longer exists.

`Unsupported` means the provider has no such knob. It says nothing about the
health of the pipeline. There are two cases:

- `SetSamplerRatio` on a sampler that has no ratio. An `AlwaysOn` or
  `AlwaysOff` sampler is never converted into one that does. The sampler
  object's identity is fixed for the provider's lifetime, and that's what makes
  retuning safe at all (every `Tracer` caches a raw pointer to it). A
  `ParentBased` sampler forwards to the root it owns, so
  `parentbased_traceidratio` retunes; if the root returns `false`, so does the
  chain.
- `SetMetricInterval` with no metrics pipeline. A provider built by
  `SdkBuilder` always has one, so this example can't demonstrate that case and
  shows the sampler case instead.

`SetSamplerRatio` rejects out-of-range values where the factory clamps them.
`MakeTraceIdRatioSampler` clamps at build time as a documented convenience. At
reload time, a caller asking for `1.5` has a bug in its administrative surface,
and clamping would hide it from the operator reading the return value.

`SetLogLevel` is process-wide. The internal log path is a free function with no
provider in scope, so providers built from different profiles share this knob
and the last writer wins.

`SetBatchOptions` isn't atomic across pipelines. It retunes the span processor,
then the log processor, and never holds both locks at once. So during a single
call, a span batch may be cut under the new options while a log batch is still
using the old ones.

---

## What to watch in Grafana while it runs

Open <http://localhost:3000> before you start the run. Every tick is a root
span, so each tick is one trace and the tick rate equals the trace rate.

1. For the first ten seconds, about 20 traces a second arrive, and the
   dashboard's table fills as fast as it refreshes.
2. After `SetSamplerRatio(0.1)` at t=10s, the rate falls to roughly 2 a
   second. On the **microtel — recent traces** dashboard you can see it thin
   out; in Explore → Tempo with
   `{resource.service.name="microtel-hot-reload-example"}` you get a shorter
   list per refresh. The console's `(last 5s: 100 emitted, 9 sampled)` line
   measures the same thing at the source.
3. Nothing changes at t=30s or t=45s. The log-level knob controls microtel's
   own diagnostics and has no effect on telemetry, so watch both
   to see the difference.
4. After the valid `SetBatchOptions` at t=50s, batches are cut every second
   instead of every five, so traces arrive in smaller, more frequent groups.
   That's hard to see in Tempo, but the collector shows it:
   `podman-compose -f examples/stack/compose.yaml -p microtel-stack logs
   otel-collector` prints a line per batch.

At the end the run prints the last sampled trace ID. Paste it into Explore →
Tempo, or:

```bash
curl -s http://localhost:3200/api/traces/<trace-id> | head -c 200
```

Then the process flushes, reports health and shuts down. The whole run takes
about 60 seconds of emitting plus a second of teardown.

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

last sampled trace_id: 9bf0b8aa…
  Grafana: http://localhost:3000 (Explore -> Tempo, or the "microtel - recent traces" dashboard)

ForceFlush: Completed
batches_sent=15 batches_failed=0 queue_depth=0
Shutdown: Completed

after Shutdown, SetSamplerRatio(0.1) -> AlreadyShutDown
         [microtel warn] SetSamplerRatio: the configured sampler has no ratio to retune
                         - nothing changed
SetSamplerRatio(0.5) on an AlwaysOn sampler -> Unsupported
  (that provider's Shutdown: Completed)
```

Trimmed to fit: full trace IDs, the per-5s report lines between the steps, and
the wrapping of the long warn lines.

---

## What can't be hot-reloaded

The endpoint, the protocol, TLS material, the resource and the sampler object
are all fixed at `Build()`. Changing any of them means building another
`Provider`, which is what
[`health_and_backpressure/`](../health_and_backpressure/) does when it moves
off a dead collector. Swapping the sampler object is deferred, and ICP 0026
explains why under "Deferred": every `Tracer` caches a raw pointer to it, so a
live swap would be a data race plus a use-after-free, and fixing that would put
a refcount round-trip inside `StartSpan`.
