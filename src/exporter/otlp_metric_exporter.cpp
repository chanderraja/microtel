// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#include "exporter/otlp_metric_exporter.hpp"

#include "microtel/error.hpp"
#include "microtel/status.hpp"

#include <cstdint>
#include <exception>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace microtel::exporter
{

constexpr auto kDestructorShutdownTimeout = std::chrono::seconds(5);

OtlpMetricExporter::OtlpMetricExporter(internal::IMetricEncoder* encoder,
                                       internal::IWireCodec* codec,
                                       OtlpMetricExporterConfig config,
                                       internal::IDiagnosticsSink* diag) noexcept
    : m_encoder(encoder),
      m_codec(codec),
      m_config(config),
      m_diag(diag),
      m_worker([this] { WorkerLoop(); })
{
}

OtlpMetricExporter::~OtlpMetricExporter() noexcept
{
    (void)Shutdown(kDestructorShutdownTimeout);
}

internal::ExportResult OtlpMetricExporter::Export(internal::MetricBatchHandle&& batch) noexcept
{
    // Read before the move: after `push_back` consumes the batch there is
    // nothing left to count.
    const auto metric_count = static_cast<std::uint64_t>(batch.Metrics().size());
    const std::scoped_lock lock{m_mu};
    if (m_shutdown.load(std::memory_order_relaxed))
    {
        RecordDropped(DropReason::PostShutdown, metric_count);
        return internal::ExportResult::AlreadyShutDown;
    }
    if (m_queue.size() >= m_config.max_queue_size)
    {
        RecordDropped(DropReason::QueueFull, metric_count);
        return internal::ExportResult::Dropped;
    }
    try
    {
        m_queue.push_back(std::move(batch));
    }
    // See OtlpExporter::Export — noexcept frame, so the catch must be wide
    // enough that nothing escapes.
    catch (const std::exception&)
    {
        RecordDropped(DropReason::QueueFull, metric_count);
        return internal::ExportResult::Dropped;
    }
    m_cv.notify_one();
    return internal::ExportResult::Success;
}

microtel::Status OtlpMetricExporter::ForceFlush(std::chrono::milliseconds timeout) noexcept
{
    {
        const std::scoped_lock lock{m_mu};
        ++m_flush_seq;
        m_cv.notify_all();
    }
    const bool completed = [&]
    {
        std::unique_lock lock{m_mu};
        return m_cv.wait_for(lock, timeout, [this] { return m_flush_done_seq >= m_flush_seq; });
    }();
    return completed ? microtel::Status::Completed : microtel::Status::TimedOut;
}

microtel::Status OtlpMetricExporter::Shutdown(std::chrono::milliseconds timeout) noexcept
{
    {
        const std::scoped_lock lock{m_mu};
        if (m_shutdown.exchange(true, std::memory_order_relaxed))
        {
            return microtel::Status::AlreadyShutDown;
        }
        ++m_flush_seq;
        m_cv.notify_all();
    }
    const bool completed = [&]
    {
        std::unique_lock lock{m_mu};
        return m_cv.wait_for(lock, timeout, [this] { return m_flush_done_seq >= m_flush_seq; });
    }();
    if (m_worker.joinable())
    {
        m_worker.join();
    }
    return completed ? microtel::Status::Completed : microtel::Status::TimedOut;
}

void OtlpMetricExporter::ProcessBatches(std::vector<internal::MetricBatchHandle>& batches)
{
    std::vector<internal::EncodedPayload> payloads;
    payloads.reserve(batches.size());
    for (const auto& batch : batches)
    {
        payloads.push_back(m_encoder->Encode(batch));
    }
    const auto results = m_codec->SendAll(std::move(payloads), m_config.export_deadline);
    for (const auto& result : results)
    {
        RecordOutcome(result);
    }
}

void OtlpMetricExporter::RecordOutcome(const internal::WireResult& result) noexcept
{
    if (m_diag == nullptr)
    {
        return;
    }
    if (result.success)
    {
        m_diag->RecordBatchSent();
        return;
    }
    m_diag->RecordBatchFailed(result.error.value_or(
        Error{.kind = Error::Kind::Network, .message = "metric export failed at wire codec"}));
}

void OtlpMetricExporter::RecordDropped(DropReason reason, std::uint64_t n) noexcept
{
    if (m_diag != nullptr)
    {
        m_diag->RecordDrop(reason, n);
    }
}

void OtlpMetricExporter::DrainQueue(std::unique_lock<std::mutex>& lock) noexcept
{
    while (!m_queue.empty())
    {
        std::vector<internal::MetricBatchHandle> batches;
        while (!m_queue.empty())
        {
            batches.push_back(std::move(m_queue.front()));
            m_queue.pop_front();
        }
        lock.unlock();
        try
        {
            ProcessBatches(batches);
        }
        // NOLINTNEXTLINE(bugprone-empty-catch) — intentional drop; diag hook deferred
        catch (const std::exception&)
        {
        }
        lock.lock();
    }
}

void OtlpMetricExporter::WorkerLoop() noexcept
{
    while (true)
    {
        std::unique_lock lock{m_mu};
        m_cv.wait(lock,
                  [this]
                  {
                      return !m_queue.empty() || m_flush_seq > m_flush_done_seq ||
                             m_shutdown.load(std::memory_order_relaxed);
                  });

        DrainQueue(lock);

        if (m_flush_seq > m_flush_done_seq)
        {
            m_flush_done_seq = m_flush_seq;
            m_cv.notify_all();
        }
        if (m_shutdown.load(std::memory_order_relaxed))
        {
            break;
        }
    }

    const std::scoped_lock lock{m_mu};
    m_flush_done_seq = m_flush_seq;
    m_cv.notify_all();
}

}  // namespace microtel::exporter
