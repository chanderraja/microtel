// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#include "exporter/otlp_exporter.hpp"

#include "microtel/error.hpp"
#include "microtel/internal/batch.hpp"
#include "microtel/status.hpp"

#include "exporter/retry_policy.hpp"

#include <chrono>
#include <exception>
#include <mutex>
#include <optional>
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
        PublishQueueDepth();
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

std::optional<internal::WireResult> OtlpExporter::RunRetryLoop(const internal::BatchHandle& batch,
                                                               std::uint32_t starting_attempt)
{
    const RetryPolicyConfig& rp = m_config.retry_policy;
    const std::uint32_t max_attempts = (rp.max_attempts > 0U) ? rp.max_attempts : 1U;
    const auto budget_deadline = ClockNow() + rp.retry_budget;

    // When starting after attempt 0 (fan-out already made the first send),
    // skip all retries if the budget is already exhausted. No attempt is made,
    // so the caller's own result stands as the batch outcome.
    if (starting_attempt > 0U && ClockNow() >= budget_deadline)
    {
        return std::nullopt;
    }

    std::optional<internal::WireResult> last;
    for (std::uint32_t attempt = starting_attempt; attempt < max_attempts; ++attempt)
    {
        auto payload = m_encoder->Encode(batch);
        last = m_codec->Send(std::move(payload), m_config.export_deadline);

        if (last->success || !last->retryable)
        {
            break;
        }

        if (attempt + 1U >= max_attempts)
        {
            break;
        }

        const auto backoff = ComputeBackoff(attempt, rp, last->retry_after, DrawJitter01());
        if (ClockNow() >= budget_deadline)
        {
            break;
        }

        std::this_thread::sleep_for(backoff);
    }
    return last;
}

void OtlpExporter::RecordOutcome(const internal::WireResult& result) noexcept
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
    // A codec may report failure without populating `error`. Recording an
    // empty message would leave GetExporterHealth() saying a batch failed and
    // refusing to say why, so name the stage instead.
    m_diag->RecordBatchFailed(result.error.value_or(
        Error{.kind = Error::Kind::Network, .message = "export failed at wire codec"}));
}

void OtlpExporter::PublishQueueDepth() noexcept
{
    if (m_diag != nullptr)
    {
        m_diag->SetQueueDepth(static_cast<std::uint64_t>(m_queue.size()));
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
    // Drain until the queue is genuinely empty. The lock is released while
    // FanOutAndProcess runs, so Export() can enqueue more batches mid-drain. A
    // single pass would leave those undrained while WorkerLoop still marked a
    // pending flush complete — letting ForceFlush return before every queued
    // batch was processed. Looping until empty under the held lock closes that
    // window and upholds ForceFlush's "queue drained" contract.
    while (!m_queue.empty())
    {
        std::vector<internal::BatchHandle> batches;
        while (!m_queue.empty())
        {
            batches.push_back(std::move(m_queue.front()));
            m_queue.pop_front();
        }
        lock.unlock();
        try
        {
            FanOutAndProcess(batches);
        }
        // NOLINTNEXTLINE(bugprone-empty-catch) — intentional drop; diag hook added in M3-C
        catch (const std::exception&)
        {
        }
        lock.lock();
        PublishQueueDepth();
    }
}

void OtlpExporter::FanOutAndProcess(const std::vector<internal::BatchHandle>& batches)
{
    if (batches.empty())
    {
        return;
    }

    std::vector<internal::EncodedPayload> payloads;
    payloads.reserve(batches.size());
    for (const auto& batch : batches)
    {
        payloads.push_back(m_encoder->Encode(batch));
    }

    // Fan-out: all requests submitted concurrently; SendAll collapses N
    // sequential round trips into one (see ICP 0007).
    const auto results = m_codec->SendAll(std::move(payloads), m_config.export_deadline);

    // The fan-out counts as attempt 0; pass starting_attempt=1 so that the
    // retry loop does not exceed the configured max_attempts total.
    for (std::size_t i = 0; i < results.size(); ++i)
    {
        internal::WireResult outcome = results[i];
        if (!outcome.success && outcome.retryable)
        {
            if (auto retried = RunRetryLoop(batches[i], 1U))
            {
                outcome = std::move(*retried);
            }
        }
        // Exactly one outcome per batch: intermediate retryable failures are
        // attempts, not failed batches.
        RecordOutcome(outcome);
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
