// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#include "sdk/batch_log_record_processor.hpp"

#include "microtel/internal/log_batch.hpp"
#include "microtel/status.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <iterator>
#include <memory>
#include <utility>
#include <vector>

namespace microtel::sdk
{

BatchLogRecordProcessor::BatchLogRecordProcessor(internal::ILogExporter* exporter,
                                                 std::shared_ptr<const Resource> resource,
                                                 BatchOptions opts) noexcept
    : m_exporter(exporter),
      m_resource(std::move(resource)),
      m_opts(opts),
      m_worker([this] { WorkerLoop(); })
{
}

constexpr auto kBlpDestructorTimeout = std::chrono::milliseconds(5000);

BatchLogRecordProcessor::~BatchLogRecordProcessor() noexcept
{
    (void)Shutdown(kBlpDestructorTimeout);
    if (m_worker.joinable())
    {
        m_worker.join();
    }
}

void BatchLogRecordProcessor::OnEmit(LogRecord&& record,
                                     const internal::InstrumentationScope& scope) noexcept
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
    m_queue.push_back(QueuedLog{.record = std::move(record), .scope = scope});
    if (m_queue.size() >= m_opts.max_export_batch_size)
    {
        m_cv.notify_one();
    }
}

microtel::Status BatchLogRecordProcessor::ForceFlush(std::chrono::milliseconds timeout) noexcept
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

bool BatchLogRecordProcessor::JoinWithTimeout(std::chrono::milliseconds timeout) noexcept
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

microtel::Status BatchLogRecordProcessor::Shutdown(std::chrono::milliseconds timeout) noexcept
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

BatchLogRecordProcessor::WakeResult BatchLogRecordProcessor::WaitAndCollect() noexcept
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
    std::vector<QueuedLog> batch;
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

void BatchLogRecordProcessor::WorkerLoop() noexcept
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

void BatchLogRecordProcessor::ExportBatch(std::vector<QueuedLog> batch) noexcept
{
    // Group records by scope, preserving first-seen order, into one
    // LogBatchHandle per (Resource, InstrumentationScope) — design §2.
    std::vector<std::pair<internal::InstrumentationScope, std::vector<LogRecord>>> groups;
    for (auto& item : batch)
    {
        const auto same_scope = [&item](const auto& group) {
            return group.first.name == item.scope.name && group.first.version == item.scope.version;
        };
        auto it = std::ranges::find_if(groups, same_scope);
        if (it == groups.end())
        {
            groups.emplace_back(std::move(item.scope), std::vector<LogRecord>{});
            it = std::prev(groups.end());
        }
        it->second.push_back(std::move(item.record));
    }

    for (auto& group : groups)
    {
        internal::LogBatchHandle handle{
            std::move(group.second), m_resource, std::move(group.first)};
        (void)m_exporter->Export(std::move(handle));
    }
}

}  // namespace microtel::sdk
