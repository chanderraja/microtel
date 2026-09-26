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
| `src/backend_upb.c` | the upb backend — the only leaf file that includes upb headers |
| `.clang-tidy` | the C static-analysis profile (§7.8) |

The nanopb backend (`src/backend_nanopb.c`) is not written yet. Until it is,
`MICROTEL_LEAF_ENCODER=nanopb` (the default) is a configure error in the
standalone build; in the main project it builds the vendored nanopb archives
but not `microtel_leaf`, with a configure warning. It never falls back to upb.

## Building

Standalone, with only a C compiler:

```bash
cmake -S leaf -B build-leaf -DMICROTEL_LEAF_ENCODER=upb
cmake --build build-leaf
```

In-tree, as part of the main project (exported as `microtel::leaf`, never a
dependency of `microtel::microtel`):

```bash
cmake -S . -B build -DMICROTEL_BUILD_LEAF=ON -DMICROTEL_LEAF_ENCODER=upb
```

### Without CMake

Compile as C11 with the rename header forced into every upb translation unit,
and include paths `leaf/include`, `leaf/src`, `gen`, `third_party/upb` and
`third_party/utf8_range`:

- `leaf/src/leaf_core.c` with `-DMICROTEL_LEAF_BACKEND_ENCODE=microtel_leaf_internal_encode_upb`
- `leaf/src/backend_upb.c` with `-include third_party/upb/microtel_upb_rename.h`
- the sources of `third_party/utf8_range/`, `third_party/upb/` and the trace
  protos under `gen/` listed in their `CMakeLists.txt`, each with the same
  `-include`

## Dependencies

libc (`memcpy`, `memmove`, `memset`, `memcmp`) and, for the upb backend, the
project's vendored and renamed upb (`microtel_upb_*`). The upb backend uses the
heap only when `config.scratch` is NULL.

## Tests

`tests/unit/leaf/` (`microtel_leaf_upb_test`): the C API through the public
header, every payload decoded with upb, plus the golden vectors in
`tests/leaf/vectors/`, which a second backend must reproduce byte for byte.
Regenerate the vectors after an intended wire change with
`MICROTEL_LEAF_WRITE_VECTORS=1 build/tests/unit/leaf/microtel_leaf_upb_test`.
`ci/scripts/symbol-scan.sh` checks the archive: no C++ runtime symbols, and
every global starts with `microtel_leaf_`.

## Style

C11, `-pedantic-errors`, no VLAs, no compiler extensions. Every external
symbol starts with `microtel_leaf_` (internal cross-file ones with
`microtel_leaf_internal_`), every macro in the public header with
`MICROTEL_LEAF_`. The caller owns all memory; every init has a free. Records in
the caller's buffer are read and written with `memcpy`, so the buffer needs no
alignment. Formatting is the project's `.clang-format`.
