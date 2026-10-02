# `console_trace`

Spans on your terminal, with nothing else running. No collector, no
containers, no network: build it, run it, read the spans.

It emits the same request trace as [`basic_trace`](../basic_trace/) (a `Server`
parent span with two `Internal` children), but exports it through an
`ExportTransport` of its own instead of to a collector. The transport decodes
each OTLP request it is handed and prints the spans to stdout.

## Run it

From a fresh clone, with a C++20 compiler, CMake 3.20+ and the OpenSSL,
nghttp2 and zlib development packages installed (the root README's
[Getting started](../../README.md#getting-started) has the install lines):

```bash
cmake -S . -B build -DMICROTEL_BUILD_TESTS=OFF -DMICROTEL_BUILD_EXAMPLES=ON
cmake --build build -j"$(nproc)"
./build/examples/microtel_example_console_trace
```

Real output from one run (IDs and durations differ every time):

```
[microtel info] resolved resource (profile "default", 2 attributes): service.name="microtel-console-example", service.version="1.0.0"
trace_id: 7b7deb8eb7704447cfb9837cee076f92
--- export request: 512 bytes of OTLP protobuf
resource service.name="microtel-console-example"
resource service.version="1.0.0"
scope microtel-console-example
  span example.db.query  kind=Internal  status=Unset  duration=3.063 ms
    trace_id  7b7deb8eb7704447cfb9837cee076f92
    span_id   9f1eab58be717eae
    parent    4f8fe2d8341485d6
    attr      db.system="postgresql"
    event     query.start
  span example.render  kind=Internal  status=Unset  duration=1.057 ms
    trace_id  7b7deb8eb7704447cfb9837cee076f92
    span_id   1029082f8528a42c
    parent    4f8fe2d8341485d6
    attr      template="widgets.html"
  span example.request  kind=Server  status=Ok  duration=4.126 ms
    trace_id  7b7deb8eb7704447cfb9837cee076f92
    span_id   4f8fe2d8341485d6
    attr      http.request.method="GET"
    attr      url.path="/api/widgets"
    attr      http.response.status_code=200
ForceFlush: Completed
batches_sent=1 batches_failed=0 rejected=0
Shutdown: Completed
```

The first line is microtel's own startup log, on stderr. The children print
before their parent because a span is exported when it ends, and the parent
ends last. The parent has no `parent` line: it is the root of the trace.

## What is real and what is not

Everything up to the last hop is the real pipeline: the tracer, the sampler,
the batch processor, the OTLP encoder and the exporter worker. The bytes the
transport decodes are, byte for byte, the `ExportTraceServiceRequest` a
collector would receive. Only the delivery is replaced.

The decoder is not. It's a small hand-written protobuf reader that knows the
OTLP fields this program prints (span name, IDs, kind, times, string, bool,
int and double attributes, event names, status code) and skips everything
else. It is there so the example links nothing beyond `microtel_sdk`. Don't
treat it as an OTLP parser: it doesn't validate, and a field it doesn't know
is silently left out.

When you want to search, filter or graph traces, send them to a collector:
[`basic_trace`](../basic_trace/) and the [examples stack](../stack/) do that
with the same trace.

## What to notice in the code

`SdkBuilder::WithExportTransport` (ICP 0036) takes the place of the endpoint.
There's no `WithEndpoint`, no `WithProtocol` and no `Connect()`, because
microtel's HTTP/2 transport isn't built. The same hook carries OTLP over UDP
in the [leaf example](../leaf/udp_full_node.cpp); here it ends at stdout.

`ConsoleExportTransport::Send` runs on the exporter's worker thread, not on
the thread that ended the spans. It formats the whole request into a string
and writes it with one `<<`, so its output cannot interleave with `main`'s.
`main` calls `ForceFlush` so the batch is exported now rather than on the
batch processor's schedule, and the transport prints while that call waits.

`Cancel` only sets a flag that makes later `Send` calls return at once. Every
`ExportTransport` has to make sure a `Send` cannot block shutdown forever;
this one never waits on anything but a write to stdout, so the flag is all it
needs. A transport that writes to a socket or a serial line needs more; see
the leaf example's `UdpExportTransport`.
