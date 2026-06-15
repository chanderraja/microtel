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
the OTLP/HTTP-protobuf connection, emit one request trace (a `Server` parent span
with two child spans, attributes, an event, and a status), `ForceFlush`, print
`GetExporterHealth()`, and `Shutdown`.

Run it against a local OTLP/HTTP collector (default endpoint
`http://localhost:4318`):

```bash
# Start a collector on :4318 first, e.g.
docker run --rm -p 4318:4318 otel/opentelemetry-collector

./build/examples/microtel_example_basic_trace
# or point at a specific endpoint:
./build/examples/microtel_example_basic_trace http://collector.internal:4318
```

If no collector is reachable, the program still runs the full lifecycle and
reports the failure through `ForceFlush`'s status and `GetExporterHealth()`.

## Scope

These mirror the trace-only v1 API. Metrics and logs examples will be added when
those signals land (v1.2 and v1.3 respectively — see `microtel-roadmap.md`).
