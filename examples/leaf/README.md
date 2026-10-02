# `leaf`

A device too small for the C++ runtime, and the gateway that speaks OTLP for
it. **Experimental in v1.2.**

`udp_leaf` plays the device. It's a C program that uses the microtel leaf
library (`microtel/leaf.h`) to build spans in static memory, encodes each batch
as one OTLP payload, and sends it to the concentrator as one UDP datagram.
`udp_concentrator` plays the gateway. It's an ordinary microtel `Provider`
with its leaf receiver turned on: it reads datagrams from its own socket,
names each leaf by the sender's `address:port`, and hands the bytes to
`LeafReceiver::Ingest`. From there the spans take the Provider's normal path to
the collector, with every leaf's spans sharing export requests.

microtel opens no socket for any of this. The leaf library does no I/O, and the
receiver takes bytes you've already read. The UDP code on both sides belongs to
the example, which is the point: swap it for a UART, CAN, BLE or a pipe and
nothing microtel-shaped changes.

The design is [`docs/leaf-concentrator-design.md`](../../docs/leaf-concentrator-design.md).

## Run it

You'll need the leaf and the concentrator compiled in. Both are off by default.

```bash
examples/stack/up.sh

cmake -S . -B build -DMICROTEL_BUILD_EXAMPLES=ON \
      -DMICROTEL_BUILD_LEAF=ON -DMICROTEL_WITH_CONCENTRATOR=ON
cmake --build build --target microtel_example_leaf_concentrator \
                    --target microtel_example_leaf_udp_leaf
```

Start the concentrator first, since it binds the port the leaves send to. Then
start two leaves from a second terminal. The first uses the defaults; the
second sends from another port, in boot-relative time mode, and stops after
three payloads.

```bash
# terminal 1
./build/examples/microtel_example_leaf_concentrator
```
```bash
# terminal 2
./build/examples/microtel_example_leaf_udp_leaf &
./build/examples/microtel_example_leaf_udp_leaf boot 9310 9312 3
```

The arguments:

```
microtel_example_leaf_concentrator [endpoint] [listen-port] [config]

  [endpoint]     OTLP/gRPC collector, default $OTEL_EXPORTER_OTLP_ENDPOINT,
                 else http://localhost:4317
  [listen-port]  UDP port on 127.0.0.1, default 9310
  [config]       TOML with the [concentrator] table, default microtel.toml here

microtel_example_leaf_udp_leaf [stamped|sync|boot] [concentrator-port] [source-port] [payloads] [command]

  defaults: stamped 9310 9311 5
  command:  wait for a controller's command before each payload; see
            "A fault, traced across devices" below
```

The concentrator waits up to a minute for the first datagram, and exits once
none has arrived for three seconds, so the example can show it flushing and
shutting down. A real gateway would loop.

### What they print

```
$ ./build/examples/microtel_example_leaf_udp_leaf
leaf 127.0.0.1:9311, time mode stamped, leaf library 1.1.1
payload 1: 457 bytes, 2 spans -> 127.0.0.1:9310
payload 2: 457 bytes, 2 spans -> 127.0.0.1:9310
payload 3: 457 bytes, 2 spans -> 127.0.0.1:9310
payload 4: 457 bytes, 2 spans -> 127.0.0.1:9310
payload 5: 457 bytes, 2 spans -> 127.0.0.1:9310
```

```
$ ./build/examples/microtel_example_leaf_udp_leaf boot 9310 9312 3
leaf 127.0.0.1:9312, time mode boot, leaf library 1.1.1
payload 1: 490 bytes, 2 spans -> 127.0.0.1:9310
payload 2: 490 bytes, 2 spans -> 127.0.0.1:9310
payload 3: 490 bytes, 2 spans -> 127.0.0.1:9310
```

