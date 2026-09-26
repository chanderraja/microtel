// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/internal/batch.hpp"
#include "microtel/internal/clock.hpp"
#include "microtel/internal/diagnostics_sink.hpp"
#include "microtel/internal/otlp_trace_decoder.hpp"
#include "microtel/internal/processor.hpp"
#include "microtel/internal/sampler.hpp"
#include "microtel/leaf_receiver.hpp"
#include "microtel/provider.hpp"
#include "microtel/sdk_builder.hpp"

#include "sdk/leaf_resource.hpp"
#include "sdk/leaf_table.hpp"
#include "sdk/leaf_time.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace microtel::sdk
{

class BatchSpanProcessor;

/// @brief What the receiver borrows from its Provider.
struct LeafReceiverDeps
{
    /// Keeps `sampler`, `processor` and `diagnostics` alive for as long as the
    /// receiver lives, which may be past its Provider (the `TracePipeline`, as
    /// for a tracer; issue #285). Null when the caller guarantees it.
    std::shared_ptr<const void> owner;
    /// Borrowed. The Provider's sampler; every span is sampled as a root.
    internal::ISampler* sampler = nullptr;
    /// Borrowed. Where accepted spans go.
    internal::ISpanProcessor* processor = nullptr;
    /// Borrowed alias of `processor` when it is a `BatchSpanProcessor`, whose
    /// `Enqueue` says whether a span was queued; null otherwise, in which case
    /// every span handed to `OnEnd` is reported as accepted.
    BatchSpanProcessor* batch_processor = nullptr;
    /// Borrowed, or null to count nothing.
    internal::IDiagnosticsSink* diagnostics = nullptr;
    /// Owned. The OTLP decoder (upb in production, a mock or fake in tests).
    std::unique_ptr<internal::IOtlpTraceDecoder> decoder;
    /// The Provider's span limits, applied to every decoded span (§3.6).
    SpanLimitOptions span_limits;
    /// Borrowed. The wall clock read for `R` when a request carries no
    /// `received_at` (§5.1); null means `std::chrono::system_clock`.
    const internal::IClock* clock = nullptr;
    /// Borrowed. The clock the leaf table's idle timeout runs on (§4.5); null
    /// means `std::chrono::steady_clock`.
    const internal::ISteadyClock* steady_clock = nullptr;
};

/// @brief The concentrator's `LeafReceiver` (`docs/leaf-concentrator-design.md`
///        §3–§5; ICP 0034).
///
/// `Ingest`, on the caller's thread and never blocking: size limits, decode,
/// validation of the whole payload (any failure rejects all of it), leaf
/// identity, the leaf's settings (static `leaves` entry and resolver answer,
/// cached in the leaf table), the time-mode rule of §5.1, Resource resolution
/// through the leaf table, time correction (§5.2–§5.4), span limits, root
/// sampling, and `Enqueue` into the Provider's span processor with the leaf's
/// Resource on every record.
///
/// @threadsafety Thread-safe. `Ingest` and `Stats` may run concurrently from
///               any threads. The only lock is the leaf table's (§3.5).
class SdkLeafReceiver final : public microtel::LeafReceiver
{
public:
    /// @throws std::bad_alloc while copying the per-leaf configuration.
    SdkLeafReceiver(LeafReceiverOptions options, LeafReceiverDeps deps);

    ~SdkLeafReceiver() noexcept override = default;

    SdkLeafReceiver(const SdkLeafReceiver&) = delete;
    SdkLeafReceiver& operator=(const SdkLeafReceiver&) = delete;
    SdkLeafReceiver(SdkLeafReceiver&&) = delete;
    SdkLeafReceiver& operator=(SdkLeafReceiver&&) = delete;

    [[nodiscard]] IngestResult Ingest(const IngestRequest& request) noexcept override;

    [[nodiscard]] LeafReceiverStats Stats() const noexcept override;

    /// @brief Refuse every later payload as `IngestStatus::ShutDown`.
    ///
    /// Called by `SdkProvider::Shutdown`, and by its fork-child handler: a
    /// single release store, so it is async-signal-safe.
    void MarkShutDown() noexcept;

private:
    struct Payload;
    struct Counters
    {
        std::atomic<std::uint64_t> payloads_accepted{0};
        std::atomic<std::uint64_t> payloads_rejected{0};
        std::atomic<std::uint64_t> leaf_reported_drops{0};
        std::atomic<std::uint64_t> payloads_out_of_memory{0};
        std::atomic<std::uint64_t> resource_attributes_dropped{0};
        std::atomic<std::uint64_t> leaf_id_conflicts{0};
        std::atomic<std::uint64_t> time_fallbacks{0};
    };

    /// The body of `Ingest`; may throw `std::bad_alloc`. Fills @p result as
    /// it goes, so a failure part-way still reports what was enqueued.
    void IngestOrThrow(const IngestRequest& request, IngestResult& result);
    /// Decode and check the payload; on failure @p result says why.
    [[nodiscard]] bool Admit(const IngestRequest& request, Payload& payload, IngestResult& result);
    /// Leaf identity (§4.1), the unknown-leaf rule (§4.4) and the time-mode
    /// rule (§5.1), for a payload that decoded and passed its own checks.
    [[nodiscard]] bool Identify(const IngestRequest& request,
                                Payload& payload,
                                IngestResult& result);
    /// The leaf's settings, from the table or resolved (§4.3); the resolver
    /// runs with no lock held.
    [[nodiscard]] std::shared_ptr<const LeafSettings> SettingsFor(std::string_view leaf_id,
                                                                  LeafTable::TimePoint now);
    /// The static entry for @p leaf_id with the resolver's answer over it.
    [[nodiscard]] LeafSettings ResolveSettings(std::string_view leaf_id);
    /// The resolver's answer, or nullopt when there is no resolver or it
    /// threw anything but `std::bad_alloc`, which propagates.
    [[nodiscard]] std::optional<LeafConfig> AskResolver(std::string_view leaf_id) const;
    /// The leaf's Resource for one ResourceSpans, from the table or resolved.
    [[nodiscard]] std::shared_ptr<const Resource> ResourceFor(
        const Payload& payload, const std::vector<KeyValue>& declared);
    /// How one ResourceSpans' timestamps become Unix times (§5); updates the
    /// boot-relative anchor, and sets @p fell_back on a sync-relative fallback.
    [[nodiscard]] TimeCorrection CorrectionFor(const Payload& payload,
                                               LeafTimeMode mode,
                                               const LeafWireInfo& info,
                                               bool& fell_back);
    /// Limits, sampling and enqueue for every span of the payload.
    void Enqueue(Payload& payload, IngestResult& result);
    /// Limits, sampling and enqueue for every span of one ResourceSpans.
    void EnqueueResourceSpans(const std::shared_ptr<const Resource>& resource,
                              const TimeCorrection& correction,
                              internal::DecodedResourceSpans& rs,
                              IngestResult& result) const noexcept;
    /// Time correction, limits, sampling and enqueue for one span; moves it
    /// out when it is handed to the processor.
    void EnqueueSpan(internal::SpanRecord& span,
                     const internal::InstrumentationScope& scope,
                     const std::shared_ptr<const Resource>& resource,
                     const TimeCorrection& correction,
                     IngestResult& result) const noexcept;
    /// `R` in Unix nanoseconds: the request's `received_at`, else the clock.
    [[nodiscard]] std::int64_t ReceivedNs(const IngestRequest& request) const noexcept;
    /// Now, on the clock the leaf table's idle timeout runs on.
    [[nodiscard]] LeafTable::TimePoint SteadyNow() const noexcept;
    /// The leaf id: the transport's, else the first the payload declares
    /// (§4.1); empty when there is none.
    [[nodiscard]] std::string SettleLeafId(const IngestRequest& request,
                                           const Payload& payload) const;
    /// Whether every declared time mode is one the leaf's config allows (§5.1).
    [[nodiscard]] bool ModesAllowed(const Payload& payload) const noexcept;
    /// Count a payload that declares an id other than the transport's (§4.4).
    void CountIdConflict(const IngestRequest& request, const Payload& payload) noexcept;
    /// Count a rejected payload once, against @p reason.
    [[nodiscard]] IngestResult Reject(IngestStatus status, DropReason reason) noexcept;
    [[nodiscard]] bool IsConfigured(std::string_view leaf_id) const;
    void RecordDrop(DropReason reason, std::uint64_t n) const noexcept;
    void RecordOutOfMemory() noexcept;

    LeafReceiverOptions m_options;
    LeafReceiverDeps m_deps;
    /// The static `leaves`, keyed by id. Immutable after construction; read
    /// without a lock.
    std::unordered_map<std::string, LeafConfig, TransparentStringHash, std::equal_to<>> m_leaves;
    LeafTable m_table;
    Counters m_counters;
    std::atomic<bool> m_shut_down{false};
};

}  // namespace microtel::sdk
