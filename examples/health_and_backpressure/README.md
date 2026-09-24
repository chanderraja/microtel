# `health_and_backpressure/`: reading `HealthSnapshot`

`Provider::GetExporterHealth()` is the only view an operator gets into the
export pipeline. This example breaks the pipeline two different ways and
prints the snapshot after each, so you can watch every field move.

```bash
examples/stack/up.sh
cmake -S . -B build -DMICROTEL_BUILD_EXAMPLES=ON && cmake --build build
./build/examples/microtel_example_health_and_backpressure    # ~20s
```

Arguments: `[endpoint]` (default `http://localhost:4317`) and `[dead-endpoint]`
(default `http://127.0.0.1:14317`, a port nothing serves).

---

## The three phases

In phase 1 (queue overflow), a provider built with
`WithBatch({.max_queue_size = 64, .max_export_batch_size = 32})`, two orders of
magnitude below the 8192 default, is fed 3000 spans from a tight loop. `End()`
never blocks (`docs/threading-model.md` §3.1). When the queue is full the
record is dropped, so an instrumented application keeps its latency and loses
telemetry instead. `queue_depth_now` climbs, and `drop_counters[QueueFull]`
counts what was lost.

Phase 2 (stalled collector) points a provider at a closed port, with short
timeouts so the failure path doesn't sit out a sixty-second retry budget.
`Connect()` fails up front. The export then fails too, which moves
`batches_failed`, `ConnectFailure` and `RetryBudgetExhausted`, leaves
`connection_state` at `Disconnected`, and puts the reason in
`last_error_message`.

Phase 3 (recovery) goes back to the live endpoint. Batches land, the counters
stay clean, and the example prints a trace ID you can check in Tempo.

Recovery builds a new provider, and that's how a real application would have
to do it too. The endpoint isn't one of the four hot-reload knobs from ICP
0026; `SetBatchOptions`, `SetMetricInterval`, `SetSamplerRatio` and
`SetLogLevel` are the whole list. An application that has to fail over to a
different collector constructs another `Provider`. Each phase here uses its own
`WithProfileName` because a profile name is released when the provider is
destroyed, and `Shutdown` alone doesn't release it.

---

## Every field, and what an operator does with it

| Field | What it means | What to do about it |
|---|---|---|
| `connection_state` | `Disconnected` · `Connecting` · `Connected` · `Reconnecting` · `Closed` | `Disconnected` points at local configuration or reachability: endpoint, TLS material, network path. `Reconnecting` points at the peer or the link. microtel was connected, the connection dropped, and the transport will re-establish it on the next export (ICP 0018). An alert keyed only on `Disconnected` misses drops, so key on both. `Closed` is terminal and means `Shutdown` has run. |
| `batches_sent` | batches the receiver accepted, **across all three signals** | If it stays flat while spans are being produced, nothing is leaving. Compare it against your own emit rate rather than an absolute number. |
| `batches_failed` | batches that ended in a non-retryable failure or exhausted their retry budget | Each one is telemetry that's gone. A steady trickle is usually auth or a protocol mismatch; a burst is usually the collector. Read `last_error_message` before anything else. |
| `queue_depth_now` | records queued right now | A depth sitting near `max_queue_size` means the exporter can't keep up, and `QueueFull` drops come next. Raise `max_queue_size` (costs memory), raise `max_export_batch_size` or lower `schedule_delay` (throughput), or sample less. |
| `drop_counters[QueueFull]` | records the queue refused | The producer is outrunning the exporter, as in phase 1. Alert on this one for "we're losing spans we generated". |
| `drop_counters[ConnectFailure]` | connection attempts that failed | Together with `Disconnected`, a configuration or reachability problem. It moves once per attempt, so it climbs faster than `batches_failed`. |
| `drop_counters[RetryBudgetExhausted]` | batches that used their whole `TimeoutOptions::retry_budget` | The peer was unreachable or overloaded for longer than the budget. A bigger budget trades memory and latency for durability; it won't fix the peer. |
| `drop_counters[NonRetryableFailure]` | batches the receiver rejected permanently | Auth, a malformed request, a protocol mismatch. Retrying can't help. In [`auth_bearer/`](../auth_bearer/) this is the counter a wrong token moves. |
| `drop_counters[RecordTooLarge]` | records above `MemoryLimitOptions::max_record_bytes` | One oversized span, not a pipeline problem. Look for an attribute holding a serialised blob. |
| `drop_counters[PostShutdown]` | records ended after `Shutdown` | Instrumentation is outliving the provider, usually a shutdown-ordering bug in the application. |
| `drop_counters[…]` (the span-limit family) | `SpanAttributeLimit`, `SpanEventLimit`, `SpanLinkLimit`, `EventAttributeLimit`, `LinkAttributeLimit`, `AttributeValueTruncated` | Limits from `WithSpanLimits` are trimming records, so data is being reshaped without anyone seeing it. Raise the limit or fix the instrumentation. |
| `last_error_message` | short, capped, pre-redacted | Read this first. It's safe to log at any level. |
| `last_error_time` | when that error happened | Tells "failing now" apart from "failed once an hour ago and recovered". The example prints its age in milliseconds. |

