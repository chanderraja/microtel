# ICP 0028: the sugar layer's public surface — `microtel::sugar`

**Status:** Accepted — decided 2026-09-15. Docs only; the headers land in the
v1.1 sugar packet, which is sequenced after [ICP 0025](0025-propagation-core.md)'s
packet 2.3b.
**Affected interfaces / docs:** four new public headers —
`include/microtel/sugar.hpp` and
`include/microtel/sugar/{span,exception,attr_key}.hpp` — one INTERFACE target in
[`CMakeLists.txt`](../../CMakeLists.txt), and the header lists in
[`ci/header_check.cpp`](../../ci/header_check.cpp),
[`tests/unit/headers_smoke_test.cpp`](../../tests/unit/headers_smoke_test.cpp)
and [`tests/consumer/main.cpp`](../../tests/consumer/main.cpp). **No existing
header changes** — `attribute.hpp`, `span.hpp` and `tracer.hpp` are untouched.
No `docs/interfaces.md` entry (sugar declares no interface, it consumes public
API), no new install rule, no CI job, no runtime dependency, no wire-format
change.
**Affected tracks:** Track A — Trace SDK, as owner of the public trace API.
Sugar adds no file under `src/`.

## Summary

`microtel::sugar` ships in v1.1 as a **header-only** layer over the public API:
`MICROTEL_TRACE_FUNCTION`, a scoped span with inline attributes, `Traced`,
`RecordException`, and `AttrKey`. It calls nothing but `include/microtel/*.hpp`,
binds its RAII to ICP 0025's `ScopedSpan`, and adds nothing to any consumer's
link closure.

## Motivation

[ICP 0024](0024-v1.1-rescope.md)'s ships-when gate opens with *"The sugar layer
has a stable API, recorded in ICP 0028 and shipped as installed headers."* This
is that ICP.

