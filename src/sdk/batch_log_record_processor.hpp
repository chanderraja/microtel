// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/internal/batch.hpp"  // InstrumentationScope
#include "microtel/internal/log_exporter.hpp"
#include "microtel/internal/log_record_processor.hpp"
#include "microtel/log_record.hpp"
#include "microtel/resource.hpp"
#include "microtel/sdk_builder.hpp"  // BatchOptions
#include "microtel/status.hpp"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace microtel::sdk
{

/// @brief Asynchronous batching log record processor.
///
/// Queues emitted records from `OnEmit` and drains them on a dedicated worker
/// thread in batches of up to `max_export_batch_size`. Draining is triggered by
/// the `schedule_delay` timer, the queue reaching `max_export_batch_size`, or an
/// explicit `ForceFlush` / `Shutdown`. On drain, queued records are grouped by
/// `(Resource, InstrumentationScope)` into one `LogBatchHandle` per scope
/// (design §2), each handed to the exporter.
///
/// The logs analogue of `BatchSpanProcessor`, sharing its concurrency structure
/// and reusing `BatchOptions` (queue / batch / delay / drop policy).
///
/// **Lifetime.** The exporter and resource are non-owning references kept alive
/// by the caller for the processor's lifetime.
///
/// @threadsafety Thread-safe — `OnEmit` / `ForceFlush` / `Shutdown` may be
///               called concurrently from multiple threads.
class BatchLogRecordProcessor final : public internal::ILogRecordProcessor
{
public:
    BatchLogRecordProcessor(internal::ILogExporter* exporter,
                            std::shared_ptr<const Resource> resource,
                            BatchOptions opts) noexcept;

    ~BatchLogRecordProcessor() noexcept override;

    BatchLogRecordProcessor(const BatchLogRecordProcessor&) = delete;
    BatchLogRecordProcessor& operator=(const BatchLogRecordProcessor&) = delete;
    BatchLogRecordProcessor(BatchLogRecordProcessor&&) = delete;
    BatchLogRecordProcessor& operator=(BatchLogRecordProcessor&&) = delete;

    void OnEmit(LogRecord&& record, const internal::InstrumentationScope& scope) noexcept override;

    [[nodiscard]] microtel::Status ForceFlush(std::chrono::milliseconds timeout) noexcept override;
    [[nodiscard]] microtel::Status Shutdown(std::chrono::milliseconds timeout) noexcept override;

private:
    /// A queued record paired with the scope of the logger that emitted it.
    struct QueuedLog
    {
        LogRecord record;
        internal::InstrumentationScope scope;
    };

    struct WakeResult
    {
        std::vector<QueuedLog> batch;
        bool done = false;
        std::size_t pending_flush_seq{0};
    };

    WakeResult WaitAndCollect() noexcept;
    [[nodiscard]] bool JoinWithTimeout(std::chrono::milliseconds timeout) noexcept;
    void WorkerLoop() noexcept;
    void ExportBatch(std::vector<QueuedLog> batch) noexcept;

    internal::ILogExporter* m_exporter;
    std::shared_ptr<const Resource> m_resource;
    BatchOptions m_opts;

    std::mutex m_mu;
    std::condition_variable m_cv;
    std::deque<QueuedLog> m_queue;
    bool m_shutdown{false};
    std::size_t m_flush_seq{0};
    std::size_t m_flush_done_seq{0};
    std::condition_variable m_flush_cv;

    std::thread m_worker;
};

}  // namespace microtel::sdk
