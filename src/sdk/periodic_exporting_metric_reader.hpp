// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/internal/metric_exporter.hpp"
#include "microtel/internal/metric_producer.hpp"
#include "microtel/internal/metric_reader.hpp"
#include "microtel/status.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace microtel::sdk
{

/// @brief Push-model `IMetricReader` that drives collect+export on a background
/// thread at a configurable interval (default 60 s per metrics-design.md §5).
///
/// Three call paths trigger a synchronous collect+export cycle:
///  - Background thread fires at each `interval` tick.
///  - `Collect(timeout)` — manual, caller-thread invocation.
///  - `ForceFlush(timeout)` — immediate caller-thread cycle, then delegates
///    to the exporter's `ForceFlush`.
///
/// `Shutdown(timeout)` wakes the background thread for a final cycle, waits
/// for it to exit, then delegates to the exporter's `Shutdown`.
///
/// Concurrent calls to `Collect`, `ForceFlush`, and the background thread are
/// serialized by `m_collect_mu` so neither the producer nor the exporter need
/// to be independently thread-safe against concurrent calls from this reader.
///
/// @threadsafety Thread-safe.
/// @noexcept All public methods; background thread entry point.
class PeriodicExportingMetricReader : public internal::IMetricReader
{
public:
    /// Default export interval per metrics-design.md §5.
    static constexpr std::chrono::milliseconds kDefaultInterval{60'000};

    /// @brief Construct the reader and start the background export thread.
    ///
    /// @param producer  Source of metric snapshots; must outlive this reader.
    /// @param exporter  Sink for exported batches; must outlive this reader.
    /// @param interval  Background export period; defaults to 60 s.
    PeriodicExportingMetricReader(internal::IMetricProducer& producer,
                                  internal::IMetricExporter& exporter,
                                  std::chrono::milliseconds interval = kDefaultInterval) noexcept;

    PeriodicExportingMetricReader(const PeriodicExportingMetricReader&) = delete;
    PeriodicExportingMetricReader& operator=(const PeriodicExportingMetricReader&) = delete;
    PeriodicExportingMetricReader(PeriodicExportingMetricReader&&) = delete;
    PeriodicExportingMetricReader& operator=(PeriodicExportingMetricReader&&) = delete;

    /// @brief Shut down the reader if not already shut down, then join the
    /// background thread. Blocks for at most one export cycle.
    ~PeriodicExportingMetricReader() noexcept override;

    /// @brief Synchronously collect a snapshot and export each batch.
    ///
    /// Serialized against the background thread's own export cycle.
    /// Returns `AlreadyShutDown` immediately if `Shutdown()` was called.
    [[nodiscard]] microtel::Status Collect(std::chrono::milliseconds timeout) noexcept override;

    /// @brief Synchronously collect + export, then delegate `ForceFlush` to
    /// the exporter. Returns `AlreadyShutDown` if already shut down.
    [[nodiscard]] microtel::Status ForceFlush(std::chrono::milliseconds timeout) noexcept override;

    /// @brief Wake the background thread for a final export, join it, then
    /// delegate `Shutdown` to the exporter. Idempotent.
    [[nodiscard]] microtel::Status Shutdown(std::chrono::milliseconds timeout) noexcept override;

private:
    /// Background thread entry point — loops until `m_shut_down` is set.
    void RunLoop() noexcept;

    /// Execute one collect+export cycle. Guarded by `m_collect_mu`.
    /// @returns `Status::Completed` or `Status::Failed`.
    microtel::Status DoCollectExport() noexcept;

    internal::IMetricProducer& m_producer;
    internal::IMetricExporter& m_exporter;
    std::chrono::milliseconds m_interval;

    std::atomic<bool> m_shut_down{false};

    std::mutex m_mu;               // guards m_wake
    std::condition_variable m_cv;  // signalled on wake or shutdown
    bool m_wake{false};            // early-wakeup flag, guarded by m_mu

    std::mutex m_collect_mu;  // serializes DoCollectExport across threads

    std::thread m_thread;  // started last; joined in Shutdown/dtor
};

}  // namespace microtel::sdk
