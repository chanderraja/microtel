# ICP 0003: M0 deferred decisions — `SslCtx` ownership, unsampled-`Span` shape, MPSC queue scope

**Status:** Accepted
**Affected interfaces / docs:** `docs/interfaces.md` §4.1, `docs/memory-model.md` §3.1 (heap), §4.2 (RAII), §8.2 (sampled allocation), `docs/threading-model.md` §3.1, public `include/microtel/span.hpp`, public `include/microtel/tracer.hpp`.
**Affected tracks:** A (Trace SDK), D (Transport).
**Spec / roadmap impact:** none — both documents intentionally untouched.

## Summary

Three small architectural questions surfaced during M0 drafting and were
recorded as "decide before sign-off." This ICP records the resolutions so
they're locked alongside the rest of M0.

1. **`SslCtx` is per-`Transport`, not process-shared.**
2. **The unsampled-`Span` returns `std::unique_ptr<Span>` to a static no-op
   singleton via a no-op deleter.** The `Tracer` API shape is unchanged.
3. **The MPSC span-queue data structure stays an M3 implementation choice.**
   Only the contract properties from `threading-model.md` §3.1 are
   normative in M0.

## Decisions

### 3.1 `SslCtx` ownership — per-`Transport`

**Selected:** each `ITransport` instance constructs and owns its own
`SslCtx`. No process-level sharing.

**Why.** v1 has exactly one `Transport` per process (one
`(endpoint, protocol)` tuple, one nghttp2 session). The "shared" form
exists in `memory-model.md` §4.2 only to anticipate the multi-`Transport`
future where it might amortise. In v1, there is nothing to share with —
the `shared_ptr` would be a single-owner `shared_ptr`, which is just
`unique_ptr` with extra atomic refcount work.

Multi-`Provider` (v1.1) and concentrator (v2.0) may revisit. Until then
keeping the type `unique_ptr<SslCtx>` removes the only required
`shared_ptr` justification in the v1 codebase.

**Concrete edits to land alongside this ICP:**

- `docs/memory-model.md` §4.2 — change "Shared (justified...)" row for
  `SslCtx` to "Owned by `Transport` (one per process in v1)."
- `docs/interfaces.md` §4.1 — drop the "the only `shared_ptr` justified
  case" note; `Transport` constructs its own `SslCtx`.
- No public-header change.

**Forward-looking note for v1.1.** When multi-`Provider` lands, the
natural shape is N `Provider`s, each with their own `Transport`, each
with their own `SslCtx` — **not** "one shared `SslCtx` among
`Provider`s." TLS configuration is rarely identical across endpoints
in practice (different CAs, different mTLS materials, different SNI),
so process-level sharing was always going to be a narrow case. This
note exists so v1.1's design discussion does not re-open the question
with the wrong default.

### 3.2 Unsampled-`Span` shape — `unique_ptr<Span>` to no-op singleton

