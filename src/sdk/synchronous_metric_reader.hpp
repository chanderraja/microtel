// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/internal/metric_exporter.hpp"
#include "microtel/internal/metric_producer.hpp"
#include "microtel/internal/metric_reader.hpp"
#include "microtel/status.hpp"

#include <atomic>
#include <chrono>

namespace microtel::sdk
{

/// @brief Synchronous (pull-on-demand) implementation of `IMetricReader`.
///
/// On each `Collect()` call: asks the `IMetricProducer` for a snapshot,
/// then forwards each `MetricBatchHandle` to the `IMetricExporter::Export()`
/// in sequence. Intended for tests and manual flush paths; the periodic
/// push-model reader is a later increment.
///
/// Holds non-owning references to both the producer and exporter; both must
/// outlive the reader.
///
/// @threadsafety Thread-safe: `m_shut_down` is atomic; `Collect` / `ForceFlush`
/// / `Shutdown` are guarded against post-shutdown calls.
class SynchronousMetricReader : public internal::IMetricReader
{
public:
    /// @brief Construct a reader that pulls from @p producer and pushes to
    /// @p exporter.
    SynchronousMetricReader(internal::IMetricProducer& producer,
                            internal::IMetricExporter& exporter) noexcept
        : m_producer(producer), m_exporter(exporter)
    {
    }

    SynchronousMetricReader(const SynchronousMetricReader&) = delete;
    SynchronousMetricReader& operator=(const SynchronousMetricReader&) = delete;
    SynchronousMetricReader(SynchronousMetricReader&&) = delete;
    SynchronousMetricReader& operator=(SynchronousMetricReader&&) = delete;
    ~SynchronousMetricReader() noexcept override = default;

    /// @brief Collect a snapshot and export each handle.
    ///
    /// Returns `AlreadyShutDown` immediately if `Shutdown()` was called.
    /// Returns `Failed` if any `Export()` call returns `Failure`.
    /// Returns `Completed` on full success.
    [[nodiscard]] microtel::Status Collect(std::chrono::milliseconds timeout) noexcept override;

    /// @brief Flush the exporter (synchronous — no queued work in this impl).
    [[nodiscard]] microtel::Status ForceFlush(std::chrono::milliseconds timeout) noexcept override;

    /// @brief Shut down the reader and its exporter. Idempotent.
    [[nodiscard]] microtel::Status Shutdown(std::chrono::milliseconds timeout) noexcept override;

private:
    internal::IMetricProducer& m_producer;
    internal::IMetricExporter& m_exporter;
    std::atomic<bool> m_shut_down{false};
};

}  // namespace microtel::sdk
