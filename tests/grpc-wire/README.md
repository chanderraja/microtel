# `tests/grpc-wire/`

The gRPC response corpus from `docs/grpc-wire-protocol.md` §7.2 and spec
§13.5. This directory holds no fixture files. Every entry in the corpus
is covered by a test that builds the response in code, and the table
below says which one.

That is a deliberate reading of the corpus, and it splits in two along
the line the codec's interface draws.

Response content (statuses, trailers, `RetryInfo`, the partial-success
body) is tested in [`tests/unit/wire/grpc/`](../unit/wire/grpc/).
`FakeTransport` hands the codec an exact `TransportResult`, which is a
byte-stable fixture written in C++ instead of in a file, and the check
needs no socket.

Response framing (GOAWAY, RST_STREAM, a message split across DATA
frames) can't be reached through `FakeTransport`. These happen below
`ITransport`, and a fake that hands over one finished body cannot
express them. They need a real nghttp2 peer, so they live in
[`tests/integration/transport/http2_send_test.cpp`](../integration/transport/http2_send_test.cpp),
which drives the real codec over the real transport against an
in-process server scripted to produce the frame in question.

## Required corpus entries and the tests that cover them

| # | Corpus entry (`grpc-wire-protocol.md` §7.2) | Test |
|---|---|---|
| 1 | Trailer-only response with `grpc-status: 0` | `GrpcWireCodecTest.Send_TrailerOnly_GrpcStatus0_Succeeds` |
| 2 | Trailer-only response with `grpc-status: 14` (UNAVAILABLE) | `GrpcWireCodecTest.Send_TrailerOnly_Unavailable_Retryable` |
| 3 | Trailer-only response without `grpc-status` (malformed) | `GrpcWireCodecTest.Send_MissingGrpcStatus_Http200_NotRetryable`, `…_Http429_Retryable`, `…_Http503_Retryable`, `…_MessageNamesTheHttpStatus` |
| 4 | Multi-DATA-frame response, message split mid-prefix | `Http2TransportSendIntegrationTest.GrpcResponse_SplitMidPrefix_Accumulates` |
| 5 | Multi-DATA-frame response, message split mid-body | `Http2TransportSendIntegrationTest.GrpcResponse_SplitMidBody_Accumulates` |
| 6 | `RESOURCE_EXHAUSTED` with inline `RetryInfo` | `GrpcWireCodecTest.Send_ResourceExhausted_WithRetryInfo_RetryableWithDelay` |
| 7 | `RESOURCE_EXHAUSTED` with **no** `RetryInfo` | `GrpcWireCodecTest.Send_ResourceExhausted_WithoutRetryInfo_NotRetryable` |
| 8 | `partial_success` in DATA with `grpc-status: 0` | `GrpcWireCodecTest.PartialSuccess_PopulatesRejectedSpans` |
| 9 | Conflicting HTTP `:status: 503` with `grpc-status: 0` | `GrpcWireCodecTest.Send_Http503WithGrpcStatus0_SucceedsPerGrpcStatus` |
| 10 | GOAWAY mid-stream during DATA | `Http2TransportSendIntegrationTest.Send_PeerGoawayRefusesStream_FailsNamingGoaway`, `…_PeerGoawayWithErrorCode_NamesTheCode`, `…_PeerGoawayAfterAccepting_CompletesThenReconnects` |
| 11 | RST_STREAM with `INTERNAL_ERROR (0x2)` from peer | `Http2TransportSendIntegrationTest.Send_PeerRstStream_FailsRequestKeepsConnection` |

Rows 1-3 and 6-9 are in
[`tests/unit/wire/grpc/grpc_wire_codec_test.cpp`](../unit/wire/grpc/grpc_wire_codec_test.cpp);
rows 4-5 and 10-11 in
[`tests/integration/transport/http2_send_test.cpp`](../integration/transport/http2_send_test.cpp).

Related but outside the corpus: the malformed-framing rows
(`Response_ShortBody_IsMalformed`, `Response_UnknownCompressionFlag_IsMalformed`,
`Response_DeclaredLengthLongerThanBody_IsMalformed`,
`Response_TrailingBytesAfterMessage_IsMalformed`) cover §2.3's rejection
cases, and `tests/fuzz/grpc_codec_fuzz.cpp` fuzzes the same parser.

## M1 ground truth

The M1 spike (since deleted, but recoverable at the `v0.1.1-m1` tag) verified
three of these against a real `otel/opentelemetry-collector:0.151.0`:
the happy path, the trailer-only response with non-zero status, and the
split-frame request. `tests/conformance/` is where that end-to-end
check lives now.

## Rules

- Responses are deterministic. A response a test asserts on is built
  byte for byte by the test. It is never regenerated and never sampled
  from a live peer.
- No real collector. The point is to test the parser against bytes. The
  in-process nghttp2 server the framing rows use is a scripted peer; the
  collector lives in `tests/conformance/`.
- A new corpus entry adds a row above. An entry with no test in this
  table is an open gap, and making that visible is what the table is
  for.
