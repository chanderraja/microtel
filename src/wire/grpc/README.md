# `src/wire/grpc/`

## Purpose

The OTLP/gRPC implementation of `IWireCodec`, built directly on
nghttp2 — **no gRPC library**. Owns the 5-byte length-prefix framing,
trailer HEADERS parsing, status interpretation including
`google.rpc.RetryInfo` decoding for `RESOURCE_EXHAUSTED`, and the
trailer-only / split-frame edge cases from `docs/grpc-wire-protocol.md`.

This is the project's load-bearing claim: gRPC unary works on top of
nghttp2 + OpenSSL with only the wire-level code in this directory.
Validated end-to-end during the M1 spike with all three variants
(happy / trailer_only / split_frame) against
`otel/opentelemetry-collector:0.151.0`.

## Owner

Track C — OTLP/gRPC wire codec.

## Implements

- The gRPC implementation of `internal::IWireCodec` (declared in
  [`include/microtel/internal/wire_codec.hpp`](../../../include/microtel/internal/wire_codec.hpp))
- The codec state machine from `docs/grpc-wire-protocol.md` §3
- `google.rpc.Status` and `google.rpc.RetryInfo` decoding via upb (the
  generated code lives under [`gen/google/rpc/`](../../../gen/), produced
  by Track F's regeneration pipeline)

## Depends on

- `EncodedPayload` (Track F)
- `ITransport`     (Track D)
- `IAuthProvider`  (Track E)
- `IDiagnosticsSink`
- The vendored `googleapis` proto subset under [`proto/google/rpc/`](../../../proto/)
  (added in M3 alongside the OTLP protos)

## Test entry points

- `tests/unit/wire/grpc/` — codec state machine, every status-code row
  from `error-model.md` §7.2.
- `tests/grpc-wire/` — the byte-level corpus from
  `docs/grpc-wire-protocol.md` §7 (trailer-only, multi-frame, split
  prefix, RST_STREAM, GOAWAY mid-stream, malformed responses).
- `tests/fuzz/grpc_codec_fuzz.cpp` — libFuzzer harness over the
  response-parser entry point; required for v1.0 release per spec §13.5.
- `tests/conformance/grpc/` — end-to-end against a real collector.

## Style notes

- **Use `frame->headers.cat`** (`NGHTTP2_HCAT_RESPONSE` vs
  `NGHTTP2_HCAT_HEADERS`) to distinguish the initial HEADERS frame from
  trailer HEADERS in the on-header callback. The trailer-only case is
  recognised by `END_STREAM=1` on the first HEADERS frame combined with
  `grpc-status` present (per `docs/grpc-wire-protocol.md` §2.5,
  validated in the M1 spike).
- **`RESOURCE_EXHAUSTED` without `RetryInfo` is non-retryable** (LOCKED —
  spec §7.2, `error-model.md` §7.2). The codec parses
  `grpc-status-details-bin` as `google.rpc.Status`, walks `details[]`
  for `RetryInfo`; absent → `retryable=false`.
- **Single-threaded.** Same contract as Track B.
- **5-byte prefix is big-endian length** (LOCKED — gRPC spec). Use
  explicit byte ops, not host-byte-order assumptions.
- **The parser does not assume one DATA frame == one gRPC message.**
  Split-prefix and split-body across frames are tested explicitly
  (M1 spike confirmed this works against real collectors).
