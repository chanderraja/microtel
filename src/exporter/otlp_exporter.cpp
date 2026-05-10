// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#include "exporter/otlp_exporter.hpp"

#include "microtel/internal/batch.hpp"
#include "microtel/status.hpp"

#include <chrono>
#include <exception>
#include <mutex>
#include <thread>
#include <utility>

namespace microtel::exporter
{

OtlpExporter::OtlpExporter(internal::IOtlpEncoder* encoder,
                           internal::IWireCodec* codec,
                           OtlpExporterConfig config,
                           internal::IDiagnosticsSink* diag,
                           internal::ISteadyClock* clock) noexcept
    : m_encoder(encoder),
      m_codec(codec),
      m_config(config),
      m_diag(diag),
      m_clock(clock),
      m_worker([this] { WorkerLoop(); })
{
}

OtlpExporter::~OtlpExporter() noexcept
{
    (void)Shutdown(std::chrono::seconds(5));
}

internal::ExportResult OtlpExporter::Export(internal::BatchHandle&& batch) noexcept
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

microtel::Status OtlpExporter::ForceFlush(std::chrono::milliseconds timeout) noexcept
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

microtel::Status OtlpExporter::Shutdown(std::chrono::milliseconds timeout) noexcept
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

void OtlpExporter::ProcessBatch(const internal::BatchHandle& batch)
{
    auto payload = m_encoder->Encode(batch);
    // M3: single attempt; retry / backoff added in M5.
    (void)m_codec->Send(std::move(payload), m_config.export_deadline);
}

void OtlpExporter::DrainQueue(std::unique_lock<std::mutex>& lock) noexcept
{
    while (!m_queue.empty())
    {
        auto batch = std::move(m_queue.front());
        m_queue.pop_front();
        lock.unlock();
        try
        {
            ProcessBatch(batch);
        }
        // NOLINTNEXTLINE(bugprone-empty-catch) — intentional drop; diag hook added in M3-C
        catch (const std::exception&)
        {
        }
        lock.lock();
    }
}

void OtlpExporter::WorkerLoop() noexcept
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

    // Signal any flush/shutdown waiters that did not see the in-loop notify.
    const std::scoped_lock lock{m_mu};
    m_flush_done_seq = m_flush_seq;
    m_cv.notify_all();
}

}  // namespace microtel::exporter
