// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/internal/exporter.hpp"  // ExportResult
#include "microtel/internal/log_batch.hpp"
#include "microtel/status.hpp"

#include <chrono>

namespace microtel::internal
{

/// @brief The protocol-agnostic log export pipeline — the logs analogue of
/// `IExporter`.
///
/// Consumes a `LogBatchHandle`, encodes it via `ILogEncoder`, sends it via the
/// shared transport, and accounts for drops. `Export` is non-blocking: a
/// successful return means the batch was accepted into the pipeline, not that
/// it was sent. The concrete `OtlpLogExporter` arrives in L4.3; L4.2's
/// processors target this interface.
///
/// @threadsafety Thread-safe.
/// @noexcept All methods.
/// @see docs/logs-design.md §3
class ILogExporter
{
public:
    virtual ~ILogExporter() noexcept = default;

    [[nodiscard]] virtual ExportResult Export(LogBatchHandle&& batch) noexcept = 0;

    [[nodiscard]] virtual microtel::Status ForceFlush(
        std::chrono::milliseconds timeout) noexcept = 0;

    [[nodiscard]] virtual microtel::Status Shutdown(std::chrono::milliseconds timeout) noexcept = 0;
};

}  // namespace microtel::internal
