# tl::expected (vendored)

## Provenance

- **Upstream:** https://github.com/TartanLlama/expected
- **Pinned tag:** `v1.3.1`
- **Pinned commit SHA:** `1770e3559f2f6ea4a5fb4f577ad22aeb30fbd8e4`
- **Upstream license:** [CC0-1.0](LICENSE) (public domain dedication; compatible with Apache-2.0)
- **Vendored at:** 2026-05-04

## What it is

A C++11-compatible implementation of `std::expected<T, E>` with a stable API
that mirrors the C++23 standard library's `std::expected`. Header-only.

## Why microtel vendors this

microtel's language floor is C++20, which keeps the RHEL 8 + devtoolset-11
commitment in `microtel-spec.md` §1, but its public API and internal
interfaces return `expected`-shaped types. `std::expected` only arrived in
C++23. [ICP 0002](../../docs/icps/0002-vendor-tl-expected.md) covers the
decision.

`tl::expected` fills that gap. The public surface goes through
[`include/microtel/expected.hpp`](../../include/microtel/expected.hpp), which
exposes `microtel::Expected<T, E>`, `microtel::Unexpected<E>` and
`microtel::make_unexpected()`. The aliases already resolve to `std::expected` /
`std::unexpected` when a translation unit is compiled as C++23 and
`<expected>` is available, and to `tl::expected` otherwise, so raising the
floor to C++23 later needs no public-API change.

## Closure impact

None. It is a single header with no runtime symbols and no shared library.
The runtime closure pinned in `microtel-spec.md` §9.1 (nghttp2, OpenSSL, upb,
zlib, optional spdlog) is unchanged.

The header is installed alongside microtel's own, as
`<includedir>/microtel/vendor/tl/expected.hpp`, because the public
`expected.hpp` includes it. The CC0-1.0 license adds no attribution
requirement to compiled artefacts;
[`THIRD_PARTY_NOTICES.md`](../../THIRD_PARTY_NOTICES.md) lists tl::expected
for completeness.

## Update procedure

Following the vendored-dependency policy in `microtel-spec.md` §9.6:

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
5. CI runs the header compile check and the full test suite under both C++20
   and C++23. §9.6 also requires the collector interop and fuzz suites before
   merge; those are the weekly `interop.yml` and `fuzz.yml` workflows, so
   trigger both manually on the PR branch.
6. Merge after reviewer sign-off.

## Files

- [`tl/expected.hpp`](tl/expected.hpp) — single-header implementation.
- [`LICENSE`](LICENSE) — upstream `COPYING` (CC0-1.0 dedication).
