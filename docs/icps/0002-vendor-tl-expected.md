# ICP 0002: Vendor `tl::expected` as `microtel::Expected`

**Status:** Accepted
**Affected interfaces / docs:** `CLAUDE.md`, `docs/coding-standards.md` §4.1 and §4.5, `docs/repository-layout.md` §4, `docs/error-model.md`, `docs/interfaces.md`, `docs/architecture.md`, `docs/sequences/connection-establishment.md`, every public header in `include/microtel/` that returns an `expected`-shaped value, every internal interface header in `include/microtel/internal/` that does the same.
**Affected tracks:** none directly (M0 docs and headers only); A, D, E, F all touch `microtel::Expected` once implementation begins.
**Spec / roadmap impact:** **none** — `microtel-spec.md` and `microtel-roadmap.md` are intentionally not modified. This ICP is the locus of the deviation.

## Summary

Pin C++20 as the language standard, vendor `tl::expected` v1.3.1 under
`third_party/tl-expected/`, and expose it via a wrapper header
`include/microtel/expected.hpp` as `microtel::Expected<T, E>` and
`microtel::Unexpected<E>`. Public headers, internal interfaces, and the
M0 design docs use `microtel::Expected`; CLAUDE.md and `coding-standards.md`
update to match.

## Motivation

The spec, `CLAUDE.md` rule 16, and `docs/coding-standards.md` §4.1 all say
"C++20 throughout" while listing `std::expected` as a preferred idiom.
`std::expected` is a **C++23** feature; the two statements are mutually
inconsistent. M0's public headers were drafted following the spec literally
and fail to compile under `-std=c++20`:

```
error: 'expected' in namespace 'std' does not name a template type
note: 'std::expected' is only available from C++23 onwards
```

This was caught by the M0 header-check executable
(`ci/header_check.cpp` + `MICROTEL_BUILD_HEADER_CHECK`).

Three resolutions were considered:

1. **Bump to C++23.** Resolves the inconsistency cleanly but loses the RHEL 8
   + devtoolset-11 (GCC 11) target that `microtel-spec.md` §1 commits to.
   GCC 13+, Clang 18+, MSVC 19.39+ would become the floor, cutting out a
   meaningful slice of the embedded-Linux audience the project explicitly
   targets.
2. **Stay C++20, vendor a polyfill.** A header-only `tl::expected` lets the
   public API keep its `expected`-shaped return type without bumping the
   language floor or extending the runtime closure. **Selected.**
3. **Stay C++20, replace the error pattern.** E.g., `std::variant<T, Error>`
   or a hand-rolled `Result<T, E>` shape. Diverges from idiomatic modern C++
   and from what the spec describes; bigger downstream churn.

## Decision

Adopt option 2.

- **Vendor:** `tl::expected` v1.3.1 (commit
  `1770e3559f2f6ea4a5fb4f577ad22aeb30fbd8e4`, CC0-1.0). Single header,
  vendored under [`third_party/tl-expected/`](../../third_party/tl-expected/).
- **Wrapper:** [`include/microtel/expected.hpp`](../../include/microtel/expected.hpp)
  exposes `microtel::Expected<T, E>` and `microtel::Unexpected<E>`. The
  wrapper aliases to `std::expected` / `std::unexpected` when compiled under
  C++23 with `<expected>` available, falling back to `tl::expected` /
  `tl::unexpected` otherwise.
- **Naming.** `microtel::Expected` (PascalCase per `coding-standards.md` §3),
  not lowercase `microtel::expected`. Same for `Unexpected`.
- **Compiler floor.** Unchanged: GCC 10+, Clang 13+, MSVC 19.29+ per spec
  §1. The polyfill makes those bounds viable for `std::expected`-style
  ergonomics.
- **C++23 migration path.** When microtel raises its C++ floor to C++23
  (likely v2.0+), `include/microtel/expected.hpp` keeps the wrapper but its
  C++23 branch becomes the only live one. `microtel::Expected` remains a
  type alias; user code that called `microtel::Expected` continues to work
  unchanged. The vendored `tl::expected` is removed at that point in a
  separate PR.

## Compile-time vs runtime closure

Per `microtel-spec.md` §9.1 the runtime dependency closure for v1 is fixed:
nghttp2, OpenSSL, upb, zlib, optional spdlog.

`tl::expected` is **a header.** Zero runtime symbols, no shared library, no
linker entry, no transitive include. Spec §9.1's closure is unchanged. This
is the structural reason the polyfill option avoids the ICP-12 dependency
discipline rule in CLAUDE.md.

`tl::expected` is vendored under `third_party/tl-expected/` per spec §9.6
("Vendored dependency policy"). The vendored README records:

- Upstream URL and pinned commit SHA.
- License file (CC0-1.0).
- Update procedure aligned with §9.6.
- Explicit "header-only, zero runtime symbols, no closure impact" note.

## Affected files

### Created

