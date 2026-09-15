// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#include "sdk/sdk_span.hpp"

#include "microtel/attribute.hpp"
#include "microtel/internal/batch.hpp"
#include "microtel/trace.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <new>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace microtel::sdk
{
namespace
{

/// @brief Longest prefix of @p s that is at most @p limit bytes and does not
///        split a UTF-8 code point.
///
/// `attribute_value_length_limit` is a **byte** budget (`memory-model.md` §7 is
/// a memory table, and the value that lands on the wire is bytes). The cut
/// backs up over UTF-8 continuation bytes because an OTLP attribute is a
/// proto3 `string`, which must hold valid UTF-8: a cut through a multi-byte
/// sequence would turn one truncated attribute into a whole batch the
/// collector refuses to parse. Backing up loses at most three more bytes.
///
/// @pre `limit < s.size()` — the caller has already decided to truncate.
[[nodiscard]] std::size_t Utf8SafePrefix(std::string_view s, std::size_t limit) noexcept
{
    constexpr auto kContinuationMask = static_cast<unsigned char>(0xC0U);
    constexpr auto kContinuationBits = static_cast<unsigned char>(0x80U);

    std::size_t cut = limit;
    while (cut > 0 && (static_cast<unsigned char>(s[cut]) & kContinuationMask) == kContinuationBits)
    {
        --cut;
    }
    return cut;
}

/// @brief Truncate every string in @p value to `limit` bytes.
///
/// @return How many strings lost bytes — 0 or 1 for a scalar, one per
///         oversized element for a string array. Each is one
///         `AttributeValueTruncated`: each element is a value, and an operator
///         reading the counter wants the number of values that lost data, not
///         the number of attributes that contained one.
///
/// Only the two string alternatives of `AttributeValue` can exceed a length
/// budget; bool / int64 / double and their arrays are fixed-width and pass
/// through untouched. Shrinking a `std::string` in place never allocates, so
/// this is safe to run inside the `noexcept` hot path.
[[nodiscard]] std::uint64_t TruncateStrings(AttributeValue& value, std::size_t limit) noexcept
{
    if (auto* const s = std::get_if<std::string>(&value); s != nullptr)
    {
        if (s->size() <= limit)
        {
            return 0;
        }
        s->resize(Utf8SafePrefix(*s, limit));
        return 1;
    }

    auto* const arr = std::get_if<std::vector<std::string>>(&value);
    if (arr == nullptr)
    {
        return 0;
    }
    std::uint64_t truncated = 0;
    for (auto& element : *arr)
    {
        if (element.size() > limit)
        {
            element.resize(Utf8SafePrefix(element, limit));
            ++truncated;
        }
    }
    return truncated;
}

/// @brief Copy @p kv with its string values clipped to @p limit, counting the
///        truncations against @p diag.
[[nodiscard]] KeyValue ClipAttribute(const KeyValue& kv,
                                     std::size_t limit,
                                     internal::IDiagnosticsSink* diag)
{
    KeyValue copy = kv;
    const std::uint64_t truncated = TruncateStrings(copy.value, limit);
    if (truncated > 0 && diag != nullptr)
    {
        diag->RecordDrop(DropReason::AttributeValueTruncated, truncated);
    }
    return copy;
}

/// @brief Run @p mutate, discarding the change if it cannot be allocated.
///
/// Every `Span` method is `noexcept` (`docs/error-model.md` §2.2, LOCKED), yet
/// each one grows a `std::string` or a `std::vector`. An allocation failure in
/// a `noexcept` frame calls `std::terminate` — so before this, a telemetry
/// library could take the host process down at exactly the moment the host was
/// already under memory pressure. §2.2's actual requirement is to drop the
/// field and return silently, which is what this does.
///
/// Each caller mutates either a local it then discards, or a container whose
/// own strong/basic guarantee leaves the record intact on throw — so a failed
/// mutation drops one field or one event, never a half-written span.
///
/// Catches `std::exception` rather than `std::bad_alloc` alone: `std::string`
/// and `std::vector` also throw `std::length_error`, and *any* exception
/// escaping a `noexcept` frame terminates. Matches the house pattern in
/// `otlp_exporter.cpp` and `sdk_builder.cpp`.
///
/// @note §2.2 also requires incrementing a drop counter. There is no
///       `DropReason` for allocation failure, and adding one is explicitly
///       ICP-gated (`provider.hpp`), so the count is deferred rather than
///       mapped onto an unrelated reason. See issue #134.
/// @brief Build one event. May throw; callers run it inside `DropOnBadAlloc`.
[[nodiscard]] internal::SpanEvent BuildEvent(std::string_view name,
                                             AttributeSpan attributes,
                                             std::chrono::system_clock::time_point timestamp,
                                             SpanLimitOptions limits,
                                             internal::IDiagnosticsSink* diag)
{
    internal::SpanEvent ev;
    ev.name = std::string{name};
    ev.timestamp = (timestamp == std::chrono::system_clock::time_point{})
                       ? std::chrono::system_clock::now()
                       : timestamp;
    for (const auto& kv : attributes)
    {
        if (ev.attributes.size() >= limits.event_attribute_count_limit)
        {
            break;
        }
        ev.attributes.push_back(ClipAttribute(kv, limits.attribute_value_length_limit, diag));
    }
    // Counted after the fact rather than inside the loop: the event is kept,
    // so the loss is the surplus attributes, not the event.
    if (attributes.size() > ev.attributes.size() && diag != nullptr)
    {
        diag->RecordDrop(DropReason::EventAttributeLimit, attributes.size() - ev.attributes.size());
    }
    return ev;
}

/// @brief Build one link. May throw; callers run it inside `DropOnBadAlloc`.
[[nodiscard]] internal::SpanLink BuildLink(const SpanContext& linked_context,
                                           AttributeSpan attributes,
                                           SpanLimitOptions limits,
                                           internal::IDiagnosticsSink* diag)
{
    internal::SpanLink lk;
    lk.linked_context = linked_context;
    for (const auto& kv : attributes)
    {
        if (lk.attributes.size() >= limits.link_attribute_count_limit)
        {
            break;
        }
        lk.attributes.push_back(ClipAttribute(kv, limits.attribute_value_length_limit, diag));
    }
    if (attributes.size() > lk.attributes.size() && diag != nullptr)
    {
        diag->RecordDrop(DropReason::LinkAttributeLimit, attributes.size() - lk.attributes.size());
    }
    return lk;
}

template <typename Mutate>
void DropOnBadAlloc(Mutate mutate) noexcept
{
    try
    {
        mutate();
    }
    // Dropping the field IS the documented behaviour (error-model.md §2.2);
    // there is nothing to handle, and rethrowing from a noexcept frame is the
    // terminate this guard exists to prevent.
    // NOLINTNEXTLINE(bugprone-empty-catch)
    catch (const std::exception&)
    {
    }
}

}  // namespace


// NOLINTNEXTLINE(readability-function-size) — 10-param constructor imposed by SdkSpan API contract
SdkSpan::SdkSpan(SpanContext context,
                 SpanContext parent_context,
                 std::string_view name,
                 SpanKind kind,
                 std::chrono::system_clock::time_point start_time,
                 internal::ISpanProcessor* processor,
                 std::shared_ptr<const Resource> resource,
                 internal::InstrumentationScope scope,
                 SpanLimitOptions limits,
                 internal::IDiagnosticsSink* diagnostics) noexcept
    : m_processor(processor),
      m_resource(std::move(resource)),
      m_scope(std::move(scope)),
      m_limits(limits),
      m_diagnostics(diagnostics)
{
    m_record.context = context;
    m_record.parent_context = parent_context;
    DropOnBadAlloc([this, &name] { m_record.name = std::string{name}; });
    m_record.kind = kind;
    m_record.start_time = (start_time == std::chrono::system_clock::time_point{})
                              ? std::chrono::system_clock::now()
                              : start_time;
}

SdkSpan::~SdkSpan() noexcept
{
    End();
}

void SdkSpan::RecordDropped(DropReason reason, std::uint64_t n) const noexcept
{
    if (m_diagnostics != nullptr)
    {
        m_diagnostics->RecordDrop(reason, n);
    }
}

SpanContext SdkSpan::GetContext() const noexcept
{
    return m_record.context;
}

bool SdkSpan::IsSampled() const noexcept
{
    return true;
}

void SdkSpan::SetAttribute(std::string_view key, AttributeValue value) noexcept
{
    if (m_ended.load(std::memory_order_relaxed))
    {
        return;
    }
    if (m_record.attributes.size() >= m_limits.attribute_count_limit)
    {
        RecordDropped(DropReason::SpanAttributeLimit);
        return;
    }
    const std::uint64_t truncated = TruncateStrings(value, m_limits.attribute_value_length_limit);
    if (truncated > 0)
    {
        RecordDropped(DropReason::AttributeValueTruncated, truncated);
    }
    DropOnBadAlloc(
        [this, &key, &value]
        { m_record.attributes.push_back({.key = std::string{key}, .value = std::move(value)}); });
}

void SdkSpan::AddEvent(std::string_view name,
                       AttributeSpan attributes,
                       std::chrono::system_clock::time_point timestamp) noexcept
{
    if (m_ended.load(std::memory_order_relaxed))
    {
        return;
    }
    if (m_record.events.size() >= m_limits.event_count_limit)
    {
        RecordDropped(DropReason::SpanEventLimit);
        return;
    }
    // All-or-nothing: BuildEvent's local is discarded if it cannot be
    // completed, so the record never holds a half-written event.
    DropOnBadAlloc(
        [this, &name, &attributes, &timestamp] {
            m_record.events.push_back(
                BuildEvent(name, attributes, timestamp, m_limits, m_diagnostics));
        });
}

void SdkSpan::AddLink(const SpanContext& linked_context, AttributeSpan attributes) noexcept
{
    if (m_ended.load(std::memory_order_relaxed))
    {
        return;
    }
    if (m_record.links.size() >= m_limits.link_count_limit)
    {
        RecordDropped(DropReason::SpanLinkLimit);
        return;
    }
    // All-or-nothing, same reasoning as AddEvent.
    DropOnBadAlloc(
        [this, &linked_context, &attributes] {
            m_record.links.push_back(
                BuildLink(linked_context, attributes, m_limits, m_diagnostics));
        });
}

void SdkSpan::SetStatus(StatusCode code, std::string_view description) noexcept
{
    if (m_ended.load(std::memory_order_relaxed))
    {
        return;
    }
    // Ok overrides anything; Error overrides Unset only; Unset never overrides.
    if (code == StatusCode::Ok)
    {
        m_record.status_code = StatusCode::Ok;
        m_record.status_description.clear();
        return;
    }
    if (code == StatusCode::Error && m_record.status_code != StatusCode::Ok)
    {
        m_record.status_code = StatusCode::Error;
        DropOnBadAlloc([this, &description]
                       { m_record.status_description = std::string{description}; });
    }
}

void SdkSpan::UpdateName(std::string_view name) noexcept
{
    if (!m_ended.load(std::memory_order_relaxed))
    {
        DropOnBadAlloc([this, &name] { m_record.name = std::string{name}; });
    }
}

void SdkSpan::End(std::chrono::system_clock::time_point end_time) noexcept
{
    if (m_ended.exchange(true, std::memory_order_acq_rel))
    {
        return;
    }
    m_record.end_time = (end_time == std::chrono::system_clock::time_point{})
                            ? std::chrono::system_clock::now()
                            : end_time;
    m_processor->OnEnd(std::move(m_record), m_scope);
}

}  // namespace microtel::sdk
