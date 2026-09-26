# `leaf/` — microtel-leaf

A C11 library that builds spans in caller-owned memory and encodes them as an
OTLP `ExportTraceServiceRequest`, for devices too small for the C++ runtime.
The application sends the bytes to a concentrator (`LeafReceiver`); the leaf
starts no thread, does no I/O and never allocates. **Experimental in v1.2.**

Design: [`docs/leaf-concentrator-design.md`](../docs/leaf-concentrator-design.md)
§1 (API) and §2 (backends).

## Files

| File | What it is |
|---|---|
| `include/microtel/leaf.h` | the only public header; valid C11 and C++ |
| `src/leaf_core.c` | config, record buffer, span building, ids, clocks, and the batch view backends read |
| `src/leaf_internal.h` | the core / backend contract (§2.2); not installed |
| `src/backend_nanopb.c` | the nanopb backend (the default) — the only leaf file that includes nanopb headers |
| `src/backend_upb.c` | the upb backend — the only leaf file that includes upb headers |
| `.clang-tidy` | the C static-analysis profile (§7.8) |

Both backends produce the same bytes for the same spans (§2.3). The nanopb
backend allocates nothing and streams: `microtel_leaf_encode_to` hands each
piece to `write` as it is encoded. The upb backend builds the payload in an
arena and calls `write` once.

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

With tests or fuzz harnesses on, the in-tree build also compiles test-only
archives that are never installed: the leaf with the other backend
(`microtel_leaf_upb` or `microtel_leaf_nanopb`) and `microtel_leaf_dual`,
which links both behind a run-time switch (`tests/leaf/dual/`).

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

## Dependencies

libc (`memcpy`, `memmove`, `memset`, `memcmp`) and the project's vendored,
renamed encoder: nanopb (`microtel_pb_*`) or upb (`microtel_upb_*`). The
nanopb leaf never uses the heap; the upb backend uses it only when
`config.scratch` is NULL.

## Tests

`tests/unit/leaf/`: `microtel_leaf_upb_test` and `microtel_leaf_nanopb_test`
build from the same sources, one per backend: the C API through the public
header, every payload decoded with upb, plus the golden vectors in
`tests/leaf/vectors/`, which both must reproduce byte for byte.
`microtel_leaf_backend_diff_test` links `microtel_leaf_dual` and compares the
two backends' bytes on the golden vectors and on 2,000 random builder
programs; `tests/fuzz/leaf_backend_diff_fuzz` does the same on fuzzed ones.
Regenerate the vectors after an intended wire change with
`MICROTEL_LEAF_WRITE_VECTORS=1 build/tests/unit/leaf/microtel_leaf_upb_test`.
`ci/scripts/symbol-scan.sh` checks the archives: no C++ runtime symbols,
every global starts with `microtel_leaf_`, and a nanopb leaf references no
heap allocator.

`tests/leaf/target/` runs the leaf away from the x86-64 host
(`ci/scripts/leaf-target.sh`, the `leaf-target` CI job): a C runner with no
test framework and no heap checks the golden vectors and a subset of the API
tests, with every buffer also at odd byte offsets, on bare-metal Cortex-M0+
and Cortex-M4 under `qemu-system-arm`; the gtest suite and the runner also run
under `qemu-aarch64` and as a 32-bit i686 process. The runner also measures
each entry point's stack.

## Footprint and example

`ci/scripts/leaf-footprint.sh <cortex-m0plus|cortex-m4|aarch64> [nanopb|upb]`
cross-builds this directory with the toolchain files in `cmake/toolchains/`,
links `examples/leaf/size_probe.c`, and reports the leaf's `.text`, `.rodata`,
`.data` and `.bss` and the worst-case stack of every public entry point
(`ci/scripts/leaf-stack.py`, from GCC's `-fcallgraph-info=su` call graph); the
`leaf-footprint` CI job runs it for both backends on every target on every PR,
and [`docs/bench-results/leaf-footprint.md`](../docs/bench-results/leaf-footprint.md)
has the release figures.

### Choosing a backend

Both backends produce the same bytes. The probe (one span, one attribute,
streamed), v1.2, bytes:

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
newlib lacks, for upb's arena; see the footprint results. [`examples/leaf/`](../examples/leaf/) is a leaf and a
concentrator talking over UDP; `tests/conformance/leaf/` runs the same path
against a real collector with each backend.

## Style

C11, `-pedantic-errors`, no VLAs, no compiler extensions. Every external
symbol starts with `microtel_leaf_` (internal cross-file ones with
`microtel_leaf_internal_`), every macro in the public header with
`MICROTEL_LEAF_`. The caller owns all memory; every init has a free. Records in
the caller's buffer are read and written with `memcpy`, so the buffer needs no
alignment. Formatting is the project's `.clang-format`.
