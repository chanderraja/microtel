# Sequence: Partial Success Handling

**Status:** M0 deliverable. Normative timeline for OTLP partial-success responses. **Most counterintuitive rule in the error model — has its own diagram for that reason.**
**See also:** `error-model.md` §6, `microtel-spec.md` §7.3, `interfaces.md` §3.2.

---

## Participants

- **Exporter worker** — calls `Send`, receives `WireResult`.
- **Wire codec** — parses the response, populates `partial_success_rejected`.
- **I/O thread** — receives the response bytes.
- **Peer** — returns 200 OK (HTTP) or `grpc-status: 0` (gRPC) with a body indicating some items were rejected.
- **`IDiagnosticsSink`** — counts rejections.

---

## Happy path — partial-success on OTLP/HTTP

```
Exporter Worker     Wire Codec        I/O Thread           Peer       Diagnostics
   |                      |                  |               |             |
   | encode(N=512 spans)                                                    |
   | Send(payload) -|---->                                                   |
   |                      | submit -|----> POST /v1/traces ---------------> |
   |                      |                  |<--- HTTP 200 OK ----------- |
   |                      |                  |    Content-Type:           |
   |                      |                  |    application/x-protobuf  |
   |                      |                  |    body: ExportTraceServiceResponse {
   |                      |                  |             partial_success {
   |                      |                  |               rejected_spans: 3,
   |                      |                  |               error_message: "schema mismatch on attr 'foo'"
   |                      |                  |             }
   |                      |                  |           }
   |                      |<-- complete --|                                  |
   |                      | parse 200 + body  -- >  ExportTraceServiceResponse
   |                      | partial_success.rejected_spans = 3              |
   |                      | record drop reason = partial_success_rejection   |
   |                      |        with count = 3                            |
   |                      |--------------------------------------------> increment counter += 3
   |                      |                                                  |
   |                      | build WireResult { success=true,                 |
   |                      |                    retryable=false (do not retry),
   |                      |                    partial_success_rejected=3,  |
   |                      |                    response_excerpt = "schema mismatch..." (capped) }
   |<-- WireResult -------|                                                  |
   |                                                                         |
   | record batch_sent (the batch DID succeed; 509 of 512 spans accepted)   |
   | release EncodedPayload                                                  |
   | (NO retry)                                                              |
```

---

## Annotations — why partial-success is **never retried**

A partial-success response means the receiver **accepted some items and rejected others**. The accepted items are committed on the receiver side. The rejected items are not.

If microtel were to retry the request, it would:

1. Re-send the original 512 spans.
2. The receiver would re-process all 512.
3. The 509 already-accepted spans would either be rejected as duplicates (best case — receiver tracks span IDs) or, more commonly, **ingested a second time** (worst case — duplicates in the storage backend).

Either way, the rejected 3 spans would still be rejected, because the rejection reason is structural (the 3 spans had bad attributes; resending them does not fix that).

**Therefore, retry is strictly worse than no-retry.** No matter what the rejection reason is, retrying a partial-success makes things worse, never better. (LOCKED — `error-model.md` §6.)

---

## What `success=true` means in `WireResult`

`success=true` with `partial_success_rejected > 0` is the documented partial-success case. The exporter:

- **Counts the batch as sent** (`batches_sent` increments). The batch as a whole succeeded.
- **Counts the rejected items separately** (`partial_success_rejection` increments by the rejected count). Operators see both numbers.
- **Does not retry.**
- **Releases the `EncodedPayload`.**
- **Logs at `warn` level** (rate-limited): `"partial success: 3 of 512 spans rejected: schema mismatch on attr 'foo'"`. The error message from the response is captured (capped at `max_response_bytes`) for the log.

---

## Variant — partial-success on OTLP/gRPC

The shape is identical except that the response message is in a DATA frame followed by `grpc-status: 0` in trailers:

```
Wire Codec (gRPC)
   |
   | DATA: ExportTraceServiceResponse {
   |         partial_success {
   |           rejected_spans: 3,
   |           error_message: "..."
   |         }
   |       }
   | trailer: grpc-status: 0
   |
   | classify: success=true, partial_success_rejected=3, retryable=false
```

The codec inspects the response message body **regardless of the trailer status**. `grpc-status: 0` plus `partial_success.rejected_spans > 0` is partial-success. `grpc-status: 0` plus `partial_success.rejected_spans == 0` (or partial-success absent) is full success.

---

## Variant — `partial_success` with `rejected_spans == 0` and a non-empty `error_message`

Per the OTLP spec, this is the receiver's way of warning the client of a non-fatal issue (e.g., deprecated attribute name) without rejecting any data. v1 treatment:

- `success=true`, `partial_success_rejected=0`, `retryable=false`.
- `partial_success_rejection` counter is **not** incremented (no items rejected).
- The `error_message` is logged at `info` level once per session (rate-limited; not per batch).

This case is rare in practice but legal per the spec. Tests cover it explicitly.

---

## Variant — partial-success body that fails to parse

If the response is `200 OK` (HTTP) or `grpc-status: 0` (gRPC) but the body is not a valid `ExportTraceServiceResponse`:

- The codec records `malformed_response`.
- `WireResult.success=false`, `retryable=false`.
- The batch is treated as failed (not partial-success). No retry.

This is conservative — a peer returning success codes with malformed bodies is broken; retrying is unlikely to help. Operators see the malformed-response counter and can correlate with peer-side issues.

---

## Annotations on counter semantics

`partial_success_rejection` counts **rejected items**, not **partial-success responses**. A partial-success response with 3 rejected items increments the counter by 3, not by 1. This matches the operator question "how many of my spans is the receiver rejecting?" rather than "how many partial-success replies have I gotten?"

A separate counter for partial-success-response *occurrences* is not in v1. If operators need it, they can derive it from the diagnostic log (rate-limited; not exact). v1.1+ may add it if there's demand.

---

## Edge cases captured by tests

- Response with `partial_success.rejected_spans = 0` and empty `error_message` — treated as full success.
- Response with `partial_success.rejected_spans > 0` and **no** `error_message` — counter increments; log emits "<no message>".
- Response with `partial_success.rejected_spans` larger than the original batch size — log a warning ("receiver reported impossibly many rejected items"); count what they reported.
- gRPC partial-success with the body in a multi-DATA-frame stream — parser correctly assembles the message.
- Partial-success response with `Retry-After` (HTTP) or `RetryInfo` (gRPC) present — these are ignored. Partial-success is never retried, full stop. (LOCKED — `error-model.md` §6.)
- Partial-success during the final batch of `Shutdown(timeout)` — counter increments; shutdown proceeds; `Status::Completed` if everything else drained in time.

These live in `tests/unit/exporter/partial_success/` and `tests/wire/partial_success/` (M3+).
