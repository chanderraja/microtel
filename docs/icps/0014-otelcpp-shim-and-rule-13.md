# ICP 0014: opentelemetry-cpp API-adapter shim, and rule 13 restated as a testable claim

**Status:** Draft
**Affected interfaces / docs:** [`CLAUDE.md`](../../CLAUDE.md) rules 12–13;
`microtel-spec.md` §6.3 and §13 (new milestone); `microtel-roadmap.md` §3 (Tier 3
row). No changes to `include/microtel/` or `docs/interfaces.md` — the shim is
additive and consumes the public API as-is.
**Affected tracks:** new optional package `adapters/otelcpp/`; no existing track
changes.

## Summary

Build `microtel_otelcpp_shim` — an optional, **source-only** adapter implementing
the `opentelemetry-cpp` API on top of microtel's SDK for all three signals — and
restate rule 13 as a description of what
[`ci/scripts/symbol-scan.sh`](../../ci/scripts/symbol-scan.sh) asserts rather
than as an unqualified slogan.

## Motivation

**The compatibility claim is already published and currently false.**
`microtel-roadmap.md` §3 lists Tier 3 (API-adapter) as "experimental: traces"
from **v1.0**, and spec §2.2 says the same. No shim code exists — `grep -r
"opentelemetry::" src/ include/` returns nothing. This is the same
spec-versus-reality drift ICP 0013 addresses for M8, except that this one is a
*published compatibility promise*, which is worse than an unused milestone slot.

**It is the adoption path.** microtel's pitch is doing opentelemetry-cpp's job
without its dependency closure. The target user already has opentelemetry-cpp
call sites and already carries the gRPC/protobuf/abseil tree. The shim is what
makes the pitch actionable without a rewrite.

**It de-risks M10.** `docs/migration-from-otel-cpp.md` is an M10 deliverable that
does not exist, as are `compatibility-matrix.md` and `interop-matrix.md` (both
named in §13.5's release gates). A migration path you have not executed cannot be
credibly documented; building the shim is what makes that guide testable.

**It has no SDK prerequisites.** Verified: the opentelemetry-cpp API ships its
own thread-local context storage (`ThreadLocalContextStorage`, backed by a
function-local `static thread_local`, in
`api/include/opentelemetry/context/runtime_context.h`), which backs
`RuntimeContext::GetCurrent/Attach/Detach` and `trace::Scope`. The shim reads
`RuntimeContext::GetCurrent()` and passes the resulting `SpanContext` into
microtel's explicit `StartSpanOptions::parent`. microtel's own missing
thread-local current-span slot blocks the Python bindings (ICP 0013) but **not**
this shim.

**Timing.** The opentelemetry-cpp API spans traces, metrics, and logs. All three
exist in microtel today (M12/M13/M14), so a shim written now covers the whole
surface; one written earlier would have needed revisiting twice.

## The constraint that shapes the design

Verified against opentelemetry-cpp `v1.19.0` and re-checked at `v1.28.0`:

1. **`WITH_ABSEIL=OFF` does not mean "no abseil."** `WITH_STL` also defaults to
   `OFF`, and in that configuration the API includes a **vendored abseil snapshot
   bundled inside its own headers** (`nostd/internal/absl/`, 17 files). Measured:
   30 vendored-absl headers enter the include graph for the header set a shim
   needs. Still true at v1.28.0; the `WITH_ABSEIL` option has since been removed
   from the root `CMakeLists.txt`, leaving `WITH_STL` as the only lever.
2. **`WITH_STL=CXX20` removes abseil entirely** — 0 absl headers, 0 absl symbols,
   no third-party headers beyond the system stdlib. `nostd::` aliases straight to
   `std::`, which matches microtel's own API (`std::variant`, `std::span`).
3. **The API is header-only** (`opentelemetry_api` is a CMake `INTERFACE` target).
   protobuf and libcurl belong to the SDK and exporters, which the shim never uses.
4. **ABI is coupled to the consumer's configuration.** The API wraps everything in
   `inline namespace v<ABI_VERSION_NO>` and `nostd::` types appear in every
   signature, so the same source compiled two ways yields link-incompatible
   symbols — the default configuration carries `absl::otel_v1::variant` *in the
   ABI*. The real matrix is roughly `ABI_VERSION_NO × STL_VERSION × WITH_GSL`,
   closer to a dozen viable combinations than to two.

(4) is decisive: **the shim cannot ship as a prebuilt binary** that works for all
consumers, because its ABI is a function of choices the consumer made.

## Proposed change

### 1. Distribution model: source-only, permanently

`microtel_otelcpp_shim` ships as source. The consumer compiles it inside their
own build against whatever opentelemetry-cpp configuration they already have. No
prebuilt shim binaries, for any configuration — a *stronger* constraint than
pinning one supported config, not a weaker one.

This is the standard shape for C++ interop layers (pybind11, nanobind) and is
what opentelemetry-cpp itself does: their API is header-only for exactly this
reason. Structurally, microtel core keeps a stable ABI of its own (`std::` types
only, no otel-cpp types in any signature), and the ABI matrix is not solved but
**relocated** — into the consumer's build, where exactly one configuration is in
play and it resolves automatically.

Rejected alternative — pinning `WITH_STL=CXX20` and shipping binaries — is in
"Rationale" below.

### 2. Packaging

- New optional target, **off by default**: `MICROTEL_BUILD_OTELCPP_SHIM=OFF`. A
  user who does not opt in never fetches opentelemetry-cpp headers.
- Lives in `adapters/otelcpp/`, with its own `README.md` per the
  directory-ownership convention.
- The opentelemetry-cpp API headers are a **build-time dependency of this package
  only**. Rule 12's runtime closure (nghttp2, OpenSSL, upb, zlib, optional
  spdlog) is unchanged — the shim adds nothing to it.

