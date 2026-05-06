# third_party/utf8_range — vendored UTF-8 validator

`utf8_range` is a small SIMD-friendly UTF-8 validator. `upb`'s wire decoder
calls into it to validate `string` fields per the protobuf spec. We vendor
it alongside `upb` because the two pin together — both come out of the
`protocolbuffers/protobuf` release tarball.

## Pin

| Field           | Value                                      |
|-----------------|--------------------------------------------|
| Source          | `protocolbuffers/protobuf` (third_party/utf8_range/) |
| Upstream tag    | `v29.4`                                    |
| Upstream commit | `1be1c9d0ea6efa2a25bd7b76186844d1669be78a` |
| License         | MIT (see `LICENSE`)                        |
| Last refreshed  | 2026-05-06                                 |

## What's vendored

Just the C entry points upb consumes:

- `utf8_range.h` — declares `utf8_range_IsValid` and `utf8_range_ValidPrefix`.
- `utf8_range.c` — portable + SSE4 implementation. Selects via
  `__SSE4_1__` / `__ARM_NEON` at compile time.

What's **not** vendored:

- `utf8_validity.{h,cc}` — C++ wrapper. We call the C API directly.
- `lemire-*`, `range*`, `naive.c`, `lookup.c` — alternative implementations
  + benchmark variants. `utf8_range.c` is the production one.
- `*_test.cc`, `fuzz/`, demo files.

## Refreshing the pin

See `third_party/upb/README.md`. The two pins are bumped together.
