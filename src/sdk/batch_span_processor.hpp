// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/internal/batch.hpp"
#include "microtel/internal/exporter.hpp"
#include "microtel/internal/processor.hpp"
#include "microtel/resource.hpp"
#include "microtel/sdk_builder.hpp"
#include "microtel/status.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace microtel
{
class Span;
class Context;
}  // namespace microtel

namespace microtel::sdk
{

/// @brief Asynchronous batching span processor.
///
/// Queues `SpanRecord`s from `OnEnd` and drains them on a dedicated worker
/// thread in batches of up to `max_export_batch_size` records.  Draining is
/// triggered by:
///   - the `schedule_delay` timer (periodic),
///   - the queue reaching `max_export_batch_size` (immediate wake), or
///   - `ForceFlush` / `Shutdown` (explicit signal).
///
/// `OnStart` is a no-op (v1 has no enrichment hooks).
///
/// **Lifetime.** The exporter and resource pointers are non-owning; the
/// caller keeps them alive for the processor's lifetime.
///
/// @threadsafety Thread-safe — `OnEnd` / `ForceFlush` / `Shutdown` may be
///               called concurrently from multiple threads.
class BatchSpanProcessor final : public internal::ISpanProcessor
{
public:
    BatchSpanProcessor(internal::IExporter* exporter,
                       std::shared_ptr<const Resource> resource,
                       internal::InstrumentationScope scope,
                       BatchOptions opts) noexcept;

    ~BatchSpanProcessor() noexcept override;

    BatchSpanProcessor(const BatchSpanProcessor&) = delete;
    BatchSpanProcessor& operator=(const BatchSpanProcessor&) = delete;
    BatchSpanProcessor(BatchSpanProcessor&&) = delete;
    BatchSpanProcessor& operator=(BatchSpanProcessor&&) = delete;

    void OnStart(microtel::Span& span, const microtel::Context& parent) noexcept override;
    void OnEnd(internal::SpanRecord&& record) noexcept override;

    [[nodiscard]] microtel::Status ForceFlush(std::chrono::milliseconds timeout) noexcept override;
    [[nodiscard]] microtel::Status Shutdown(std::chrono::milliseconds timeout) noexcept override;

private:
    struct WakeResult
    {
        std::vector<internal::SpanRecord> batch;
        bool done = false;
    };

    WakeResult WaitAndCollect() noexcept;
    [[nodiscard]] bool JoinWithTimeout(std::chrono::milliseconds timeout) noexcept;
    void WorkerLoop() noexcept;
    void ExportBatch(std::vector<internal::SpanRecord> batch) noexcept;

    internal::IExporter* m_exporter;
    std::shared_ptr<const Resource> m_resource;
    internal::InstrumentationScope m_scope;
    BatchOptions m_opts;

    std::mutex m_mu;
    std::condition_variable m_cv;
    std::deque<internal::SpanRecord> m_queue;
    bool m_shutdown{false};
    std::size_t m_flush_seq{0};
    std::size_t m_flush_done_seq{0};
    std::condition_variable m_flush_cv;

    std::thread m_worker;
};

}  // namespace microtel::sdk
