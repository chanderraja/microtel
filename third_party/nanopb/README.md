# third_party/nanopb: vendored nanopb runtime (leaf only)

`nanopb` is the small, heap-free protobuf runtime the leaf's nanopb encoder
backend uses (`MICROTEL_LEAF_ENCODER=nanopb`,
[`docs/leaf-concentrator-design.md`](../../docs/leaf-concentrator-design.md)
§2). It is a dependency of the leaf artifact only
([ICP 0031](../../docs/icps/0031-leaf-concentrator-in-v1.3.md) Decision 4): it
never enters `microtel::microtel`, the concentrator, or any other shipped
archive, and `ci/scripts/symbol-scan.sh` fails any non-leaf archive that
references it.

## Pin

| Field           | Value                                      |
|-----------------|--------------------------------------------|
| Source          | `nanopb/nanopb`                            |
| Upstream tag    | `nanopb-0.4.9.2`                           |
| Upstream commit | `160d4f09e5fabb2b66aa2dea32d4f38ace2c4b3f` |
| License         | zlib (see `LICENSE.txt`)                   |
| Last refreshed  | 2026-09-25                                 |

0.4.9.2 (2026-08-24) rather than 0.4.9.1, the newest release on PyPI, because
it fixes two advisories and a bug with callback fields in submessages and
oneofs (#1061), and the leaf puts callback fields inside the `AnyValue` oneof.
The runtime fixes are all in `pb_decode.c`, which the leaf does not compile,
but #1061 also changes the generator, and the pin should not trail a security
release. The generator is therefore run from the git tag, not from PyPI.

## What's vendored

The encoder runtime only:

```
pb.h            core types and field descriptors
pb_common.h/.c  field iteration, shared by encode and decode
pb_encode.h/.c  the encoder
LICENSE.txt     zlib
```

What's not vendored, and why:

- `pb_decode.h/.c`: the leaf only encodes; every decode is on the concentrator
  side, which uses upb (design §2.1, §3.4).
- `generator/`: the code generator is a developer-time tool (ICP 0031
  Decision 4). The generated output is committed under
  [`gen/nanopb/`](../../gen/nanopb/), so building microtel never needs Python
  or the generator.
- Upstream's tests, examples, build files (CMake, Bazel, PlatformIO, Zephyr,
  Conan, SwiftPM) and docs.

Three files here are not upstream: this README, `CMakeLists.txt`, and
[`microtel_pb_rename.h`](microtel_pb_rename.h), which renames every globally
visible nanopb symbol to `microtel_pb_*`, and the generated OTLP descriptors
(`opentelemetry_proto_*_msg` and their tables) to
`microtel_pb_opentelemetry_proto_*`. It is force-included with `-include`, so
the vendored sources stay byte-identical to upstream and `gen/nanopb/` stays
byte-identical to the generator's output. Its comment carries the rationale
and the regeneration recipe.

## Refreshing the pin

1. `git -C /tmp clone --depth 1 --branch <new-tag> https://github.com/nanopb/nanopb.git`
2. Copy `pb.h`, `pb_common.c`, `pb_common.h`, `pb_encode.c`, `pb_encode.h` and
   `LICENSE.txt` from `/tmp/nanopb` over the files here. No in-place patches.
3. Refresh the pin table above, and the version in `THIRD_PARTY_NOTICES.md`.
4. Regenerate `gen/nanopb/` with the generator from the **same tag**:
   `ci/scripts/regen-protos.sh ... --nanopb-generator /tmp/nanopb/generator/protoc-gen-nanopb`
   (the script's header has the full recipe). Generated files check
   `PB_PROTO_HEADER_VERSION`, so a generator/runtime mismatch fails to compile.
   Update the tag in the `regen-check` job of `.github/workflows/ci.yml` and in
   the script's header to match.
5. Regenerate [`microtel_pb_rename.h`](microtel_pb_rename.h) with the recipe in
   its comment. `ci/scripts/symbol-scan.sh` fails if any global escapes it.
6. Build with `-DMICROTEL_BUILD_LEAF=ON` and run `nanopb_encode_test`, which
   compares an encoded `ExportTraceServiceRequest` with golden bytes.

## Build

[`CMakeLists.txt`](CMakeLists.txt) compiles `pb_common.c` and `pb_encode.c`
into the static library `microtel_nanopb` as C11, with `PB_NO_ERRMSG`, and
attaches the rename header as a PUBLIC compile option.
[`gen/nanopb/CMakeLists.txt`](../../gen/nanopb/CMakeLists.txt) builds the
generated descriptors into `microtel_nanopb_gen` on top of it. Both are added
only when the top-level project is configured with `MICROTEL_BUILD_LEAF=ON` and
`MICROTEL_LEAF_ENCODER=nanopb`, and neither reads a variable of the main
project, so the leaf's standalone build (`cmake -S leaf`) can add them too.
Neither is installed yet.
