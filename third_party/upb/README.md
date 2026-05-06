# third_party/upb — vendored upb runtime

`upb` is the small, allocation-light protobuf runtime that microtel uses to
encode OTLP messages on the wire. Per spec §9.5 we vendor a pinned subset
rather than depending on an installed protobuf-cpp runtime: it keeps the
runtime dependency closure to {nghttp2, OpenSSL, zlib} + this directory.

## Pin

| Field           | Value                                      |
|-----------------|--------------------------------------------|
| Source          | `protocolbuffers/protobuf` (upb/ subtree)  |
| Upstream tag    | `v29.4`                                    |
| Upstream commit | `1be1c9d0ea6efa2a25bd7b76186844d1669be78a` |
| License         | BSD-3-Clause (see `LICENSE`)               |
| Last refreshed  | 2026-05-06                                 |

`utf8_range` (a sibling under `third_party/utf8_range/`) is a hard
dependency of `upb/wire/decode.c` and is vendored from the same protobuf
release at the same commit. It carries a separate MIT license — see
`third_party/utf8_range/LICENSE`.

## What's vendored

The minimum runtime needed to compile generated accessors and encode/decode
protobuf messages:

```
upb/base/         — fundamental types, status, string_view
upb/mem/          — arena allocator
upb/port/         — portability macros, def.inc / undef.inc preludes
upb/hash/         — internal hash tables
upb/mini_table/   — compact runtime descriptors
upb/mini_descriptor/ — minimal descriptor format
upb/message/      — message accessors (get/set fields)
upb/wire/         — wire encode + decode
upb/generated_code_support.h — umbrella include for generated `.upb.h`
```

What's **not** vendored (and why):

- `upb/io/`, `upb/json/`, `upb/text/`, `upb/util/` — JSON / text encoding,
  reflection-driven I/O. v1 only emits binary protobuf, no text or JSON.
- `upb/reflection/` — full descriptor-driven reflection. Mini-tables are
  enough for generated code paths.
- `upb/lex/` — `.proto` lexer (used by reflection). Same reasoning.
- `upb/conformance/` — conformance test harness, upstream-only.
- `upb/test/`, `*_test.{c,cc}`, `*_test.*` — upstream test sources.
- `upb/bazel/`, `BUILD*` files — the bazel build is upstream-only.
- `*.hpp` C++ wrappers — optional convenience headers we don't consume.

## Refreshing the pin

The pin is refreshed by re-running the rsync that produced this tree;
no in-place patches. To bump:

1. `git -C /tmp clone --depth 1 --branch <new-tag> https://github.com/protocolbuffers/protobuf.git`
2. `rm -rf third_party/upb/upb`
3. For each subdir in `{base, mem, port, hash, mini_table, mini_descriptor, message, wire}`:
   `rsync -a --include='*/' --include='*.c' --include='*.h' --exclude='*_test.*' --exclude='BUILD*' --exclude='*' /tmp/protobuf/upb/<sub>/ third_party/upb/upb/<sub>/`
4. Also copy `upb/port/def.inc`, `upb/port/undef.inc`, and
   `upb/generated_code_support.h` (the rsync filter above skips `*.inc`).
5. Refresh `LICENSE` and the pin table in this README.
6. Refresh `third_party/utf8_range/{utf8_range.h,utf8_range.c,LICENSE}` from
   the same upstream commit.
7. Regenerate the upb accessors under `gen/` (see M3-F2 docs).
8. Run the full test suite — the wire round-trip tests will catch any
   incompatible field-number or descriptor-format changes immediately.

A bump that touches the wire encode/decode contract goes through the ICP
process (`docs/icps/`) since it changes a load-bearing dependency for
every byte microtel emits.

## No CMake yet

This commit only vendors source. The CMake target that compiles upb into
a `microtel_upb` static library lands with M3-F2 alongside the generated
accessors that consume it.
