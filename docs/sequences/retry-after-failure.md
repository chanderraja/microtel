# Sequence: Retry After Failure

**Status:** M0 deliverable. Normative timeline for the retry path on a retryable export failure.
**See also:** `error-model.md` §7 (retry classification), `interfaces.md` §4.3, §4.4, `microtel-spec.md` §7.

---

## Participants

- **Exporter worker** — drives the retry loop.
- **Wire codec** — classifies the response, computes `WireResult`.
- **I/O thread** — submits to the wire and reports completion.
- **`ISteadyClock`** — monotonic clock for backoff and budget arithmetic.
- **`IDiagnosticsSink`** — counts the various outcomes.

---

## Happy path — retryable failure recovers on second attempt

```
Exporter Worker      Wire Codec       I/O Thread        Peer       SteadyClock
   |                      |                 |             |            |
   | encode(batch) ->                                                   |
   | EncodedPayload                                                     |
   | retry_attempt = 1                                                  |
   | t_budget_start = Now() <----------------------------- read --------|
   |                                                                    |
   |--- Send(payload, deadline = per_export) ---->                       |
   |                      |                 |             |             |
   |                      | submit to transport |          |             |
   |                      |               -|- enqueue --->|             |
   |                      |                 | HEADERS + DATA + END_STREAM |
   |                      |                 |--------------------------> |
   |                      |                 |<------- 503 + Retry-After: 2|
   |                      |                 | response ready            |
   |                      |<--- complete --|             |               |
   |                      | classify: success=false,                     |
   |                      |           retryable=true,                    |
   |                      |           retry_after=2s                     |
   |<--- WireResult ------|                                              |
   |                                                                    |
   | record dropped EncodedPayload (release unique_ptr<byte[]>)         |
   | (NOT retryable_failure_recovered yet — only on success)            |
   |                                                                    |
   | check budget: (Now() - t_budget_start) < retry_budget? yes         |
   |                                                                    |
   | sleep retry_after + jitter(0..0.5*retry_after)                     |
   |                                                                    |
   | retry_attempt = 2                                                  |
   | encode(batch) -> NEW EncodedPayload  (per-encode arena rule)       |
   |                                                                    |
   |--- Send(payload, deadline = per_export) ---->                       |
   |                      | submit -|- enqueue ---> HEADERS+DATA+END    |
   |                      |                 |<-------- 200 (no body)    |
   |                      |<-- complete --|                              |
   |                      | classify: success=true                       |
   |<--- WireResult(success=true) -|                                     |
   |                                                                    |
   | record retryable_failure_recovered                                 |
   | record batch_sent                                                  |
   | release EncodedPayload                                             |
```

---

## Annotations

1. **Encoded bytes do not survive a retry.** The `EncodedPayload` from attempt 1 is released as soon as the failed `Send` returns. Attempt 2 calls `Encode()` again, producing a fresh arena and a fresh `EncodedPayload`. (LOCKED — `memory-model.md` §3, ICP 0001.)
2. **Retry classification is the codec's responsibility.** The codec produces `WireResult.retryable` and `retry_after`; the exporter does not reinterpret. For HTTP, `retry_after` comes from the `Retry-After` response header; for gRPC, from `RetryInfo.retry_delay` in `grpc-status-details-bin`. (`error-model.md` §7, `grpc-wire-protocol.md` §2.4.)
3. **Backoff with jitter.** When `retry_after` is absent (e.g., gRPC `UNAVAILABLE` without `RetryInfo`), the exporter applies exponential backoff: `min(base * 2^(attempt-1), cap) ± jitter`. v1 uses `base=1s`, `cap=30s`, jitter is uniform `[0, 0.5*delay)`. The exact constants are pinned in M5; the shape is documented here.
4. **The retry budget is enforced by the exporter, not the codec.** `t_budget_start` is recorded at the first `Send` attempt; if the next sleep would push elapsed time past `retry_budget`, the exporter exits the loop with `retry_budget_exhausted` instead of attempting another send.
5. **Counters are recorded only on terminal outcomes.** `retryable_failure_recovered` increments only when a retry succeeds. `retry_budget_exhausted` increments on budget timeout. Each retry attempt itself is **not** counted (otherwise the counter would conflate "2 attempts to succeed" with "2 batches retried at all").

---

## Variant — non-retryable failure

```
Exporter Worker      Wire Codec       I/O Thread       Peer
   |                      |                |              |
   | Send(payload) -|--->                                  |
   |                      | submit -|---> HEADERS+DATA+END |
   |                      |                |<--- 415 ------|
   |                      |<-- complete --|                |
   |                      | classify: success=false,       |
   |                      |           retryable=false      |
   |<-- WireResult --|                                     |
   |                                                       |
   | record non_retryable_failure                          |
   | record batch_failed                                   |
   | release EncodedPayload                                |
   | <-- skip retry loop entirely -->                      |
```

Examples of non-retryable failures: HTTP 415 (Unsupported Media Type), HTTP 404, gRPC `INVALID_ARGUMENT`, gRPC `RESOURCE_EXHAUSTED` **without** `RetryInfo`, malformed responses. See `error-model.md` §7 for the full matrix.

---

## Variant — retry budget exhausted

```
Exporter Worker (loop iteration N where retry_after > remaining_budget)
   |
   | check budget: (Now() - t_budget_start) + retry_after > retry_budget? yes
   | record retry_budget_exhausted
   | record batch_failed
   | release EncodedPayload
   | exit loop, return ExportResult::Failure
```

`retry_budget` covers all retries of a single batch (default 60s, spec §7.3). If exceeded, the batch is dropped non-retryably. The application observes this only via `GetExporterHealth()`; `Export()` itself returned `Success` long ago when the batch was first accepted.

---

## Variant — local timeout (`per_export`) fires

If `per_export` (default 10s) elapses while a single attempt is in flight, the codec sends `RST_STREAM/CANCEL` (gRPC) or aborts the stream (HTTP). The completion arrives with `Error::Kind::Cancelled`; the codec returns `retryable=true` with a small backoff (jitter only — no `retry_after` from a peer that didn't respond).

The next attempt is subject to the **remaining** retry budget; the failed attempt's elapsed time still counts.

---

## Edge cases captured by tests

- `Retry-After` with HTTP-date format vs delta-seconds (codec accepts both).
- `RetryInfo.retry_delay` larger than remaining `retry_budget` — exporter exits with `retry_budget_exhausted`.
- `RetryInfo.retry_delay = 0` (immediate retry permitted).
- Attempt N succeeds: exactly one `retryable_failure_recovered` increment.
- All N attempts fail and budget exhausts: exactly one `retry_budget_exhausted` increment, no `retryable_failure_recovered`.
- Concurrent `Shutdown` while sleeping in backoff — sleep wakes early; exporter checks shutdown state and exits.
