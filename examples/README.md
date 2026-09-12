# `examples/`

Standalone, runnable programs that demonstrate the public `microtel::*` API.
These are **not** part of the default build and are not held to the diff-coverage
or test-presence gates — they exist to be read and run.

## Building

Examples are gated behind `MICROTEL_BUILD_EXAMPLES` (default `OFF`):

```bash
cmake -S . -B build -DMICROTEL_BUILD_EXAMPLES=ON
cmake --build build --target microtel_example_basic_trace
```

The example links `microtel_sdk`, which transitively provides the public headers
and all runtime link dependencies (nghttp2, OpenSSL, zlib).

## Examples

### `basic_trace/`

The smallest end-to-end trace flow: build a `Provider` with `SdkBuilder`, open
the OTLP/gRPC connection, emit one request trace (a `Server` parent span with
two child spans, attributes, an event, and a status), `ForceFlush`, print
`GetExporterHealth()`, and `Shutdown`.

Run it against a local OTLP/gRPC collector (default endpoint
`http://localhost:4317`):

```bash
# Start a collector on :4317 first, e.g.
docker run --rm -p 4317:4317 otel/opentelemetry-collector

./build/examples/microtel_example_basic_trace
# or point at a specific endpoint:
./build/examples/microtel_example_basic_trace http://collector.internal:4317
```

OTLP/gRPC, not OTLP/HTTP, because microtel is HTTP/2-only: a plaintext
`http://` endpoint means HTTP/2 with prior knowledge, and a stock collector's
OTLP/HTTP receiver on `:4318` serves HTTP/1.1 only, so nothing can be
delivered. gRPC is h2c by definition and is unaffected. To use OTLP/HTTP,
point the example at an `https://` endpoint and change the `WithProtocol` call
to `microtel::Protocol::Http`. See
[`compatibility-matrix.md`](../docs/compatibility-matrix.md).

If no collector is reachable, the program still runs the full lifecycle and
reports the failure through `ForceFlush`'s status and `GetExporterHealth()`.

## Scope

These mirror the trace-only v1 API. Metrics and logs examples will be added when
those signals land (v1.2 and v1.3 respectively — see `microtel-roadmap.md`).
