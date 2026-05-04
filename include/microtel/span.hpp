// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/attribute.hpp"
#include "microtel/trace.hpp"

#include <chrono>
#include <optional>
#include <string_view>

namespace microtel
{

/// @brief Options for `Tracer::StartSpan`.
///
/// All fields are optional. Designated-initialiser construction is the
/// expected idiom: `tracer->StartSpan("name", {.kind = SpanKind::Server})`.
struct StartSpanOptions
{
    SpanKind                                   kind         = SpanKind::Internal;
    std::optional<SpanContext>                 parent;             ///< if unset, current Context is used
    std::chrono::system_clock::time_point      start_time   {};    ///< if zero-valued, "now" is used
    AttributeSpan                              attributes;         ///< initial attributes; copied if span is sampled
};

/// @brief A span — the unit of trace work.
///
/// Returned from `Tracer::StartSpan`. Owns no observable resource for the
/// caller; the underlying record is owned by the SDK. The handle's destructor
/// calls `End()` if the span has not been ended explicitly (RAII auto-end).
///
/// Hot-path methods (`SetAttribute`, `AddEvent`, `AddLink`, `SetStatus`,
/// `UpdateName`, `End`) are `noexcept`. On any internal failure (queue full,
/// span limits exceeded, post-shutdown call) microtel drops the field or the
/// record, increments diagnostics, and returns silently.
///
/// Concurrent access from multiple threads to a **single** span requires
/// external synchronisation. Distinct spans may be used concurrently from
/// distinct threads without coordination.
///
/// @threadsafety Externally synchronized (per-span); concurrent use of distinct spans is safe.
/// @noexcept All hot-path methods.
///
/// @see docs/architecture.md §3.1
/// @see docs/threading-model.md §10
/// @see docs/memory-model.md §8 (zero-allocation unsampled-span guarantee)
class Span
{
public:
    Span() noexcept                          = default;
    virtual ~Span() noexcept                 = default;

    Span(const Span&)                        = delete;
    Span& operator=(const Span&)             = delete;
    Span(Span&&) noexcept                    = default;
    Span& operator=(Span&&) noexcept         = default;

    /// @brief Identifying state for this span. Stable across the span's lifetime.
    [[nodiscard]] virtual SpanContext GetContext() const noexcept = 0;

    /// @brief True if this span will be exported (sampler returned `RecordAndSample`).
    [[nodiscard]] virtual bool IsSampled() const noexcept = 0;

    /// @brief Set or update an attribute on this span.
    ///
    /// If the per-span attribute count limit is reached, the new attribute is
    /// dropped and counted; existing values are not evicted.
    ///
    /// @param key   borrowed; copied into the span record on the sampled path.
    /// @param value moved or copied into the span record on the sampled path.
    virtual void SetAttribute(std::string_view key, AttributeValue value) noexcept = 0;

    /// @brief Add an event with optional attributes to this span.
    ///
    /// If the per-span event count limit is reached, the new event is dropped
    /// and counted.
    ///
    /// @param name        event name; borrowed.
    /// @param attributes  event-specific attributes; bounded by `event_attribute_count_limit`.
    /// @param timestamp   event time; if zero, "now" is used.
    virtual void AddEvent(std::string_view name,
                          AttributeSpan attributes = {},
                          std::chrono::system_clock::time_point timestamp = {}) noexcept = 0;

    /// @brief Add a link from this span to another span context.
    ///
    /// If the per-span link count limit is reached, the new link is dropped
    /// and counted.
    virtual void AddLink(const SpanContext& linked_context,
                         AttributeSpan attributes = {}) noexcept = 0;

    /// @brief Set the span's status code and optional description.
    ///
    /// `Unset` is the initial value. Spec dictates that `Ok` overrides any
    /// prior `Error`, and `Error` overrides any prior `Unset`. `Ok` does not
    /// override another `Ok`; `Error` does not override another `Error`.
    virtual void SetStatus(StatusCode code, std::string_view description = {}) noexcept = 0;

    /// @brief Update the span name.
    virtual void UpdateName(std::string_view name) noexcept = 0;

    /// @brief End the span. Idempotent — subsequent calls are no-ops.
    ///
    /// @param end_time if zero, "now" is used.
    virtual void End(std::chrono::system_clock::time_point end_time = {}) noexcept = 0;
};

}  // namespace microtel
