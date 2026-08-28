// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#include "sdk/sdk_span.hpp"

#include "microtel/attribute.hpp"
#include "microtel/internal/batch.hpp"
#include "microtel/trace.hpp"

#include <chrono>
#include <cstddef>
#include <new>
#include <string>
#include <string_view>
#include <utility>

namespace microtel::sdk
{
namespace
{

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
                                             std::size_t attribute_limit)
{
    internal::SpanEvent ev;
    ev.name = std::string{name};
    ev.timestamp = (timestamp == std::chrono::system_clock::time_point{})
                       ? std::chrono::system_clock::now()
                       : timestamp;
    for (const auto& kv : attributes)
    {
        if (ev.attributes.size() >= attribute_limit)
        {
            break;
        }
        ev.attributes.push_back(kv);
    }
    return ev;
}

/// @brief Build one link. May throw; callers run it inside `DropOnBadAlloc`.
[[nodiscard]] internal::SpanLink BuildLink(const SpanContext& linked_context,
                                           AttributeSpan attributes,
                                           std::size_t attribute_limit)
{
    internal::SpanLink lk;
    lk.linked_context = linked_context;
    for (const auto& kv : attributes)
    {
        if (lk.attributes.size() >= attribute_limit)
        {
            break;
        }
        lk.attributes.push_back(kv);
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


// NOLINTNEXTLINE(readability-function-size) — 9-param constructor imposed by SdkSpan API contract
SdkSpan::SdkSpan(SpanContext context,
                 SpanContext parent_context,
                 std::string_view name,
                 SpanKind kind,
                 std::chrono::system_clock::time_point start_time,
                 internal::ISpanProcessor* processor,
                 std::shared_ptr<const Resource> resource,
                 internal::InstrumentationScope scope,
                 SpanLimitOptions limits) noexcept
    : m_processor(processor),
      m_resource(std::move(resource)),
      m_scope(std::move(scope)),
      m_limits(limits)
{
    m_record.context = context;
    m_record.parent_context = parent_context;
    DropOnBadAlloc([&] { m_record.name = std::string{name}; });
    m_record.kind = kind;
    m_record.start_time = (start_time == std::chrono::system_clock::time_point{})
                              ? std::chrono::system_clock::now()
                              : start_time;
}

SdkSpan::~SdkSpan() noexcept
{
    End();
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
        return;
    }
    DropOnBadAlloc(
        [&]
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
        return;
    }
    // All-or-nothing: BuildEvent's local is discarded if it cannot be
    // completed, so the record never holds a half-written event.
    DropOnBadAlloc(
        [&]
        {
            m_record.events.push_back(
                BuildEvent(name, attributes, timestamp, m_limits.event_attribute_count_limit));
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
        return;
    }
    // All-or-nothing, same reasoning as AddEvent.
    DropOnBadAlloc(
        [&]
        {
            m_record.links.push_back(
                BuildLink(linked_context, attributes, m_limits.link_attribute_count_limit));
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
        DropOnBadAlloc([&] { m_record.status_description = std::string{description}; });
    }
}

void SdkSpan::UpdateName(std::string_view name) noexcept
{
    if (!m_ended.load(std::memory_order_relaxed))
    {
        DropOnBadAlloc([&] { m_record.name = std::string{name}; });
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
    m_processor->OnEnd(std::move(m_record));
}

}  // namespace microtel::sdk
