# Developing microtel-leaf

For contributors working on `leaf/` itself: its sources, the backend contract,
tests, and the CI jobs that check it. Using the leaf in firmware is covered in
[`README.md`](README.md). The project-wide rules are in
[`CONTRIBUTING.md`](../CONTRIBUTING.md).

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
| `nanopb/otlp_trace.options` | nanopb generator options for the trace descriptors (§2.4): every string, bytes and repeated field is a callback, so no generated struct has a fixed-size array and nothing needs `PB_ENABLE_MALLOC`. Read by `ci/scripts/regen-protos.sh`; the output is committed under `gen/nanopb/` |
| `.clang-tidy` | the C static-analysis profile (§7.8) |

Both backends produce the same bytes for the same spans (§2.3). The nanopb
backend allocates nothing and streams: `microtel_leaf_encode_to` hands each
piece to `write` as it is encoded. The upb backend builds the payload in an
arena and calls `write` once.

## Test-only builds

With tests or fuzz harnesses on, the in-tree build also compiles test-only
archives that are never installed: the leaf with the other backend
(`microtel_leaf_upb` or `microtel_leaf_nanopb`) and `microtel_leaf_dual`,
which links both behind a run-time switch (`tests/leaf/dual/`).

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

`tests/conformance/leaf/` runs the [`examples/leaf/`](../examples/leaf/) path
against a real collector with each backend.

## Footprint

`ci/scripts/leaf-footprint.sh <cortex-m0plus|cortex-m4|aarch64> [nanopb|upb]`
cross-builds this directory with the toolchain files in `cmake/toolchains/`,
links `examples/leaf/size_probe.c`, and reports the leaf's `.text`, `.rodata`,
`.data` and `.bss` and the worst-case stack of every public entry point
(`ci/scripts/leaf-stack.py`, from GCC's `-fcallgraph-info=su` call graph); the
`leaf-footprint` CI job runs it for both backends on every target on every PR,
and [`docs/bench-results/leaf-footprint.md`](../docs/bench-results/leaf-footprint.md)
has the release figures.

## Style

C11, `-pedantic-errors`, no VLAs, no compiler extensions. Every external
symbol starts with `microtel_leaf_` (internal cross-file ones with
`microtel_leaf_internal_`), every macro in the public header with
`MICROTEL_LEAF_`. The caller owns all memory; every init has a free. Records in
the caller's buffer are read and written with `memcpy`, so the buffer needs no
alignment. Formatting is the project's `.clang-format`.
