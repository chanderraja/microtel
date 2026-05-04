# Sequence: Backpressure and Drop

**Status:** M0 deliverable. Normative timeline for queue overflow under sustained load.
**See also:** `microtel-spec.md` §5.4, `memory-model.md` §6, `error-model.md` §3, `threading-model.md` §3.1.

This is the sequence operators care about most: what does microtel do when telemetry production exceeds export throughput?

---

## Participants

- **Caller threads** — multiple application threads ending spans.
- **Exporter worker** — drains the queue, exports batches.
- **Span queue** — bounded MPSC, capacity `max_queue_size` (default 8192).
- **`IDiagnosticsSink`** — counts drops by reason.

---

## Happy path — no overflow

```
Caller A         Caller B          Span Queue (capacity 8192)        Exporter Worker
  |                 |                          |                            |
  | End()          | End()                                                  |
  |--enqueue-|---->|                                                        |
  |                 |--enqueue-|------------>                                |
  | (queue depth = 2)                                                       |
  |                                                                         |
  |                                            (sleep on cv until           |
  |                                             max_export_batch_size       |
  |                                             or schedule_delay)          |
  |                                                                         |
  | <continued enqueues>                       (queue depth grows)          |
  |                                                                         |
  | depth >= max_export_batch_size  --signal cv-->                          |
  |                                                                         |
  |                                                              | drain:  |
  |                                                              |   pull |
  |                                                              |   N    |
  |                                                              |   recs |
  |                                                              | encode |
  |                                                              | Send   |
  |                                            (queue depth -= N)          |
```

`max_queue_size` (8192) and `max_export_batch_size` (512) are independent. The worker drains in batches of up to 512; the queue holds up to 8192.

---

## Variant — overflow with `drop_newest` (default)

```
Caller A         Span Queue (depth = 8192)        Diagnostics
  |                          |                          |
  | End()                                                |
  |--try enqueue-|-----> queue is full                  |
  |                          | reject the new record    |
  |                          | (caller's record discarded)
  |                                                      |
  |               record drop reason = queue_full -|--->|
  |                                                      | atomic increment
  |                                                      | of queue_full counter
  |                                                      |
  | (caller's End() returns silently — noexcept)        |
  |                                                      |
  | (existing 8192 records remain queued; FIFO preserved) |
```

**Drop-newest preserves FIFO ordering of already-queued records.** This means when the worker drains, it pulls the *oldest* records — the ones from the start of the overload — rather than freshly-enqueued records. Operators focused on diagnosing the *onset* of an overload event get those traces; operators focused on *current state* may want `drop_oldest` instead (next variant).

A rate-limited diagnostic is emitted by the diagnostics sink the first time `queue_full` increments and periodically while drops continue. Default rate: first occurrence + every 1s while dropping (token-bucket per `error-model.md` §9.2).

---

## Variant — overflow with `drop_oldest`

Configured via `batch.drop_policy = "drop_oldest"`.

```
Caller A          Worker (drains opportunistically)         Span Queue
  |                              |                                |
  | End()                                                         |
  |--try enqueue-|--> queue is full                                |
  |                              |                                |
  |                              | (worker observes "full at      |
  |                              |  enqueue" signal)              |
  |                              | drop oldest 1 record           |
  |                              | record drop reason = queue_full|
  |               retry enqueue -|--> success                      |
  |                                                                |
  | (caller's End() returns silently)                              |
```

Implementation: when `drop_oldest` is configured, the queue's "full" path triggers a single-element shed by the worker (or by a wait-free protocol on the queue, depending on the M3 implementation choice). Either way, **the producer never blocks** — the eviction happens as part of the enqueue, on the producer's behalf.

`drop_newest` and `drop_oldest` increment the **same** counter (`queue_full`) — the policy difference is which records survive, not how it's accounted. Operators distinguish via the `batch.drop_policy` setting they configured.

---

## Variant — record-too-large

```
Caller          Span Queue                    Diagnostics
  |                  |                              |
  | End() (this span has 200 KiB of attribute data) |
  |--check size-|                                   |
  |             record_estimate > max_record_bytes  |
  |             (64 KiB default)                    |
  |             reject the record                   |
  |             record drop reason = record_too_large -|->
  |                                                  |
  | (caller's End() returns silently)                |
```

This drop happens **at enqueue time, before reaching the queue.** A 200-KiB span never enters the queue and never consumes capacity. The size check is the conservative encoded-size estimate (`memory-model.md` §6).

---

## Variant — span-limit drops (per-span, not per-queue)

These drops happen on `SetAttribute` / `AddEvent` / `AddLink`, *while the span is still being built*, not at `End()`:

```
Caller             Span object                        Diagnostics
  |                     |                                  |
  | SetAttribute(k129, v129)  -- (span already has 128 attrs) -|
  |                     | reject the attribute              |
  |                     | record drop reason = span_attribute_limit -|->
  |                                                         |
  | (caller's SetAttribute returns silently)                |
  |                                                         |
  | <span continues to be usable>                           |
```

Limit drops do not drop the **span** — they drop the *attribute / event / link* that would have exceeded the limit. The span itself is still queued and exported with its first 128 attributes.

`attribute_value_truncated` is similar: oversize string values are truncated; the attribute is kept; the truncation is counted.

---

## Annotations

1. **The producer never blocks** (LOCKED — `threading-model.md` §3.1). Whether the policy is `drop_newest` or `drop_oldest`, the producer's `End()` returns in bounded time without waiting on I/O or on a slow consumer.
2. **Drops are accounted at the source** — the layer that decides the drop is the one that increments the counter. `queue_full` is incremented by the producer-side enqueue path (or the worker's eviction path under `drop_oldest`); `record_too_large` by the producer-side size check; `span_attribute_limit` etc. by the API layer.
3. **Diagnostics are rate-limited.** Per-reason token bucket, default 1 burst of 10 then 1/sec sustained. The bucket is per `(level, reason)` pair so different drop reasons do not crowd each other out.
4. **Operators see the cumulative counts in `GetExporterHealth()`** plus rate-limited log lines. The combination tells them both the steady-state drop rate and the recent narrative.

---

## How operators tell whether they are dropping

```cpp
auto health = provider->GetExporterHealth();
if (health.drop_counters[DropReason::queue_full] > prev_count) {
    // Either increase max_queue_size, increase max_export_batch_size,
    // increase schedule_delay (less aggressive), reduce production rate,
    // or scale up the receiver.
}
```

The values are monotonically non-decreasing; a delta against a previous read indicates drops in the interval. The sink does not provide a sliding-window rate in v1; applications compute it themselves.

---

## Edge cases captured by tests

- 16 caller threads enqueuing concurrently while the worker drains — the queue must reach capacity, drop FIFO-correctly, and never crash.
- `max_queue_size = 1` — extreme bound; the queue is essentially always full; tests assert correctness, not throughput.
- `max_record_bytes` set very small — every record is too large; test asserts every span drops with `record_too_large` and no records reach the queue.
- Span that fills its attribute limit at attribute 64 (custom limit); attributes 65..N silently dropped; span is exported with 64 attributes.
- Diagnostic rate-limiter: 10000 drops in 1 second produce ≤ 12 log emissions (10 burst + 2 in the second), but increment the counter exactly 10000 times.

These live in `tests/unit/sdk/processor/` and `tests/integration/backpressure/` (M3+).
