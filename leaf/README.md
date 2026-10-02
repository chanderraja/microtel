# `leaf/` — microtel-leaf

A C11 library that builds spans in caller-owned memory and encodes them as an
OTLP `ExportTraceServiceRequest`, for devices too small for the C++ runtime.
The application sends the bytes to a concentrator (`LeafReceiver`); the leaf
starts no thread, does no I/O and never allocates. **Experimental in v1.2.**

This page is for firmware authors using the leaf. Working on the leaf itself
(its sources, tests, golden vectors, fuzzing and CI jobs) is covered in
[`DEVELOPMENT.md`](DEVELOPMENT.md).

Design: [`docs/leaf-concentrator-design.md`](../docs/leaf-concentrator-design.md)
§1 (API) and §2 (backends).

**The leaf and concentrator are experimental in v1.2:** traces only, off by
default, and the C API may change in any 1.x minor. The roadmap stabilises the
API in v2.0 and adds reference ports for STM32 HAL, Zephyr and FreeRTOS in
v2.1.

## How the leaf and concentrator fit together

<p align="center">
  <img alt="Devices running microtel-leaf send OTLP payload bytes over their own link (UART, CAN, BLE, UDP, MQTT) to a gateway, where LeafReceiver::Ingest feeds a microtel Provider that adds a per-device Resource, corrects clocks, and batches and exports over OTLP gRPC or HTTP to a collector or backend." src="../docs/images/leaf-concentrator.svg" width="860">
</p>

- **The leaf** builds spans in memory the caller owns and encodes them as a
  standard OTLP `ExportTraceServiceRequest`. It starts no thread, does no I/O
  and never allocates: your firmware sends the bytes over whatever link it
  already has.
- **The concentrator** is an ordinary microtel `Provider` built with
  `-DMICROTEL_WITH_CONCENTRATOR=ON`. Its `LeafReceiver` takes the bytes you
  read from that link, validates them, gives each device its own Resource
  (`device.id` is the transport id you pass in, which the payload can't
  override), converts device clocks to Unix time, and sends spans from every
  device through the normal sampling, batching and retry pipeline, many
  devices per export request.
- microtel opens no socket facing the devices: that link is whatever they
  already speak. The gateway's only connection is its outgoing OTLP export.

On the device, in C:

```c
static microtel_leaf_t g_leaf;        /* 256 bytes of opaque state */
static uint8_t g_records[256];        /* spans live here until encoded */

microtel_leaf_init(&g_leaf, sizeof g_leaf, &config, g_records, sizeof g_records);

microtel_leaf_span_t span;
microtel_leaf_span_start(&g_leaf, &span, "sensor.read", 11, MICROTEL_LEAF_SPAN_KIND_CLIENT, NULL);
microtel_leaf_span_set_attribute(&g_leaf, span, &reading);
microtel_leaf_span_end(&g_leaf, span);

microtel_leaf_encode(&g_leaf, frame, sizeof frame, &written);  /* or stream with encode_to */
uart_send(frame, written);                                     /* your link, not ours */
```

On the gateway, in C++:

```cpp
auto provider = *microtel::SdkBuilder{}
                     .FromFile("microtel.toml")   // [concentrator] enabled = true
                     .WithEndpoint("http://collector:4317")
                     .WithProtocol(microtel::Protocol::Grpc)
                     .Build();
const auto receiver = provider->GetLeafReceiver();

// For each frame read from the link:
const microtel::IngestResult result = receiver->Ingest({
    .leaf_id = "can0:0x1a4",                        // becomes device.id
    .payload = frame_bytes,
    .received_at = std::chrono::system_clock::now(),
});
```

Per-device settings live in the concentrator's config: a `service.name` per
device, Resource keys every device should carry, an allow-list of known
devices, and each device's time mode. A leaf's clock usually doesn't know the
time of day, so it can send timestamps stamped relative to the moment of
encoding, relative to the last clock sync, or relative to boot, and the
concentrator converts them. A Linux board next to the sensors can also run the
full C++ SDK and export through `SdkBuilder::WithExportTransport` onto the
same link and concentrator. The concentrator's settings are in
[`docs/configuration.md`](../docs/configuration.md) §3.14; the design is in
[`docs/leaf-concentrator-design.md`](../docs/leaf-concentrator-design.md) §3
(ingest), §4 (per-leaf identity and configuration) and §5 (time modes).

