// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/attribute.hpp"
#include "microtel/trace.hpp"

#include <chrono>
#include <memory>
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

namespace internal
{

/// @brief Deleter for `SpanHandle`.
///
/// Holds a function-pointer indirection so the unsampled and sampled paths
/// can carry different cleanup behaviour through the same `unique_ptr` type.
///
/// - Unsampled path: `deleter` is a no-op; the `Span*` is a static singleton
///   in non-heap storage and must not be `delete`d.
/// - Sampled path: `deleter` runs `delete` (or returns the span to a per-
///   thread freelist if M3 measures it pays off).
///
/// **Implementation flexibility — see ICP 0003.** This struct's exact shape
/// (function pointer, flag-in-`Span` branch, two-deleter-types trick) may be
/// revised in v1.x based on benchmark findings. The public `SpanHandle`
/// alias below is the **stable surface**; revising `SpanDeleter`'s internals
/// does not require an ICP.
struct SpanDeleter
{
    /// @brief Cleanup function. `nullptr` means no-op (unsampled singleton).
    void (*deleter)(Span* span) noexcept = nullptr;

    /// @brief Invoked by `unique_ptr` on destruction. `noexcept`.
    void operator()(Span* span) const noexcept;
};

}  // namespace internal

/// @brief Public alias for the handle returned by `Tracer::StartSpan`.
///
/// Move-only. Semantically a `unique_ptr` to a `Span` with a custom deleter
/// that dispatches to either a no-op (unsampled path) or actual cleanup
/// (sampled path). The compile-time interface for application code is
/// identical regardless of sampling outcome.
///
/// @see docs/icps/0003-m0-deferred-decisions.md §3.2
using SpanHandle = std::unique_ptr<Span, internal::SpanDeleter>;

}  // namespace microtel
