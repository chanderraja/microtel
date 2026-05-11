// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#include "exporter/otlp_exporter.hpp"

#include "microtel/internal/batch.hpp"
#include "microtel/status.hpp"

#include "exporter/retry_policy.hpp"

#include <chrono>
#include <exception>
#include <mutex>
#include <random>
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
      m_rng(
          static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count())),
      m_worker([this] { WorkerLoop(); })
{
}

// Maximum time the destructor waits for the worker to drain on implicit shutdown.
constexpr auto kDestructorShutdownTimeout = std::chrono::seconds(5);

OtlpExporter::~OtlpExporter() noexcept
{
    (void)Shutdown(kDestructorShutdownTimeout);
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
    RunRetryLoop(batch);
}

void OtlpExporter::RunRetryLoop(const internal::BatchHandle& batch)
{
    const RetryPolicyConfig& rp = m_config.retry_policy;
    const std::uint32_t max_attempts = (rp.max_attempts > 0U) ? rp.max_attempts : 1U;
    const auto budget_deadline = ClockNow() + rp.retry_budget;

    for (std::uint32_t attempt = 0; attempt < max_attempts; ++attempt)
    {
        auto payload = m_encoder->Encode(batch);
        const auto result = m_codec->Send(std::move(payload), m_config.export_deadline);

        if (result.success || !result.retryable)
        {
            break;
        }

        if (attempt + 1U >= max_attempts)
        {
            break;
        }

        const auto backoff = ComputeBackoff(attempt, rp, result.retry_after, DrawJitter01());
        if (ClockNow() >= budget_deadline)
        {
            break;
        }

        std::this_thread::sleep_for(backoff);
    }
}

internal::TimePointSteady OtlpExporter::ClockNow() const noexcept
{
    if (m_clock != nullptr)
    {
        return m_clock->Now();
    }
    return std::chrono::steady_clock::now();
}

double OtlpExporter::DrawJitter01() noexcept
{
    std::uniform_real_distribution<double> dist{0.0, 1.0};
    return dist(m_rng);
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
