# ICP 0020: `install()` rules, the exported target set, and what `microtel::` means

**Status:** Accepted — signed off 2026-08-31. Decision only; implementation is
M9/M10 work. The decision shapes what M10 must carry rather than being M10.
**Affected interfaces / docs:** the **exported CMake target names become public
API** — that is the substance of this ICP. Also `ci/scripts/symbol-scan.sh`
(scope changes from build tree to install tree); `README.md` /
`docs/development.md` (consumer instructions).
**Affected tracks:** packaging / release (M9–M10).

## Summary

microtel has **no `install()` rules at all**. `cmake --install` produces
nothing, so `find_package(microtel)` is impossible and the only ways to consume
the library are vendoring the source or hand-writing a find module (issue #19).

This ICP decides **what gets exported and under what names**, because those
names are a compatibility surface that cannot change after v1.0. It does not
decide when to implement — that is M9/M10 work — but the decision shapes what
M10 has to carry, and install rules block packaging, which blocks vcpkg and
Conan, which is the actual distribution path.

## What exists to export

Twelve shipped static libraries, with this dependency graph:

```
microtel_sdk ──┬─ microtel_config
               ├─ microtel_exporter
               ├─ microtel_transport ── microtel_common
               ├─ microtel_http_wire ─┐
               ├─ microtel_grpc_wire ─┴─ microtel_otlp_response
               └─ microtel_encoder ───── microtel_upb_gen

microtel_upb_runtime ── microtel_utf8_range        (vendored)
microtel_headers (INTERFACE, carries include/)     (linked by all of the above)
```

`microtel_preflight_lib` is tool-only and is **not** exported.

## Decision 1 — export both the components and an aggregate

Export every component with its real `target_link_libraries` graph, and add
`microtel::microtel` as an `INTERFACE` target over the public set. CMake then
derives link order from the graph, so the ordering sensitivity of a
twelve-archive static closure stops being the consumer's problem — which it
would very much be if we shipped a bare list.

```cmake
find_package(microtel REQUIRED)
target_link_libraries(my_app PRIVATE microtel::microtel)
```

**Correction to a motivating argument.** The case for component granularity is
often "a consumer who wants traces and no metrics gets a smaller closure". That
consumer **cannot be served by this decomposition**, and the ICP should not
imply otherwise: `microtel_sdk` contains the metrics and logs implementations
alongside traces (8 metric sources and 3 log sources in one archive). The
libraries are split by **layer** — sdk / transport / wire / encoder — not by
**signal**. Component targets therefore buy layer-level reuse (linking the
encoder without the SDK, say), not signal-level subsetting.

Signal subsetting would require splitting `microtel_sdk`, which is a much
larger change and is not proposed here. Recording the limitation because the
smaller-closure argument is the project's own pitch, and it does not currently
hold at this seam.

## Decision 2 — one target is supported API

The rest are **exported because the static link closure requires them, and
unsupported**: they may merge, split, or disappear without an ICP.

| Target | Status |
|---|---|
| `microtel::microtel` | **Public — the only supported name.** |
| `microtel::sdk`, `::transport`, `::encoder`, `::exporter`, `::config`, `::common`, `::http_wire`, `::grpc_wire`, `::otlp_response`, `::upb_gen`, `::upb_runtime`, `::utf8_range` | Exported, **not supported**. Present so the link closure resolves. |

**`microtel::sdk` was demoted at review**, and the reason is this section's own
principle: no component exposes public headers directly, so a consumer never
needs to name one. `microtel::sdk` would be a promise with no use case the
aggregate does not already serve, and the counter-argument — that naming a
component shows which layer you depend on — is answered by the aggregate
resolving to the same closure anyway. **Promotion is available later if someone
asks; demotion is not.**

The line has a principled basis rather than being a judgement call: **no
static component library exposes public headers directly.** Every one of them
gets `include/` through the `microtel_headers` INTERFACE target, and
`include/microtel/*.hpp` is the public API while `include/microtel/internal/`
and everything in `src/` is not. A consumer never needs to name a component to
get a public header — so components are link-graph plumbing, and the aggregate
is the only thing to promise.

"Exported but not supported" is a coherent and common position, but only if it
is **written down**. Left ambiguous, every exported name becomes a de-facto
commitment the first time someone depends on it.

## Decision 3 — the adapters are not installed as binaries

Two INTERFACE targets sit outside the twelve static libraries and need an
explicit position, because both reference `include/` directly and would
otherwise fall through the gaps of Decision 2:

- **`microtel_otelcpp_shim`** — **must not be installed as a binary, ever.**
  ICP 0014 makes it source-only for a hard reason: the shim's ABI depends on
  configuration choices the *consumer* makes when building opentelemetry-cpp
  (`OPENTELEMETRY_STL_VERSION`, `ABI_VERSION_NO`), so the same source compiled
  under two configurations produces link-incompatible symbols. A prebuilt
  artifact would be wrong for most consumers and silently so. Install its
  **headers** if anything; never an archive, and never an exported target that
  implies one.
- **`microtel_spdlog_bridge`** — INTERFACE and header-only. Installing the
  headers is harmless and useful; it adds nothing to the link closure.

**Decided: install adapter headers; export no adapter targets.** A consumer
opts in by adding the shim to their own build, exactly as today.

The concern that shipping headers invites the prebuilt-binary expectation was
considered and rejected: **that expectation comes from an exported target, not
from headers in an include directory**, and no adapter target is exported.
Withholding the headers would mean the shim can only be consumed by pointing at
a source tree — worse ergonomics for what is meant to be the project's main
adoption path.

Instead, `src/adapters/otelcpp/`'s public header carries a comment stating why
there is no binary. A one-paragraph explanation where someone is already looking
is a cheaper answer than absence, which explains nothing.

## Decision 4 — prefix the vendored upb symbols, before v1.0

Exporting `microtel::upb_runtime` means shipping a static library, under our
namespace, containing upstream protobuf's code. Two consequences:

1. **Duplicate symbols with different provenance.** A consumer who also links
   real upb gets two definitions of `upb_*`, with no diagnostic beyond a link
   error or — worse, with static libraries — silent selection of one.
2. **The namespace asserts ownership of code we did not write.** `microtel::`
   in front of vendored protobuf code is at best misleading.

Three options:

- **(a) Prefix the vendored symbols** (`microtel_upb_*`) so collision is
  impossible by construction.
- **(b) Document the hazard and do nothing.**
- **(c) Offer `MICROTEL_USE_SYSTEM_UPB`** so consumers can opt out of the
  vendored copy.

**Decided: (a), and in scope for v1.0.** The cost asymmetry settles it —
prefixing before v1.0 is a build-system change; after, it is an ABI break for
every consumer who linked the exported target. And the duplicate-symbol failure
mode with static libraries — silent selection of one definition, no diagnostic
— is undiagnosable in the field, which is the worst category of defect to ship
knowingly.

**The rule-13 scan update ships in the same change, not separately.**
`ci/scripts/symbol-scan.sh` explicitly permits `upb_*` and `utf8_range_*` as
vendored-closure members; prefixed symbols would fail that allowance. Landing
the prefix without the scan update breaks CI, and landing the allowance first
would silently widen what rule 13 permits. They are one change.

## Decision 5 — `symbol-scan` must follow the install tree

`ci/scripts/symbol-scan.sh` currently scans `libmicrotel_*.a` under the **build
directory**. Once `cmake --install` exists, "shipped" means the **install
tree**, and the gate must scan that instead — otherwise the dependency-closure
claim that the whole project rests on (rule 13) is verified against artifacts
that are not the ones users receive.

This is the closing half of a linkage already noted: the scan asserts what
ships, and until now nothing else defined "ships".

## Decision 6 — a consumer test, or the export set breaks silently

**Export-set breakage is invisible to every in-tree test.** Everything builds
because everything is a subdirectory; a missing `install(TARGETS …)`, a
forgotten transitive dependency, or an unexported vendored archive shows up
only in a consumer that is not part of this build.

Required: a separate CMake project under `tests/consumer/` that
`find_package(microtel REQUIRED)`s, links `microtel::microtel`, compiles a
program against the public headers, and runs in CI.

**It must run against a genuinely installed tree, in its own build
directory** — `cmake --install` to a staging prefix, then configure the
consumer with `CMAKE_PREFIX_PATH` pointing at *that*. Not at the build tree.
The weaker version is easier to write, looks identical in CI, and misses
precisely what this test exists to catch: a missing `install(FILES …)` for
headers, or an export set referencing a target it never installed. Both are
invisible when `find_package` resolves against a build directory that still has
every source file in place.

Without this, the export set is asserted rather than tested — the pattern issue
#134 catalogues.

## Migration

- No source or public-header change; this is packaging.
- `README.md` and `docs/development.md` gain the `find_package` usage.
- The bench harness's in-tree `MICROTEL_BUILD_BENCH=ON` workaround (issue #19)
  can stay; it is not blocked on this and there is no reason to churn it.

## Resolved at review

1. **Aggregate only.** `microtel::sdk` demoted — see Decision 2. Fewer
   promises; promotion is available later, demotion is not.
2. **(a) symbol prefixing, in scope for v1.0**, with the rule-13 scan update as
   part of the same change — see Decision 4.
3. **No `microtel::detail::` prefix.** Two reasons. It makes the names ugly for
   plumbing that CMake resolves automatically and nobody types. More
   importantly, a `detail::` marker on some exported targets invites the
   reading that anything *not* so marked is supported — reintroducing the
   ambiguity somewhere new. Decision 2's principle, written down in the
   package's own documentation, is sufficient; a consumer who links
   `microtel::grpc_wire` anyway did it deliberately.
4. **Install the shim's headers** — see Decision 3.

## Open questions

None outstanding.
