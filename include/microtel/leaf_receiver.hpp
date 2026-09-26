// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/attribute.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

/// @file
/// The concentrator's ingest API: `LeafReceiver`, obtained from
/// `Provider::GetLeafReceiver()` (ICP 0034, `docs/leaf-concentrator-design.md`
/// §3.2). Section numbers in the comments below refer to that design.
///
/// **Experimental in v1.2.** The receiver is compiled in only with
/// `MICROTEL_WITH_CONCENTRATOR=ON` (default OFF, design §6.2). The types here
/// are always declared; in a build without the option, and in any provider
/// built without `SdkBuilder::WithLeafReceiver`, `GetLeafReceiver()` returns a
/// receiver whose `Ingest` answers `IngestStatus::Disabled`.

namespace microtel
{

/// @brief The time mode a leaf payload declares (design §5).
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

/// @brief What `LeafReceiver::Ingest` did with one payload (§3.3).
enum class IngestStatus : std::uint8_t
{
    Accepted = 0,           ///< every span entered the pipeline
    PartiallyAccepted = 1,  ///< decoded, but the pipeline dropped some spans
    Malformed = 2,          ///< not decodable, or failed validation (§3.4)
    TooLarge = 3,           ///< over a size limit (§3.7)
    UnknownLeaf = 4,        ///< leaf not configured and unknown_leaf = reject
    ShutDown = 5,           ///< Provider shut down
    Disabled = 6,           ///< concentrator not enabled or not compiled in
    OutOfMemory = 7,        ///< allocation failed while processing (§3.3)
};

/// @brief The result of one `Ingest` call. A small value; reporting a failure
///        never allocates.
///
/// For an accepted payload the three counts add up to the number of spans it
/// decoded to. For a rejected one they are all zero.
struct IngestResult
{
    IngestStatus status = IngestStatus::Disabled;
    std::uint32_t spans_accepted = 0;  ///< entered the span processor
    std::uint32_t spans_sampled_out = 0;
    std::uint32_t spans_dropped = 0;  ///< dropped by limits or the processor
};

/// @brief Counters that describe the receiver, not the export pipeline.
///
/// Drops of whole payloads are also counted on the Provider's
/// `HealthSnapshot::drop_counters` (`leaf_payload_malformed`,
/// `leaf_payload_too_large`, `leaf_unknown`, `post_shutdown`), so they show in
/// `GetExporterHealth()` whether or not the caller reads `IngestResult`.
struct LeafReceiverStats
{
    std::uint64_t payloads_accepted = 0;
    std::uint64_t payloads_rejected = 0;
    std::uint64_t leaves_tracked = 0;  ///< current size of the leaf table
    std::uint64_t leaves_evicted = 0;
    std::uint64_t leaf_reported_drops = 0;          ///< sum of microtel.leaf.dropped_* (§1.7)
    std::uint64_t time_fallbacks = 0;               ///< sync-relative payloads re-anchored (§5.3)
    std::uint64_t payloads_out_of_memory = 0;       ///< §3.3
    std::uint64_t resource_attributes_dropped = 0;  ///< over max_leaf_resource_bytes (§4.5)
    std::uint64_t leaf_id_conflicts = 0;            ///< payload declared a different id (§4.4)
};

/// @brief What the receiver does with a leaf that has no configuration (§4.4).
enum class UnknownLeafPolicy : std::uint8_t
{
    /// Process it with the fleet defaults, its own Resource and its id.
    Accept = 0,
    /// Reject its payloads as `IngestStatus::UnknownLeaf`, counted `leaf_unknown`.
    Reject = 1,
};

/// @brief Configuration for one leaf (§4.3).
struct LeafConfig
{
    /// Unset: the receiver's `default_time_mode`. Set: the payload's declared
    /// mode must be this one or `ConcentratorStamped` (§5.1).
    std::optional<LeafTimeMode> time_mode;
    /// Merged above the leaf's own Resource and below its id (§4.4).
    std::vector<KeyValue> resource;
};

/// @brief Looks up the configuration of a leaf the receiver has not seen yet
///        (§4.3): for fleets whose per-device table lives in a database or an
///        inventory service rather than in `LeafReceiverOptions::leaves`.
///
/// Called the first time a leaf id is seen, and again after the leaf's entry
/// is evicted from the leaf table (§4.5). It runs on the `Ingest` caller's
/// thread with no microtel lock held, so it may block, but every `Ingest` for
/// a new leaf waits for it. Two threads that race on a new leaf may both call
/// it; the first answer is kept.
///
/// `std::nullopt` means "not configured", which `unknown_leaf` then governs.
/// An answer sits above the leaf's static entry in `leaves`, per key. A
/// `microtel.leaf.*` key or the `leaf_id_attribute` key in the answer's
/// Resource is ignored, and keys that would take the configured Resource over
/// `max_leaf_resource_bytes` are dropped and counted in
/// `LeafReceiverStats::resource_attributes_dropped`. A resolver that throws is
/// treated as having answered `std::nullopt`.
///
/// @threadsafety Must be safe to call concurrently from several threads.
using LeafConfigResolver = std::function<std::optional<LeafConfig>(std::string_view leaf_id)>;

/// @brief Options for `SdkBuilder::WithLeafReceiver` (§4.3), and the
///        `[concentrator]` TOML table and `MICROTEL_CONCENTRATOR_*` variables
///        that set the same fields (§4.2, `docs/configuration.md` §3.14).
///
/// Every limit is validated by `SdkBuilder::Build()`; a value it rejects fails
/// the build with `ConfigError::Kind::InvalidValue`.
struct LeafReceiverOptions
{
    bool enabled = true;  ///< WithLeafReceiver implies enabled
    /// Payloads larger than this are rejected before decode (§3.7).
    std::uint32_t max_payload_bytes = 64U * 1024U;
    /// Payloads decoding to more spans than this are rejected (§3.7).
    std::uint32_t max_spans_per_payload = 512;
    /// Bound on the leaf table; the least recently seen entry is evicted (§4.5).
    std::uint32_t max_leaves = 1024;
    /// Bound on one leaf's resolved Resource, keys plus values (§4.5).
    std::uint32_t max_leaf_resource_bytes = 2U * 1024U;
    /// A leaf not seen for longer than this loses its table entry, checked
    /// when an entry is inserted (§4.5). Must be positive.
    std::chrono::seconds leaf_idle_timeout{3600};
    UnknownLeafPolicy unknown_leaf = UnknownLeafPolicy::Accept;
    /// The Resource key the leaf id is exported as, and the payload key the id
    /// is read from when `IngestRequest::leaf_id` is empty. `""` disables both
    /// (§4.1).
    std::string leaf_id_attribute = "device.id";
    std::optional<LeafTimeMode> default_time_mode;  ///< unset: auto
    /// A sync-relative payload whose last clock sync is older than this is
    /// corrected as concentrator-stamped instead (§5.3). Must be positive.
    std::chrono::seconds max_sync_age{3600};
    /// A sync-relative payload whose encode time is further than this from the
    /// receive time is corrected as concentrator-stamped instead (§5.3). Must
    /// be positive.
    std::chrono::seconds max_clock_skew{300};
    /// How long a boot-relative sample counts towards the leaf's anchor
    /// (§5.4). Must be positive.
    std::chrono::seconds boot_anchor_window{600};
    /// Fills gaps in every leaf's Resource; the lowest layer (§4.4).
    std::vector<KeyValue> leaf_defaults_resource;
    /// Per-leaf configuration, keyed by leaf id. A leaf listed here is
    /// "configured" for the purposes of `unknown_leaf`.
    std::vector<std::pair<std::string, LeafConfig>> leaves;
    /// Optional; consulted for every leaf the table does not hold (§4.3).
    LeafConfigResolver resolver;
};

/// @brief Receives OTLP trace payloads from leaves and feeds their spans into
///        the Provider's pipeline (§3).
///
/// Obtained from `Provider::GetLeafReceiver()`. microtel opens no socket: the
/// application owns the transport and hands each payload to `Ingest`.
class LeafReceiver
{
public:
    LeafReceiver() noexcept = default;
    virtual ~LeafReceiver() noexcept = default;

    LeafReceiver(const LeafReceiver&) = delete;
    LeafReceiver& operator=(const LeafReceiver&) = delete;
    LeafReceiver(LeafReceiver&&) = delete;
    LeafReceiver& operator=(LeafReceiver&&) = delete;

    /// @brief Decode, validate and enqueue one leaf payload.
    ///
    /// Runs on the caller's thread and never blocks (§3.5). A payload that
    /// fails a size limit, decoding or validation is rejected whole (§3.4).
    /// An allocation failure is reported as `IngestStatus::OutOfMemory`, never
    /// thrown (§3.3).
    ///
    /// @threadsafety Thread-safe; may be called concurrently from any thread.
    [[nodiscard]] virtual IngestResult Ingest(const IngestRequest& request) noexcept = 0;

    /// @brief A snapshot of the receiver's counters.
    /// @threadsafety Thread-safe.
    [[nodiscard]] virtual LeafReceiverStats Stats() const noexcept = 0;
};

}  // namespace microtel