```
$ ./build/examples/microtel_example_leaf_concentrator
concentrator: UDP 127.0.0.1:9310 -> http://localhost:4317 (OTLP/gRPC)
config: /path/to/microtel/examples/leaf/microtel.toml
127.0.0.1:9312  490 bytes  Accepted  spans_accepted=2
127.0.0.1:9311  457 bytes  Accepted  spans_accepted=2
127.0.0.1:9311  457 bytes  Accepted  spans_accepted=2
127.0.0.1:9312  490 bytes  Accepted  spans_accepted=2
127.0.0.1:9311  457 bytes  Accepted  spans_accepted=2
127.0.0.1:9312  490 bytes  Accepted  spans_accepted=2
127.0.0.1:9311  457 bytes  Accepted  spans_accepted=2
127.0.0.1:9311  457 bytes  Accepted  spans_accepted=2
payloads_accepted=8 payloads_rejected=0 leaves_tracked=2 time_fallbacks=0
ForceFlush: Completed
batches_sent=2 batches_failed=0
Shutdown: Completed
```

Each line of the concentrator's output is one `Ingest` call and its
`IngestResult`. The two leaves' payloads interleave, and the Provider's batch
processor collects them regardless of which leaf they came from.

`batches_sent=2` is not two export requests. The concentrator keeps one batch
per device and joins a drain's batches into one request, but counts each
batch it sends (design §3.6.1), so the 2 is one batch per leaf. The counter
can't say how many requests reached the collector, and neither can the
collector's normal pipeline, whose batch processor merges requests. Sent to the
collector's unbatched receiver instead, the same run shows all sixteen spans
from both devices arriving in a single request; see
[One request, many devices](#one-request-many-devices).

A payload is about 460 bytes for two spans with four attributes and an event
between them. The boot-relative leaf's payloads are 33 bytes bigger, since
they also carry the boot id the concentrator anchors its timestamps to.

## What the collector receives

The leaves don't send trace IDs to anything you can print, so look the traces
up by what the concentrator put on them:

```bash
$ curl -s -G http://localhost:3200/api/search \
    --data-urlencode 'q={ resource.service.name =~ "greenhouse-.*" }' | jq -r \
    '.traces[] | "\(.traceID)  \(.rootServiceName)  \(.rootTraceName)"'
e8dcfc5a6ff89cee02a97b4c873d7003  greenhouse-north  greenhouse.cycle
42d6a0fbcd17f9d97311d47776a9fefa  greenhouse-north  greenhouse.cycle
a9aee00560df49e1506d6f1e3222b3d7  greenhouse-sensor  greenhouse.cycle
...
```

Eight traces, one per payload. Five of them come from `greenhouse-north` and
three from `greenhouse-sensor`, although both leaves run the same firmware,
which calls itself `greenhouse-sensor`. The Resource of one of the north
traces explains why:

```bash
$ curl -s http://localhost:3200/api/traces/e8dcfc5a6ff89cee02a97b4c873d7003 | \
    jq -c '.batches[0].resource.attributes | map({(.key): (.value | to_entries[0].value)}) | add'
{"deployment.environment":"example","device.id":"127.0.0.1:9311","greenhouse.zone":"north","service.version":"0.3.1","service.name":"greenhouse-north"}
```

Every key has a source, and the order they merge in is the design's §4.4:

| Key | From |
|---|---|
| `deployment.environment` | `[concentrator.leaf_defaults.resource]` in `microtel.toml`: a default for every leaf |
| `service.version` | the leaf's own Resource, set in firmware |
| `service.name`, `greenhouse.zone` | `[concentrator.leaves."127.0.0.1:9311"]`: this device's entry overrides the firmware's `service.name` |
| `device.id` | the transport id, `address:port`, which the leaf cannot choose or override |

The second leaf, on port 9312, has no entry in `microtel.toml`, so it keeps
the name its firmware gives it. `unknown_leaf = "accept"` is why its payloads
got in at all; with `"reject"`, the entries would be an allow-list and its
payloads would come back `UnknownLeaf`.

None of the leaf's bookkeeping reaches the collector. Every payload carries
reserved `microtel.leaf.*` attributes (wire version, time mode, encode time,
boot id, leaf-side drop counts), and the concentrator strips them after reading
them.

In Grafana (<http://localhost:3000>) the traces show up on the "microtel —
recent traces" dashboard within about ten seconds. Each has a
`greenhouse.cycle` root with a 40 ms `sensor.read` child, carrying the
reading's attributes and a `sample.taken` event.

## Time

A leaf's clock usually has no idea what time it is. The payload says which of
three modes its timestamps are in, and the concentrator converts them to Unix
time before anything is exported:

| Mode | The leaf sends | The concentrator computes |
|---|---|---|
| `stamped` | its own clock readings, plus the reading at encode time `E` | `t + (R − E)`, where `R` is when the datagram arrived |
| `sync` | Unix times, converted with its last `microtel_leaf_clock_sync` | nothing, if the sync is recent and the clocks agree within `max_clock_skew`; otherwise it falls back to `stamped` and counts `time_fallbacks` |
| `boot` | time since boot, plus a boot id | `t + B`, where `B` is an anchor kept per leaf from the last 16 `R − E` samples |

`stamped` is exact about durations within a payload and late by the link's
latency. `boot` keeps one timeline across all of a boot's payloads. `sync` is
for a device that learns the wall time now and then, from GPS, an RTC, or the
concentrator itself. The leaf here reads the host's clock once at start-up.
Run it with `sync` and the concentrator's `time_fallbacks` stays at 0.

`R` is the `received_at` the concentrator passes to `Ingest`, which here is
`system_clock::now()` right after `recvfrom`. A concentrator that reads its
link in batches should stamp each datagram when it arrives, not when it gets
round to ingesting it.

## The leaf half, line by line

Everything the leaf owns is caller-allocated and static:

```c
static microtel_leaf_t g_leaf;                   /* 256 bytes of opaque state */
static uint8_t g_records[RECORD_BUFFER_BYTES];   /* spans live here until encoded */

st = microtel_leaf_init(&g_leaf, sizeof(g_leaf), &config, g_records, sizeof(g_records));
```

`sizeof(g_leaf)` is passed so that a library built with a bigger state than
this header describes refuses to start instead of writing past the object.
The config names the clock, the random source (for ids), the Resource and the
scope, and is copied, so it can live on the stack.

Spans are built with calls that copy their strings into the record buffer, so
nothing the leaf is given has to outlive the call:

```c
microtel_leaf_span_start(&g_leaf, &root, kCycle, sizeof(kCycle) - 1u,
                         MICROTEL_LEAF_SPAN_KIND_INTERNAL, NULL);
microtel_leaf_span_start(&g_leaf, &read, kRead, sizeof(kRead) - 1u,
                         MICROTEL_LEAF_SPAN_KIND_CLIENT, &root);
...
microtel_leaf_encode(&g_leaf, datagram, sizeof(datagram), &written);
sendto(fd, datagram, written, 0, ...);
```

`microtel_leaf_encode` writes one `ExportTraceServiceRequest` and releases the
spans it encoded. When the buffer is too small it consumes nothing and
reports the size it needs. A device that can't hold the whole payload uses
`microtel_leaf_encode_to` instead: with the default nanopb encoder it hands
the bytes to a callback piece by piece.

Every call returns a status. A full record buffer or a per-span limit drops
the item and counts it, and the next payload reports the count to the
concentrator, which adds it to `LeafReceiverStats::leaf_reported_drops`.

## The concentrator half

```cpp
auto built = microtel::SdkBuilder{}
                 .FromFile(config)                  // [concentrator] enabled = true
                 .WithEndpoint(endpoint)
                 .WithProtocol(microtel::Protocol::Grpc)
                 .WithServiceName("microtel-leaf-concentrator")
                 .Build();
const auto receiver = provider->GetLeafReceiver();

const microtel::IngestResult result = receiver->Ingest(microtel::IngestRequest{
    .leaf_id = leaf_id,                             // "127.0.0.1:9311"
    .payload = std::span<const std::byte>(buffer.data(), n),
    .received_at = std::chrono::system_clock::now(),
});
```

`Ingest` runs on the calling thread and never throws. It decodes the payload
into its own arena, checks it all-or-nothing (a payload that fails any rule is
`Malformed` and none of its spans get in), gives it the leaf's Resource,
corrects its timestamps and enqueues the spans. It returns before anything is
exported. Rejections are counted in `LeafReceiverStats` and in three drop
counters in `GetExporterHealth()`: `leaf_payload_malformed`,
`leaf_payload_too_large` and `leaf_unknown`. You can see one by sending
something that isn't a payload:

```bash
printf 'not otlp' | nc -u -w1 127.0.0.1 9310
```

```
127.0.0.1:54942  8 bytes  Malformed  spans_accepted=0
```

The concentrator's own `service.name` is not merged into leaf spans. Its
Resource describes the gateway, and putting it on the devices' spans would
attribute them all to the gateway. Keys every leaf should carry go in
`leaf_defaults.resource`.

The receiver is compiled in only with `-DMICROTEL_WITH_CONCENTRATOR=ON`,
because it parses untrusted bytes and pulls upb's decoder into the link. Built
without it, `[concentrator] enabled = true` makes `Build()` fail and says so.

## Building the leaf for a microcontroller

The leaf builds on its own with only a C11 compiler, no C++ toolchain, and no
nghttp2, OpenSSL or zlib:

```bash
cmake -S leaf -B build-m4 \
      -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/arm-none-eabi.cmake \
      -DMICROTEL_LEAF_CPU=cortex-m4 -DCMAKE_BUILD_TYPE=MinSizeRel
cmake --build build-m4
cmake --install build-m4 --prefix leaf-m4
```

That installs `leaf.h` and three archives: `libmicrotel_leaf.a` and the
vendored, renamed nanopb (`libmicrotel_nanopb.a`, `libmicrotel_nanopb_gen.a`).
Link all three into the firmware. The nanopb build never allocates, and a CI
check fails if it ever references `malloc` or `free`.

[`size_probe.c`](size_probe.c) is the smallest useful firmware: one span, one
attribute, one streaming encode. The `leaf-footprint` CI job links it with
each backend for Cortex-M0+, Cortex-M4 and aarch64 on every PR and reports the
leaf's share of the image and its worst-case stack in the job summary. `ci/scripts/leaf-footprint.sh cortex-m4` runs the
same measurement locally if `arm-none-eabi-gcc` is installed. The latest
figures are in
[`docs/bench-results/leaf-footprint.md`](../../docs/bench-results/leaf-footprint.md).

For a Linux-class device, the upb encoder (`-DMICROTEL_LEAF_ENCODER=upb`)
produces byte-for-byte the same payloads. It takes about one and a half times
the flash on the same target (14.4 KB against 9.3 KB on a Cortex-M4), encodes into an arena (pass `config.scratch` to keep it off the
heap), and calls `write` once per payload instead of once per field.

## A full C++ node on the same link

Not every device on the link is a microcontroller. The Linux board next to
the sensors can run the full C++ runtime, with sampling, batching, retries and
the whole Tracer API, but it may have no route to the collector either.
`udp_full_node` is that board. It is an ordinary microtel program that exports
through an `ExportTransport` of its own instead of HTTP/2
([ICP 0036](../../docs/icps/0036-custom-export-transport.md)). Each encoded
OTLP request goes to the same concentrator as one UDP datagram, from a fixed
source port.

It needs neither build option, only a concentrator to send to. Start the
concentrator, then a leaf and the node:

```bash
# terminal 1
./build/examples/microtel_example_leaf_concentrator
```
```bash
# terminal 2
./build/examples/microtel_example_leaf_udp_leaf &
./build/examples/microtel_example_leaf_full_node
```

```
microtel_example_leaf_full_node [concentrator-port] [source-port] [cycles] [leaf-port]

  defaults: 9310 9313 5, and no leaf-port
  leaf-port: send each cycle's command to the leaf on this port; see
             "A fault, traced across devices" below
```

### What it prints

```
$ ./build/examples/microtel_example_leaf_full_node
full node 127.0.0.1:9313 -> 127.0.0.1:9310 (OTLP over UDP)
cycle 1: 2 spans
cycle 2: 2 spans
cycle 3: 2 spans
cycle 4: 2 spans
cycle 5: 2 spans
ForceFlush: Completed
batches_sent=1 batches_failed=0
Shutdown: Completed
```

```
$ ./build/examples/microtel_example_leaf_concentrator
concentrator: UDP 127.0.0.1:9310 -> http://localhost:4317 (OTLP/gRPC)
config: /path/to/microtel/examples/leaf/microtel.toml
127.0.0.1:9311  457 bytes  Accepted  spans_accepted=2
127.0.0.1:9311  457 bytes  Accepted  spans_accepted=2
127.0.0.1:9311  457 bytes  Accepted  spans_accepted=2
127.0.0.1:9313  1261 bytes  Accepted  spans_accepted=10
127.0.0.1:9311  457 bytes  Accepted  spans_accepted=2
127.0.0.1:9311  457 bytes  Accepted  spans_accepted=2
payloads_accepted=6 payloads_rejected=0 leaves_tracked=2 time_fallbacks=0
ForceFlush: Completed
batches_sent=2 batches_failed=0
Shutdown: Completed
```

The node batched its five cycles, ten spans, into one request, and the
concentrator took it as one payload from `127.0.0.1:9313`. The leaf and the
node share the concentrator's leaf table and its export requests.

### The concentrator trusts the node's clock by name

The node's request is an ordinary `ExportTraceServiceRequest` with no
`microtel.leaf.*` header, so it declares no time mode. The concentrator
refuses such a payload unless the sender is named with `time_mode = "unix"`,
which is what `microtel.toml` does for the node's port:

```toml
[concentrator.leaves."127.0.0.1:9313"]
time_mode = "unix"
```

That says the node already holds Unix time, so its timestamps pass through
unchanged. Without the entry the same datagram comes back `Malformed`.
`default_time_mode = "unix"` would trust every sender instead.

In the collector, the node keeps its own `service.name`, since its entry sets
no Resource, and gets the same `device.id` and defaults as any leaf:

```bash
$ curl -s -G http://localhost:3200/api/search \
    --data-urlencode 'q={ resource.service.name = "greenhouse-controller" }' | jq -r \
    '.traces[] | "\(.traceID)  \(.rootServiceName)  \(.rootTraceName)"'
838b993be5c99cd6f21b5c4254d6b34d  greenhouse-controller  greenhouse.control
e69bac32d8cb786d192165a709c4e993  greenhouse-controller  greenhouse.control
...
$ curl -s http://localhost:3200/api/traces/838b993be5c99cd6f21b5c4254d6b34d | \
    jq -c '.batches[0].resource.attributes | map({(.key): (.value | to_entries[0].value)}) | add'
{"deployment.environment":"example","device.id":"127.0.0.1:9313","service.name":"greenhouse-controller"}
```

### The transport

```cpp
microtel::SendResult Send(const microtel::ExportRequest& request) override
{
    if (m_cancelled.load()) { return Failure(NonRetryable, "cancelled"); }
    if (request.bytes.size() > kMaxDatagram) { return Failure(NonRetryable, ...); }
    SetSendTimeout(request.deadline);          // SO_SNDTIMEO, at least 1 ms
    if (::send(m_socket.Get(), request.bytes.data(), request.bytes.size(), 0) < 0)
    {
        return FromErrno(errno);               // EAGAIN, ECONNREFUSED: Retryable
    }
    return {.outcome = microtel::SendOutcome::Success};
}

void Cancel() noexcept override
{
    m_cancelled.store(true);
    (void)::shutdown(m_socket.Get(), SHUT_RDWR);   // wakes a blocked send
}
```

Two things every `ExportTransport` has to get right, and the example does both:

- **A `Send` that can block must be bounded.** It gets a deadline: now plus
  `TimeoutOptions::per_export`, and never later than the shutdown deadline.
  Here it becomes `SO_SNDTIMEO`, which must never be zero, because zero means
  "block forever".
- **`Cancel` must wake a `Send` that is blocked anyway.** `Provider::Shutdown`
  calls it once if its timeout expires with a `Send` in flight. A `Send` that
  ignores both would make the Provider's destructor wait forever, which is why
  `Cancel` is pure virtual.

UDP is fire and forget, so `Success` means the datagram left this host, and
`HealthSnapshot::connection_state` reads `Connected` for as long as sends
succeed, not because anything was heard back.

### Three size limits

A request passes three limits: the node's `max_request_bytes`, the
concentrator's `max_payload_bytes` (64 KiB by default) and the link's frame
size, which for UDP is 65,507 bytes. The node sets `max_request_bytes` to
60 KiB, under both, so the exporter stops joining batches before a request
would be too big for one datagram or for the concentrator. A request between
the concentrator's limit and the node's is refused as `TooLarge` there, counted
`leaf_payload_too_large`, and lost. Keep the node's cap at or below the
concentrator's. A single batch bigger than the cap is still sent whole, with a
warning. `BatchOptions::max_export_batch_size` is the knob for that case.

## A fault, traced across devices

So far every leaf payload is a trace of its own. The point of tracing a
greenhouse is to follow one decision across the devices it touches, so here
the full node plays the controller and drives the leaf:

1. The controller starts `greenhouse.control` and a child,
   `irrigation.check`, and sends the leaf one command datagram carrying that
   child's W3C `traceparent`.
2. The leaf parses the trace id and span id out of it and starts its
   `sensor.read` with `microtel_leaf_span_start_remote`, so the span is a
   child of `irrigation.check`. Its soil sensor never answers: the read takes
   250 ms, records a `sensor.timeout` event, and ends with status `ERROR`.
3. The leaf encodes and sends the payload to the concentrator as before. The
   concentrator gives it its Resource and corrects its clock. Nothing about
   the concentrator changes.

The command is one UDP datagram with no reply, no acknowledgement and no
retry. A lost command is a read that never happens, and `irrigation.check`
ends as soon as the datagram is sent, so the leaf's span outlasts its parent.
The leaf's id and timestamps still come from the concentrator, exactly as for
any other payload; only the trace id and parent span id come from the
controller.

### Run it

Start the leaf in command mode before the controller, so it is listening on
port 9311 when the first command arrives. A second, ordinary leaf on 9312 is
there to show fan-in. The concentrator exports to the collector's unbatched
receiver on 4319 (see [One request, many devices](#one-request-many-devices));
4317 works just as well for the trace.

```bash
# terminal 1
./build/examples/microtel_example_leaf_concentrator http://localhost:4319
```
```bash
# terminal 2
./build/examples/microtel_example_leaf_udp_leaf stamped 9310 9311 3 command &
./build/examples/microtel_example_leaf_udp_leaf boot 9310 9312 3 &
sleep 1
./build/examples/microtel_example_leaf_full_node 9310 9313 3 9311
```

### What they print

```
$ ./build/examples/microtel_example_leaf_full_node 9310 9313 3 9311
full node 127.0.0.1:9313 -> 127.0.0.1:9310 (OTLP over UDP)
commands -> leaf 127.0.0.1:9311 (one datagram each)
cycle 1: 2 spans, sent "irrigation.check traceparent=00-807175be716d2ef6fcc3286b303fff10-71461118183f8b6e-01"
cycle 2: 2 spans, sent "irrigation.check traceparent=00-a8c5f38fc73f15700f5304376150b34d-c2c55f18a26bd3da-01"
cycle 3: 2 spans, sent "irrigation.check traceparent=00-962434dd976e5dc3cf065c9474ee1828-8caddf3cd3e69020-01"
ForceFlush: Completed
batches_sent=1 batches_failed=0
Shutdown: Completed
```

```
$ ./build/examples/microtel_example_leaf_udp_leaf stamped 9310 9311 3 command
leaf 127.0.0.1:9311, time mode stamped, leaf library 1.2.0
waiting for commands on 127.0.0.1:9311
command 1: "irrigation.check traceparent=00-807175be716d2ef6fcc3286b303fff10-71461118183f8b6e-01"
payload 1: 446 bytes, 1 span -> 127.0.0.1:9310
command 2: "irrigation.check traceparent=00-a8c5f38fc73f15700f5304376150b34d-c2c55f18a26bd3da-01"
payload 2: 446 bytes, 1 span -> 127.0.0.1:9310
command 3: "irrigation.check traceparent=00-962434dd976e5dc3cf065c9474ee1828-8caddf3cd3e69020-01"
payload 3: 446 bytes, 1 span -> 127.0.0.1:9310
```

```
$ ./build/examples/microtel_example_leaf_concentrator http://localhost:4319
concentrator: UDP 127.0.0.1:9310 -> http://localhost:4319 (OTLP/gRPC)
config: /path/to/microtel/examples/leaf/microtel.toml
127.0.0.1:9312  490 bytes  Accepted  spans_accepted=2
127.0.0.1:9311  446 bytes  Accepted  spans_accepted=1
127.0.0.1:9312  490 bytes  Accepted  spans_accepted=2
127.0.0.1:9311  446 bytes  Accepted  spans_accepted=1
127.0.0.1:9313  681 bytes  Accepted  spans_accepted=6
127.0.0.1:9311  446 bytes  Accepted  spans_accepted=1
127.0.0.1:9312  490 bytes  Accepted  spans_accepted=2
payloads_accepted=7 payloads_rejected=0 leaves_tracked=3 time_fallbacks=0
ForceFlush: Completed
batches_sent=3 batches_failed=0
Shutdown: Completed
```

### What Tempo shows

Search for the failed reads, and each one comes back as part of a trace whose
root is the controller's:

```bash
$ curl -s -G http://localhost:3200/api/search \
    --data-urlencode 'q={ resource.service.name = "greenhouse-north" && name = "sensor.read" && status = error }' \
    --data-urlencode "start=$(( $(date +%s) - 3600 ))" --data-urlencode "end=$(date +%s)" | \
    jq -r '.traces[] | "\(.traceID)  \(.rootServiceName)  \(.rootTraceName)"'
962434dd976e5dc3cf065c9474ee1828  greenhouse-controller  greenhouse.control
a8c5f38fc73f15700f5304376150b34d  greenhouse-controller  greenhouse.control
807175be716d2ef6fcc3286b303fff10  greenhouse-controller  greenhouse.control
```

`start` and `end` make Tempo search its stored blocks as well; without them
this stack's Tempo searches only about the last 30 seconds.

One of those traces, span by span, with each span's parent and the Resource it
came with:

```bash
$ curl -s http://localhost:3200/api/traces/807175be716d2ef6fcc3286b303fff10 | jq -r '
    [.batches[] | (.resource.attributes | map({(.key): .value.stringValue}) | add) as $r
     | .scopeSpans[].spans[] | . + {svc: $r["service.name"], dev: $r["device.id"]}]
    | (map({(.spanId): .name}) | add) as $names
    | sort_by(.startTimeUnixNano | tonumber) | .[]
    | "\(.name)  parent=\($names[.parentSpanId // ""] // "-")  \(.svc)  \(.dev)  \(((.endTimeUnixNano | tonumber) - (.startTimeUnixNano | tonumber)) / 1e6 | floor) ms  \(.status.code // "")"'
greenhouse.control  parent=-  greenhouse-controller  127.0.0.1:9313  0 ms
irrigation.check  parent=greenhouse.control  greenhouse-controller  127.0.0.1:9313  0 ms
sensor.read  parent=irrigation.check  greenhouse-north  127.0.0.1:9311  250 ms  STATUS_CODE_ERROR
```

One trace, two devices, two `service.name`s, two `device.id`s, and the fault
where it happened:

```bash
$ curl -s http://localhost:3200/api/traces/807175be716d2ef6fcc3286b303fff10 | jq -c '
    .batches[].scopeSpans[].spans[] | select(.name == "sensor.read") | {status, events: [.events[] |
    {name, attributes: (.attributes | map({(.key): (.value | to_entries[0].value)}) | add)}]}'
{"status":{"message":"soil-moisture sensor did not answer","code":"STATUS_CODE_ERROR"},"events":[{"name":"sensor.timeout","attributes":{"timeout.ms":"250","attempts":"3"}}]}
```

Grafana's Tempo datasource serves the same trace, so pasting one of the ids
into Explore shows the leaf's `sensor.read` under the controller's
`irrigation.check`, marked as an error.

### How the context crosses

The controller formats its span's context with the C++ API's own propagator,
the same one an HTTP service would use for a header:

```cpp
microtel::W3CTraceContextPropagator{}.Inject(check->GetContext(),
    [&command](std::string_view header, std::string_view value)
    {
        if (header == "traceparent") { command.append(" traceparent=").append(value); }
    });
```

The leaf has no propagator, so `udp_leaf.c` parses the 55-character
`traceparent` itself: 32 hex digits of trace id and 16 of span id, which go
straight into `microtel_leaf_span_start_remote`. It ignores the sampled flag,
since the leaf has no sampler and the controller only sends commands from
spans it records. A device could just as well take the 24 raw bytes in a
binary frame; the text form is self-describing, and easy to read off a packet
capture.

## One request, many devices

The concentrator joins every device's batch from one drain into a single
export request with one `ResourceSpans` per device (design §3.6.1), but none of
its counters show that: `batches_sent` counts batches, one per device, not
requests. The collector's normal pipeline can't show it either, because its
batch processor merges requests before anything is stored or logged.

So the stack's collector has a second OTLP/gRPC receiver for this, on port
4319. Its pipeline has no batch processor and logs each request in full with a
`debug` exporter at `detailed` verbosity, then forwards it to Tempo like the
main pipeline. Examples that export to 4317 never touch it.
[`fan-in-check.sh`](fan-in-check.sh) reads that log and prints one line per
request with the devices in it. After the run above:

```
$ examples/leaf/fan-in-check.sh
request 1: 3 ResourceSpans, 15 spans
  device.id=127.0.0.1:9312  service.name=greenhouse-sensor
  device.id=127.0.0.1:9311  service.name=greenhouse-north
  device.id=127.0.0.1:9313  service.name=greenhouse-controller
fan-in: 1 of 1 requests carried more than one device (most: 3)
```

Fifteen spans from three devices in one request, although the concentrator
reported `batches_sent=3`. The two-leaf run at the top of this page, sent to
4319, does the same with its sixteen spans:

```
request 1: 2 ResourceSpans, 16 spans
  device.id=127.0.0.1:9311  service.name=greenhouse-north
  device.id=127.0.0.1:9312  service.name=greenhouse-sensor
fan-in: 1 of 1 requests carried more than one device (most: 2)
```

The script lists every request on 4319 since the stack started, exits 0 if
any of them carried more than one device and 1 if none did, and reads a saved
collector log from stdin with `-`. How many requests a run takes depends on
timing: a drain takes the spans every device has sent by then, and anything
that arrives after it waits for the next one. The conformance test
(`tests/conformance/leaf/leaf_e2e_test.cpp`) asserts the same property with
its own unbatched receiver and a file exporter.

## Exit codes

The concentrator returns `0` on success, `1` if `Build()` fails or the port
can't be bound, and `2` if `ForceFlush` did not complete, a batch failed, or the collector rejected spans via OTLP partial success.
The leaf returns `0` on success, `1` if the leaf or its socket can't be set
up, `2` for bad arguments, and `3` if an encode or a send failed or, in
command mode, if no valid command arrived within 30 seconds. The full node returns `0` on success,
`1` if one of its sockets can't be set up or `Build()` fails, and `2` if
`ForceFlush` did not complete, a batch failed, or the collector rejected spans via OTLP partial success. A command it can't send marks
that cycle's `irrigation.check` as an error and does not change the exit
code.