Both backends produce byte-identical payloads, which CI checks against golden
vectors and a differential fuzzer. The leaf's tests run bare-metal on Cortex-M0+
and Cortex-M4 under QEMU, and under aarch64 and 32-bit x86 Linux.

## The header

`include/microtel/leaf.h` is the only public header; valid C11 and C++. Every
macro in it starts with `MICROTEL_LEAF_`, and every symbol the library
exports with `microtel_leaf_`.

## Building

Standalone, with only a C compiler (nanopb by default):

```bash
cmake -S leaf -B build-leaf [-DMICROTEL_LEAF_ENCODER=upb]
cmake --build build-leaf
```

The standalone build needs just a C11 cross-compiler; for a Cortex-M target,
add `-DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/arm-none-eabi.cmake`.

In-tree, as part of the main project (exported as `microtel::leaf`, never a
dependency of `microtel::microtel`):

```bash
cmake -S . -B build -DMICROTEL_BUILD_LEAF=ON [-DMICROTEL_LEAF_ENCODER=upb]
```

### Without CMake

Compile as C11, with include paths `leaf/include` and `leaf/src`, and
`leaf/src/leaf_core.c` with
`-DMICROTEL_LEAF_BACKEND_ENCODE=microtel_leaf_internal_encode_<backend>`.

nanopb: `leaf/src/backend_nanopb.c`, `third_party/nanopb/pb_common.c`,
`third_party/nanopb/pb_encode.c` and the four `.pb.c` files under
`gen/nanopb/`, each with `-DPB_NO_ERRMSG` and
`-include third_party/nanopb/microtel_pb_rename.h`; include paths
`third_party/nanopb` and `gen/nanopb`.

upb: `leaf/src/backend_upb.c` with
`-include third_party/upb/microtel_upb_rename.h`, and the sources of
`third_party/utf8_range/`, `third_party/upb/` and the trace protos under
`gen/` listed in their `CMakeLists.txt`, each with the same `-include`;
include paths `gen`, `third_party/upb` and `third_party/utf8_range`.

## Dependencies and memory

libc (`memcpy`, `memmove`, `memset`, `memcmp`) and the project's vendored,
renamed encoder: nanopb (`microtel_pb_*`) or upb (`microtel_upb_*`). The
nanopb leaf never uses the heap; the upb backend uses it only when
`config.scratch` is NULL.

The caller owns all memory; every init has a free. Records in the caller's
buffer are read and written with `memcpy`, so the buffer needs no alignment.

## Choosing a backend

Both backends produce the same bytes for the same spans (§2.3). The nanopb
backend allocates nothing and streams: `microtel_leaf_encode_to` hands each
piece to `write` as it is encoded. The upb backend builds the payload in an
arena and calls `write` once.

The probe (`examples/leaf/size_probe.c`: one span, one attribute, streamed),
v1.2, bytes:

| | nanopb (default) | upb |
|---|---|---|
| Flash, Cortex-M0+ / Cortex-M4 / aarch64 | 9,472 / 9,298 / 16,366 | 14,632 ¹ / 14,392 / 23,966 |
| Leaf static RAM (`.data` + `.bss`), Cortex-M / aarch64 | 0 / 808 | 65 / 769 |
| Caller RAM | 512: `microtel_leaf_t` 256 + record buffer 256 | 2,560: the same + 2 KiB encode scratch (or the heap) |
| Worst-case stack, encode (static), Cortex-M4 / aarch64 | 3,392 / 6,864 | 2,944 / 5,296 |
| Heap | never | only with no `config.scratch` |
| `encode_to` | streams each field as it is encoded; no payload buffer | builds the payload in the arena, one `write` |
| Choose it for | microcontrollers: the smallest flash and no heap | Linux-class devices, or a firmware that already links upb |

¹ ARMv6-M (Cortex-M0 / M0+) needs an `__atomic_compare_exchange_4`, which
newlib lacks, for upb's arena; see the footprint results.

## Footprint

[`docs/bench-results/leaf-footprint.md`](../docs/bench-results/leaf-footprint.md)
has the release figures: `.text`, `.rodata`, `.data` and `.bss` and the
worst-case stack of every public entry point, for both backends on every
target. To measure on your own toolchain, see
[`DEVELOPMENT.md`](DEVELOPMENT.md#footprint).

## Examples

- [`examples/leaf/`](../examples/leaf/) runs devices and a concentrator over
  UDP into the bundled collector, Tempo and Grafana stack.
- [`examples/leaf_mqtt/`](../examples/leaf_mqtt/) does the same over MQTT,
  with coreMQTT on the device side.