### 3. Rule 13, restated as what the CI job asserts

Replace the current text in `CLAUDE.md`:

> 13. **No gRPC library, no abseil, no protobuf-cpp runtime. Ever.** The wire
>     encoder is upb. The HTTP/2 transport is nghttp2 directly.

with:

> 13. **No shipped microtel artifact links or contains symbols from gRPC,
>     abseil, or the protobuf C++ runtime.** This is enforced mechanically by
>     [`ci/scripts/symbol-scan.sh`](ci/scripts/symbol-scan.sh) (CI job
>     `symbol-scan`, a required check), which scans every `libmicrotel_*.a` and
>     the `microtel-preflight` binary and fails on any symbol — **defined or
>     undefined** — whose demangled name begins with `absl::`, `absl_`, `grpc::`,
>     `grpc_`, `GRPC_`, or `google::protobuf::`. Undefined references count:
>     an archive carrying `U absl::…` makes abseil a link requirement for every
>     consumer. Vendored upb and utf8_range are members of the closure — their
>     `upb_*`, `utf8_range_*`, and upb-generated `google_protobuf_*` C accessors
>     are explicitly **not** violations. The wire encoder is upb; the HTTP/2
>     transport is nghttp2 directly.
>
>     Optional **source-distributed** adapters may compile against third-party
>     headers already present in the consumer's build, provided they add nothing
>     to the consumer's link closure. See ICP 0014.

The wording is deliberately a description of the job's assertion rather than an
independent claim that happens to be tested. If the rule and the job ever drift,
the job is what people will trust, so the rule is written to track it.

### 4. Closure claims exclude the shim

README, spec §3, and the M7 footprint numbers state the dependency closure for
**microtel core**. The shim is named as an optional adapter and is excluded from
those figures.

### 5. Milestone

Add **M17 – opentelemetry-cpp API-adapter shim** to spec §13, and amend ICP 0013
(still a draft) to renumber Python bindings from M17 to **M18**, so the numbers
track the intended execution order. Scope: traces, metrics, and logs;
experimental tier; `docs/migration-from-otel-cpp.md` written against the working
shim.

## Migration

- Contributors and agents: rule 13 is unchanged in substance for core. What
  changes is that it now has a precise, testable statement and an explicit
  carve-out for source-distributed adapters.
- Nothing to rebuild. `MICROTEL_BUILD_OTELCPP_SHIM` defaults off, so existing
  builds are unaffected.
- Consumers of the shim must build it from source. This is documented, not a
  temporary limitation.

## Rationale & alternatives

- **Pin `WITH_STL=CXX20`, ship prebuilt binaries** — rejected. It defeats the
  shim's only purpose. Adopters would have to rebuild opentelemetry-cpp with an
  ABI-breaking flag, which (a) invalidates every prebuilt otel-cpp artifact they
  hold — distro package, vcpkg/Conan cache, build-farm output — and (b) breaks
  binary instrumentation libraries from opentelemetry-cpp-contrib compiled
  against the default config. It replaces "edit no call sites" with "rebuild your
  telemetry stack," imposed on exactly the users the shim exists to attract.
  *Explicitly not a reason to reject it:* "consumers may predate C++20."
  Measured, `WITH_STL=CXX20` compiles at C++17 and fails only at C++14, and
  microtel's public headers already require C++20 — so no population is excluded
  that microtel does not already exclude. That argument would expire silently if
  a C++17 compatibility mode is ever added; the two grounds above do not.
- **A C-ABI boundary at the shim** — rejected. It would permit a prebuilt shim,
  but costs a hand-maintained C surface and type erasure at every call, and buys
  nothing that source distribution does not already provide.
- **Keep rule 13's "Ever" unqualified and forbid the shim** — rejected. On a
  default-configuration consumer, adding the shim is a literal no-op with respect
  to abseil: the vendored absl is inline header code already being compiled into
  that consumer's translation units at every site that includes an otel header,
  and the shim contributes no new link inputs, no new symbols, no new transitive
  anything. Rule 13's subject matter — what microtel drags into a build — is not
  engaged. The honest response to a rule whose letter overshoots its intent is to
  amend the rule through this process, not to obey the letter or quietly
  reinterpret it.
- **Defer the shim until after Python (ICP 0013)** — rejected. The shim has no
  SDK prerequisites while Python has one (the thread-local current-span slot), the
  compatibility claim is already published, and Tier 3's v1.4 promotion gate
  (≥10 real applications, ≥5 external migration validators) needs wall-clock soak
  time that only starts once the shim exists.

## Open question for the reviewer

Whether `symbol-scan` should additionally be run over a **consumer-side smoke
build** that links the shim, asserting the shim adds nothing to the link closure
beyond what the consumer's own otel-cpp already required. That would test §3's
carve-out rather than merely asserting it, but needs a fixture consumer project
and is not proposed here.
