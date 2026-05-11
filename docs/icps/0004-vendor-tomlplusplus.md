# ICP 0004: Vendor toml++ for microtel.toml parsing (M6-A)

**Status:** Accepted
**Affected interfaces / docs:** `src/common/config/`, `docs/configuration.md` (to be written in M6-A)
**Affected tracks:** E (Configuration)
**Spec / roadmap impact:** Closes the TOML-parser selection deferred from M2 chunk 5 in `src/common/config/README.md`.

## Summary

M6 requires parsing `microtel.toml`. This ICP selects **toml++** (a.k.a.
`tomlplusplus`) as the TOML parser and defines how it is integrated into
the build.

## Decision

**Selected library:** [toml++](https://github.com/marzer/tomlplusplus)
— version **v3.4.0**, fetched via CMake `FetchContent` at configure time
(pinned to the `v3.4.0` git tag).

**Integration pattern:** FetchContent (same as spdlog). The library is
not committed to `third_party/` because the amalgamated header is ~15 000
lines; FetchContent is consistent with the existing spdlog pattern and
equally reproducible via the pinned tag.

**CMake target:** `tomlplusplus::tomlplusplus` (INTERFACE, header-only).
Linked `PRIVATE` into `microtel_config`; not re-exported to consumers.

## Rationale

Two candidates were evaluated:

| | **toml++** | cpptoml |
|---|---|---|
| Maintenance | Active (last release 2023) | Abandoned (2020) |
| C++ standard | C++17 / C++20 native | C++11 |
| Header-only | Yes (single `toml.hpp`) | Yes |
| `std::optional` API | Yes | Partial |
| Exception mode | Yes (catchable `toml::parse_error`) | Yes |
| License | MIT | MIT |

toml++ wins on maintenance and API quality. cpptoml is abandoned and its
API requires more boilerplate.

## Dependency closure impact

**Build-time only.** toml++ is included in `microtel_config` which is
linked `PRIVATE` into `microtel_sdk`. The header is not exposed via any
`PUBLIC` include path, so downstream consumers of the installed microtel
library do not need toml++ on their include path.

**Runtime closure unchanged.** toml++ is a header-only library; it adds
zero shared-library symbols to the installed `libmicrotel.so`. The
runtime closure in `microtel-spec.md` §9.1 (nghttp2, OpenSSL, upb, zlib,
optional spdlog) is unaffected.

## Unknown-key strictness

toml++ does not implement strict-key enforcement natively. `microtel_config`
implements it manually by iterating each parsed table's keys against a
whitelist. Violations are converted to `ConfigError::Kind::UnknownKey`
(or logged as warnings / silently ignored, per `[config] unknown_keys`).

## Update procedure

1. Update the `GIT_TAG` in the root `CMakeLists.txt` FetchContent block.
2. Run the full test suite; fix any API breaks (toml++ semver is stable
   within major versions).
3. Update this ICP's version reference.
4. Open a PR with the changelog excerpt and a note confirming the
   `microtel_config` test suite is green.

## Sign-off

| Reviewer | Date | Status |
|---|---|---|
| Chander Raja | 2026-05-11 | Accepted |
