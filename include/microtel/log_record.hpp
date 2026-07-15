// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/attribute.hpp"
#include "microtel/trace.hpp"

#include <chrono>
#include <string>
#include <vector>

namespace microtel
{

/// @brief OTel severity levels, normalized per the Logs Data Model.
///
/// Numeric values match OTLP `SeverityNumber` (proto field values 0–24).
/// The four variants per base level (e.g. `Trace`, `Trace2`, `Trace3`,
/// `Trace4`) let log bridges map fine-grained native levels into the OTel
/// space without information loss.
///
/// @see https://opentelemetry.io/docs/specs/otel/logs/data-model/#field-severitynumber
enum class SeverityNumber : std::uint8_t
{
    Unspecified = 0,
    Trace = 1,
    Trace2 = 2,
    Trace3 = 3,
    Trace4 = 4,
    Debug = 5,
    Debug2 = 6,
    Debug3 = 7,
    Debug4 = 8,
    Info = 9,
    Info2 = 10,
    Info3 = 11,
    Info4 = 12,
    Warn = 13,
    Warn2 = 14,
    Warn3 = 15,
    Warn4 = 16,
    Error = 17,
    Error2 = 18,
    Error3 = 19,
    Error4 = 20,
    Fatal = 21,
    Fatal2 = 22,
    Fatal3 = 23,
    Fatal4 = 24,
};

/// @brief A log record passed to `Logger::Emit()`.
///
/// All fields map directly to the OTel Logs Data Model and the OTLP
/// `LogRecord` proto message. Fields are optional by the spec; unset fields
/// retain their zero / empty default.
///
/// **Time fields:** A zero-valued `std::chrono::system_clock::time_point`
/// (the default) signals "not provided". The SDK sets `observed_time` to
/// the current wall time at `Emit()` if it is unset. `time` represents
/// when the event occurred; a zero value means unknown.
///
/// **Trace correlation:** If a span is active when `Emit()` is called and
/// `trace_id` is left as the invalid default, the SDK fills in `trace_id`,
/// `span_id`, and `trace_flags` from the active span context
/// (v1.3 feature; see `docs/logs-design.md §4`).
///
/// @see https://opentelemetry.io/docs/specs/otel/logs/data-model/
struct LogRecord
{
    /// When the event occurred (0 = unknown / not provided).
    std::chrono::system_clock::time_point time;

    /// When the SDK observed the record. SDK sets this at Emit() if unset.
    std::chrono::system_clock::time_point observed_time;

    /// Normalized severity. Defaults to `SeverityNumber::Unspecified`.
    SeverityNumber severity_number = SeverityNumber::Unspecified;

    /// Original severity string as known at the source (e.g. `"WARNING"`).
    std::string severity_text;

    /// Log body — a scalar value or array.
    ///
    /// Uses `AttributeValue` (the same `std::variant` as span attributes),
    /// which covers `bool`, `int64_t`, `double`, `string`, and arrays of each.
    /// For an unstructured text message, set to `std::string`.
    AttributeValue body;

    /// Key-value attributes describing the specific event occurrence.
    std::vector<KeyValue> attributes;

    /// Count of attributes dropped because the per-record limit was reached.
    std::uint32_t dropped_attributes_count = 0;

    /// Trace identifier. SDK fills in from the active span context when
    /// left as the invalid (all-zeros) default and a span is active.
    TraceId trace_id;

    /// Span identifier. SDK fills in from the active span context when
    /// left as the invalid (all-zeros) default and a span is active.
    SpanId span_id;

    /// Trace flags (W3C). SDK fills in from the active span context when
    /// `trace_id` is invalid and a span is active.
    TraceFlags trace_flags;

    /// Short event name (e.g. `"exception"`, `"http.request"`). Optional.
    std::string event_name;
};

}  // namespace microtel
