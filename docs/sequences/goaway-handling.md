# Sequence: GOAWAY Handling

**Status:** M0 deliverable. Normative timeline for HTTP/2 GOAWAY received mid-batch.
**See also:** `grpc-wire-protocol.md` §6.2, `architecture.md` §3.6, `microtel-spec.md` §5.2.

---

## Participants

- **Exporter worker** — has one or more outstanding `Send` calls.
- **Wire codec** — owns the in-flight request records.
- **I/O thread** — owns the nghttp2 session.
- **nghttp2** — surfaces stream-level outcomes per the GOAWAY rules.
- **Peer** — sends the GOAWAY frame.

---

## Happy path — GOAWAY received between requests

```
Exporter Worker     Wire Codec     I/O Thread     nghttp2     Peer
    |                    |              |             |          |
    | Send (batch 1) --|---> submit --|--> stream 1  -|--------> |
    |                    |              | DATA + END_STREAM ----> |
    |                    |              |<------ HEADERS + DATA --|
    |                    |              |<-- response stream 1 ---|
    |<--- WireResult(success=true) ---|                            |
    |                                                              |
    |                                  |<--- GOAWAY(last=1) -------|
    |                                  | nghttp2: no new streams   |
    |                                  | -> trigger reconnect      |
    |                                                              |
    | (next batch waits until reconnect completes)                 |
    |                                                              |
    | I/O thread: Close current session                            |
    |             open new socket (reconnect path: see             |
    |             connection-establishment.md)                     |
    | I/O thread state: Connected                                  |
    |                                                              |
    | Send (batch 2) -- proceeds on the new connection ----------> |
```

---

## Annotations

1. **GOAWAY is a graceful shutdown signal from the peer.** It carries a "last accepted stream ID." Any stream with ID ≤ last is permitted to complete; any new stream attempted after GOAWAY arrives is rejected.
2. **nghttp2 surfaces this automatically.** No microtel code reads the GOAWAY frame directly. nghttp2 marks the session as draining and refuses to open new streams. Existing streams complete normally if their IDs are ≤ last.
3. **The I/O thread observes the session-level signal** (via nghttp2 callback) and triggers a reconnect: closes the current session, releases `SslSession` and `Socket`, opens a new TCP connection, runs the TLS + ALPN + SETTINGS handshake again. The reconnect path is the same as initial connect (`connection-establishment.md`).
4. **The exporter worker is decoupled from the reconnect.** It blocks on the next `Send` only because the I/O thread is reconnecting; once `Connected`, the next batch proceeds. No drop occurs solely from observing GOAWAY — only the in-flight stream(s) that were rejected (next variant).
5. **Backoff on reconnect.** If reconnect fails, the I/O thread enters the same exponential-backoff-with-jitter loop as the initial connect. The exporter's `Send` calls during this period observe `transport_busy` or block on the request queue.

---

## Variant — GOAWAY received with in-flight rejected streams

```
Exporter Worker     Wire Codec     I/O Thread       nghttp2       Peer
    |                    |              |              |            |
    | Send (A) --|---> submit ----|---> stream 5 -|--> HEADERS ----> |
    | Send (B) --|---> submit ----|---> stream 7 -|--> HEADERS ----> |
    |                                              <-- GOAWAY(last=5) -|
    |                                  | nghttp2 rejects stream 7  |
    |                                  |   (REFUSED_STREAM)         |
    |                                  | stream 5 continues         |
    |                                  |<------- response 5 -------- |
    |<-- WireResult(A) success ---                                   |
    |                                                                |
    | for stream 7: codec gets REFUSED_STREAM completion             |
    |<-- WireResult(B) retryable=true, retry_after = small jitter   |
    |                                                                |
    | retry-loop kicks in for batch B (see retry-after-failure.md)   |
    | reconnect happens in parallel; once Connected, retry succeeds  |
```

Stream IDs are assigned by nghttp2 in order; the peer's GOAWAY identifies the last stream **the peer accepted**. Streams with IDs **greater than** last are guaranteed to be rejected: nghttp2 surfaces them as `REFUSED_STREAM`. The wire codec treats `REFUSED_STREAM` exactly like a transient failure: `WireResult{retryable=true}`, small jittered backoff. No data loss as long as the retry budget covers the reconnect time.

---

## Variant — GOAWAY with `error_code` indicating peer-side problem

GOAWAY carries an error code. v1 treats them as a single category — drain-and-reconnect — for simplicity. The error code is captured in the diagnostic log at `info` level so operators can correlate with peer-side incidents:

| GOAWAY error code | Diagnostic |
|---|---|
| `NO_ERROR (0)` | "peer initiated graceful shutdown" |
| `PROTOCOL_ERROR (1)` | "peer reported HTTP/2 protocol error" |
| `INTERNAL_ERROR (2)` | "peer reported internal error" |
| `ENHANCE_YOUR_CALM (11)` | "peer signalled rate-limit; consider tuning" |
| Other | code value emitted as decimal |

Reconnect timing applies regardless of error code. v1 does not back off differently for `ENHANCE_YOUR_CALM`; v1.1 may add per-error-code tuning if production data shows the need.

---

## Variant — GOAWAY during shutdown

If the application calls `Provider::Shutdown` and the peer simultaneously sends GOAWAY:

1. The shutdown signal is observed first if the worker is awake; if the worker is sleeping, GOAWAY's wakeup wins.
2. Either way, the I/O thread sends its own GOAWAY in response (graceful close handshake) and closes the session.
3. The shutdown drain proceeds against the local queue; in-flight batches complete or time out under the shutdown deadline.
4. No reconnect is attempted during shutdown. (`shutdown-drain.md`.)

---

## Edge cases captured by tests

- GOAWAY received before any DATA was sent on a stream — codec retries cleanly.
- GOAWAY with `last_stream_id = 0` (peer accepted nothing) — all in-flight rejected; retry budget covers them.
- GOAWAY followed immediately by TCP RST — reconnect handles the absent session.
- Multiple GOAWAYs from the same peer (legal per HTTP/2; second one carries lower or equal `last_stream_id`) — nghttp2 handles deduplication.
- GOAWAY with extremely high `last_stream_id` (peer being lenient) — no special handling needed.

These live in `tests/grpc-wire/goaway/` and `tests/integration/transport_goaway/` (M3+).
