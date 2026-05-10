// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#include "sdk/sdk_span.hpp"

#include "microtel/attribute.hpp"
#include "microtel/internal/batch.hpp"
#include "microtel/trace.hpp"

#include <chrono>
#include <string>
#include <string_view>
#include <utility>

namespace microtel::sdk
{

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
    m_record.name = std::string{name};
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
    m_record.attributes.push_back({.key = std::string{key}, .value = std::move(value)});
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
    internal::SpanEvent ev;
    ev.name = std::string{name};
    ev.timestamp = (timestamp == std::chrono::system_clock::time_point{})
                       ? std::chrono::system_clock::now()
                       : timestamp;
    for (const auto& kv : attributes)
    {
        if (ev.attributes.size() < m_limits.event_attribute_count_limit)
        {
            ev.attributes.push_back(kv);
        }
    }
    m_record.events.push_back(std::move(ev));
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
    internal::SpanLink lk;
    lk.linked_context = linked_context;
    for (const auto& kv : attributes)
    {
        if (lk.attributes.size() < m_limits.link_attribute_count_limit)
        {
            lk.attributes.push_back(kv);
        }
    }
    m_record.links.push_back(std::move(lk));
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
        m_record.status_description = std::string{description};
    }
}

void SdkSpan::UpdateName(std::string_view name) noexcept
{
    if (!m_ended.load(std::memory_order_relaxed))
    {
        m_record.name = std::string{name};
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
