// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#include "exporter/otlp_metric_exporter.hpp"

#include "microtel/status.hpp"

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
                                       OtlpMetricExporterConfig config) noexcept
    : m_encoder(encoder), m_codec(codec), m_config(config), m_worker([this] { WorkerLoop(); })
{
}

OtlpMetricExporter::~OtlpMetricExporter() noexcept
{
    (void)Shutdown(kDestructorShutdownTimeout);
}

internal::ExportResult OtlpMetricExporter::Export(internal::MetricBatchHandle&& batch) noexcept
{
    const std::scoped_lock lock{m_mu};
    if (m_shutdown.load(std::memory_order_relaxed))
    {
        return internal::ExportResult::AlreadyShutDown;
    }
    if (m_queue.size() >= m_config.max_queue_size)
    {
        return internal::ExportResult::Dropped;
    }
    try
    {
        m_queue.push_back(std::move(batch));
    }
    catch (const std::bad_alloc&)
    {
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
    (void)m_codec->SendAll(std::move(payloads), m_config.export_deadline);
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