- [`docs/icps/0002-vendor-tl-expected.md`](0002-vendor-tl-expected.md) — this ICP.
- [`include/microtel/expected.hpp`](../../include/microtel/expected.hpp) — wrapper.
- [`third_party/tl-expected/`](../../third_party/tl-expected/):
  - [`tl/expected.hpp`](../../third_party/tl-expected/tl/expected.hpp) — vendored upstream header.
  - [`LICENSE`](../../third_party/tl-expected/LICENSE) — upstream CC0-1.0 dedication.
  - [`README.md`](../../third_party/tl-expected/README.md) — vendored-dep README per spec §9.6.

### Modified — public headers

Every header below adds `#include "microtel/expected.hpp"` and replaces
`std::expected` with `microtel::Expected`, `std::unexpected` with
`microtel::Unexpected`, and removes `#include <expected>`:

- `include/microtel/error.hpp` (Doxygen comment text only — no signature change).
- `include/microtel/provider.hpp`
- `include/microtel/sdk_builder.hpp`

### Modified — internal interface headers

- `include/microtel/internal/auth_provider.hpp`
- `include/microtel/internal/reactor.hpp`
- `include/microtel/internal/resource_detector.hpp`
- `include/microtel/internal/transport.hpp`

### Modified — design documents

- [`CLAUDE.md`](../../CLAUDE.md) — Hard-rule 16 ("Initialization returns
  `std::expected<T, Error>`") becomes `microtel::Expected<T, Error>`. Style
  cheatsheet line listing `std::expected` becomes `microtel::Expected`. A
  one-line note explains the alias.
- [`docs/coding-standards.md`](../coding-standards.md) — §4.1 modern-idiom
  list and §4.5 exceptions section both swap `std::expected` →
  `microtel::Expected`, with a one-line note on the alias.
- [`docs/repository-layout.md`](../repository-layout.md) — §4
  ("Committed vs generated") adds `third_party/tl-expected/` to the
  vendored list.
- [`docs/error-model.md`](../error-model.md) — every textual reference to
  `std::expected` in §1, §2.1, §4.1, §4.2, §8 swaps to
  `microtel::Expected`, with a one-line note in §2.1.
- [`docs/interfaces.md`](../interfaces.md) — every interface signature in §4
  using `std::expected` swaps to `microtel::Expected`.
- [`docs/architecture.md`](../architecture.md) — §1 reference to
  `std::expected` updates.
- [`docs/sequences/connection-establishment.md`](../sequences/connection-establishment.md)
  — variant section reference to `std::expected` updates.

### Not modified

- **`microtel-spec.md`** — the spec stays as-is. The ICP is the heads-up
  document that records the deviation. When v1.0 ships, the spec gets a
  one-line update calling out that "init paths return
  `microtel::Expected<T, Error>` per ICP 0002."
- **`microtel-roadmap.md`** — no roadmap impact. The C++23 adoption decision
  is captured here; the actual milestone for it lands in a future ICP if
  and when the floor moves.

## Migration

Pre-M0-close, this ICP is the migration. M0 header drafts that referenced
`std::expected` are corrected in the same PR that lands this ICP. No
existing source code is affected (none exists in M0).

After M3 (implementation), authors writing code that returns an `expected`-
shaped value use `microtel::Expected<T, E>` and `microtel::Unexpected<E>`,
including `microtel/expected.hpp`. Users embedding microtel see
`microtel::Expected` in headers; if they prefer the standard `std::expected`,
they can convert at the boundary:

```cpp
#if __cplusplus >= 202302L
auto std_result = microtel::SdkBuilder().Build()
    .transform([](auto&& p) -> std::shared_ptr<microtel::Provider> {
        return std::move(p);
    });
#endif
```

The two types share enough surface that conversion is mechanical.

## Rationale & alternatives

**Why `tl::expected` specifically.** Mature, header-only, single-author
project with stable API since v1.0 (2019). CC0-1.0 is the most permissive
license and adds no attribution overhead. The implementation tracks the
C++23 standard's API closely; switching to `std::expected` later is a
type-alias change, not an API change.

**Why not Boost.Outcome or `std::variant<T, E>` styles.** Boost.Outcome
adds a Boost-shaped dependency closure microtel actively avoids;
`std::variant<T, E>` lacks the ergonomic affordances (`value_or`, `transform`,
the unexpected-construction shorthand) that make `expected`-shaped APIs
pleasant. `tl::expected` is the smallest thing that gives the spec's
intent without changing the language floor.

**Why a wrapper, not direct `tl::expected` use.** Public-API users
shouldn't see the polyfill name in their headers. The wrapper insulates
them from the implementation choice; the C++23 migration becomes an
internal change rather than an API break.

**Why PascalCase `Expected`.** `coding-standards.md` §3 spells type names
PascalCase. Lowercase `microtel::expected` would conflict with the project's
own naming rules. The mismatch with `std::expected` (lowercase) is
acceptable — the alias is the project's own type, and the cosmetic
distinction signals to readers that they are using microtel's surface.

## Verification

The M0 header-check executable
([`ci/header_check.cpp`](../../ci/header_check.cpp)) compiles cleanly under
both `-std=c++20` (exercises `tl::expected` branch) and `-std=c++23`
(exercises `std::expected` branch) with `-Wall -Wextra -Wpedantic -Werror`.
Both compiles are part of the M0 acceptance criteria; both must remain green.
