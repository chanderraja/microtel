# tl::expected (vendored)

## Provenance

- **Upstream:** https://github.com/TartanLlama/expected
- **Pinned tag:** `v1.3.1`
- **Pinned commit SHA:** `1770e3559f2f6ea4a5fb4f577ad22aeb30fbd8e4`
- **Upstream license:** [CC0-1.0](LICENSE) (public domain dedication; compatible with Apache-2.0)
- **Vendored at:** 2026-05-04

## What is it

A C++11-compatible implementation of `std::expected<T, E>` with a stable API
that mirrors the C++23 standard library's `std::expected`. Header-only.

## Why microtel vendors this

Per [ICP 0002](../../docs/icps/0002-vendor-tl-expected.md), microtel pins its
language standard at C++20 to preserve the RHEL 8 + devtoolset-11 commitment
in `microtel-spec.md` §1, but uses `expected`-shaped return types in the
public API and internal interfaces. C++20 does not include `std::expected`;
that ships in C++23.

`tl::expected` fills the gap until microtel adopts C++23. Public surface area
is wrapped in [`include/microtel/expected.hpp`](../../include/microtel/expected.hpp),
which exposes `microtel::Expected<T, E>` and `microtel::Unexpected<E>` as
aliases. When the project's compiler floor moves to C++23, the wrapper aliases
flip over to `std::expected` / `std::unexpected` with no public-API change.

## Closure impact

**None.** This is a single header, zero runtime symbols, zero new shared
library. The runtime closure pinned in `microtel-spec.md` §9.1 (nghttp2,
OpenSSL, upb, zlib, optional spdlog) is unchanged.

The CC0-1.0 license adds no new attribution requirement to compiled artefacts.
Source-distribution `THIRD_PARTY_NOTICES.md` lists tl::expected for
completeness.

## Update procedure

Per `microtel-spec.md` §9.6:

1. Pick the new upstream tag.
2. Update the pin in this README (tag, commit SHA, vendored-at date).
3. Replace `tl/expected.hpp` and `LICENSE` from the new commit.
4. Open a PR. The PR description includes:
   - Upstream changelog excerpt covering the version range.
   - API/ABI diff notes (any breaking changes? expected to be none for
     `tl::expected`'s minor versions).
   - Confirmation that the wrapper in
     [`include/microtel/expected.hpp`](../../include/microtel/expected.hpp)
     still compiles with both branches (`-std=c++20` exercises the
     `tl::expected` branch; `-std=c++23` exercises the `std::expected`
     branch).
5. CI runs the M0 header-check, plus (post-M3) the full test suite, including
   collector interop and fuzz.
6. Merge after reviewer sign-off.

## Files

- [`tl/expected.hpp`](tl/expected.hpp) — single-header implementation.
- [`LICENSE`](LICENSE) — upstream `COPYING` (CC0-1.0 dedication).
