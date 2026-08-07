// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/internal/log_batch.hpp"
#include "microtel/internal/log_encoder.hpp"
#include "microtel/internal/log_exporter.hpp"
#include "microtel/internal/wire_codec.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace microtel::exporter
{

/// @brief Configuration for `OtlpLogExporter`.
struct OtlpLogExporterConfig
{
    /// @brief Maximum number of log batches held in the worker queue.
    /// `Export` returns `Dropped` when the queue is at capacity.
    std::size_t max_queue_size = 256;
    /// @brief Per-export deadline passed to `IWireCodec::SendAll`.
    std::chrono::milliseconds export_deadline{std::chrono::seconds(10)};
};

/// @brief Logs export pipeline — the logs analogue of `OtlpExporter`.
///
/// Owns a worker thread. `Export` is non-blocking: it enqueues the batch and
/// returns immediately. The worker drains the queue, encodes each batch via
/// `ILogEncoder`, and submits the encoded bytes to `IWireCodec::SendAll`.
///
/// The `IWireCodec` must be pointed at the OTLP logs endpoint (`/v1/logs` for
/// HTTP, the logs gRPC path for gRPC-over-HTTP/2).
///
/// Retry / backoff mirror the metrics exporter: deferred to a later increment.
///
/// **Dependencies (all non-owning):**
/// - `ILogEncoder` — required.
/// - `IWireCodec` — required; must be connected before the first `Export` call.
///
/// @threadsafety `Export` is thread-safe. `ForceFlush` and `Shutdown` are
///   caller-thread-safe and idempotent.
/// @see docs/logs-design.md §3
class OtlpLogExporter final : public internal::ILogExporter
{
public:
    explicit OtlpLogExporter(internal::ILogEncoder* encoder,
                             internal::IWireCodec* codec,
                             OtlpLogExporterConfig config = {}) noexcept;

    ~OtlpLogExporter() noexcept override;

    OtlpLogExporter(const OtlpLogExporter&) = delete;
    OtlpLogExporter& operator=(const OtlpLogExporter&) = delete;
    OtlpLogExporter(OtlpLogExporter&&) = delete;
    OtlpLogExporter& operator=(OtlpLogExporter&&) = delete;

    /// @brief Enqueue a log batch for export. Non-blocking.
    ///
    /// Returns `AlreadyShutDown` if `Shutdown` has been called, `Dropped` if the
    /// queue is at capacity, otherwise `Success`.
    [[nodiscard]] internal::ExportResult Export(internal::LogBatchHandle&& batch) noexcept override;

    [[nodiscard]] microtel::Status ForceFlush(std::chrono::milliseconds timeout) noexcept override;

    [[nodiscard]] microtel::Status Shutdown(std::chrono::milliseconds timeout) noexcept override;

private:
    void WorkerLoop() noexcept;
    void DrainQueue(std::unique_lock<std::mutex>& lock) noexcept;
    void ProcessBatches(std::vector<internal::LogBatchHandle>& batches);

    internal::ILogEncoder* m_encoder;
    internal::IWireCodec* m_codec;
    OtlpLogExporterConfig m_config;

    std::deque<internal::LogBatchHandle> m_queue;
    std::mutex m_mu;
    std::condition_variable m_cv;
    std::uint64_t m_flush_seq = 0;
    std::uint64_t m_flush_done_seq = 0;
    std::atomic<bool> m_shutdown{false};
    std::thread m_worker;
};

}  // namespace microtel::exporter
