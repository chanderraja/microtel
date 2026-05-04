# Sequence: gRPC Trailer-Only and Multi-Frame Parsing

**Status:** M0 deliverable. Two related gRPC wire-protocol edge cases that bite codec implementations.
**See also:** `grpc-wire-protocol.md` §2.3, §2.5, §3, §9, `error-model.md` §7.2.

These cases collapse into one sequence diagram because they exercise the same parser and their interactions are subtle.

---

## Participants

- **gRPC wire codec** — owns the response parser state machine.
- **I/O thread** — receives nghttp2 callbacks and copies bytes into the codec's buffer.
- **nghttp2** — surfaces frames as callback events.
- **Peer** — sends the gRPC response in one of the shapes below.

---

## Shape 1 — trailer-only (status in initial HEADERS, no DATA)

```
I/O Thread        nghttp2                        Peer
   |                  |                            |
   |                  |<------- HEADERS frame -----|
   |                  |        :status: 200        |
   |                  |        content-type: application/grpc
   |                  |        grpc-status: 14    (UNAVAILABLE)
   |                  |        grpc-message: "service overloaded"
   |                  |        (no grpc-status-details-bin)
   |                  |        END_STREAM=1        |
   |                  |                            |
   | on_header(":status", "200")                    |
   | on_header("grpc-status", "14")                 |
   | on_header("grpc-message", "service overloaded")|
   | on_stream_close(stream_id, NO_ERROR)           |
   |                                                |
   | parser state: WAIT_PREFIX (no DATA seen)       |
   | END_STREAM observed                            |
   | -> trailer-only path                           |
   |                                                |
   | classify:                                       |
   |   grpc-status = 14 (UNAVAILABLE) -> retryable  |
   |   no RetryInfo -> jittered backoff              |
   |                                                 |
   | WireResult { success=false, retryable=true,    |
   |              retry_after = jitter,             |
   |              error.message = "service overloaded" } |
```

### Annotations

1. **Trailer-only is recognised by `END_STREAM=1` on the first HEADERS frame** combined with the presence of `grpc-status` in those headers. (`grpc-wire-protocol.md` §2.5.)
2. **The parser does not enter the DATA-reading states.** It transitions directly from `STREAMING` to `STATUS_ONLY` to `COMPLETE`.
3. **HTTP `:status` is captured for diagnostics** but classification follows `grpc-status`. (LOCKED — gRPC spec §3.)

---

## Shape 2 — multi-DATA-frame response with split message prefix

```
I/O Thread       nghttp2                  Peer
   |                 |                       |
   |                 |<----- HEADERS frame --|
   |                 |     :status: 200      |
   |                 |     content-type: application/grpc
   |                 |     (no END_STREAM)   |
   | on_header(":status","200")              |
   |                                          |
   |                 |<----- DATA frame 1 ---|
   |                 |     bytes [0..2]      |
   |                 |     CF=00, then        |
   |                 |     length high bytes [00 00]
   | on_data_chunk(stream, bytes[0..2])      |
   | parser: WAIT_PREFIX -> READ_PREFIX(2/5)  |
   |                                          |
   |                 |<----- DATA frame 2 ---|
   |                 |     bytes [3..6]      |
   |                 |     length low bytes [00 09 ...]
   |                 |     +1 byte of body   |
   | on_data_chunk(stream, bytes[3..6])       |
   | parser: READ_PREFIX(5/5) -> length = 9   |
   |         move into READ_BODY(1/9)         |
   |                                          |
   |                 |<----- DATA frame 3 ---|
   |                 |     bytes [7..15]     |
   |                 |     remaining 8 body  |
   |                 |     END_STREAM=1      |
   | on_data_chunk(stream, bytes[7..15])      |
   | parser: READ_BODY(9/9) -> COMPLETE        |
   |                                          |
   |                 |<----- HEADERS frame --|
   |                 |     grpc-status: 0    |
   |                 |     END_STREAM (from DATA, prior)
   | on_header("grpc-status", "0")            |
   | on_stream_close(stream_id, NO_ERROR)     |
   |                                          |
   | parser produces: 1 message of 9 bytes    |
   | parse message as ExportTraceServiceResponse
   | grpc-status = 0 -> success               |
   | check partial_success in body            |
   |                                          |
   | WireResult { success=true, ...}          |
```

### Annotations

