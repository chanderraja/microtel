# ICP 0011: Add `LogAttributeLimit` to the `DropReason` enum

**Status:** Accepted
**Affected interfaces / docs:** `include/microtel/provider.hpp` (`DropReason`, `kDropReasonCount`), `src/sdk/drop_reason_names.hpp`, `docs/error-model.md` §3.
**Affected tracks:** none beyond the logs pipeline (Track A / SDK).

## Summary

Add one enumerator, `DropReason::LogAttributeLimit`, so the log emit path can
account for attributes dropped when a `LogRecord` exceeds its per-record
attribute limit.

## Motivation

`docs/logs-design.md` §5 (Accepted) enforces a per-record attribute cap in
`SdkLogger::Emit`; surplus attributes are dropped and counted. Every drop in
microtel is accounted through `IDiagnosticsSink::RecordDrop(DropReason)`, and
`DropReason` values index the public `HealthSnapshot::drop_counters` array — so
adding one is a change to the public health surface, which `provider.hpp`
explicitly flags as requiring an ICP (the same rule that produced ICP 0008 for
the metric drop reasons). L4.1 is the first increment that needs the counter, so
it lands with this ICP.

## Proposed change

- `include/microtel/provider.hpp`: append `LogAttributeLimit = 23` to
  `DropReason`; bump `kDropReasonCount` from 23 to 24.
- `src/sdk/drop_reason_names.hpp`: add the `case DropReason::LogAttributeLimit`
  → `"log_attribute_limit"` arm (the switch is exhaustive under `-Wswitch`, so
  the arm is required to compile).
- `docs/error-model.md` §3: add the `log_attribute_limit` row.

No existing enumerator value changes; the addition is append-only, so serialized
counter ordering is preserved. `HealthSnapshot`, `DiagnosticsCounters`, and
`FakeDiagnosticsSink` all size their arrays from `kDropReasonCount` and pick up
the new slot automatically.

## Migration

None for existing callers. Consumers that iterate `HealthSnapshot::drop_counters`
by index see one additional trailing counter; those that index by a specific
`DropReason` are unaffected. Enumerator values 0–22 are unchanged.

## Rationale & alternatives

- **Reuse an existing reason** (e.g. `RecordTooLarge`) — rejected: conflates a
  whole-record size drop with a per-record attribute-count drop, losing the
  signal operators need to tune the attribute limit.
- **No counter, silent truncation** — rejected: violates the project's
  drop-and-count discipline; `LogRecord::dropped_attributes_count` records
  per-record loss but gives no process-level health signal.
