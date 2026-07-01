# ICP 0008 — Metric drop reasons in `DropReason` / `HealthSnapshot`

**Status:** Accepted
**Author:** Chander Raja
**Affected interfaces / docs:** `include/microtel/provider.hpp`
(`DropReason`, `kDropReasonCount`, `HealthSnapshot::drop_counters`);
`docs/error-model.md` §3 (the `DropReason` table)
**Affected tracks:** Track A — SDK (owns `provider.hpp` and the public health
surface); consumed by the M12 metrics implementation. Docs.

## Summary

Append three metric-specific enumerators to `DropReason` (and bump
`kDropReasonCount` 20 → 23) so the v1.2 metrics pipeline can account for
dropped/degraded measurements through the existing `HealthSnapshot` surface.

## Motivation

The metrics signal (v1.2 / M12, designed in `docs/metrics-design.md`) introduces
failure modes the current span/export-only `DropReason` enum doesn't cover:

- A measurement whose attribute set exceeds the per-instrument cardinality limit
  and is folded into the `otel.metric.overflow` series (`metrics-design.md` §2).
- An async-instrument callback abandoned for exceeding the per-collection
  deadline, dropping its measurements for that cycle (`metrics-design.md` §4).
- A non-finite (`NaN` / ±`Inf`) measurement, which OTel requires dropping.

`DropReason`'s own Doxygen states adding an entry is an ICP, because the
enumerator order is part of the `HealthSnapshot::drop_counters` array-indexing
contract. This ICP records that change so M12 can reference it, per the
"ICP merges before the implementing PR" process.

## Proposed change

In `include/microtel/provider.hpp`, **append** (preserving 0–19) three
enumerators and bump the count:

```cpp
enum class DropReason : std::uint8_t
{
    // ... existing 0–19 unchanged ...
    ShutdownTimeout = 19,
    CardinalityOverflow = 20,    ///< attr-set exceeded the cardinality limit;
                                 ///< folded into the otel.metric.overflow series
    MetricCallbackTimeout = 21,  ///< async callback exceeded the collection
                                 ///< deadline; its measurements were dropped
    NonFiniteValue = 22,         ///< NaN / ±Inf measurement dropped (OTel rule)
};

inline constexpr std::size_t kDropReasonCount = 23;  // was 20
```

No new **export-side** reasons are added: metric export failures reuse the
existing transport reasons (`NonRetryableFailure`, `RetryBudgetExhausted`,
`TransportBusy`, `ConnectFailure`, `ForceFlushTimeout`, `ShutdownTimeout`)
because the metrics pipeline shares the nghttp2 transport (`metrics-design.md`
§5/§10).

Also update:
- `docs/error-model.md` §3 — add the three rows to the `DropReason` table.
- The `DropReason` → string mapping used by internal diagnostics/logging — add
  the three names (M12).
- Increment the new counters at their metric drop sites in `src/sdk` /
  `src/exporter` (M12).

## Migration

- **Append-only.** Enumerators 0–19 keep their values and `drop_counters`
  indices, so no consumer of an existing reason changes.
- `kDropReasonCount` goes 20 → 23 and `HealthSnapshot::drop_counters` grows to
  23 entries. Consumers that iterate the array via `kDropReasonCount` (e.g. the
  bench emit-app's `std::accumulate` over `drop_counters.begin()/.end()`) are
  unaffected. Code must not hard-code `20`.
- `HealthSnapshot` is returned **by value** from `GetExporterHealth()`, so the
  3×`uint64_t` growth is a fresh copy each call — source-compatible, no
  cross-boundary ABI lock. No trace behavior changes.

## Rationale & alternatives

- **A separate metrics-only drop-counter array** — rejected: fragments the
  single documented health surface; one `HealthSnapshot` is simpler and the
  array is already the contract.
- **Silent cardinality overflow (no counter)** — rejected: cardinality is the
  dominant metrics failure mode; operators need the signal.
- **Reuse a generic existing reason for overflow** — rejected: overflow is
  metric-specific and operationally distinct from a hard drop (the measurement
  is still aggregated, into the overflow series).
- **Reorder/insert rather than append** — rejected: would break the
  index contract for every existing reason.
