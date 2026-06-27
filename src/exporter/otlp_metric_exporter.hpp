// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/internal/metric_encoder.hpp"
#include "microtel/internal/metric_exporter.hpp"
#include "microtel/internal/wire_codec.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>

namespace microtel::exporter
{

/// @brief Configuration for `OtlpMetricExporter`.
struct OtlpMetricExporterConfig
{
    /// @brief Maximum number of metric batches held in the worker queue.
    /// `Export` returns `Dropped` when the queue is at capacity.
    std::size_t max_queue_size = 256;
    /// @brief Per-export deadline passed to `IWireCodec::Send`.
    std::chrono::milliseconds export_deadline{std::chrono::seconds(10)};
};

/// @brief Metrics export pipeline — the metrics analogue of `OtlpExporter`.
///
/// Owns a worker thread. `Export` is non-blocking: it enqueues the batch and
/// returns immediately. The worker drains the queue, encodes each batch via
/// `IMetricEncoder`, and submits the encoded bytes to `IWireCodec::Send`.
///
/// The `IWireCodec` must be pointed at the OTLP metrics endpoint
/// (`/v1/metrics` for HTTP, metrics gRPC path for gRPC-over-HTTP/2).
///
/// Retry / backoff are deferred to the M5 metrics analogue.
///
/// **Dependencies (all non-owning):**
/// - `IMetricEncoder` — required.
/// - `IWireCodec` — required; must be connected before first `Export` call.
///
/// @threadsafety `Export` is thread-safe. `ForceFlush` and `Shutdown` are
///   caller-thread-safe and idempotent.
class OtlpMetricExporter final : public internal::IMetricExporter
{
public:
    explicit OtlpMetricExporter(internal::IMetricEncoder* encoder,
                                internal::IWireCodec* codec,
                                OtlpMetricExporterConfig config = {}) noexcept;

    ~OtlpMetricExporter() noexcept override;

    OtlpMetricExporter(const OtlpMetricExporter&) = delete;
    OtlpMetricExporter& operator=(const OtlpMetricExporter&) = delete;
    OtlpMetricExporter(OtlpMetricExporter&&) = delete;
    OtlpMetricExporter& operator=(OtlpMetricExporter&&) = delete;

    /// @brief Enqueue a metric batch for export. Non-blocking.
    ///
    /// Returns `AlreadyShutDown` if `Shutdown` has been called, `Dropped` if
    /// the queue is at capacity, otherwise `Success`.
    [[nodiscard]] internal::ExportResult Export(
        internal::MetricBatchHandle&& batch) noexcept override;

    [[nodiscard]] microtel::Status ForceFlush(std::chrono::milliseconds timeout) noexcept override;

    [[nodiscard]] microtel::Status Shutdown(std::chrono::milliseconds timeout) noexcept override;

private:
    void WorkerLoop() noexcept;
    void DrainQueue(std::unique_lock<std::mutex>& lock) noexcept;
    void ProcessBatches(std::vector<internal::MetricBatchHandle>& batches);

    internal::IMetricEncoder* m_encoder;
    internal::IWireCodec* m_codec;
    OtlpMetricExporterConfig m_config;

    std::deque<internal::MetricBatchHandle> m_queue;
    std::mutex m_mu;
    std::condition_variable m_cv;
    std::uint64_t m_flush_seq = 0;
    std::uint64_t m_flush_done_seq = 0;
    std::atomic<bool> m_shutdown{false};
    std::thread m_worker;
};

}  // namespace microtel::exporter