**Selected:** `Tracer::StartSpan` returns `std::unique_ptr<Span>` in all
cases. On the unsampled path, the returned `unique_ptr` points at a
**static no-op `Span` singleton**, with a **no-op custom deleter**
(`std::unique_ptr<Span, NoopDeleter>` actually — see below for the public
type's exact spelling).

The two alternatives considered:

- (B) Public `Span` is a value type; sampled and unsampled both return
  by value. Rejected — RAII auto-end becomes ambiguous when copying is
  permitted, and the polymorphic dispatch needed for sampled-path
  variability is awkward without indirection.
- (C) `Tracer::StartSpan` returns a raw pointer to a singleton on the
  unsampled path and a heap-allocated `Span*` on the sampled path,
  with the application responsible for `End()`. Rejected — error-prone;
  RAII auto-end is one of the API's load-bearing ergonomics.

**Why (A) wins.**

- The unsampled path performs no allocation (LOCKED — `memory-model.md`
  §8.1). The custom deleter is empty; the singleton lives in static
  storage; nothing is freed.
- The sampled path allocates exactly one `Span` (or pulls from a per-
  thread freelist when M3 measures it pays off). Same deleter type;
  the sampled deleter actually deletes.
- RAII auto-end works identically on both paths because the
  `unique_ptr<Span>`-shaped object is the same.
- The application's compile-time interface is unchanged regardless of
  sampling decision.

**Concrete public-API spelling.** A small custom deleter type lives in
`include/microtel/span.hpp`:

```cpp
namespace microtel
{

class Span;

namespace internal
{
    /// @brief Deleter for `std::unique_ptr<Span, SpanDeleter>`. Holds a
    /// type-erased function pointer; the unsampled path uses a no-op,
    /// the sampled path uses a delete-and-recycle.
    struct SpanDeleter
    {
        void (*deleter)(Span*) = nullptr;
        void operator()(Span* s) const noexcept;
    };
}

using SpanHandle = std::unique_ptr<Span, internal::SpanDeleter>;

}  // namespace microtel
```

`Tracer::StartSpan` returns `SpanHandle`. Application code remains
ergonomic:

```cpp
auto span = tracer->StartSpan("name");
span->SetAttribute("k", "v");
// RAII auto-end at scope exit, with the right deleter dispatched.

// SpanHandle is move-only — same as unique_ptr.
auto child = std::move(span);  // ok, transfers ownership
auto copy  = span;             // compile error, as intended
```

**Implementation flexibility.** The `internal::SpanDeleter` shape may
be revised in v1.x based on benchmark findings — for instance, a
stateless deleter that branches on a flag inside `Span` itself would
shrink `SpanHandle` from two pointers to one at the cost of one branch
on destruction. `SpanHandle` (the public alias) is the **stable
surface**; only the deleter's internal shape may change, and that
change does not require an ICP. M7's benchmark harness will surface
whether the change is worth making.

**Concrete edits to land alongside this ICP:**

- `include/microtel/span.hpp` — define `internal::SpanDeleter` and the
  `SpanHandle` alias.
- `include/microtel/tracer.hpp` — change `StartSpan` return type from
  `std::unique_ptr<Span>` to `SpanHandle`.
- `docs/memory-model.md` §8.1 — replace the prose "the no-op singleton
  is returned via `unique_ptr<Span>` with a custom deleter" with a
  reference to `SpanHandle` and a one-line example.
- `docs/interfaces.md` value-types subsection (§3 if it grows one for
  public types, otherwise inline in the per-interface section for the
  `Tracer` cross-reference).

### 3.3 MPSC queue shape — stays deferred

**Selected:** **no change.** `memory-model.md` §8.2 and
`threading-model.md` §3.1 already say the producer-side property is
"wait-free or near-wait-free; bounded; never blocks on I/O" and the
exact data structure is an M3 choice driven by benchmarks.

This ICP affirms that for sign-off; it does not pin a structure.

The two structures still under consideration:

- **(a) bounded ring buffer with atomic head/tail, drop-newest on full.**
  Simplest, predictable, allocation-free push.
- **(b) lock-free intrusive linked list with per-thread freelists.** Higher
  throughput at scale; harder to write correctly.

**Default for M3:** structure (a) — bounded ring buffer with atomic
head/tail. M3 implements (a) without further review.

**Switching to (b) requires an ICP** demonstrating, via benchmark data
from `microtel-bench`, that the per-thread freelist contention saving
exceeds the lock-free correctness cost. (a) is the path of least
resistance; (b) is taken only with measured justification.

If (a) turns out to be unimplementable to the §3.1 contract properties
in `threading-model.md` (wait-free producer push, never blocks on
I/O), that's also an ICP — at which point we evaluate (b) or a third
option.

## Migration

Pre-M0-close. No existing source code; no migration required for users.

## Rationale & alternatives

Captured per-decision above. The cross-cutting note: each of these is a
one-step decision that does not cascade into other locked contracts.
Locking them in this ICP rather than separate ones keeps the M0 review
surface small — the reviewer can sign off on the trio in one pass.

## Sign-off

| Decision | Reviewer | Date | Status |
|---|---|---|---|
| 3.1 `SslCtx` per-`Transport` | Chander Raja | 2026-05-04 | Accepted |
| 3.2 `SpanHandle` (unique_ptr + no-op deleter) | Chander Raja | 2026-05-04 | Accepted |
| 3.3 MPSC queue stays deferred (M3 default: (a)) | Chander Raja | 2026-05-04 | Accepted |
