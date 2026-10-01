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

- [`examples/leaf/`](../examples/leaf/) is a leaf and a concentrator talking
  over UDP.
- [`examples/leaf_mqtt/`](../examples/leaf_mqtt/) is the same example over
  MQTT, with coreMQTT on the device side.
