// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#include "sdk/batch_span_processor.hpp"

#include "microtel/internal/batch.hpp"
#include "microtel/status.hpp"

#include <algorithm>
#include <chrono>
#include <memory>
#include <utility>
#include <vector>

namespace microtel::sdk
{

BatchSpanProcessor::BatchSpanProcessor(internal::IExporter* exporter,
                                       std::shared_ptr<const Resource> resource,
                                       internal::InstrumentationScope scope,
                                       BatchOptions opts) noexcept
    : m_exporter(exporter),
      m_resource(std::move(resource)),
      m_scope(std::move(scope)),
      m_opts(opts),
      m_worker([this] { WorkerLoop(); })
{
}

constexpr auto kBspDestructorTimeout = std::chrono::milliseconds(5000);

BatchSpanProcessor::~BatchSpanProcessor() noexcept
{
    (void)Shutdown(kBspDestructorTimeout);
    if (m_worker.joinable())
    {
        m_worker.join();
    }
}

void BatchSpanProcessor::OnStart(microtel::Span& /*span*/,
                                 const microtel::Context& /*parent*/) noexcept
{
    // No-op — v1 has no in-process span enrichment hooks.
}

void BatchSpanProcessor::OnEnd(internal::SpanRecord&& record) noexcept
{
    const std::scoped_lock lock{m_mu};
    if (m_shutdown)
    {
        return;
    }
    if (m_queue.size() >= m_opts.max_queue_size)
    {
        if (m_opts.drop_policy == DropPolicy::DropOldest)
        {
            m_queue.pop_front();
        }
        else
        {
            return;  // DropNewest: discard incoming record
        }
    }
    m_queue.push_back(std::move(record));
    if (m_queue.size() >= m_opts.max_export_batch_size)
    {
        m_cv.notify_one();
    }
}

microtel::Status BatchSpanProcessor::ForceFlush(std::chrono::milliseconds timeout) noexcept
{
    std::unique_lock lock{m_mu};
    if (m_shutdown)
    {
        return microtel::Status::AlreadyShutDown;
    }
    const std::size_t target = ++m_flush_seq;
    m_cv.notify_one();
    const bool done =
        m_flush_cv.wait_for(lock, timeout, [this, target] { return m_flush_done_seq >= target; });
    return done ? microtel::Status::Completed : microtel::Status::TimedOut;
}

bool BatchSpanProcessor::JoinWithTimeout(std::chrono::milliseconds timeout) noexcept
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (m_worker.joinable())
    {
        if (std::chrono::steady_clock::now() >= deadline)
        {
            return false;
        }
        m_worker.join();
    }
    return true;
}

microtel::Status BatchSpanProcessor::Shutdown(std::chrono::milliseconds timeout) noexcept
{
    {
        const std::scoped_lock lock{m_mu};
        if (m_shutdown)
        {
            return microtel::Status::AlreadyShutDown;
        }
        m_shutdown = true;
        m_cv.notify_one();
    }
    return JoinWithTimeout(timeout) ? microtel::Status::Completed : microtel::Status::TimedOut;
}

BatchSpanProcessor::WakeResult BatchSpanProcessor::WaitAndCollect() noexcept
{
    std::unique_lock<std::mutex> lock{m_mu};
    m_cv.wait_for(lock,
                  m_opts.schedule_delay,
                  [this]
                  {
                      return m_shutdown || m_flush_seq > m_flush_done_seq ||
                             m_queue.size() >= m_opts.max_export_batch_size;
                  });

    const std::size_t count =
        std::min(m_queue.size(), static_cast<std::size_t>(m_opts.max_export_batch_size));
    std::vector<internal::SpanRecord> batch;
    batch.reserve(count);
    for (std::size_t i = 0; i < count; ++i)
    {
        batch.push_back(std::move(m_queue.front()));
        m_queue.pop_front();
    }

    std::size_t pending_flush_seq = 0;
    if (m_flush_seq > m_flush_done_seq && m_queue.empty())
    {
        pending_flush_seq = m_flush_seq;
    }

    return {
        .batch = std::move(batch),
        .done = m_shutdown && m_queue.empty(),
        .pending_flush_seq = pending_flush_seq,
    };
}

void BatchSpanProcessor::WorkerLoop() noexcept
{
    while (true)
    {
        auto [batch, done, pending_flush_seq] = WaitAndCollect();
        if (!batch.empty())
        {
            ExportBatch(std::move(batch));
        }
        if (pending_flush_seq > 0)
        {
            const std::scoped_lock lock{m_mu};
            m_flush_done_seq = pending_flush_seq;
            m_flush_cv.notify_all();
        }
        if (done)
        {
            break;
        }
    }
}

void BatchSpanProcessor::ExportBatch(std::vector<internal::SpanRecord> batch) noexcept
{
    internal::BatchHandle handle{std::move(batch), m_resource, m_scope};
    (void)m_exporter->Export(std::move(handle));
}

}  // namespace microtel::sdk
