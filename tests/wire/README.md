# `tests/wire/`

Protocol byte-level coverage for the encoder and the OTLP/HTTP codec.
This directory holds no tests or fixture files. It is an index: each
theme below names the tests that cover it, all of them under
[`tests/unit/wire/`](../unit/wire/). Responses are built byte for byte
in the test instead of being loaded from disk, so there is no separate
corpus to drift out of step with the code.

For the gRPC codec, see [`tests/grpc-wire/`](../grpc-wire/).

## Themes and the tests that cover them

| Theme | Tests |
|---|---|
| Encoder: `ExportTraceServiceRequest` payloads, field by field, decoded back and compared | [`tests/unit/wire/otlp_encoder_test.cpp`](../unit/wire/otlp_encoder_test.cpp) (`OtlpEncoderTest.*`), plus [`otlp_metric_encoder_test.cpp`](../unit/wire/otlp_metric_encoder_test.cpp) and [`otlp_log_encoder_test.cpp`](../unit/wire/otlp_log_encoder_test.cpp) |
| Encoder: arena lifetime under the encoder | [`tests/unit/wire/upb_arena_test.cpp`](../unit/wire/upb_arena_test.cpp) |
| HTTP: partial-success body parsing | [`tests/unit/wire/otlp_response_test.cpp`](../unit/wire/otlp_response_test.cpp) (`ParseRejectedSpansTest.*`), `HttpWireCodecTest.PartialSuccess_*` |
| HTTP: gzip request compression and response inflation | [`tests/unit/wire/gzip_test.cpp`](../unit/wire/gzip_test.cpp), `HttpWireCodecTest.Send_Compression*`, `HttpWireCodecTest.Response_Gzip*` |

## `error-model.md` §7.1 rows and the tests that cover them

One test per row of the OTLP/HTTP classification matrix, all in
[`tests/unit/wire/http/http_wire_codec_test.cpp`](../unit/wire/http/http_wire_codec_test.cpp).

| §7.1 row | Test |
|---|---|
| 2xx, no body | `Send_200_Succeeds`, `Send_206_Succeeds` |
| 2xx, partial-success body, rejected = 0 | `PartialSuccess_EmptyBody_ZeroRejected` |
| 2xx, partial-success body, rejected > 0 | `PartialSuccess_PopulatesRejectedSpans` |
| 429 | `Send_429_IsRetryable`, `Send_429_WithRetryAfterHeader_PropagatesDelay` |
| 502, 503, 504 | `Send_502_IsRetryable`, `Send_503_IsRetryable`, `Send_503_WithRetryAfterHeader_PropagatesDelay`, `Send_504_IsRetryable` |
| 404 | `Send_404_IsNonRetryable` |
| 415 | `Send_415_IsNonRetryable` |
| Other 4xx | `Send_Other4xx_IsNonRetryable` |
| Other 5xx | `Send_500_IsNonRetryable` |
| Connection failure / TLS failure / read timeout | `Send_TransportFailure_ReturnsError`, `Send_WhenDisconnectedAndConnectFails_ReturnsRetryableWithoutSending`, `Diagnostics_ConnectFails_CountsConnectFailure` |
| Response > `max_response_bytes` | `Send_ResponseTooLarge_IsNonRetryableAndCounted`, `Send_TrailersTooLarge_CountsResponseTooLarge` (both codecs) for the classification; the cap itself lives in the transport and is tested end to end in `tests/integration/transport/http2_send_test.cpp` (`Send_ResponseOverMaxResponseBytes_FailsAndDropsTheBody`, `Send_TrailersOverMaxTrailerBytes_Fails`, `GrpcExport_OversizedResponse_IsTerminalAndCounted`) |
| Decompressed body > `max_decompressed_bytes` | `Response_DecompressionBomb_RecordsDecompressionTooLarge` |
| Body unparseable / malformed encoding | `Response_UnknownContentEncoding_IsMalformed`, `Response_CorruptGzipBody_IsMalformed` |

## Rules

- Bytes are deterministic. A response a test asserts on is built by the
  test, byte for byte, and CI regenerates nothing.
- No external dependencies: no collector, no network. The live check is
  [`tests/conformance/`](../conformance/).
- Every row of the matrix has a test. A row with no test in the table
  above is an open gap, and the table is where that has to show.
