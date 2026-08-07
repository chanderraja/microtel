// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#include "sdk/simple_log_record_processor.hpp"

#include "microtel/internal/log_batch.hpp"

#include <chrono>
#include <memory>
#include <utility>
#include <vector>

namespace microtel::sdk
{

SimpleLogRecordProcessor::SimpleLogRecordProcessor(
    internal::ILogExporter* exporter, std::shared_ptr<const Resource> resource) noexcept
    : m_exporter(exporter), m_resource(std::move(resource))
{
}

void SimpleLogRecordProcessor::OnEmit(LogRecord&& record,
                                      const internal::InstrumentationScope& scope) noexcept
{
    std::vector<LogRecord> records;
    records.push_back(std::move(record));
    internal::LogBatchHandle handle{std::move(records), m_resource, scope};
    (void)m_exporter->Export(std::move(handle));
}

microtel::Status SimpleLogRecordProcessor::ForceFlush(
    std::chrono::milliseconds /*timeout*/) noexcept
{
    // Nothing is buffered — every record is exported synchronously in OnEmit.
    return microtel::Status::Completed;
}

microtel::Status SimpleLogRecordProcessor::Shutdown(std::chrono::milliseconds timeout) noexcept
{
    return m_exporter->Shutdown(timeout);
}

}  // namespace microtel::sdk
