// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/log_record.hpp"

namespace microtel
{

/// @brief Issues log records for one instrumentation scope.
///
/// Obtained from `Provider::GetLogger(name, version)` (wired in v1.3 M14-L5).
/// The logger holds a non-owning back-reference to the provider; the
/// application must not hold a `Logger` past the provider's shutdown or
/// destruction.
///
/// `Emit()` is the hot-path entry point — `noexcept` and never blocks on I/O.
/// If the SDK is shut down or the export pipeline is overloaded, `Emit()`
/// drops the record, increments diagnostics, and returns silently.
///
/// **Trace correlation:** If `record.trace_id` is the invalid all-zeros default
/// and a span is active in the current thread, the SDK fills in `trace_id`,
/// `span_id`, and `trace_flags` from the active span context before enqueueing
/// the record.
///
/// @threadsafety Thread-safe. Concurrent `Emit()` calls from any number of
///               threads are safe.
/// @noexcept All hot-path methods.
///
/// @see docs/logs-design.md
class Logger
{
public:
    Logger() noexcept = default;
    virtual ~Logger() noexcept = default;

    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;
    Logger(Logger&&) noexcept = default;
    Logger& operator=(Logger&&) noexcept = default;

    /// @brief Emit a log record.
    ///
    /// Takes ownership of `record`. The SDK copies trace context into the
    /// record if trace correlation is enabled and `record.trace_id` is
    /// invalid. On any internal failure the record is silently dropped and
    /// the appropriate drop counter is incremented.
    ///
    /// @param record log record to emit. Moved into the SDK pipeline.
    ///
    /// @threadsafety Thread-safe.
    /// @noexcept Always succeeds (failures are counted, not reported).
    virtual void Emit(LogRecord record) noexcept = 0;
};

}  // namespace microtel
