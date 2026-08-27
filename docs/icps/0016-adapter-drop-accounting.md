# ICP 0016: Drop accounting for adapter-level omissions

**Status:** Draft
**Affected interfaces / docs:** `include/microtel/provider.hpp` (`DropReason`, `kDropReasonCount`, and one new method on `Provider`); `docs/error-model.md` §3.
**Affected tracks:** M17 (otelcpp shim); any future bridge that must omit at the adapter layer (Python M18).

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

Proposes one new public method and two new `DropReason` values so both show
up in `GetExporterHealth()`.

## Why ICP 0015's answer does not carry over

0015 rejected drop accounting for the *attribute* case (Option A) on three
grounds: dropping loses data Option B preserves; the public surface would
grow for an optional adapter; and a counter only some paths can increment
reads as "no problems" when it means "nothing wired". The first ground does
the real work there — **Option B exists for attributes**. For measurements it
does not: there is no string a histogram can record into. Omission is the
only honest behaviour, which flips the calculus: the choice is not
"account vs. preserve" but "account vs. silence".

## Proposal

1. Two `DropReason` values, appended (existing values unchanged):

   ```cpp
   /// A measurement with no faithful representation was omitted by a
   /// source-distributed adapter (e.g. otel-cpp uint64 > INT64_MAX).
   AdapterUnrepresentableMeasurement = 24,
   /// An application observer callback threw during collection; the
   /// exception was contained at the adapter boundary (ICP 0016).
   AdapterCallbackFailure = 25,
   ```

2. One narrow public method:

   ```cpp
   /// @brief Record a drop that occurred above the SDK, in a
   /// source-distributed adapter. Increments the matching drop counter.
   /// @threadsafety Thread-safe. @noexcept
   virtual void RecordAdapterDrop(DropReason reason) noexcept = 0;
   ```

   Restricted by convention to the two `Adapter*` reasons; other values are
   counted but adapters have no business sending them (documented, not
   enforced — enforcement would cost a check on a hot path).

3. The shim's `MeterShim` / instrument shims take the provider they were
   created from (they already hold it transitively) and call
   `RecordAdapterDrop` at the two sites.

## Migration

- `kDropReasonCount` grows by two; `HealthSnapshot::drop_counters` and
  `drop_reason_names.hpp` extend mechanically — same shape as ICP 0008/0011.
- One new pure virtual on `Provider` is a breaking change for out-of-tree
  `Provider` implementations; in-tree there is exactly one (`SdkProvider`)
  plus test fakes.

## Rationale & alternatives

- **Reuse `NonFiniteValue`** — rejected; documented as the OTel-spec NaN/±Inf
  rule, and muddying it hides both meanings.
- **A shim-local counter surfaced some other way** — rejected; operators
  should not need a second health surface for adapter drops.
- **Do nothing** — rejected by the review that prompted this ICP: silence
  here is a day of someone's debugging.

## Open question for the reviewer

Whether `RecordAdapterDrop` should be free-standing on `Provider` (proposed)
or whether adapters should receive an opaque diagnostics handle at
construction. The handle is cleaner layering but a larger API; the method is
one line an adapter can call with what it already holds.
