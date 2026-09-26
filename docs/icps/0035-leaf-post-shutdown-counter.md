# ICP 0035: count leaf payloads after Shutdown separately from `post_shutdown`

**Status:** Accepted — decided 2026-09-26.
**Affected interfaces / docs:**
- `include/microtel/leaf_receiver.hpp` (`LeafReceiverStats` gains one field)
- `docs/error-model.md` §3 (the `post_shutdown` row loses the note ICP 0034 added)
- ICP 0034 (amends its `post_shutdown` note; that file is not edited)

**Affected tracks:** SDK (concentrator), documentation.

## Summary

A leaf payload that arrives at `LeafReceiver::Ingest` after `Shutdown` is
counted in a new `LeafReceiverStats::payloads_post_shutdown` field instead of
in `DropReason::PostShutdown`, which goes back to counting records only.

## Motivation

ICP 0034 made a late `Ingest` count one `post_shutdown` per *payload*, because
the payload is never decoded and its span count is unknown. Every other
producer of `post_shutdown` counts *records*, and `docs/error-model.md` §3
defines the counter that way. One counter with two units can't be read
reliably: 1,000 on a concentrator could be 1,000 spans or 1,000 payloads of
up to hundreds of spans each.

Receiver-shaped counters belong in `LeafReceiverStats`, which already counts
payloads (`payloads_accepted`, `payloads_rejected`, `payloads_out_of_memory`).
The same reasoning put Resource-budget drops there instead of in a
`DropReason` (design §9 decision 4).

## Proposed change

1. `LeafReceiverStats` gains, after `leaf_id_conflicts`:

   ```cpp
   std::uint64_t payloads_post_shutdown = 0;  ///< Ingest after Shutdown; not decoded
   ```

2. `Ingest` after `Shutdown` still returns its existing `IngestStatus` and does
   no work, but increments `payloads_post_shutdown` and **not**
   `DropReason::PostShutdown`. It is also not counted in `payloads_rejected`,
   so the two payload counters stay disjoint.
3. `docs/error-model.md` §3: remove the per-payload note from the
   `post_shutdown` row; the row is records-only again, as it was before
   ICP 0034. Point to `LeafReceiverStats::payloads_post_shutdown` for late leaf
   payloads.

## Migration

The concentrator is experimental and has not been released, so no user relies
on the ICP 0034 behaviour. `LeafReceiverStats` is an aggregate returned by
value; adding a trailing field is source-compatible.

## Rationale & alternatives

- **Keep the ICP 0034 note.** Rejected: it gives `post_shutdown` two units.
- **Decode the payload to count its spans.** Rejected: it spends decode work,
  and an allocation, on data that is going to be dropped anyway, during
  shutdown.
- **A fourth leaf `DropReason`.** Rejected for the same reason as design §9
  decision 4: nothing enters the pipeline, so this is a receiver-side count,
  not a pipeline drop, and a new enumerator would grow `HealthSnapshot` for it.
