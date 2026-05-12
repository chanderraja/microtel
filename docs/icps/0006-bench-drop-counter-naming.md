# ICP 0006: Align bench-spec.md §5 drop-counter names with DropReason enum

**Status:** Accepted
**Affected interfaces / docs:** `docs/bench-spec.md` §5
**Affected tracks:** None (docs only)
**Spec / roadmap impact:** None

## Summary

The drop-counter metric table in `docs/bench-spec.md` §5 uses an informal
description ("queue overflow") that doesn't map to the actual
`microtel::DropReason` enum values. Update §5 to reference the real enum
names so that the bench driver implementation has an authoritative source.

## Motivation

When the B0 bench driver reads drop counters from the SUT via
`IDiagnosticsSink::RecordDrop(DropReason, uint64_t)`, it must know the
exact enum names to request per-reason breakdowns. A spec that says
"queue overflow" is ambiguous — the actual enum is `DropReason::QueueFull`.

## Proposed change

Replace the single "Items dropped (queue overflow)" row in §5 with an
expanded row referencing the real `DropReason` enumerators (see
`include/microtel/provider.hpp`):

| Metric | Source | Units |
|---|---|---|
| Items dropped — total | SUT counter (sum of all `DropReason` buckets) | count |
| Items dropped — `QueueFull` | `RecordDrop(DropReason::QueueFull)` counter | count |
| Items dropped — `RecordTooLarge` | `RecordDrop(DropReason::RecordTooLarge)` counter | count |
| Items dropped — `SpanAttributeLimit` | `RecordDrop(DropReason::SpanAttributeLimit)` counter | count |
| Items dropped — `AttributeValueTruncated` | `RecordDrop(DropReason::AttributeValueTruncated)` counter | count |

The remaining 16 `DropReason` values are captured in the total; the
B0 driver exposes only the four most actionable ones in the report.
Additional per-reason rows can be added in B1/B2 as needed.

## Rationale

Spelling the enum names in the spec doc makes it trivially reviewable:
anyone can grep `include/microtel/provider.hpp` and confirm the names exist.
The informal description would have caused a compile-time surprise during
B0 driver implementation.

## Sign-off

| Reviewer | Date | Status |
|---|---|---|
| Chander Raja | 2026-05-11 | Accepted |