1. **The 5-byte prefix can be split across DATA frames.** The parser must hold the partial prefix until enough bytes arrive. (LOCKED — `grpc-wire-protocol.md` §2.3.)
2. **The 4-byte length field is big-endian.** Implementation must use `htonl`/`ntohl` or equivalent — never assume host byte order.
3. **The parser does not assume one DATA frame == one gRPC message.** This is the most common implementation mistake; it works for simple test fixtures and breaks against real servers under load.
4. **OTLP unary expects exactly one message.** If a second prefix appears in the byte stream after the first message completes, the codec rejects the response as `malformed_response`. The parser tracks this explicitly so the rejection is clean.
5. **END_STREAM and the trailer HEADERS are independent events from nghttp2's perspective.** The trailer carries the final status; the END_STREAM signals that DATA is done. They typically arrive together but are surfaced via separate callbacks.

---

## Shape 3 — combination: trailer-only with partial response body

This combination is **illegal** per the gRPC spec, but real proxies emit it occasionally. The codec rejects it.

```
I/O Thread       nghttp2                  Peer
   |                 |                       |
   |                 |<----- HEADERS frame --|
   |                 |     :status: 200      |
   |                 |     grpc-status: 0    |  <- claims success
   |                 |     END_STREAM=1      |  <- but no DATA yet
   |                 |<----- DATA frame -----|  <- DATA arrives anyway
   |                 |     [some bytes]      |  <- after END_STREAM
   |                 |     <invalid>          |
   |                                          |
   | parser detects DATA after END_STREAM     |
   | record malformed_response                |
   | WireResult { success=false, retryable=false, ...}
```

nghttp2 itself rejects DATA after END_STREAM at the HTTP/2 layer; the codec sees a stream-level error rather than the DATA. Either way, the result is `malformed_response`.

---

## Shape 4 — RST_STREAM mid-DATA

```
I/O Thread        nghttp2                       Peer
   |                  |                            |
   |                  |<-------- DATA frame -------|
   |                  |    bytes [0..3]            |
   | on_data_chunk    |                            |
   | parser: READ_PREFIX(4/5)                      |
   |                  |                            |
   |                  |<------- RST_STREAM --------|
   |                  |    error_code: CANCEL      |
   | on_stream_close(stream, CANCEL)               |
   | parser state discarded                        |
   |                                                |
   | WireResult { success=false, retryable=true,   |
   |              error.kind = Cancelled,          |
   |              retry_after = jitter }            |
```

`RST_STREAM` from the peer is treated as a transient failure — retryable with a small jittered backoff. The peer's error code is recorded in the diagnostic log. Specific codes do not change microtel's behaviour in v1.

---

## Annotations across all shapes

1. **The parser's state machine** (`grpc-wire-protocol.md` §3) is `WAIT_PREFIX → READ_PREFIX(0..5) → READ_BODY(0..length) → COMPLETE`, with `STATUS_ONLY` for the trailer-only path. State is per-stream; multiple in-flight streams have independent parser instances.
2. **Bytes never accumulate without bound.** The parser holds at most 5 bytes of prefix in `READ_PREFIX`, plus the partial body up to `length`. `length` itself is bounded by `max_record_bytes` (request side) and `max_response_bytes` (response side); a peer claiming a length larger than that is rejected as `malformed_response`.
3. **The codec is fuzzed against arbitrary byte sequences** (`grpc-wire-protocol.md` §7.4). The fuzzer's invariants:
   - No crashes.
   - No ASAN / UBSAN findings.
   - Memory growth is bounded by the response-size limits regardless of input.
   - No infinite loops (parser always makes progress or terminates).

---

## Edge cases captured by tests

The byte-level corpus in `tests/grpc-wire/` covers (M3+):

- Trailer-only `OK` (success with no body — legal but unusual).
- Trailer-only `UNAVAILABLE` (the most common production case).
- Trailer-only without `grpc-status` (`malformed_response`).
- Trailer-only with HTTP `:status: 502` and no `grpc-status` (fallback to HTTP-status retryable).
- Multi-frame with prefix split 1+4, 2+3, 3+2, 4+1 (cover all split positions).
- Multi-frame with body split into 2, 3, 4 chunks of various sizes.
- Single frame containing two messages (rejected as `malformed_response`).
- Single frame with prefix only, no body, then trailer-only `OK` (`malformed_response`).
- Compressed body (`grpc-encoding: gzip`) split mid-stream (parser decompresses incrementally; bounded by `max_decompressed_bytes`).
- Maximum-size message (`max_response_bytes - 1`); message exactly at the cap; one byte over the cap (`response_too_large`).
- `RST_STREAM` after each parser state.
