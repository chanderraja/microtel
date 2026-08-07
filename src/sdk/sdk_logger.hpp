// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/internal/batch.hpp"  // InstrumentationScope
#include "microtel/internal/diagnostics_sink.hpp"
#include "microtel/internal/icurrent_span_source.hpp"
#include "microtel/internal/log_record_processor.hpp"
#include "microtel/log_record.hpp"
#include "microtel/logger.hpp"

#include <cstdint>

namespace microtel::sdk
{

/// @brief Per-logger record limits. The public builder surface arrives in L5;
/// for now this is an SDK-internal knob with the design-doc default.
struct LogLimitOptions
{
    /// Maximum attributes retained per record; surplus attributes are dropped
    /// and counted (`docs/logs-design.md` §5).
    std::uint32_t max_attributes = 128;
};

/// @brief Production `Logger` implementation for one instrumentation scope.
///
/// `Emit` stamps `observed_time` when unset, fills trace context from the active
/// span when `trace_id` is the invalid default, enforces the per-record
/// attribute limit, and hands the record to the processor tagged with this
/// logger's scope. All references are borrowed — the `Provider` (or test
/// fixture) keeps the processor, current-span source, and diagnostics sink alive
/// for the logger's lifetime. The current-span source and diagnostics sink may
/// be null (correlation / drop accounting disabled respectively).
///
/// @threadsafety Thread-safe when the processor, span source, and sink are.
/// @noexcept `Emit` never throws (failures are counted, not reported).
/// @see docs/logs-design.md §1, §4, §5
class SdkLogger final : public microtel::Logger
{
public:
    SdkLogger(internal::ILogRecordProcessor* processor,
              internal::InstrumentationScope scope,
              const internal::ICurrentSpanSource* current_span_source,
              internal::IDiagnosticsSink* diagnostics,
              LogLimitOptions limits) noexcept;

    ~SdkLogger() noexcept override = default;

    SdkLogger(const SdkLogger&) = delete;
    SdkLogger& operator=(const SdkLogger&) = delete;
    SdkLogger(SdkLogger&&) noexcept = default;
    SdkLogger& operator=(SdkLogger&&) noexcept = default;

    void Emit(LogRecord record) noexcept override;

private:
    void FillTraceContext(LogRecord& record) const noexcept;
    void EnforceAttributeLimit(LogRecord& record) const noexcept;

    internal::ILogRecordProcessor* m_processor;
    internal::InstrumentationScope m_scope;
    const internal::ICurrentSpanSource* m_current_span_source;
    internal::IDiagnosticsSink* m_diagnostics;
    LogLimitOptions m_limits;
};

}  // namespace microtel::sdk
