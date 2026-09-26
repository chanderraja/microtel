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

microtel_example_leaf_udp_leaf [stamped|sync|boot] [concentrator-port] [source-port] [payloads]

  defaults: stamped 9310 9311 5
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
processor collects them regardless of which leaf they came from. Sixteen spans
from two devices left in two batches, because the leaves took longer than the
Provider's five-second schedule delay to send everything.

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

## Exit codes

The concentrator returns `0` on success, `1` if `Build()` fails or the port
can't be bound, and `2` if `ForceFlush` did not complete. The leaf returns `0`
on success, `1` if the leaf or its socket can't be set up, `2` for bad
arguments, and `3` if an encode or a send failed.
