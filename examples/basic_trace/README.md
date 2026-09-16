# `basic_trace`

The smallest end-to-end microtel flow, and the house style every other example
follows.

It builds a `Provider` with `SdkBuilder`, opens the OTLP/gRPC connection, emits
one request trace — a `Server` parent span with two `Internal` children,
attributes, an event, and a status — prints the trace ID, `ForceFlush`es,
prints `GetExporterHealth()`, and `Shutdown`s.

## Run it

Start the shared stack, then run the binary with no arguments — its default
endpoint is the stack's collector:

```bash
examples/stack/up.sh

cmake -S . -B build -DMICROTEL_BUILD_EXAMPLES=ON
cmake --build build --target microtel_example_basic_trace

./build/examples/microtel_example_basic_trace
```

```
trace_id: 532267361510131fac037e635454ce60
ForceFlush: Completed
batches_sent=1 batches_failed=0 queue_depth=0
Shutdown: Completed
```

Open <http://localhost:3000> — the **microtel — recent traces** dashboard is
the home page and the trace shows up within about ten seconds. Or go straight
at it with the printed ID:

```bash
curl -s http://localhost:3200/api/traces/532267361510131fac037e635454ce60
```

A different endpoint is `argv[1]`:

```bash
./build/examples/microtel_example_basic_trace http://collector.internal:4317
```

## What to notice in the code

- **The trace ID is printed.** `EmitRequestTrace` returns
  `SpanContext::trace_id.ToHex()`. Without it there is no way to tell "the
  export failed" from "I am looking at the wrong time range", and every example
  in this tree prints one for that reason.
- **`StartSpanOptions` is initialised with every field** —
  `.kind`/`.parent`/`.start_time`/`.attributes`. A partial designated
  initialiser trips `-Wmissing-field-initializers` under the `-Werror` the
  examples build with.
- **String attribute values are explicitly `std::string`.** A bare string
  literal is a `const char*` and would bind to the `bool` alternative of
  `microtel::AttributeValue` — silently recording `true` instead of your
  string.
- **`Connect()` is optional** and called anyway. The connection is established
  lazily on first export; calling it eagerly turns a misconfigured endpoint
  into a message up front. The example warns and continues, so the full
  lifecycle still runs with nothing listening — `ForceFlush`'s status and
  `GetExporterHealth()` then report the failure, which is what the API is for.
- **The `Status` switch is exhaustive**, including the setter-only
  `InvalidArgument` and `Unsupported` that `ForceFlush` never returns. That is
  what makes `-Wswitch` catch the next enumerator instead of a `default:`
  swallowing it.

## Finding this trace among others

The service name is what a backend groups by, and it is set at build time:

```cpp
.WithServiceName("microtel-basic-example")
.WithServiceVersion("1.0.0")
```

In Grafana's **Explore → Tempo**, that is the TraceQL filter:

```
{ resource.service.name = "microtel-basic-example" }
```

Change the string and rebuild to tell two runs apart — there is no
`--service-name` flag, because a real service names itself once at startup and
an example that took it from the command line would be teaching the wrong
thing. `OTEL_SERVICE_NAME` is the mechanism for deployment-time overrides; see
[`docs/configuration.md`](../../docs/configuration.md) §3.

## Protocol

`Protocol::Grpc` against plaintext `http://`, not OTLP/HTTP. microtel is
HTTP/2-only, so plaintext OTLP/HTTP is h2c with prior knowledge and cannot
reach an HTTP/1.1-only collector receiver — see
[`docs/compatibility-matrix.md`](../../docs/compatibility-matrix.md) §4 and the
[`examples/README.md`](../README.md) note. gRPC is h2c by definition and is
unaffected. For OTLP/HTTP, point this at an `https://` endpoint and add
`.WithProtocol(microtel::Protocol::Http)`.
