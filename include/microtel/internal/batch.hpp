// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/attribute.hpp"
#include "microtel/resource.hpp"
#include "microtel/trace.hpp"

#include <chrono>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace microtel::internal
{

/// @brief Identifier for the instrumentation library that produced a span.
struct InstrumentationScope
{
    std::string name;
    std::string version;
};

/// @brief Worker-thread shape of a completed span.
///
/// Owns its attribute / event / link buffers. Copies of borrowed views taken
/// at `End()` time are realised here. Pre-`End()`, the active `Span` object
/// holds the same fields in a hot-path-optimised representation.
struct SpanRecord
{
    SpanContext context;
    SpanContext parent_context;  ///< invalid if root span
    std::string name;
    SpanKind kind = SpanKind::Internal;
    StatusCode status_code = StatusCode::Unset;
    std::string status_description;
    std::chrono::system_clock::time_point start_time;
    std::chrono::system_clock::time_point end_time;
    std::vector<KeyValue> attributes;
    std::vector<class SpanEvent> events;
    std::vector<class SpanLink> links;
};

/// @brief A timestamped event attached to a span.
class SpanEvent
{
public:
    std::string name;
    std::chrono::system_clock::time_point timestamp;
    std::vector<KeyValue> attributes;
};

/// @brief A link from a span to another span context.
class SpanLink
{
public:
    SpanContext linked_context;
    std::vector<KeyValue> attributes;
};

/// @brief A move-only owning batch of completed span records sharing one
/// `Resource` and one `InstrumentationScope`.
///
/// Constructed by the `BatchSpanProcessor`; consumed by `IExporter::Export`.
class BatchHandle
{
public:
    BatchHandle() noexcept = default;

    BatchHandle(std::vector<SpanRecord> records,
                std::shared_ptr<const Resource> resource,
                InstrumentationScope scope) noexcept
        : m_records(std::move(records)), m_resource(std::move(resource)), m_scope(std::move(scope))
    {
    }

    BatchHandle(const BatchHandle&) = delete;
    BatchHandle& operator=(const BatchHandle&) = delete;
    BatchHandle(BatchHandle&&) noexcept = default;
    BatchHandle& operator=(BatchHandle&&) noexcept = default;
    ~BatchHandle() noexcept = default;

    [[nodiscard]] std::span<const SpanRecord> Spans() const noexcept
    {
        return {m_records.data(), m_records.size()};
    }
    [[nodiscard]] const Resource& ResourceRef() const noexcept
    {
        return *m_resource;
    }
    [[nodiscard]] const InstrumentationScope& Scope() const noexcept
    {
        return m_scope;
    }

private:
    std::vector<SpanRecord> m_records;
    std::shared_ptr<const Resource> m_resource;
    InstrumentationScope m_scope;
};

}  // namespace microtel::internal
