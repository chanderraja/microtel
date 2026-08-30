# ICP 0020: `install()` rules, the exported target set, and what `microtel::` means

**Status:** Draft
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

## Decision 2 — only two targets are supported API

The rest are **exported because the static link closure requires them, and
unsupported**: they may merge, split, or disappear without an ICP.

| Target | Status |
|---|---|
| `microtel::microtel` | **Public.** The supported entry point. |
| `microtel::sdk` | **Public.** For consumers who want the SDK explicitly. |
| `microtel::transport`, `::encoder`, `::exporter`, `::config`, `::common`, `::http_wire`, `::grpc_wire`, `::otlp_response`, `::upb_gen`, `::upb_runtime`, `::utf8_range` | Exported, **not supported**. Present so the link closure resolves. |

The line has a principled basis rather than being a judgement call: **no
static component library exposes public headers directly.** Every one of them
gets `include/` through the `microtel_headers` INTERFACE target, and
`include/microtel/*.hpp` is the public API while `include/microtel/internal/`
and everything in `src/` is not. A consumer never needs to name a component to
get a public header — so components are link-graph plumbing, and only the
aggregate and the SDK are things to promise.

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

Proposed: install adapter headers, export **no** adapter targets. A consumer
opts in by adding the shim to their own build, exactly as today.

## Decision 4 — the vendored upb needs an answer, and cheaper now than later

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

**Proposed: (a) is the intended eventual answer, (b) is acceptable for v1.0 if
(a) does not fit the schedule.** This is recorded now specifically because
**symbol prefixing is dramatically cheaper before v1.0 than after** — afterwards
it is an ABI break for anyone who linked the exported target. Note this
interacts with rule 13's symbol scan, which explicitly permits `upb_*` and
`utf8_range_*` as vendored-closure members; prefixing would require updating
that allowance.

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
`find_package(microtel REQUIRED)`s from a real install tree, links
`microtel::microtel`, compiles a program against the public headers, and runs
in CI. Without it the export set is asserted, not tested — the pattern issue
#134 catalogues.

## Migration

- No source or public-header change; this is packaging.
- `README.md` and `docs/development.md` gain the `find_package` usage.
- The bench harness's in-tree `MICROTEL_BUILD_BENCH=ON` workaround (issue #19)
  can stay; it is not blocked on this and there is no reason to churn it.

## Open questions for the reviewer

1. **Is `microtel::sdk` worth promising alongside the aggregate**, or should the
   aggregate be the only supported name? Fewer promises are easier to keep; the
   counter-argument is that the aggregate hides which layer a consumer actually
   depends on.
2. **(a), (b) or (c) for the vendored upb** — and if (a), is it in scope for
   v1.0? The recommendation above is (a)-eventually / (b)-if-necessary, but the
   cost asymmetry means this is worth settling deliberately rather than by
   default.
3. Should the exported-but-unsupported targets carry a name that makes their
   status obvious — a `microtel::detail::` prefix, say — rather than relying on
   documentation nobody reads?
4. Decision 3 proposes installing adapter *headers* while exporting no adapter
   *targets*. Is that the right split, or should the otel-cpp shim's headers
   stay out of the install tree entirely so that nothing about it looks
   installable? The shim is opt-in source today and there is an argument that
   shipping its headers invites exactly the prebuilt-binary expectation ICP
   0014 exists to prevent.
