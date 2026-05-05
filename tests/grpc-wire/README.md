# `tests/grpc-wire/`

The corpus from `docs/grpc-wire-protocol.md` §7. Each fixture is a
recorded HTTP/2 byte stream containing a gRPC payload; the codec is
run against it; the test asserts the resulting `WireResult`.

This directory exists separately from [`tests/wire/`](../wire/)
because the gRPC corpus is large and the edge cases are subtle —
keeping it separate makes the invariants easier to read.

## Required corpus entries (per spec §13.5 / `docs/grpc-wire-protocol.md` §7.2)

- Trailer-only response with `grpc-status: 0`.
- Trailer-only response with `grpc-status: 14` (UNAVAILABLE).
- Trailer-only response without `grpc-status` (malformed).
- Multi-DATA-frame response with a single message split mid-prefix.
- Multi-DATA-frame response with a single message split mid-body.
- Response with `RESOURCE_EXHAUSTED` and inline `RetryInfo` in
  `grpc-status-details-bin`.
- Response with `RESOURCE_EXHAUSTED` and **no** `RetryInfo`.
- Response with `partial_success` in DATA and `grpc-status: 0` in trailers.
- Response with conflicting HTTP `:status: 503` and `grpc-status: 0`.
- GOAWAY mid-stream during DATA.
- RST_STREAM with `INTERNAL_ERROR (0x2)` from peer.

## M1 ground-truth

The M1 spike (now deleted; recoverable at the `v0.1.1-m1` tag) verified
three of these against a real `otel/opentelemetry-collector:0.151.0`:
the happy path, the trailer-only response with non-zero status, and the
split-frame request. The recorded byte streams from those runs can be
captured and replayed here as M3 lands the codec.

## Bar

- **Byte-stable fixtures.** Same as `tests/wire/`. No regeneration in
  CI.
- **No real collector.** The whole point is testing the parser against
  byte streams, not a live server. The fixtures are the contract.
