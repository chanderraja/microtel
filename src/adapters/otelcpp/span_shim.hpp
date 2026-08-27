// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/span.hpp"

#include "adapters/otelcpp/abi_guard.hpp"
#include "adapters/otelcpp/attribute_conversion.hpp"
#include "adapters/otelcpp/context_conversion.hpp"

#include <chrono>
#include <string_view>
#include <utility>
#include <vector>

#include <opentelemetry/common/attribute_value.h>
#include <opentelemetry/common/key_value_iterable.h>
#include <opentelemetry/common/timestamp.h>
#include <opentelemetry/trace/span.h>
#include <opentelemetry/trace/span_context.h>
#include <opentelemetry/trace/span_metadata.h>

/// @file
/// `SpanShim` — implements `opentelemetry::trace::Span` (ABI v1) over a
/// `microtel::SpanHandle`. Every otel-cpp call forwards onto the underlying
/// microtel span; attribute values route through `ConvertAttributeValue`
/// (ICP 0015), identities through `context_conversion.hpp`.

namespace microtel::adapters::otelcpp
{

/// @brief An otel-cpp span backed by a microtel span.
///
/// Owns the `SpanHandle`; the handle's RAII auto-end fires when the shim is
/// destroyed, satisfying otel-cpp's "destructor ends the span" contract
/// without a second `End` call.
///
/// `IsRecording` reports microtel's sampling decision: microtel has no
/// record-without-export state, so recording and sampled coincide.
///
/// @threadsafety Externally synchronized (per-span), matching the underlying
///               `microtel::Span` contract.
/// @noexcept All methods; failures inside the SDK drop and count per rule 14.
class SpanShim final : public opentelemetry::trace::Span
{
public:
    /// @param span the microtel span to forward onto. Must be non-null, which
    ///             `Tracer::StartSpan` guarantees; a null handle is UB.
    explicit SpanShim(microtel::SpanHandle span) noexcept : m_span{std::move(span)} {}

    // The overrides below would otherwise hide the base class's non-virtual
    // convenience overloads (initializer-list and iterable-template forms).
    using opentelemetry::trace::Span::AddEvent;

    void SetAttribute(opentelemetry::nostd::string_view key,
                      const otel_common::AttributeValue& value) noexcept override
    {
        m_span->SetAttribute(ToStringView(key), ConvertAttributeValue(value));
    }

    void AddEvent(opentelemetry::nostd::string_view name) noexcept override
    {
        m_span->AddEvent(ToStringView(name));
    }

    void AddEvent(opentelemetry::nostd::string_view name,
                  otel_common::SystemTimestamp timestamp) noexcept override
    {
        m_span->AddEvent(
            ToStringView(name), {}, static_cast<std::chrono::system_clock::time_point>(timestamp));
    }

    void AddEvent(opentelemetry::nostd::string_view name,
                  otel_common::SystemTimestamp timestamp,
                  const otel_common::KeyValueIterable& attributes) noexcept override
    {
        const std::vector<microtel::KeyValue> converted = ConvertKeyValues(attributes);
        m_span->AddEvent(ToStringView(name),
                         microtel::AttributeSpan{converted},
                         static_cast<std::chrono::system_clock::time_point>(timestamp));
    }

    void SetStatus(opentelemetry::trace::StatusCode code,
                   opentelemetry::nostd::string_view description) noexcept override
    {
        m_span->SetStatus(ToMicrotelStatusCode(code), ToStringView(description));
    }

    void UpdateName(opentelemetry::nostd::string_view name) noexcept override
    {
        m_span->UpdateName(ToStringView(name));
    }

    /// @brief End the span.
    ///
    /// otel-cpp expresses an explicit end time on the steady clock; microtel's
    /// `End` takes system time. A set steady end time is mapped through the
    /// current steady→system offset — exact for "now", approximate in
    /// proportion to how far in the past the caller's timestamp lies. A
    /// zero-valued option forwards microtel's own "now" sentinel unchanged.
    void End(const opentelemetry::trace::EndSpanOptions& options = {}) noexcept override
    {
        if (options.end_steady_time.time_since_epoch() == std::chrono::nanoseconds::zero())
        {
            m_span->End();
            return;
        }
        const auto steady_offset =
            std::chrono::steady_clock::time_point{options.end_steady_time.time_since_epoch()} -
            std::chrono::steady_clock::now();
        m_span->End(std::chrono::system_clock::now() +
                    std::chrono::duration_cast<std::chrono::system_clock::duration>(steady_offset));
    }

    [[nodiscard]] opentelemetry::trace::SpanContext GetContext() const noexcept override
    {
        return ToOtelSpanContext(m_span->GetContext());
    }

    [[nodiscard]] bool IsRecording() const noexcept override
    {
        return m_span->IsSampled();
    }

private:
    [[nodiscard]] static std::string_view ToStringView(
        opentelemetry::nostd::string_view view) noexcept
    {
        return {view.data(), view.size()};
    }

    [[nodiscard]] static microtel::StatusCode ToMicrotelStatusCode(
        opentelemetry::trace::StatusCode code) noexcept
    {
        switch (code)
        {
            case opentelemetry::trace::StatusCode::kOk:
                return microtel::StatusCode::Ok;
            case opentelemetry::trace::StatusCode::kError:
                return microtel::StatusCode::Error;
            case opentelemetry::trace::StatusCode::kUnset:
                return microtel::StatusCode::Unset;
        }
        return microtel::StatusCode::Unset;
    }

    microtel::SpanHandle m_span;
};

}  // namespace microtel::adapters::otelcpp
