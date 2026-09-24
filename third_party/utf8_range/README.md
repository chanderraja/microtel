# third_party/utf8_range: vendored UTF-8 validator

`utf8_range` is a small SIMD-friendly UTF-8 validator. `upb`'s wire decoder
calls into it to validate `string` fields, as the protobuf spec requires. We
vendor it alongside `upb` because the two are pinned together: both come out
of the `protocolbuffers/protobuf` release.

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
- `utf8_range.c` — portable and SSE4.1 implementations, selected at compile
  time on `__SSE4_1__`. Other targets, ARM64 included, get the portable path.

What's not vendored:

- `utf8_validity.{h,cc}`, the C++ wrapper. We call the C API directly.
- `lemire-*`, `range*`, `naive.c`, `lookup.c`: alternative implementations
  and benchmark variants. `utf8_range.c` is the production one.
- `*_test.cc`, `fuzz/`, demo files.

## Build

[`CMakeLists.txt`](CMakeLists.txt), which is ours and not upstream, builds the
static library `microtel_utf8_range`. It force-includes
[`third_party/upb/microtel_upb_rename.h`](../upb/microtel_upb_rename.h), so the
shipped symbols are `microtel_utf8_range_*` (ICP 0020 Decision 4). The archive
is installed so a static link resolves, but the header is never installed.

## Refreshing the pin

See [`third_party/upb/README.md`](../upb/README.md). The two pins are bumped
together.
