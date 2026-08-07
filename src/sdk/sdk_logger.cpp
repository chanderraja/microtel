// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#include "sdk/sdk_logger.hpp"

#include "microtel/provider.hpp"  // DropReason
#include "microtel/trace.hpp"     // SpanContext

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace microtel::sdk
{

SdkLogger::SdkLogger(internal::ILogRecordProcessor* processor,
                     internal::InstrumentationScope scope,
                     const internal::ICurrentSpanSource* current_span_source,
                     internal::IDiagnosticsSink* diagnostics,
                     LogLimitOptions limits) noexcept
    : m_processor(processor),
      m_scope(std::move(scope)),
      m_current_span_source(current_span_source),
      m_diagnostics(diagnostics),
      m_limits(limits)
{
}

void SdkLogger::FillTraceContext(LogRecord& record) const noexcept
{
    if (m_current_span_source == nullptr || record.trace_id.IsValid())
    {
        return;
    }
    const SpanContext ctx = m_current_span_source->GetCurrentSpan();
    if (ctx.IsValid())
    {
        record.trace_id = ctx.trace_id;
        record.span_id = ctx.span_id;
        record.trace_flags = ctx.trace_flags;
    }
}

void SdkLogger::EnforceAttributeLimit(LogRecord& record) const noexcept
{
    if (record.attributes.size() <= m_limits.max_attributes)
    {
        return;
    }
    const std::size_t overflow = record.attributes.size() - m_limits.max_attributes;
    record.attributes.resize(m_limits.max_attributes);
    record.dropped_attributes_count += static_cast<std::uint32_t>(overflow);
    if (m_diagnostics != nullptr)
    {
        m_diagnostics->RecordDrop(DropReason::LogAttributeLimit, overflow);
    }
}

void SdkLogger::Emit(LogRecord record) noexcept
{
    if (record.observed_time == std::chrono::system_clock::time_point{})
    {
        record.observed_time = std::chrono::system_clock::now();
    }
    FillTraceContext(record);
    EnforceAttributeLimit(record);
    if (m_processor != nullptr)
    {
        m_processor->OnEmit(std::move(record), m_scope);
    }
}

}  // namespace microtel::sdk
