# ICP 0034: `Provider::GetLeafReceiver`, the `LeafReceiver` API, and three leaf `DropReason`s

**Status:** Accepted — records decisions signed off in docs/leaf-concentrator-design.md (2026-09-26).
**Affected interfaces / docs:**
- `include/microtel/provider.hpp`: new pure virtual `GetLeafReceiver`;
  `DropReason` gains three enumerators; `kDropReasonCount` 24 → 27
- `include/microtel/leaf_receiver.hpp` (new public header)
- `src/sdk/sdk_provider.{hpp,cpp}`, `src/sdk/drop_reason_names.hpp`,
  `tests/fakes/fake_provider.hpp` (in the implementing packet)
- `docs/error-model.md` §3 (three rows, and a note on the `post_shutdown` row)
- `docs/interfaces.md` §6 (the `IReceiver` note)

**Affected tracks:** A (SDK). No transport, encoder or wire change.

## Summary

Record the two interface changes that
[`docs/leaf-concentrator-design.md`](../leaf-concentrator-design.md) §8 says
need an ICP: a `Provider::GetLeafReceiver()` pure virtual returning the new
public `LeafReceiver`, and three `DropReason` enumerators for leaf payloads
that fail before they yield any records.

## Motivation

The design doc (issue #319, implementing [ICP 0031](0031-leaf-concentrator-in-v1.3.md)
in v1.2 per [ICP 0032](0032-release-reorder-v1.1.1.md)) was signed off with
these decisions already made: §3.2 (public API), §3.3 (error model), §9
decisions 2 and 3. Both touch locked public surface: `Provider` is an M0
interface, and `DropReason` indexes `HealthSnapshot::drop_counters`, which
`provider.hpp` says needs an ICP to extend. This ICP decides nothing new. It
puts the decisions under a number so the implementing packet can cite it. The
precedents are [ICP 0012](0012-provider-get-logger.md) (`GetLogger`) and
ICPs [0008](0008-metric-drop-reasons.md) and
[0011](0011-log-attribute-limit-drop-reason.md) (appended drop reasons).

## Proposed change

### 1. `Provider::GetLeafReceiver` and `LeafReceiver` (design §3.2, §3.3)

Appended to `Provider` after its last existing virtual:

```cpp
// include/microtel/provider.hpp
[[nodiscard]] virtual std::shared_ptr<LeafReceiver> GetLeafReceiver() = 0;
```

It returns a no-op receiver, whose `Ingest` returns `IngestStatus::Disabled`,
when the concentrator is not enabled in config or not compiled in
(`MICROTEL_WITH_CONCENTRATOR`, default OFF, design §6.2). This matches how
`GetLogger` returns a no-op logger.

The new header, exactly as design §3.2 declares it:

```cpp
// include/microtel/leaf_receiver.hpp
namespace microtel
{

enum class LeafTimeMode : std::uint8_t
{
    ConcentratorStamped = 0,
    SyncRelative = 1,
    BootRelative = 2,
};

/// @brief One leaf payload handed to the receiver.
struct IngestRequest
{
    /// Transport-derived identity of the sender (§4.1). May be empty, in
    /// which case the payload's `leaf_id_attribute` value is used. Whichever
    /// id is used is also exported as that attribute (§4.4).
    std::string_view leaf_id;
    /// The OTLP ExportTraceServiceRequest bytes. Borrowed for the call only.
    std::span<const std::byte> payload;
    /// When the application received the payload. Unset means "now" (read
    /// from the Provider's clock at the start of the call).
    std::optional<std::chrono::system_clock::time_point> received_at;
};

enum class IngestStatus : std::uint8_t
{
    Accepted = 0,          ///< every span entered the pipeline
    PartiallyAccepted = 1, ///< decoded, but the pipeline dropped some spans
    Malformed = 2,         ///< not decodable, or failed validation (§3.4)
    TooLarge = 3,          ///< over a size limit (§3.7)
    UnknownLeaf = 4,       ///< leaf not configured and unknown_leaf = reject
    ShutDown = 5,          ///< Provider shut down
    Disabled = 6,          ///< concentrator not enabled or not compiled in
    OutOfMemory = 7,       ///< allocation failed while processing (§3.3)
};

struct IngestResult
{
    IngestStatus status = IngestStatus::Disabled;
    std::uint32_t spans_accepted = 0; ///< entered the span processor
    std::uint32_t spans_sampled_out = 0;
    std::uint32_t spans_dropped = 0;  ///< dropped by limits or the processor
};

/// @brief Counters that describe the receiver, not the export pipeline.
struct LeafReceiverStats
{
    std::uint64_t payloads_accepted = 0;
    std::uint64_t payloads_rejected = 0;
    std::uint64_t leaves_tracked = 0;      ///< current size of the leaf table
    std::uint64_t leaves_evicted = 0;
    std::uint64_t leaf_reported_drops = 0; ///< sum of microtel.leaf.dropped_* (§1.7)
    std::uint64_t time_fallbacks = 0;      ///< sync-relative payloads re-anchored (§5.3)
    std::uint64_t payloads_out_of_memory = 0;       ///< §3.3
    std::uint64_t resource_attributes_dropped = 0;  ///< over max_leaf_resource_bytes (§4.5)
    std::uint64_t leaf_id_conflicts = 0;   ///< payload declared a different id (§4.4)
};

class LeafReceiver
{
public:
    virtual ~LeafReceiver() noexcept = default;

    /// @threadsafety Thread-safe; may be called concurrently from any thread.
    [[nodiscard]] virtual IngestResult Ingest(const IngestRequest& request) noexcept = 0;

    [[nodiscard]] virtual LeafReceiverStats Stats() const noexcept = 0;
};

}  // namespace microtel
```

