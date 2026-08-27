// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/log_record.hpp"

#include "adapters/otelcpp/abi_guard.hpp"
#include "adapters/otelcpp/attribute_conversion.hpp"
#include "adapters/otelcpp/context_conversion.hpp"

#include <chrono>
#include <cstdint>
#include <string>
#include <utility>

#include <opentelemetry/common/attribute_value.h>
#include <opentelemetry/logs/log_record.h>
#include <opentelemetry/logs/severity.h>
#include <opentelemetry/trace/span_id.h>
#include <opentelemetry/trace/trace_flags.h>
#include <opentelemetry/trace/trace_id.h>

/// @file
/// `LogRecordShim` — implements `opentelemetry::logs::LogRecord` (ABI v1) as
/// an accumulator that builds a `microtel::LogRecord` field by field, then
/// hands it to `LoggerShim::EmitLogRecord` (`logger_shim.hpp`).
///
/// **Severity** is a plain numeric cast: otel-cpp's `Severity` and
/// `microtel::SeverityNumber` share the same 0–24 numbering (OTLP
/// `SeverityNumber`), pinned by a `static_assert` below.
///
/// **`SetEventId(id, name)`** has no full microtel counterpart —
/// `microtel::LogRecord` carries `event_name` but no integer id. Applying
/// ICP 0015's preserve-or-omit principle to a field it didn't originally
/// cover: the name half survives into `event_name`, the numeric id is
/// omitted. This is a leaf implementation detail, not a policy proposal —
/// no microtel interface changes, so no ICP.

namespace microtel::adapters::otelcpp
{

static_assert(static_cast<std::uint8_t>(opentelemetry::logs::Severity::kFatal4) ==
                  static_cast<std::uint8_t>(microtel::SeverityNumber::Fatal4),
              "otel-cpp Severity and microtel::SeverityNumber numbering diverged");

/// @brief Accumulates one log record's fields for `LoggerShim`.
///
/// Not thread-safe by design: otel-cpp's own contract is that a `LogRecord`
/// is created, populated, and emitted by a single call sequence on one
/// thread — the same assumption every otel-cpp SDK backend makes.
class LogRecordShim final : public opentelemetry::logs::LogRecord
{
public:
    void SetTimestamp(opentelemetry::common::SystemTimestamp timestamp) noexcept override
    {
        m_record.time = static_cast<std::chrono::system_clock::time_point>(timestamp);
    }

    void SetObservedTimestamp(opentelemetry::common::SystemTimestamp timestamp) noexcept override
    {
        m_record.observed_time = static_cast<std::chrono::system_clock::time_point>(timestamp);
    }

    void SetSeverity(opentelemetry::logs::Severity severity) noexcept override
    {
        m_record.severity_number =
            static_cast<microtel::SeverityNumber>(static_cast<std::uint8_t>(severity));
    }

    void SetBody(const opentelemetry::common::AttributeValue& message) noexcept override
    {
        m_record.body = ConvertAttributeValue(message);
    }

    void SetAttribute(opentelemetry::nostd::string_view key,
                      const opentelemetry::common::AttributeValue& value) noexcept override
    {
        m_record.attributes.push_back(
            {.key = std::string{key.data(), key.size()}, .value = ConvertAttributeValue(value)});
    }

    void SetEventId(std::int64_t /*id*/,
                    opentelemetry::nostd::string_view name = {}) noexcept override
    {
        if (!name.empty())
        {
            m_record.event_name.assign(name.data(), name.size());
        }
    }

    void SetTraceId(const opentelemetry::trace::TraceId& trace_id) noexcept override
    {
        m_record.trace_id = ToMicrotelTraceId(trace_id);
    }

    void SetSpanId(const opentelemetry::trace::SpanId& span_id) noexcept override
    {
        m_record.span_id = ToMicrotelSpanId(span_id);
    }

    void SetTraceFlags(const opentelemetry::trace::TraceFlags& trace_flags) noexcept override
    {
        m_record.trace_flags = microtel::TraceFlags{trace_flags.flags()};
    }

    /// @brief Take ownership of the accumulated record.
    ///
    /// Called exactly once, by `LoggerShim::EmitLogRecord`, immediately
    /// before this object is destroyed.
    [[nodiscard]] microtel::LogRecord ReleaseRecord() noexcept
    {
        return std::move(m_record);
    }

private:
    microtel::LogRecord m_record;
};

}  // namespace microtel::adapters::otelcpp
