# ICP 0015: Policy for attribute values microtel's model cannot represent

**Status:** Draft
**Affected interfaces / docs:** possibly `include/microtel/provider.hpp` (`DropReason`, `kDropReasonCount`) and `docs/error-model.md` §3 — **only if Option A is chosen**. Option B (recommended) touches no locked interface. Shim behaviour is documented in `src/adapters/otelcpp/README.md`.
**Affected tracks:** M17 (otelcpp shim); sets precedent for any future bridge (Python M18, other log bridges).

## Summary

Decide what the opentelemetry-cpp shim does with attribute values that have no
faithful `microtel::AttributeValue` representation. Recommends **Option B:
preserve the value with a degraded type**, which avoids both data loss and any
growth of the public health surface.

## Motivation

otel-cpp's `common::AttributeValue` has **16** alternatives; microtel's has
**8**, matching what the OpenTelemetry data model actually specifies. Thirteen
map exactly or widen losslessly. Three do not:

| otel-cpp alternative | Problem |
|---|---|
| `uint64_t` > `INT64_MAX` | No unsigned 64-bit attribute type in the OTel data model |
| `span<const uint64_t>` with any such element | Same, per element |
| `span<const uint8_t>` (bytes) | OTLP's `AnyValue` has `bytes_value`; `microtel::AttributeValue` does not expose one |

otel-cpp's own header marks `uint64_t` and the reserved spans as *"Not currently
supported by the specification, but reserved for future use"* — so these are
outside the OTel data model regardless of what microtel does.

**Clamping is not on the table.** Reporting `INT64_MAX` for a value the
application never set is a silent data corruption: a reader cannot distinguish
it from a genuine `INT64_MAX`. Every option below preserves or omits, never
invents.

The live question is whether the shim **drops** (and must therefore account for
the drop) or **degrades** (and need not).

## Option A — drop, and account for it

Drop the attribute; record it so operators can see it happened.

Requires:

1. A new `DropReason` enumerator. Adding one extends the public
   `HealthSnapshot::drop_counters` array, which `provider.hpp` explicitly flags
   as ICP-worthy — the rule that produced ICP 0008 and ICP 0011.
2. **A way for the shim to record it at all**, which does not currently exist.
   This is the substantive problem. Every existing drop is recorded through
   `IDiagnosticsSink::RecordDrop`, reached from *inside* the SDK. The spdlog
   bridge never needs it because its drops happen inside `SdkLogger::Emit`, below
   the adapter. The otelcpp shim is different: conversion fails **above**
   microtel, so the value never reaches the SDK and microtel's diagnostics
   cannot observe it. Closing that gap means either a new public mutation
   method on `Provider` (e.g. `RecordAdapterDrop`) or handing adapters an
   internal sink — both materially larger public-API changes than the
   enumerator itself.

**Cost:** the user's data is gone, *and* the public surface grows in two places
for the benefit of an optional adapter.

## Option B — preserve the value, degrade the type (recommended)

Convert to a representation microtel does have, keeping the value exact:

| Case | Becomes | Fidelity |
|---|---|---|
| `uint64_t` > `INT64_MAX` | `std::string` holding the exact decimal digits | value exact, type changed |
| `span<const uint64_t>` with such an element | `std::vector<std::string>`, every element rendered decimally | value exact, type changed |
| `span<const uint8_t>` | `std::string` holding lowercase hex, no separators | value exact, type changed |

Nothing is lost and nothing is invented. `18446744073709551615` arrives as the
string `"18446744073709551615"`, which is exactly what the application set.

Uniformity matters for the array case: if one element overflows, **all**
elements render as strings, so the array stays homogeneous — `AttributeValue`'s
array alternatives cannot hold mixed types, and a half-converted array would be
worse than either extreme.

**Cost:** a consumer filtering on attribute *type* sees a string where an
integer was set. This is visible and documented, unlike a dropped attribute,
which is indistinguishable from one never set.

**No new `DropReason`. No new public API. Nothing to account for**, because
nothing is dropped.

## Recommendation

**Option B.** Three reasons:

1. **It loses less.** Dropping destroys the value; degrading keeps it. For the
   debugging use case attributes exist to serve, a string with the right digits
   beats an absent field.
2. **It does not grow the public surface for an optional adapter.** Option A
   needs both a `DropReason` enumerator and a new way for adapters to reach
   diagnostics — a permanent addition to microtel's core public API, driven
   entirely by a package excluded from the dependency-closure claims.
3. **It keeps the health surface honest.** A drop counter that only some
   adapters can increment invites the reading that an empty counter means "no
   problems", when it may mean "nothing was wired to report."

The affected inputs are, by otel-cpp's own documentation, outside the OTel
specification. Spending public API on them is disproportionate.

## Migration

- Contributors and agents: the shim's conversion returns a value for every
  otel-cpp alternative. `std::optional` disappears from
  `ConvertAttributeValue`'s signature.
- No change to any locked interface, so nothing to rebuild.
- The type degradation is documented in `src/adapters/otelcpp/README.md` and
  covered by tests asserting the exact rendered strings.

## Rationale & alternatives

- **Clamp to `INT64_MAX`** — rejected. Silent corruption; indistinguishable
  from a real `INT64_MAX`.
- **Drop only the offending array element** — rejected. Changes the array's
  length, so index-based correlation with a parallel attribute silently breaks.
- **Add a bytes alternative to `microtel::AttributeValue`** — rejected for now.
  It is the *correct* long-term answer for `span<uint8_t>` (OTLP models
  `bytes_value` natively) but it is a change to a locked public type, needs
  encoder and wire-conformance work, and would hold up M17 for a case no OTel
  semantic convention currently uses. Worth revisiting on real demand.
- **Base64 rather than hex for bytes** — rejected. Hex is unambiguous, has no
  padding, and is what a debugging reader can eyeball.

## Open question for the reviewer

Whether the degraded string forms should carry a marker (e.g. a companion
`<key>.microtel.degraded_from = "uint64"` attribute) so a consumer can tell a
degraded value from one the application genuinely set as a string. It makes the
degradation self-describing at the cost of doubling the attribute count for
affected keys. Not proposed here; the cost seems high for inputs this rare.