**The prerequisite just landed.** Every helper below is a scoped span, and until
ICP 0025 there was no scope to hang one on: `Tracer::StartAsCurrentSpan`
returned a bare `SpanHandle` and installed nothing
(`include/microtel/tracer.hpp`, the placeholder comment *"To be added in v1.1
once the `Context` machinery is fully fleshed out"*). ICP 0025 §3 turns it into
a `ScopedSpan` that installs the span as current and restores the caller's
context in reverse declaration order, and makes it **move-constructible "so a
factory can return one"** — which is exactly what this layer is: factories that
return one.

**Sugar is the release's ergonomic half and the one with no runtime cost.**
Nothing here is a new capability; every helper compiles to the two or three
public calls the user would have written. That is the reason it can be
header-only, and the reason spec §18.1 excludes it from conformance testing.

## Proposed change

### 0. Namespace, and the `mt` alias nobody declares

The namespace is **`microtel::sugar`**. `microtel-spec.md` §8's deferral table
and §18.1, and [`docs/architecture.md`](../architecture.md) §6, all name it that
way; `microtel-roadmap.md` §5 writes the helpers as `mt::Span`, `mt::Traced`,
`mt::AttrKey`. **The spec wording wins.** `mt` is a *consumer-side* alias:

```cpp
namespace mt = microtel::sugar;   // in the consumer's code, never in ours
```

No microtel header declares it. A library that squats a two-character global
name has taken something it cannot give back, and the alias costs the consumer
one line in whatever file wants it. `docs/` examples spell the alias once at the
top and use `mt::` after, so roadmap §5's spelling stays readable.

### 1. The surface

Three headers, plus an umbrella `microtel/sugar.hpp` that includes them. Every
signature below is grounded in a header as it exists today, or as ICP 0025
locks it.

**`include/microtel/sugar/span.hpp`**

```cpp
namespace microtel::sugar
{

/// @brief Start a span named for the enclosing function and make it current.
///
/// The name is `loc.function_name()` — static storage, so passing it as a
/// `std::string_view` to `StartAsCurrentSpan` (which copies on the sampled
/// path) is safe.
[[nodiscard]] inline ::microtel::ScopedSpan TraceFunction(
    ::microtel::Tracer& tracer,
    ::microtel::SpanKind kind = ::microtel::SpanKind::Internal,
    std::source_location loc = std::source_location::current()) noexcept;

/// @brief RAII scoped span with inline attributes.
///
/// `attributes` is viewed, not owned: the `std::initializer_list` backing
/// array lives to the end of the enclosing full-expression, and
/// `StartAsCurrentSpan` copies the attributes into the span record inside this
/// call. The returned scope holds no reference to them.
[[nodiscard]] inline ::microtel::ScopedSpan Span(
    ::microtel::Tracer& tracer,
    std::string_view name,
    std::initializer_list<::microtel::KeyValue> attributes = {},
    ::microtel::SpanKind kind = ::microtel::SpanKind::Internal) noexcept;

/// @brief Run `fn` inside a scoped span; returns whatever `fn` returns.
///
/// If `fn` throws, the span is ended by `ScopedSpan`'s destructor during
/// unwinding and the exception propagates **unrecorded** — recording it is
/// `mt::TryCatch`, roadmap §5 v1.4.
template <std::invocable Fn>
inline decltype(auto) Traced(::microtel::Tracer& tracer,
                             std::string_view name,
                             Fn&& fn) noexcept(std::is_nothrow_invocable_v<Fn&&>);

}  // namespace microtel::sugar

#define MICROTEL_TRACE_FUNCTION(tracer) /* see below */
```

The macro declares the variable the function cannot:

```cpp
#define MICROTEL_SUGAR_CAT_(a, b) a##b
#define MICROTEL_SUGAR_CAT(a, b) MICROTEL_SUGAR_CAT_(a, b)

/// Declares a scoped span for the enclosing function. One per scope.
#define MICROTEL_TRACE_FUNCTION(tracer)                                     \
    const ::microtel::ScopedSpan MICROTEL_SUGAR_CAT(microtel_fn_scope_,     \
                                                   __LINE__) =             \
        ::microtel::sugar::TraceFunction(tracer)
```

Copy-initialisation from the prvalue is guaranteed elision in C++20, so the
deleted move-assignment and the absent copy constructor are not in play; `const`
is deliberate — a scope that cannot be moved out of cannot be destroyed out of
order, which is the one programming error ICP 0025 §3 documents and does not
check.

**`MICROTEL_TRACE_FUNCTION` takes the tracer.** Roadmap §5 writes it with no
argument; that shape needs a process-global tracer, and microtel has none —
`Provider::GetTracer` (`include/microtel/provider.hpp`) is the only way to
obtain one and there is no `SetGlobalProvider` anywhere in `include/`. Adding
one for sugar's benefit would put a mutable global with static-destruction
order problems underneath a convenience macro. See Discrepancies.

**`include/microtel/sugar/exception.hpp`**

```cpp
/// @brief Set `Error` status and add the OTel `exception` event.
///
/// `exception.type` is `typeid(e).name()` — the implementation-mangled name,
/// **not** demangled; `exception.message` is `e.what()`. The status
/// description is `e.what()`.
///
/// @noexcept The status is set first and cannot fail. If building the two
///           event attributes throws `std::bad_alloc`, the event is dropped
///           and the call returns.
inline void RecordException(::microtel::Span& span, const std::exception& e) noexcept;

/// @brief Overload for a `catch` site with no `std::exception` in hand, or
///        one that has a better type name than `typeid` can give.
inline void RecordException(::microtel::Span& span,
                            std::string_view type,
                            std::string_view message) noexcept;
```

Both are two calls on the public `Span`: `SetStatus(StatusCode::Error, …)` and
`AddEvent("exception", …)` (`include/microtel/span.hpp`). `exception.stacktrace`
and `exception.escaped` are omitted — microtel captures no stack traces, and
whether an exception escaped the span's scope is not knowable from inside the
helper. The name is left mangled deliberately: demangling needs
`abi::__cxa_demangle`, which `malloc`s on an error path and is ABI-specific, and
the mangled form is stable and greppable — a collector-side processor, or the
second overload, gives a pretty name where one is wanted. That overload is also
the answer for a consumer compiling with `-fno-rtti`, since `typeid` is what the
first one costs.

**`include/microtel/sugar/attr_key.hpp`** — see §2.

### 2. `AttrKey` is a call-site binder, and nothing else

```cpp
class AttrKey
{
public:
    /// @param key borrowed — must outlive this object. A string literal is the
    ///            intended argument: `constexpr AttrKey kHttpMethod{"http.method"};`
    constexpr explicit AttrKey(std::string_view key) noexcept;

    [[nodiscard]] constexpr std::string_view Key() const noexcept;

    /// @brief `span.SetAttribute(Key(), std::move(value))`.
    void Set(::microtel::Span& span, ::microtel::AttributeValue value) const noexcept;

    /// @brief Build a `KeyValue` for the `AttributeSpan` paths. Allocates iff
    ///        the key exceeds `std::string`'s small-buffer capacity.
    [[nodiscard]] ::microtel::KeyValue operator()(::microtel::AttributeValue value) const;

private:
    std::string_view m_key;
};
```

Rule of zero — no user-declared destructor, all five special members implicit.
A `constexpr` `AttrKey` at namespace scope is constant-initialised, so there is
no static-initialisation-order question and no allocation anywhere in the type.

**What it buys:** one place where a key name is spelled, a distinct type so a
key cannot be passed where a value is expected, and a `string_view` whose length
is fixed at compile time — no `strlen` at each of N call sites where the
compiler cannot see the literal.

**What it does not buy, and will not in v1.1:** pre-encoded wire bytes.
`AttrKey` does **not** carry a protobuf-encoded key for the encoder to splice
in. That optimisation is rejected here for two independent reasons:

1. **It is an ABI change on `attribute.hpp`.** The encoder reaches attributes
   through `KeyValue` / `AttributeValue` (`include/microtel/attribute.hpp`);
   carrying pre-encoded bytes means a new member or a new `variant` alternative
   on types that are copied by value throughout the public API — a second ABI
   event on a header this ICP otherwise does not touch, for a benefit nobody
   has measured.
2. **It would put wire encoding in a public header.** CLAUDE.md rule 13 confines
   upb to `src/wire/encoder/`; a pre-encoded key is OTLP field framing, and the
   sugar header is the worst possible place for it.

The idea is not dead, it is scheduled: `microtel-roadmap.md` §4 v1.5 lists
*"Compile-time attribute key encoding (for callers using `mt::AttrKey`)"* under
the optimised hot path, where it can be priced against the M7 benchmark numbers.
Shipping `AttrKey` now as a plain binder is what makes that later change
source-compatible for callers — they already hold the key in a type we own.

`AttrKey` also does not remove the `std::string` copy in `KeyValue`: that key is
owned by declaration (`attribute.hpp`), and changing it is the same ABI event.

### 3. Layering: public API only

**Sugar calls only `include/microtel/*.hpp`.** It includes nothing from `src/`,
nothing from `include/microtel/internal/`, and in particular it never touches
`internal::SpanDeleter` — whose own Doxygen says its shape "may be revised in
v1.x based on benchmark findings" and that `SpanHandle` is the stable surface
([ICP 0003](0003-m0-deferred-decisions.md)). Sugar's RAII is
`microtel::ScopedSpan` (ICP 0025 §3) and `microtel::SpanHandle`; sugar defines
**no RAII type of its own**, which is why nothing here can get the
end-then-restore ordering wrong — that ordering lives in `ScopedSpan`'s member
declaration order and is tested there.

Consequence, checked mechanically: the sugar headers must compile standalone,
exactly as an outside consumer sees them. The packet adds all four to
[`ci/header_check.cpp`](../../ci/header_check.cpp) (no gtest, `-Werror`) and to
[`tests/unit/headers_smoke_test.cpp`](../../tests/unit/headers_smoke_test.cpp)
(with gtest, to catch name collisions), and adds `#include <microtel/sugar.hpp>`
plus one call to [`tests/consumer/main.cpp`](../../tests/consumer/main.cpp) —
the ICP 0020 Decision 6 smoke test that builds against an **installed** tree.
That last one is the only test that can prove the header-only claim, because it
links `microtel::microtel` and nothing else.

**One spelling rule for the headers themselves.** `microtel::sugar::Span` is a
*function*, so inside `namespace microtel::sugar` the name `Span` no longer
finds `microtel::Span`. Sugar headers therefore spell the core types
fully-qualified — `::microtel::Span`, `::microtel::ScopedSpan` — as the
signatures above do. Consumers are unaffected: they write `mt::Span(...)` for
the factory and `microtel::Span` for the class.

### 4. Packaging

Header-only, following the shape of the spdlog bridge
([`src/adapters/spdlog/CMakeLists.txt`](../../src/adapters/spdlog/CMakeLists.txt)) —
an `INTERFACE` library, include directories, `cxx_std_20`, no sources:

```cmake
add_library(microtel_sugar INTERFACE)
target_link_libraries(microtel_sugar INTERFACE microtel_headers)
target_compile_features(microtel_sugar INTERFACE cxx_std_20)
```

Three deviations from that precedent, each with a reason:

- **It lives in the root `CMakeLists.txt`**, beside `microtel_headers`, not in a
  `src/` subdirectory. The bridge needs a directory because
  `MICROTEL_USE_SPDLOG` gates its `add_subdirectory`; sugar is gated by nothing
  and owns no source file, and ICP 0020 already centralises the target surface
  in the root file.
- **It is not optional.** Sugar is public API in v1.1, not an opt-in adapter.
- **It is not added to `MICROTEL_EXPORTED_TARGETS`.** ICP 0020 Decision 2 draws
  the supported line at `microtel::microtel` on the principle that *no component
  exposes public headers directly* — `microtel_headers` puts `include/` on the
  aggregate's interface, so `find_package(microtel)` +
  `microtel::microtel` + `#include <microtel/sugar.hpp>` already works. An
  exported `microtel::sugar` would be a name with no use case, and Decision 2's
  own rule is that promotion is available later and demotion is not. In-tree,
  the target is what sugar's tests and examples name.

**No install rule is added.** `install(DIRECTORY include/microtel …)` is
recursive, so `include/microtel/sugar/` ships with everything else — the same
mechanism ICP 0025 §2 relies on for `baggage.hpp` (ICP 0020 Decision 2).

## Restated exclusions

- **Conformance.** `microtel-spec.md` §18.1: *"Sugar APIs are explicitly
  non-goals for compatibility testing — conformance tests target the OTel-like
  API and wire output, not convenience wrappers."* Nothing under
  `tests/conformance/` gains a sugar case. Sugar gets ordinary unit tests
  (`tests/unit/sugar/`, over `tests/fakes/fake_tracer.hpp` and
  `tests/fakes/fake_span.hpp`) plus the three compile gates in §3.
  This exclusion is also what lets `RecordException` set `Error` status, which
  opentelemetry-cpp's similarly-named `Span::RecordException` does not do:
  roadmap §5 v1.1 specifies both halves, and sugar is not measured against the
  OTel API surface.
- **`mt::Timer` stays deferred to v1.2.** It records to a histogram, and
  roadmap §5 v1.2 plus [`docs/metrics-design.md`](../metrics-design.md) §8 both
  carry it as the v1.1-deferred piece. v1.1 ships no timer, not even a
  duration-only placeholder.
- **Python equivalents stay deferred to M18.** Roadmap §5 v1.1 and spec §13's
  M16 row both list `@mt.trace_function`, `mt.span`, `mt.traced` and
  `mt.record_exception` alongside the C++ helpers, but
  [ICP 0013](0013-rescope-defer-python-bindings.md) moved **all** Python
  bindings to M18, which follows M16. v1.1's sugar is C++-only. Spec §13.4's
  "no orphan C++ surfaces" rule gives that one release of grace and makes the
  Python sugar M18's obligation, not a v1.1 gap.

## Discrepancies found while writing this

1. **`MICROTEL_TRACE_FUNCTION()` as written in roadmap §5 cannot be built.**
   It takes no tracer and microtel has no global provider. Recorded rather than
   silently rewritten: the macro ships as `MICROTEL_TRACE_FUNCTION(tracer)`, and
   a global-provider accessor — if it is ever wanted — is its own ICP, argued on
   its own merits and not smuggled in under a convenience macro.
2. **Roadmap §5 spells the namespace `mt::`; spec §8 and §18.1 spell it
   `microtel::sugar`.** Resolved in §0 above: the spec wins, `mt` is a consumer
   alias. The roadmap text is illustrative and needs no amendment — it is how
   the code reads *after* the alias.
3. **Spec §13's M16 row promises Python sugar in a milestone that precedes
   M18.** As above; the C++/Python split is a scope statement worth correcting
   in the pass that closes v1.1, not here.
4. **Neither header-compile gate includes `context.hpp` or `logger.hpp`.**
   Pre-existing; ICP 0025's packet 2.3b closes the `context.hpp` half of it for
   `ci/header_check.cpp`. The sugar packet adds its own four headers to both
   lists and nothing else — widening the gates is someone's issue, not a
   side-effect of this one.

## Migration

Nothing to do today; this ICP locks a surface. The sugar packet carries the four
headers, the CMake target, the three compile-gate edits, and `tests/unit/sugar/`.
It **must land after ICP 0025's packet 2.3b** — `ScopedSpan` and the
`StartAsCurrentSpan` return type are the layer's whole foundation.

**Consumers:** nothing breaks. Sugar is additive, header-only, and adds no
symbol to any archive; a v1.0 program that never includes it is bit-identical.
Callers hold `std::shared_ptr<Tracer>` from `Provider::GetTracer`, so the call
sites read `mt::Span(*tracer, "checkout", {{"cart.id", "c-42"}})` — the helpers
take `Tracer&` because a borrowed non-owning reference is what rule 7 asks for
and what keeps sugar out of the ownership question.

**Documentation:** the v1.1 documentation pass adds a sugar section showing the
`namespace mt = microtel::sugar;` alias once, and `examples/` gains one program
using the macro. Neither is a gate.

## Rationale & alternatives

- **A sugar-owned RAII type (`sugar::ScopedSpan`)** — rejected. It would
  duplicate the end-then-restore ordering that ICP 0025 §3 puts in
  `ScopedSpan`'s member declaration order, and two types with that ordering is
  one too many. Factories returning `microtel::ScopedSpan` are why ICP 0025
  made it move-constructible.
- **`Traced` catching and recording exceptions** — rejected for v1.1. Roadmap
  §5 v1.4 already names `mt::TryCatch` for exactly that, and a `Traced` that
  swallowed-and-rethrew would make the v1.4 helper redundant and the v1.1 one
  surprising. `Traced` is conditionally `noexcept` so a `noexcept` caller keeps
  its guarantee.
- **`AttrKey` holding a `std::string` instead of a `std::string_view`** —
  rejected. It would make the type allocate, lose `constexpr` construction, and
  reintroduce static-initialisation order as a concern — all to serve a caller
  who builds keys at runtime, which is the case `SetAttribute(std::string_view,
  …)` already serves directly.
- **Pre-encoding attribute keys in `AttrKey`** — rejected; §2.
- **Declaring `namespace mt = microtel::sugar;` in a microtel header** —
  rejected; §0.
- **A separate installed package (`microtel-sugar`)** — rejected. It is four
  headers with no link closure; a second package would buy a consumer the right
  to *not* install four files.
- **Exporting `microtel::sugar` as a CMake target** — rejected; §4, on ICP
  0020 Decision 2's principle. Available later if someone asks.
- **Waiting for v1.2 so `mt::Timer` ships with the rest** — rejected. Every
  helper locked here needs only what v1.1 already has; the timer is the single
  piece that needs a histogram, roadmap §5 already parks it in v1.2, and ICP
  0024's gate clause 1 prices the sugar layer as a v1.1 deliverable.
