# `tests/wire/`

Protocol byte-level coverage for the encoder and the OTLP/HTTP codec.
This directory holds **no fixture files**: each theme below names the
tests that cover it, all of them under
[`tests/unit/wire/`](../unit/wire/). Responses are built byte-for-byte
in the test rather than loaded from disk — same contract, no separate
corpus to drift.

For the gRPC codec, see [`tests/grpc-wire/`](../grpc-wire/).

## Themes → covering tests

| Theme | Tests |
|---|---|
| `encoder/` — `ExportTraceServiceRequest` payloads, field by field, decoded back and compared | [`tests/unit/wire/otlp_encoder_test.cpp`](../unit/wire/otlp_encoder_test.cpp) (`OtlpEncoderTest.*`), plus [`otlp_metric_encoder_test.cpp`](../unit/wire/otlp_metric_encoder_test.cpp) and [`otlp_log_encoder_test.cpp`](../unit/wire/otlp_log_encoder_test.cpp) |
| `encoder/` — arena lifetime under the encoder | [`tests/unit/wire/upb_arena_test.cpp`](../unit/wire/upb_arena_test.cpp) |
| `http/` — partial-success body parsing | [`tests/unit/wire/otlp_response_test.cpp`](../unit/wire/otlp_response_test.cpp) (`ParseRejectedSpansTest.*`), `HttpWireCodecTest.PartialSuccess_*` |
| `http/` — gzip request compression and response inflation | [`tests/unit/wire/gzip_test.cpp`](../unit/wire/gzip_test.cpp), `HttpWireCodecTest.Send_Compression*`, `HttpWireCodecTest.Response_Gzip*` |

## `error-model.md` §7.1 rows → covering tests

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
| Response > `max_response_bytes` | **none** — the cap `gzip.cpp` and `error-model.md` both assume is not enforced anywhere in `src/`, so there is no behaviour to test yet |
| Decompressed body > `max_decompressed_bytes` | `Response_DecompressionBomb_RecordsDecompressionTooLarge` |
| Body unparseable / malformed encoding | `Response_UnknownContentEncoding_IsMalformed`, `Response_CorruptGzipBody_IsMalformed` |

## Bar

- **Deterministic bytes.** A response a test asserts on is built by the
  test, byte for byte; CI regenerates nothing.
- **No external dependencies.** No collector, no network. The live
  check is `tests/conformance/`.
- **One test per matrix row.** A row with no test in the table above is
  an open gap, and the table is where it has to show.
