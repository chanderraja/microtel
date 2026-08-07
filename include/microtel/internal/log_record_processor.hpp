// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/internal/batch.hpp"  // InstrumentationScope
#include "microtel/log_record.hpp"
#include "microtel/status.hpp"

#include <chrono>

namespace microtel::internal
{

/// @brief Consumes emitted log records and routes them onward.
///
/// The logs analogue of `ISpanProcessor`. v1.3 ships `BatchLogRecordProcessor`
/// (queue + worker + batch; L4.2) and `SimpleLogRecordProcessor` (synchronous,
/// for tests). A log record has no lifetime, so there is a single `OnEmit`
/// entry point rather than the span processor's `OnStart`/`OnEnd` pair.
///
/// `OnEmit` is callable from any caller thread, thread-safe, and `noexcept`.
/// The `scope` identifies the instrumentation scope of the emitting `Logger`;
/// the processor groups records by `(Resource, InstrumentationScope)` when it
/// forms a `LogBatchHandle`.
///
/// @threadsafety Thread-safe.
/// @noexcept All methods.
/// @see docs/logs-design.md §2
class ILogRecordProcessor
{
public:
    virtual ~ILogRecordProcessor() noexcept = default;

    virtual void OnEmit(LogRecord&& record, const InstrumentationScope& scope) noexcept = 0;

    [[nodiscard]] virtual microtel::Status ForceFlush(
        std::chrono::milliseconds timeout) noexcept = 0;

    [[nodiscard]] virtual microtel::Status Shutdown(std::chrono::milliseconds timeout) noexcept = 0;
};

}  // namespace microtel::internal