The counter array is indexed by `DropReason`, and the enumerator order is part
of the public contract, so adding one needs an ICP. `main.cpp` has the full
name table as an exhaustive `switch` with no `default`, which makes `-Wswitch`
fail the build if the enum grows.

---

## Sample run

```
=== phase 1: a tiny queue, filled faster than it drains ===
max_queue_size=64 max_export_batch_size=32 spans=3000
  [after 500 spans]
    connection_state=Connecting batches_sent=0 batches_failed=0 queue_depth_now=12
    drops: QueueFull=6
  [after 1500 spans]
    connection_state=Connected batches_sent=0 batches_failed=0 queue_depth_now=40
    drops: QueueFull=124
  [after 3000 spans]
    connection_state=Connected batches_sent=1 batches_failed=0 queue_depth_now=45
    drops: QueueFull=140
  ForceFlush: Completed
  [after flush]
    connection_state=Connected batches_sent=90 batches_failed=0 queue_depth_now=0
    drops: QueueFull=140
  Shutdown: Completed

=== phase 2: the collector is not there (http://127.0.0.1:14317) ===
  Connect() failed as expected: connection refused
  [after the failed connect]
    connection_state=Disconnected batches_sent=0 batches_failed=0 queue_depth_now=0
    drops: ConnectFailure=1
  ForceFlush: Completed
  [after the failed flush]
    connection_state=Disconnected batches_sent=0 batches_failed=1 queue_depth_now=0
    drops: RetryBudgetExhausted=1 ConnectFailure=4
    last_error: connection refused
    last_error_age_ms: 0
  Shutdown: Completed

=== phase 3: back on a live collector (http://localhost:4317) ===
  ForceFlush: Completed
  [after flush]
    connection_state=Connected batches_sent=1 batches_failed=0 queue_depth_now=0
    drops: none
  trace_id: 44a69e6bf8a2f20cfa792a43a95421ee
  Grafana: http://localhost:3000 (Explore -> Tempo, or the "microtel - recent traces" dashboard)
  Shutdown: Completed
```

(The `[after 1000 spans]` and similar intermediate reports are trimmed.)

Two things in that transcript are easy to misread.

`ForceFlush` returns `Completed` in phase 2. It drained the queue within the
timeout. Whether the drained batch then made it across the wire is a separate
question, and `batches_failed` answers it. A flush status is not a delivery
receipt.

In phase 1, `batches_sent` jumps from 1 to 90 across the flush. The
five-second `schedule_delay` was still holding most of the queue, and the flush
is what cut those batches.

---

## Why the burst is 3000 spans and not 30000

Every tick is a root span, so 3000 spans make 3000 one-span traces. An earlier
draft used 20000. The drops looked the same and the collector accepted
everything, but afterwards Tempo quietly failed to make the recovery phase's
trace retrievable. The demo stack is a laptop-scale single binary
(`examples/stack/tempo.yaml`), and burying it proves nothing about microtel.
3000 overflows a 64-slot queue many times over, which is all the phase needs.
