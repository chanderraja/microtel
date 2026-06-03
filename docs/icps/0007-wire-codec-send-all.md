# ICP 0007 — Add `SendAll` to `IWireCodec` for HTTP/2 fan-out

**Status:** Accepted  
**Author:** Chander Raja  
**Affects:** `IWireCodec` (docs/interfaces.md §4.3)

## Motivation

`OtlpExporter::DrainQueue` currently processes batches one at a time:
encode → `codec->Send` → wait for 200 OK → next batch. With the default
`max_export_batch_size=512` and 10,000 spans, this produces 20 sequential
HTTP/2 round trips. The HTTP/2 transport already supports concurrent streams
but the exporter never submits more than one at a time, serialising all
20 into ~5.5 ms of flush latency.

Adding `SendAll` lets `HttpWireCodec` submit all pending requests to nghttp2
before waiting for any response, reducing flush latency from O(N × RTT) to
O(RTT).

## Interface change

Add a non-pure virtual method to `IWireCodec`:

```cpp
[[nodiscard]] virtual std::vector<WireResult> SendAll(
    std::vector<EncodedPayload>&& payloads,
    std::chrono::milliseconds deadline);
```

**Default implementation** calls `Send` in a loop — all existing mocks and
the gRPC codec inherit correct behaviour without changes.

**`HttpWireCodec` override**: for each payload, build headers and call
`transport->Send` (non-blocking) to obtain a `RequestHandle`. All handles
are stored with their owning `EncodedPayload` (keeping the `span` alive),
then waited on in submission order. Effectively turns N sequential round
trips into one.

## Retry interaction

`FanOutAndProcess` counts the initial `SendAll` as attempt 0. On a retryable
failure it calls `RunRetryLoop(batch, starting_attempt=1)` so that the total
number of attempts stays at `max_attempts` (not `max_attempts+1`). A
pre-loop budget check prevents an extra send when `retry_budget=0`.

## Affected components

- `include/microtel/internal/wire_codec.hpp` — new `SendAll` with default impl
- `src/wire/http/http_wire_codec.{hpp,cpp}` — fan-out override
- `src/exporter/otlp_exporter.{hpp,cpp}` — `DrainQueue` → `FanOutAndProcess`,
  `RunRetryLoop` gains `starting_attempt` parameter
- `tests/unit/exporter/otlp_exporter_test.cpp` — no changes needed; mock's
  default `SendAll` delegates to `Send`, so all existing assertions hold
- `tests/unit/wire/http/http_wire_codec_test.cpp` — new test covering fan-out