Section numbers in the comments refer to the design doc. The contract,
from design §3.3:

- `Ingest` is `noexcept`, runs on the caller's thread and never blocks
  (design §3.5). It returns a small value struct rather than
  `Expected<T, Error>`, so reporting a failure does not allocate.
- `std::bad_alloc` is caught at the boundary (never `catch (...)`). The
  payload is reported as `IngestStatus::OutOfMemory` and counted in
  `LeafReceiverStats::payloads_out_of_memory`, **not** as a `DropReason`.
  Running out of memory is the concentrator's problem, not the leaf's, and
  folding it into `leaf_payload_too_large` would send an operator to the
  wrong fix.
- Resource-budget drops (design §4.5, §9 decision 4) are counted in
  `LeafReceiverStats::resource_attributes_dropped`, not as a `DropReason`.

The same header also carries `LeafReceiverOptions`, `LeafConfig`,
`LeafConfigResolver` and `UnknownLeafPolicy` (design §4.3), consumed by the
new `SdkBuilder::WithLeafReceiver`. Those are additive and design §8 lists
them as needing no ICP. They are named here only so the header's full content
is on record.

### 2. Three `DropReason` enumerators (design §3.3, §9 decision 3)

Appended at the end of the enum, after `LogAttributeLimit = 23`:

| Enumerator | Value | Counter name | Counted when | Unit |
|---|---|---|---|---|
| `LeafPayloadMalformed` | 24 | `leaf_payload_malformed` | the payload fails to decode, fails validation (design §3.4), declares an unsupported wire version, or its time mode conflicts with the leaf's config (§5.1) | payloads |
| `LeafPayloadTooLarge` | 25 | `leaf_payload_too_large` | the payload exceeds `max_payload_bytes`, `max_spans_per_payload`, the decode depth limit, or the decode arena cap (§3.7) | payloads |
| `LeafUnknown` | 26 | `leaf_unknown` | the leaf is not configured and `unknown_leaf = "reject"` (§4.4) | payloads |

`kDropReasonCount` goes from 24 to 27. These are the first reasons counted in
payloads rather than records: a payload that cannot be decoded has no
trustworthy span count. `docs/error-model.md` §3 records the unit on each
row. The same section's `post_shutdown` row gains a note: an `Ingest` after
`Shutdown` counts one `post_shutdown` per payload, because the payload is not
decoded (design §3.3).

**Numbering against ICP 0030.** Draft [ICP 0030](0030-compile-time-feature-selection.md)
claims `SignalNotCompiled = 23`, which `LogAttributeLimit` already holds. If
this ICP lands first, 0030 takes the next free value (27) when it is accepted.
Whichever lands second takes the next free value at that time. No enumerator
is ever renumbered.

## Migration

- **`HealthSnapshot` size and ABI.** `drop_counters` is a
  `std::array<std::uint64_t, kDropReasonCount>`, so it grows by three elements
  and `sizeof(HealthSnapshot)` grows by 24 bytes. The new `Provider` virtual
  changes the vtable. Spec §19 makes binary compatibility best-effort within a
  minor release and not guaranteed across minors. Both changes ship in v1.2,
  so consumers recompile against the v1.2 headers. Appending the virtual last
  and the enumerators last keeps existing vtable slots and counter indices
  where they were.
- **Source compatibility.** Code that indexes `drop_counters` by a
  `DropReason` is unaffected; code that iterates it sees three more trailing
  counters. Exhaustive `switch`es over `DropReason` need three new arms
  (in-tree: `src/sdk/drop_reason_names.hpp`, which `-Wswitch` enforces).
- **`Provider` implementers.** External subclasses must add a
  `GetLeafReceiver` override, the same one-time break ICP 0012 made. In tree,
  `SdkProvider` and `tests/fakes/fake_provider.hpp` (which returns a no-op
  receiver) are updated in the implementing packet. Callers of `Provider` are
  unaffected.
- **Docs.** `docs/error-model.md` §3 gets its rows, and `docs/interfaces.md`
  §6 gets its `IReceiver` note replaced, in the packet that adds the code.

## Rationale & alternatives

All considered and rejected in the design doc:

- **A free factory** `MakeLeafReceiver(std::shared_ptr<Provider>)` (design
  §3.2). It leaves `Provider` untouched but would have to downcast to
  `SdkProvider` to reach its sampler, processor, sink and clock, which fails
  for any other `Provider`, including the test fakes.
- **`Ingest` directly on `Provider`** (§3.2). One type fewer, but every
  Provider carries a concentrator-only call, and `Stats()` needs a home anyway.
- **One reason, `leaf_payload_rejected`** (§3.3). An operator could not tell a
  firmware bug from a mis-sized limit from a config gap without application
  logs. Three counters cost nothing at runtime.
- **Reusing `record_too_large`** for oversize payloads (§3.3). The counter
  would then mean two different things.
- **A fourth `DropReason` for `OutOfMemory`** (§3.3). It would be another
  ICP-gated counter for a condition the process's own memory monitoring
  already shows. `IngestStatus` is new, so a value there costs nothing.
