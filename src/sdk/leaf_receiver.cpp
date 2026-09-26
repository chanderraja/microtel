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
#include "sdk/span_limits.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
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

}  // namespace

/// One payload on its way through `Ingest`.
struct SdkLeafReceiver::Payload
{
    std::vector<internal::DecodedResourceSpans> decoded;
    std::vector<LeafTimeMode> modes;  ///< index-aligned with `decoded`
    std::string leaf_id;
    const LeafConfig* config = nullptr;  ///< null for an unconfigured leaf
};

SdkLeafReceiver::SdkLeafReceiver(LeafReceiverOptions options, LeafReceiverDeps deps)
    : m_options(std::move(options)), m_deps(std::move(deps)), m_table(m_options.max_leaves)
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
        // Counted per payload: it is not decoded, so its spans are not known.
        return Reject(IngestStatus::ShutDown, DropReason::PostShutdown);
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
    Payload payload;
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
    // the decode.
    if (!request.leaf_id.empty() && m_options.unknown_leaf == UnknownLeafPolicy::Reject &&
        !IsConfigured(request.leaf_id))
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

    const auto it = m_leaves.find(std::string_view{payload.leaf_id});
    payload.config = it == m_leaves.end() ? nullptr : &it->second;
    if (payload.config == nullptr && m_options.unknown_leaf == UnknownLeafPolicy::Reject)
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
    const bool configured = payload.config != nullptr && payload.config->time_mode.has_value();
    const std::optional<LeafTimeMode> allowed =
        configured ? payload.config->time_mode : m_options.default_time_mode;
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

std::shared_ptr<const Resource> SdkLeafReceiver::ResourceFor(const Payload& payload,
                                                             const std::vector<KeyValue>& declared)
{
    const std::uint64_t hash = HashDeclaredResource(declared);
    if (auto cached = m_table.Find(payload.leaf_id, hash); cached != nullptr)
    {
        return cached;
    }
    // Resolved with no lock held; two threads racing on a new leaf may both
    // get here, and the table keeps the first (§3.5).
    auto resolved = ResolveLeafResource(LeafResourceLayers{
        .defaults = &m_options.leaf_defaults_resource,
        .declared = &declared,
        .configured = payload.config != nullptr ? &payload.config->resource : nullptr,
        .id_key = m_options.leaf_id_attribute,
        .leaf_id = payload.leaf_id,
        .budget = m_options.max_leaf_resource_bytes,
    });
    m_counters.resource_attributes_dropped.fetch_add(resolved.attributes_dropped,
                                                     std::memory_order_relaxed);
    return m_table.Insert(payload.leaf_id, hash, std::move(resolved.resource));
}

void SdkLeafReceiver::Enqueue(Payload& payload, IngestResult& result)
{
    for (auto& rs : payload.decoded)
    {
        const LeafWireInfo info = ReadWireInfo(rs.resource);
        m_counters.leaf_reported_drops.fetch_add(NonNegative(info.dropped_spans) +
                                                     NonNegative(info.dropped_items),
                                                 std::memory_order_relaxed);
        m_counters.resource_attributes_dropped.fetch_add(rs.dropped_resource_attributes,
                                                         std::memory_order_relaxed);
        RecordDrop(DropReason::SpanAttributeLimit, rs.dropped_span_attributes);

        EnqueueResourceSpans(ResourceFor(payload, rs.resource), rs, result);
    }
}

void SdkLeafReceiver::EnqueueResourceSpans(const std::shared_ptr<const Resource>& resource,
                                           internal::DecodedResourceSpans& rs,
                                           IngestResult& result) const noexcept
{
    for (auto& scope : rs.scopes)
    {
        for (auto& span : scope.spans)
        {
            EnqueueSpan(span, scope.scope, resource, result);
        }
    }
}

void SdkLeafReceiver::EnqueueSpan(internal::SpanRecord& span,
                                  const internal::InstrumentationScope& scope,
                                  const std::shared_ptr<const Resource>& resource,
                                  IngestResult& result) const noexcept
{
    // Time correction (§5) is applied here once the time modes land; until
    // then timestamps pass through as the leaf wrote them.
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
        .time_fallbacks = 0,
        .payloads_out_of_memory = m_counters.payloads_out_of_memory.load(std::memory_order_relaxed),
        .resource_attributes_dropped =
            m_counters.resource_attributes_dropped.load(std::memory_order_relaxed),
        .leaf_id_conflicts = m_counters.leaf_id_conflicts.load(std::memory_order_relaxed),
    };
}

}  // namespace microtel::sdk
