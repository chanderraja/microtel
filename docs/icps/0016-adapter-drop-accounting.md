# ICP 0016: Drop accounting for adapter-level omissions

**Status:** Accepted — signed off 2026-08-30; shim-local design implemented in #127.
**Affected interfaces / docs:** none — no locked microtel interface is
touched. New shim-local header, documented in
`src/adapters/otelcpp/README.md`.
**Affected tracks:** M17 (otelcpp shim); any future bridge that must omit at
the adapter layer (Python M18) is a candidate to *reuse the pattern*, not to
share a mechanism with — see "Forward-compatibility" below.

## Summary

Two events inside the otelcpp shim currently vanish without a trace, and both
are the same shape: something happened *above* the SDK, where
`IDiagnosticsSink` is unreachable.

1. **Unrepresentable measurement omitted.** A `uint64_t` counter or histogram
   measurement above `INT64_MAX` has no `int64_t` representation and no
   degraded type to preserve into (unlike attributes — ICP 0015). The shim
   omits it, per "preserve or omit, never invent". A measurement that
   silently evaporates is the kind of thing an operator debugs for a day.
2. **Observer callback threw.** The shim's collection fan-out catches
   anything an application observer callback throws — the alternative is
   `std::terminate` inside a telemetry library. The failure is swallowed
   with nothing incremented.

Proposes a **shim-local counter** — a small header under
`src/adapters/otelcpp/`, exposing a stable, thread-safe accessor — rather
than any addition to microtel core. An earlier draft of this ICP proposed a
new `Provider::RecordAdapterDrop` method and two new `DropReason` values; see
"Rationale & alternatives" for why that's rejected in favor of the
shim-local shape.

## Why ICP 0015's answer does not carry over

0015 rejected drop accounting for the *attribute* case (Option A) on three
grounds: dropping loses data Option B preserves; the public surface would
grow for an optional adapter; and a counter only some paths can increment
reads as "no problems" when it means "nothing wired". The first ground does
the real work there — **Option B exists for attributes**. For measurements it
does not: there is no string a histogram can record into. Omission is the
only honest behaviour, which flips the calculus: the choice is not
"account vs. preserve" but "account vs. silence". This part of the reasoning
is unaffected by where the accounting lives — silence is still the wrong
answer; what changed on revision is *where* the accounting should live.

## Proposal

A new header, `src/adapters/otelcpp/shim_diagnostics.hpp`, header-only like
the rest of the shim:

```cpp
namespace microtel::adapters::otelcpp
{

/// @brief Adapter-local events the shim cannot report through
/// microtel::Provider's own diagnostics, because they happen above the SDK.
struct ShimDiagnostics
{
    std::uint64_t unrepresentable_measurements_omitted = 0;
    std::uint64_t observer_callback_failures = 0;
};

/// @brief Current counts, as of the call. Thread-safe, noexcept.
[[nodiscard]] ShimDiagnostics GetShimDiagnostics() noexcept;

}  // namespace microtel::adapters::otelcpp
```

Backed by function-local `static std::atomic<std::uint64_t>` counters
(the standard header-only-singleton-counter pattern; no `.cpp` file, no new
CMake source entry — consistent with every other file in this directory).
Incremented at the two existing sites: `CounterShim`/`HistogramShim::Forward`
(measurement omission) and `ObservableCallbackRegistry::Invoke`'s
`catch (...)` (callback failure) — both already documented as the point
where the current silence happens; this adds one atomic increment at each,
nothing else.

### Forward-compatibility

The condition that would flip this recommendation is **generality**: if
Python (M18) or a future log bridge needs the same kind of adapter-level
drop accounting, N shim-local counters with N different shapes is worse than
one designed mechanism, and building that mechanism *once*, informed by two
real callers, beats building it speculatively now with one. To keep that
option open without paying for it today: the public accessor
(`GetShimDiagnostics`) is the stable surface; its backing store is an
implementation detail. If a general adapter-diagnostics mechanism gets built
later, this function is reimplemented to read from it, and every existing
caller of `GetShimDiagnostics()` sees no change.

## Migration

- No change to any locked interface, so nothing to rebuild.
- New optional header; only the otelcpp shim's own translation units
  reference it.
- Documented in `src/adapters/otelcpp/README.md` alongside the shim's other
  degradation/omission policies.

## Rationale & alternatives

- **Reuse `NonFiniteValue`** — rejected; documented as the OTel-spec NaN/±Inf
  rule, and muddying it hides both meanings.
- **A new `Provider::RecordAdapterDrop` method plus two new `DropReason`
  values** (this ICP's own earlier proposal) — rejected on reconsideration.
  A `uint64_t` measurement above `INT64_MAX` is a value north of 9.2
  quintillion; in practice that's an overflow bug or uninitialized memory,
  not a real measurement. Building permanent public API on microtel
  core — plus a `DropReason` enumerator, extending `HealthSnapshot::
  drop_counters` for every consumer forever — for an event that essentially
  never fires in a real deployment is disproportionate in exactly the way
  ICP 0015 argued about attributes. This is **not** "operators don't matter"
  or "a second health surface is inherently bad" — it's that the specific
  cost (permanent core-API growth) is disproportionate to the specific
  benefit (a counter that reads zero in every real deployment). The
  condition for revisiting this rejection is stated above under
  "Forward-compatibility": a second adapter with the same need.
- **Do nothing** — rejected: silence here is a day of someone's debugging,
  and the shim-local shape makes that cost near-zero to avoid.

## Open question for the reviewer

None outstanding. The prior draft's open question (whether
`RecordAdapterDrop` should be free-standing on `Provider` or delivered via an
opaque handle) is moot — neither shape is being built.
