# ICP 0015: Policy for attribute values microtel's model cannot represent

**Status:** Accepted — signed off 2026-08-26; Option B implemented in #110
**Affected interfaces / docs:** none — Option B was chosen, which touches no locked interface. Shim behaviour is documented in `src/adapters/otelcpp/README.md`.
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

> **Resolved at acceptance: no marker.** The affected inputs are outside the
> OTel data model by otel-cpp's own documentation; doubling the attribute count
> for them buys self-description nobody has asked for. Revisit on real demand,
> as a follow-up ICP if it would change the shim's public behaviour.

## Addendum (2026-08-27) — retrospective review findings

A retrospective review of this ICP's reasoning, after acceptance and
implementation, surfaced four points worth recording. None changes the
Option B recommendation; the original decision text above is left as written
— this section adds to the record rather than editing it.

**1. Hex encoding interacts badly with `attribute_value_length_limit`, if
that limit is ever enforced.** `span<const uint8_t>` renders at two hex
characters per byte, so any byte span over 2048 bytes produces a string
longer than the 4096-character default `attribute_value_length_limit`. A
naive byte-offset truncation of that string can land on an odd nibble
boundary, producing a string that is *still valid hex* but decodes to a
different final byte than the application set — silent corruption
indistinguishable from a correctly-encoded value, which is exactly the
failure mode this ICP rejects clamping to avoid. **This does not happen
today**: `attribute_value_length_limit` is a declared config field
(`SpanLimitOptions::attribute_value_length_limit`) and `DropReason::
AttributeValueTruncated` is a declared enum value, but nothing in the
codebase currently reads the former or increments the latter —
`SdkSpan::SetAttribute` enforces only `attribute_count_limit`, a count cap,
not a per-value length cap. The interaction is real but not yet live. It is
far cheaper to record this now, while nobody has built the enforcement, than
to discover it after the fact via a corrupted payload: whoever implements
`attribute_value_length_limit` enforcement should treat hex-encoded byte
attributes as a case that needs truncation to an even nibble boundary (or
omission above the limit, matching this ICP's own preserve-or-omit
principle), not a generic string-truncate. Flagged in
`attribute_conversion.hpp`'s `RenderBytesAsHex` doc comment so it's
discoverable from the code that produces the affected values, not only from
this ICP. A large-byte-span test
(`LargeByteSpanEncodesCorrectlyAtScale`) now pins correct encoding at scale,
though it cannot test the truncation interaction itself until the
enforcement it would interact with exists.

**2. The array-uniformity cost is understated.** The Option B cost line
above ("a consumer filtering on attribute type sees a string where an
integer was set") describes the mildest possible consequence. The sharper
one: `[1, 2, 3]` and `[1, 2, 2⁶³]` export as different attribute *types*
(int64 array vs. string array) under the same key, depending on the values
observed at runtime, not on any static schema property. Backends with schema
inference or fixed field mappings commonly reject or coerce on a type
conflict for an established field — the failure mode isn't a confused
downstream filter, it's a rejected record, potentially the whole span.
Option A (drop) does not have this property: an absent attribute is
something every backend already handles. This doesn't change the
recommendation — Option A's own costs (data loss, permanent public-API
growth for an optional adapter) are still worse — but the trade-off is
sharper than "Cost:" above states.

**3. The alternatives considered for Option A's accounting gap were
incomplete.** Option A's costing above considers only "a new `DropReason`
enumerator" plus "a new public mutation method on `Provider` or an internal
sink" — both permanent core-API growth. A third shape exists: a
shim-local counter, behind the shim's own header, touching no microtel core
API at all. This would have made *accounted-for* dropping viable at much
lower cost than this ICP assumed, and the Recommendation section's reason 2
("does not grow the public surface") would not have applied against it —
only reason 1 ("it loses less") would have. The recommendation still holds
on that ground alone: Option B preserves the value and needs no accounting
of any kind, which strictly beats an accounted-for drop regardless of how
cheap the accounting is. The shim-local-counter shape is now evaluated
properly, for the analogous case of an omitted *measurement* (no degraded
form exists for a number), in [ICP 0016](0016-adapter-drop-accounting.md).

**4. The indistinguishability argument against clamping is not applied to
the chosen option.** Clamping is rejected above because *"a reader cannot
distinguish it from a genuine `INT64_MAX`."* Option B's degraded strings have
the same property in a weaker form: a consumer cannot distinguish
`"18446744073709551615"` produced by degradation from a string the
application genuinely set. The harm is lower — clamping fabricates a value
that was never set; degradation preserves the exact value that was — but the
same argument structure applies to both, and the original text doesn't name
that it's making a different call on the same axis. Resolving it explicitly:
**indistinguishability is disqualifying only when it can misrepresent a
value the application never set (clamping); it is acceptable when the value
is exactly what the application set (degradation)**. That is the actual
ground on which the marker question above is decided against — not "the
cost seems high," which was true but not the decisive reason.

**5. "Revisit on real demand" (bytes alternative) is not a free option.**
Adding a native bytes alternative to `microtel::AttributeValue` later — the
rejected-for-now alternative above — would be a breaking behavior change for
any consumer that has come to depend on the current hex-string form (a
filter, a dashboard, a stored query). Worth stating plainly rather than
leaving the migration cost implicit in "revisit."
