// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#include "sdk/leaf_receiver.hpp"

#include "microtel/attribute.hpp"
#include "microtel/internal/batch.hpp"
#include "microtel/internal/otlp_trace_decoder.hpp"
#include "microtel/internal/sampler.hpp"
#include "microtel/leaf_receiver.hpp"
#include "microtel/log_sink.hpp"
#include "microtel/provider.hpp"
#include "microtel/trace.hpp"

#include "common/internal_log.hpp"
#include "sdk/batch_span_processor.hpp"
#include "sdk/leaf_resource.hpp"
#include "sdk/leaf_table.hpp"
#include "sdk/leaf_time.hpp"
#include "sdk/span_limits.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace microtel::sdk
{
namespace
{

/// Decode depth (§3.7): room for the deepest legal path in the trace schema
/// (nine levels) and a stop to recursion bombs.
constexpr std::uint16_t kDecodeMaxDepth = 16;
/// Decode arena cap: `kArenaFactor * max_payload_bytes + kArenaFloor` (§3.7).
/// §3.7's first estimate was 4x. Measured against upb v29.4 when this landed,
/// legitimate payloads need 6-12x their wire size, up to ~15x for dense
/// numeric arrays, plus about 2 KiB of fixed arena overhead that dominates a
/// small payload. 16x plus 16 KiB covers the worst measured case with margin;
/// otlp_trace_decoder_test pins it.
constexpr std::size_t kArenaFactor = 16;
constexpr std::size_t kArenaFloor = std::size_t{16} * 1024U;

/// The string value of @p key in @p attrs, if it is there and a string.
[[nodiscard]] const std::string* StringValue(const std::vector<KeyValue>& attrs,
                                             std::string_view key) noexcept
{
    const auto it = std::ranges::find(attrs, key, &KeyValue::key);
    return it == attrs.end() ? nullptr : std::get_if<std::string>(&it->value);
}

[[nodiscard]] bool Declares(const std::vector<KeyValue>& attrs, std::string_view key) noexcept
{
    return std::ranges::find(attrs, key, &KeyValue::key) != attrs.end();
}

/// §3.4's per-span rules that a decoded `SpanRecord` can still break.
[[nodiscard]] bool SpansValid(const internal::DecodedResourceSpans& rs) noexcept
{
    for (const auto& scope : rs.scopes)
    {
        const bool any_invalid = std::ranges::any_of(scope.spans,
                                                     [](const internal::SpanRecord& s)
                                                     {
                                                         return !s.context.trace_id.IsValid() ||
                                                                !s.context.span_id.IsValid() ||
                                                                s.end_time < s.start_time;
                                                     });
        if (any_invalid)
        {
            return false;
        }
    }
    return true;
}

[[nodiscard]] std::uint64_t NonNegative(std::optional<std::int64_t> v) noexcept
{
    return v.has_value() && *v > 0 ? static_cast<std::uint64_t>(*v) : 0;
}

/// @p d in nanoseconds, saturated rather than overflowing for durations
/// beyond about 292 years.
[[nodiscard]] std::int64_t SaturatingNs(std::chrono::seconds d) noexcept
{
    constexpr std::int64_t kNsPerSecond = 1'000'000'000;
    constexpr std::int64_t kMaxSeconds = std::numeric_limits<std::int64_t>::max() / kNsPerSecond;
    return std::clamp<std::int64_t>(d.count(), -kMaxSeconds, kMaxSeconds) * kNsPerSecond;
}

}  // namespace

/// One payload on its way through `Ingest`.
struct SdkLeafReceiver::Payload
{
    std::vector<internal::DecodedResourceSpans> decoded;
    std::vector<LeafTimeMode> modes;  ///< index-aligned with `decoded`
    std::string leaf_id;
    std::shared_ptr<const LeafSettings> settings;  ///< set by `Identify`
    std::int64_t received = 0;                     ///< `R`, Unix nanoseconds (§5.1)
    LeafTable::TimePoint now;                      ///< for the leaf table
};

SdkLeafReceiver::SdkLeafReceiver(LeafReceiverOptions options, LeafReceiverDeps deps)
    : m_options(std::move(options)),
      m_deps(std::move(deps)),
      m_table(m_options.max_leaves,
              std::chrono::nanoseconds{SaturatingNs(m_options.leaf_idle_timeout)})
{
    for (const auto& [id, config] : m_options.leaves)
    {
        m_leaves.insert_or_assign(id, config);
    }
}

IngestResult SdkLeafReceiver::Ingest(const IngestRequest& request) noexcept
{
    if (m_shut_down.load(std::memory_order_acquire))
    {
        // Not decoded, so its spans are not known: counted as a payload in
        // LeafReceiverStats, never as a post_shutdown record (ICP 0035).
        m_counters.payloads_post_shutdown.fetch_add(1, std::memory_order_relaxed);
        return IngestResult{.status = IngestStatus::ShutDown};
    }
    IngestResult result{.status = IngestStatus::Accepted};
    try
    {
        IngestOrThrow(request, result);
    }
    // The concentrator is short of memory, not the leaf at fault: reported as
    // OutOfMemory and never as leaf_payload_too_large, and not a DropReason
    // (ICP 0034). length_error is the same condition from a size computation.
    catch (const std::bad_alloc&)
    {
        RecordOutOfMemory();
        result.status = IngestStatus::OutOfMemory;
    }
    catch (const std::length_error&)
    {
        RecordOutOfMemory();
        result.status = IngestStatus::OutOfMemory;
    }
    return result;
}

void SdkLeafReceiver::IngestOrThrow(const IngestRequest& request, IngestResult& result)
{
    // R is read once, at the start of the call (§5.1).
    Payload payload{.decoded = {},
                    .modes = {},
                    .leaf_id = {},
                    .settings = nullptr,
                    .received = ReceivedNs(request),
                    .now = SteadyNow()};
    if (!Admit(request, payload, result) || !Identify(request, payload, result))
    {
        return;
    }
    Enqueue(payload, result);
    result.status =
        result.spans_dropped > 0 ? IngestStatus::PartiallyAccepted : IngestStatus::Accepted;
    m_counters.payloads_accepted.fetch_add(1, std::memory_order_relaxed);
}

bool SdkLeafReceiver::Admit(const IngestRequest& request, Payload& payload, IngestResult& result)
{
    if (request.payload.size() > m_options.max_payload_bytes)
    {
        result = Reject(IngestStatus::TooLarge, DropReason::LeafPayloadTooLarge);
        return false;
    }
    if (request.leaf_id.size() > kMaxLeafIdBytes)
    {
        result = Reject(IngestStatus::Malformed, DropReason::LeafPayloadMalformed);
        return false;
    }
    // A transport id is enough to refuse an unknown leaf before paying for
    // the decode, unless a resolver may yet configure it.
    if (!request.leaf_id.empty() && m_options.unknown_leaf == UnknownLeafPolicy::Reject &&
        !m_options.resolver && !IsConfigured(request.leaf_id))
    {
        result = Reject(IngestStatus::UnknownLeaf, DropReason::LeafUnknown);
        return false;
    }

    const internal::DecodeLimits limits{
        .max_spans = m_options.max_spans_per_payload,
        .max_depth = kDecodeMaxDepth,
        .max_arena_bytes = (kArenaFactor * m_options.max_payload_bytes) + kArenaFloor,
    };
    auto decoded = m_deps.decoder->Decode(request.payload, limits);
    if (!decoded.has_value())
    {
        result = decoded.error() == internal::DecodeFailure::TooLarge
                     ? Reject(IngestStatus::TooLarge, DropReason::LeafPayloadTooLarge)
                     : Reject(IngestStatus::Malformed, DropReason::LeafPayloadMalformed);
        return false;
    }
    payload.decoded = std::move(*decoded);

    // All or nothing (§3.4): every ResourceSpans must carry a valid wire
    // header and valid spans, or none of the payload is taken.
    payload.modes.reserve(payload.decoded.size());
    for (const auto& rs : payload.decoded)
    {
        const auto mode = CheckWireInfo(ReadWireInfo(rs.resource));
        if (!mode.has_value() || !SpansValid(rs))
        {
            break;
        }
        payload.modes.push_back(*mode);
    }
    if (payload.decoded.empty() || payload.modes.size() != payload.decoded.size())
    {
        result = Reject(IngestStatus::Malformed, DropReason::LeafPayloadMalformed);
        return false;
    }
    return true;
}

bool SdkLeafReceiver::Identify(const IngestRequest& request, Payload& payload, IngestResult& result)
{
    payload.leaf_id = SettleLeafId(request, payload);
    if (payload.leaf_id.empty() || payload.leaf_id.size() > kMaxLeafIdBytes)
    {
        result = Reject(IngestStatus::Malformed, DropReason::LeafPayloadMalformed);
        return false;
    }

    payload.settings = SettingsFor(payload.leaf_id, payload.now);
    if (!payload.settings->configured && m_options.unknown_leaf == UnknownLeafPolicy::Reject)
    {
        result = Reject(IngestStatus::UnknownLeaf, DropReason::LeafUnknown);
        return false;
    }
    if (!ModesAllowed(payload))
    {
        result = Reject(IngestStatus::Malformed, DropReason::LeafPayloadMalformed);
        return false;
    }
    CountIdConflict(request, payload);
    return true;
}

std::string SdkLeafReceiver::SettleLeafId(const IngestRequest& request,
                                          const Payload& payload) const
{
    if (!request.leaf_id.empty())
    {
        return std::string{request.leaf_id};
    }
    const std::string_view id_key = m_options.leaf_id_attribute;
    if (id_key.empty())
    {
        return {};
    }
    // No transport id: the first id the payload declares (§4.1).
    for (const auto& rs : payload.decoded)
    {
        if (const auto* const declared = StringValue(rs.resource, id_key); declared != nullptr)
        {
            return *declared;
        }
    }
    return {};
}

bool SdkLeafReceiver::ModesAllowed(const Payload& payload) const noexcept
{
    // §5.1: the leaf declares, the config constrains.
    const std::optional<LeafTimeMode> allowed = payload.settings->time_mode.has_value()
                                                    ? payload.settings->time_mode
                                                    : m_options.default_time_mode;
    return std::ranges::all_of(
        payload.modes, [&allowed](LeafTimeMode mode) { return TimeModeAllowed(mode, allowed); });
}

void SdkLeafReceiver::CountIdConflict(const IngestRequest& request, const Payload& payload) noexcept
{
    // The transport id wins; a payload that claims another is counted, which
    // is how an operator finds cloned or mis-flashed images (§4.4).
    const std::string_view id_key = m_options.leaf_id_attribute;
    if (request.leaf_id.empty() || id_key.empty())
    {
        return;
    }
    const bool conflict =
        std::ranges::any_of(payload.decoded,
                            [&payload, id_key](const internal::DecodedResourceSpans& rs)
                            {
                                const auto* const declared = StringValue(rs.resource, id_key);
                                return Declares(rs.resource, id_key) &&
                                       (declared == nullptr || *declared != payload.leaf_id);
                            });
    if (conflict)
    {
        m_counters.leaf_id_conflicts.fetch_add(1, std::memory_order_relaxed);
    }
}

std::shared_ptr<const LeafSettings> SdkLeafReceiver::SettingsFor(std::string_view leaf_id,
                                                                 LeafTable::TimePoint now)
{
    if (auto cached = m_table.Settings(leaf_id, now); cached != nullptr)
    {
        return cached;
    }
    // Resolved with no lock held; two threads racing on a new leaf may both
    // get here, and the table keeps the first answer (§3.5).
    auto fresh = std::make_shared<const LeafSettings>(ResolveSettings(leaf_id));
    return m_table.AdoptSettings(leaf_id, std::move(fresh), now);
}

LeafSettings SdkLeafReceiver::ResolveSettings(std::string_view leaf_id)
{
    LeafSettings settings;
    if (const auto it = m_leaves.find(leaf_id); it != m_leaves.end())
    {
        settings = LeafSettings{
            .configured = true, .time_mode = it->second.time_mode, .resource = it->second.resource};
    }
    const auto answer = AskResolver(leaf_id);
    if (!answer.has_value())
    {
        return settings;
    }
    // The resolver's answer sits above the static entry, per key (§4.3).
    settings.configured = true;
    if (answer->time_mode.has_value())
    {
        settings.time_mode = answer->time_mode;
    }
    const std::uint64_t dropped =
        MergeResolverResource(settings.resource,
                              answer->resource,
                              LeafResourceLayers{
                                  .defaults = &m_options.leaf_defaults_resource,
                                  .declared = nullptr,
                                  .configured = nullptr,
                                  .id_key = m_options.leaf_id_attribute,
                                  .leaf_id = leaf_id,
                                  .budget = m_options.max_leaf_resource_bytes,
                              });
    m_counters.resource_attributes_dropped.fetch_add(dropped, std::memory_order_relaxed);
    return settings;
}

std::optional<LeafConfig> SdkLeafReceiver::AskResolver(std::string_view leaf_id) const
{
    if (!m_options.resolver)
    {
        return std::nullopt;
    }
    try
    {
        return m_options.resolver(leaf_id);
    }
    catch (const std::bad_alloc&)
    {
        // Out of memory is the concentrator's condition, reported as such by
        // Ingest, not a resolver that failed to answer.
        throw;
    }
    catch (const std::exception& e)
    {
        internal::LogImpl(LogLevel::Warn,
                          std::string{"leaf receiver: the leaf config resolver threw ("} +
                              e.what() + "); the leaf is treated as not configured");
        return std::nullopt;
    }
}

std::shared_ptr<const Resource> SdkLeafReceiver::ResourceFor(const Payload& payload,
                                                             const std::vector<KeyValue>& declared)
{
    const std::uint64_t hash = HashDeclaredResource(declared);
    if (auto cached = m_table.Find(payload.leaf_id, hash, payload.now); cached != nullptr)
    {
        return cached;
    }
    // Resolved with no lock held; two threads racing on a new leaf may both
    // get here, and the table keeps the first (§3.5).
    auto resolved = ResolveLeafResource(LeafResourceLayers{
        .defaults = &m_options.leaf_defaults_resource,
        .declared = &declared,
        .configured = &payload.settings->resource,
        .id_key = m_options.leaf_id_attribute,
        .leaf_id = payload.leaf_id,
        .budget = m_options.max_leaf_resource_bytes,
    });
    m_counters.resource_attributes_dropped.fetch_add(resolved.attributes_dropped,
                                                     std::memory_order_relaxed);
    return m_table.Insert(payload.leaf_id, hash, std::move(resolved.resource), payload.now);
}

TimeCorrection SdkLeafReceiver::CorrectionFor(const Payload& payload,
                                              LeafTimeMode mode,
                                              const LeafWireInfo& info,
                                              bool& fell_back)
{
    // CheckWireInfo has made sure each mode's own attributes are present.
    if (mode == LeafTimeMode::SyncRelative)
    {
        const SyncLimits limits{.max_sync_age = SaturatingNs(m_options.max_sync_age),
                                .max_clock_skew = SaturatingNs(m_options.max_clock_skew)};
        if (auto trusted = SyncRelative(
                payload.received, info.encode_time.value_or(0), info.sync_age.value_or(0), limits))
        {
            return *trusted;
        }
        // A stale or wrong sync: corrected as concentrator-stamped (§5.3).
        fell_back = true;
    }
    else if (mode == LeafTimeMode::BootRelative)
    {
        const BootSample sample{
            .boot_id = info.boot_id.value_or(0),
            .offset = SaturatingSub(payload.received, info.encode_time.value_or(0)),
            .at = payload.received,
        };
        return TimeCorrection{
            .offset = m_table.UpdateBootAnchor(
                payload.leaf_id, sample, SaturatingNs(m_options.boot_anchor_window), payload.now),
            .fixed = std::nullopt};
    }
    return ConcentratorStamped(payload.received, info.encode_time);
}

void SdkLeafReceiver::Enqueue(Payload& payload, IngestResult& result)
{
    bool fell_back = false;
    for (std::size_t i = 0; i < payload.decoded.size(); ++i)
    {
        auto& rs = payload.decoded[i];
        const LeafWireInfo info = ReadWireInfo(rs.resource);
        m_counters.leaf_reported_drops.fetch_add(NonNegative(info.dropped_spans) +
                                                     NonNegative(info.dropped_items),
                                                 std::memory_order_relaxed);
        m_counters.resource_attributes_dropped.fetch_add(rs.dropped_resource_attributes,
                                                         std::memory_order_relaxed);
        RecordDrop(DropReason::SpanAttributeLimit, rs.dropped_span_attributes);

        const TimeCorrection correction =
            CorrectionFor(payload, payload.modes.at(i), info, fell_back);
        EnqueueResourceSpans(ResourceFor(payload, rs.resource), correction, rs, result);
    }
    if (fell_back)
    {
        m_counters.time_fallbacks.fetch_add(1, std::memory_order_relaxed);
    }
}

void SdkLeafReceiver::EnqueueResourceSpans(const std::shared_ptr<const Resource>& resource,
                                           const TimeCorrection& correction,
                                           internal::DecodedResourceSpans& rs,
                                           IngestResult& result) const noexcept
{
    for (auto& scope : rs.scopes)
    {
        for (auto& span : scope.spans)
        {
            EnqueueSpan(span, scope.scope, resource, correction, result);
        }
    }
}

void SdkLeafReceiver::EnqueueSpan(internal::SpanRecord& span,
                                  const internal::InstrumentationScope& scope,
                                  const std::shared_ptr<const Resource>& resource,
                                  const TimeCorrection& correction,
                                  IngestResult& result) const noexcept
{
    // §3.6 step 1: leaf timestamps become Unix times.
    ApplyTimeCorrection(span, correction);
    ApplySpanLimits(span, m_deps.span_limits, m_deps.diagnostics);

    // Every span is sampled as a root (§3.6): a leaf does no sampling, so
    // there is no parent decision to inherit, and a trace-id sampler then
    // decides the same for every span of a trace. As for an in-process root,
    // only the decision is used.
    const internal::SamplingContext ctx{
        .parent = {},
        .span_kind = span.kind,
        .span_name = span.name,
        .initial_attributes = span.attributes,
        .links = span.links,
        .trace_id = span.context.trace_id,
    };
    if (m_deps.sampler->ShouldSample(ctx).decision != internal::SamplingDecision::RecordAndSample)
    {
        ++result.spans_sampled_out;
        return;
    }
    span.context.trace_flags = TraceFlags{TraceFlags::kSampled};
    span.resource = resource;
    if (m_deps.batch_processor == nullptr)
    {
        m_deps.processor->OnEnd(std::move(span), scope);
        ++result.spans_accepted;
        return;
    }
    if (m_deps.batch_processor->Enqueue(std::move(span), scope))
    {
        ++result.spans_accepted;
        return;
    }
    ++result.spans_dropped;
}

IngestResult SdkLeafReceiver::Reject(IngestStatus status, DropReason reason) noexcept
{
    RecordDrop(reason, 1);
    m_counters.payloads_rejected.fetch_add(1, std::memory_order_relaxed);
    return IngestResult{.status = status};
}

std::int64_t SdkLeafReceiver::ReceivedNs(const IngestRequest& request) const noexcept
{
    std::chrono::system_clock::time_point r;
    if (request.received_at.has_value())
    {
        r = *request.received_at;
    }
    else
    {
        r = m_deps.clock != nullptr ? m_deps.clock->Now() : std::chrono::system_clock::now();
    }
    return std::chrono::duration_cast<std::chrono::nanoseconds>(r.time_since_epoch()).count();
}

LeafTable::TimePoint SdkLeafReceiver::SteadyNow() const noexcept
{
    return m_deps.steady_clock != nullptr ? m_deps.steady_clock->Now()
                                          : std::chrono::steady_clock::now();
}

bool SdkLeafReceiver::IsConfigured(std::string_view leaf_id) const
{
    return m_leaves.contains(leaf_id);
}

void SdkLeafReceiver::RecordDrop(DropReason reason, std::uint64_t n) const noexcept
{
    if (n > 0 && m_deps.diagnostics != nullptr)
    {
        m_deps.diagnostics->RecordDrop(reason, n);
    }
}

void SdkLeafReceiver::RecordOutOfMemory() noexcept
{
    const std::uint64_t count =
        m_counters.payloads_out_of_memory.fetch_add(1, std::memory_order_relaxed) + 1;
    // The internal log is not rate-limited yet (error-model.md §9.2), so this
    // limits itself: the 1st, 2nd, 4th, 8th ... failure is logged.
    if ((count & (count - 1)) == 0)
    {
        internal::LogImpl(LogLevel::Warn,
                          "leaf receiver: allocation failed while ingesting a payload; it was not "
                          "processed (payloads_out_of_memory in LeafReceiver::Stats)");
    }
}

void SdkLeafReceiver::MarkShutDown() noexcept
{
    m_shut_down.store(true, std::memory_order_release);
}

LeafReceiverStats SdkLeafReceiver::Stats() const noexcept
{
    return LeafReceiverStats{
        .payloads_accepted = m_counters.payloads_accepted.load(std::memory_order_relaxed),
        .payloads_rejected = m_counters.payloads_rejected.load(std::memory_order_relaxed),
        .leaves_tracked = m_table.Size(),
        .leaves_evicted = m_table.Evicted(),
        .leaf_reported_drops = m_counters.leaf_reported_drops.load(std::memory_order_relaxed),
        .time_fallbacks = m_counters.time_fallbacks.load(std::memory_order_relaxed),
        .payloads_out_of_memory = m_counters.payloads_out_of_memory.load(std::memory_order_relaxed),
        .resource_attributes_dropped =
            m_counters.resource_attributes_dropped.load(std::memory_order_relaxed),
        .leaf_id_conflicts = m_counters.leaf_id_conflicts.load(std::memory_order_relaxed),
        .payloads_post_shutdown = m_counters.payloads_post_shutdown.load(std::memory_order_relaxed),
    };
}

}  // namespace microtel::sdk
