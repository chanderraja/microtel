# `health_and_backpressure/` — reading `HealthSnapshot`

`Provider::GetExporterHealth()` is the only window an operator has into the
export pipeline. This example breaks the pipeline two different ways and prints
the snapshot after each, so every field is seen moving.

```bash
examples/stack/up.sh
cmake -S . -B build -DMICROTEL_BUILD_EXAMPLES=ON && cmake --build build
./build/examples/microtel_example_health_and_backpressure    # ~20s
```

Arguments: `[endpoint]` (default `http://localhost:4317`) and `[dead-endpoint]`
(default `http://127.0.0.1:14317`, a port nothing serves).

---

## The three phases

**1. Queue overflow.** A provider with `WithBatch({.max_queue_size = 64,
.max_export_batch_size = 32})` — two orders of magnitude below the 8192 default
— fed 3000 spans from a tight loop. `End()` never blocks
(`docs/threading-model.md` §3.1): when the queue is full the record is
**dropped**, so an instrumented application keeps its latency and loses
telemetry. `queue_depth_now` climbs, `drop_counters[QueueFull]` counts what was
lost.

**2. Stalled collector.** A provider pointed at a closed port, with short
timeouts so the failure path does not sit out a sixty-second retry budget.
`Connect()` fails up front; the export then fails too, moving `batches_failed`,
`ConnectFailure` and `RetryBudgetExhausted`, with `connection_state` stuck at
`Disconnected` and the reason in `last_error_message`.

**3. Recovery.** The live endpoint again: batches land, the counters stay
clean, and a trace ID is printed for checking against Tempo.

**Recovery builds a new provider, and that is the honest shape.** The endpoint
is not one of the four hot-reload knobs (ICP 0026) — `SetBatchOptions`,
`SetMetricInterval`, `SetSamplerRatio` and `SetLogLevel` are the whole list. An
application that must fail over to a different collector constructs another
`Provider`; each phase here therefore uses its own `WithProfileName`, because a
profile name is released by destruction, not by `Shutdown`.

---

## Every field, and what an operator does with it

| Field | What it means | What to do about it |
|---|---|---|
| `connection_state` | `Disconnected` · `Connecting` · `Connected` · `Reconnecting` · `Closed` | `Disconnected` points at **local configuration or reachability** — endpoint, TLS material, network path. `Reconnecting` points at **the peer or the link**: microtel was connected, the connection dropped, and the transport will re-establish it on the next export (ICP 0018). Alerting keyed only on `Disconnected` misses drops; key on both. `Closed` is terminal — `Shutdown` has run. |
| `batches_sent` | batches the receiver accepted, **across all three signals** | Flat while spans are being produced means nothing is leaving. Compare against your own emit rate, not against an absolute. |
| `batches_failed` | batches that ended in a non-retryable failure or exhausted their retry budget | Each one is telemetry that is gone. A steady trickle is usually auth or a protocol mismatch; a burst is usually the collector. Read `last_error_message` before anything else. |
| `queue_depth_now` | records queued **right now** | A depth that sits near `max_queue_size` means the exporter cannot keep up: the next thing that happens is `QueueFull`. Raise `max_queue_size` (memory), raise `max_export_batch_size` / lower `schedule_delay` (throughput), or sample less. |
| `drop_counters[QueueFull]` | records the queue refused | The producer is outrunning the exporter — phase 1. This is the counter to alert on for "we are losing spans we generated". |
| `drop_counters[ConnectFailure]` | connection attempts that failed | With `Disconnected`, a configuration or reachability problem. Note it moves **once per attempt**, so it climbs faster than `batches_failed`. |
| `drop_counters[RetryBudgetExhausted]` | batches that used their whole `TimeoutOptions::retry_budget` | The peer was unreachable or overloaded for longer than the budget. Raising the budget trades memory and latency for durability; it does not fix the peer. |
| `drop_counters[NonRetryableFailure]` | batches the receiver rejected permanently | Auth, a malformed request, a protocol mismatch. Retrying cannot help — see [`auth_bearer/`](../auth_bearer/), where this is the counter a wrong token moves. |
| `drop_counters[RecordTooLarge]` | records above `MemoryLimitOptions::max_record_bytes` | One oversized span, not a pipeline problem. Find the attribute that is a serialised blob. |
| `drop_counters[PostShutdown]` | records ended after `Shutdown` | Instrumentation outliving the provider. Usually a shutdown-ordering bug in the application. |
| `drop_counters[…]` (the span-limit family) | `SpanAttributeLimit`, `SpanEventLimit`, `SpanLinkLimit`, `EventAttributeLimit`, `LinkAttributeLimit`, `AttributeValueTruncated` | Structural limits from `WithSpanLimits` trimming records. Data is being silently reshaped — raise the limit or fix the instrumentation. |
| `last_error_message` | short, capped, pre-redacted | The first thing to read. It is safe to log at any level. |
| `last_error_time` | when that error happened | Distinguishes "failing now" from "failed once an hour ago and recovered". The example prints its age in milliseconds. |

The counter array is indexed by `DropReason`, and the enumerator order is part
of the public contract — adding one is an ICP. `main.cpp` carries the full
name table as an exhaustive `switch` with no `default`, so `-Wswitch` fails the
build if the enum grows.

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
  Shutdown: Completed
```

Two details worth noticing in that transcript:

- **`ForceFlush` returns `Completed` in phase 2.** It drained the queue within
  the timeout; that the drained batch then failed on the wire is a different
  question, and `batches_failed` is where it is answered. A flush status is not
  a delivery receipt.
- **`batches_sent` jumps from 1 to 90 across the flush** in phase 1. The
  five-second `schedule_delay` was still holding most of the queue; the flush
  is what cut those batches.

---

## Why the burst is 3000 spans and not 30000

Every tick is a root span, so 3000 spans are 3000 one-span traces. An earlier
draft used 20000 — the drops looked identical, the collector accepted
everything, and Tempo quietly failed to make the recovery phase's trace
retrievable afterwards. The demo stack is a laptop-scale single binary
(`examples/stack/tempo.yaml`); burying it proves nothing about microtel. 3000
overflows a 64-slot queue many times over, which is all the phase needs.
