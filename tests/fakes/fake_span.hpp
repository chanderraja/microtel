// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/span.hpp"
#include "microtel/trace.hpp"

#include <chrono>
#include <string>
#include <utility>
#include <vector>

namespace microtel::testing
{

/// @brief Fake `microtel::Span` that records every mutation for inspection.
///
/// Tests configure `context` / `sampled` up front and assert on the recorded
/// calls afterwards. Single-threaded use only (no internal locking).
///
/// Attribute spans are copied into owned vectors at call time — the borrowed
/// `AttributeSpan` contract means the caller's storage may vanish after the
/// call returns, exactly what these recordings must survive.
class FakeSpan : public microtel::Span
{
public:
    struct RecordedEvent
    {
        std::string name;
        std::vector<microtel::KeyValue> attributes;
        std::chrono::system_clock::time_point timestamp;
    };

    struct RecordedLink
    {
        microtel::SpanContext context;
        std::vector<microtel::KeyValue> attributes;
    };

    struct RecordedStatus
    {
        microtel::StatusCode code = microtel::StatusCode::Unset;
        std::string description;
    };

    microtel::SpanContext context;
    bool sampled = true;

    std::vector<microtel::KeyValue> attributes;
    std::vector<RecordedEvent> events;
    std::vector<RecordedLink> links;
    std::vector<RecordedStatus> statuses;
    std::vector<std::string> names;
    std::vector<std::chrono::system_clock::time_point> end_calls;

    [[nodiscard]] microtel::SpanContext GetContext() const noexcept override
    {
        return context;
    }

    [[nodiscard]] bool IsSampled() const noexcept override
    {
        return sampled;
    }

    void SetAttribute(std::string_view key, microtel::AttributeValue value) noexcept override
    {
        attributes.push_back({.key = std::string{key}, .value = std::move(value)});
    }

    void AddEvent(std::string_view name,
                  microtel::AttributeSpan event_attributes,
                  std::chrono::system_clock::time_point timestamp) noexcept override
    {
        events.push_back({.name = std::string{name},
                          .attributes = {event_attributes.begin(), event_attributes.end()},
                          .timestamp = timestamp});
    }

    void AddLink(const microtel::SpanContext& linked_context,
                 microtel::AttributeSpan link_attributes) noexcept override
    {
        links.push_back({.context = linked_context,
                         .attributes = {link_attributes.begin(), link_attributes.end()}});
    }

    void SetStatus(microtel::StatusCode code, std::string_view description) noexcept override
    {
        statuses.push_back({.code = code, .description = std::string{description}});
    }

    void UpdateName(std::string_view name) noexcept override
    {
        names.emplace_back(name);
    }

    void End(std::chrono::system_clock::time_point end_time) noexcept override
    {
        end_calls.push_back(end_time);
    }
};

}  // namespace microtel::testing
