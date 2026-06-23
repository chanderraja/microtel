// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/internal/exporter.hpp"  // ExportResult (reused, signal-agnostic)
#include "microtel/internal/metric_batch.hpp"
#include "microtel/status.hpp"

#include <chrono>

namespace microtel::internal
{

/// @brief The metrics export pipeline. Owns its own worker thread, encodes via
/// `IMetricEncoder`, and sends via an `IWireCodec` pointed at the metrics
/// service path — sharing the same `ITransport` connection as the span
/// exporter (`metrics-design.md` §5; concurrent `Send` is gated by ICP 0009).
///
/// The metrics analogue of `IExporter`. `Export` is non-blocking: a successful
/// return means the batch was accepted into the pipeline, not that it was sent.
///
/// @threadsafety Thread-safe.
/// @noexcept All methods.
class IMetricExporter
{
public:
    virtual ~IMetricExporter() noexcept = default;

    [[nodiscard]] virtual ExportResult Export(MetricBatchHandle&& batch) noexcept = 0;

    [[nodiscard]] virtual microtel::Status ForceFlush(
        std::chrono::milliseconds timeout) noexcept = 0;

    [[nodiscard]] virtual microtel::Status Shutdown(std::chrono::milliseconds timeout) noexcept = 0;
};

}  // namespace microtel::internal
