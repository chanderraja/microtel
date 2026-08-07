// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/internal/batch.hpp"  // InstrumentationScope
#include "microtel/internal/log_exporter.hpp"
#include "microtel/internal/log_record_processor.hpp"
#include "microtel/log_record.hpp"
#include "microtel/resource.hpp"
#include "microtel/status.hpp"

#include <chrono>
#include <memory>

namespace microtel::sdk
{

/// @brief Synchronous, non-batching log record processor (for tests / debug).
///
/// Each `OnEmit` builds a single-record `LogBatchHandle` — Resource from
/// construction, scope from the call — and hands it straight to the exporter.
/// No queue, no worker thread. `ForceFlush` is a no-op (nothing is buffered);
/// `Shutdown` delegates to the exporter. The logs analogue of
/// `SimpleSpanProcessor`.
///
/// **Lifetime.** The exporter pointer is non-owning; the caller keeps it alive
/// for the processor's lifetime. Resource ownership is shared.
///
/// @threadsafety Thread-safe — the exporter permits concurrent `Export`, and
///               Resource is immutable post-construction.
class SimpleLogRecordProcessor final : public internal::ILogRecordProcessor
{
public:
    SimpleLogRecordProcessor(internal::ILogExporter* exporter,
                             std::shared_ptr<const Resource> resource) noexcept;

    ~SimpleLogRecordProcessor() noexcept override = default;

    SimpleLogRecordProcessor(const SimpleLogRecordProcessor&) = delete;
    SimpleLogRecordProcessor& operator=(const SimpleLogRecordProcessor&) = delete;
    SimpleLogRecordProcessor(SimpleLogRecordProcessor&&) = delete;
    SimpleLogRecordProcessor& operator=(SimpleLogRecordProcessor&&) = delete;

    void OnEmit(LogRecord&& record, const internal::InstrumentationScope& scope) noexcept override;

    [[nodiscard]] microtel::Status ForceFlush(std::chrono::milliseconds timeout) noexcept override;
    [[nodiscard]] microtel::Status Shutdown(std::chrono::milliseconds timeout) noexcept override;

private:
    internal::ILogExporter* m_exporter;
    std::shared_ptr<const Resource> m_resource;
};

}  // namespace microtel::sdk
