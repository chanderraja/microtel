// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#include "exporter/otlp_log_exporter.hpp"

#include "microtel/error.hpp"
#include "microtel/internal/encoded_payload.hpp"
#include "microtel/status.hpp"

#include <cstdint>
#include <exception>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace microtel::exporter
{

constexpr auto kDestructorShutdownTimeout = std::chrono::seconds(5);

OtlpLogExporter::OtlpLogExporter(internal::ILogEncoder* encoder,
                                 internal::IWireCodec* codec,
                                 OtlpLogExporterConfig config,
                                 internal::IDiagnosticsSink* diag) noexcept
    : m_encoder(encoder),
      m_codec(codec),
      m_config(config),
      m_diag(diag),
      m_worker([this] { WorkerLoop(); })
{
}

OtlpLogExporter::~OtlpLogExporter() noexcept
{
    (void)Shutdown(kDestructorShutdownTimeout);
}

internal::ExportResult OtlpLogExporter::Export(internal::LogBatchHandle&& batch) noexcept
{
    // Read before the move: after `push_back` consumes the batch there is
    // nothing left to count.
    const auto record_count = static_cast<std::uint64_t>(batch.Records().size());
    const std::scoped_lock lock{m_mu};
    if (m_shutdown.load(std::memory_order_relaxed))
    {
        RecordDropped(DropReason::PostShutdown, record_count);
        return internal::ExportResult::AlreadyShutDown;
    }
    if (m_queue.size() >= m_config.max_queue_size)
    {
        RecordDropped(DropReason::QueueFull, record_count);
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
        RecordDropped(DropReason::QueueFull, record_count);
        return internal::ExportResult::Dropped;
    }
    m_cv.notify_one();
    return internal::ExportResult::Success;
}

microtel::Status OtlpLogExporter::ForceFlush(std::chrono::milliseconds timeout) noexcept
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

microtel::Status OtlpLogExporter::Shutdown(std::chrono::milliseconds timeout) noexcept
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

void OtlpLogExporter::ProcessBatches(std::vector<internal::LogBatchHandle>& batches)
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

void OtlpLogExporter::RecordOutcome(const internal::WireResult& result) noexcept
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
        Error{.kind = Error::Kind::Network, .message = "log export failed at wire codec"}));
}

void OtlpLogExporter::RecordDropped(DropReason reason, std::uint64_t n) noexcept
{
    if (m_diag != nullptr)
    {
        m_diag->RecordDrop(reason, n);
    }
}

void OtlpLogExporter::RecordDrainFailure(std::string_view what) noexcept
{
    if (m_diag == nullptr)
    {
        return;
    }
    // No `DropReason` names "the encoder threw", and adding one is an ICP
    // (`docs/interfaces.md` §3.5), so the failed-batch counter and the last
    // error carry it — which is what an operator reads out of
    // `GetExporterHealth()`.
    m_diag->RecordBatchFailed(
        Error{.kind = Error::Kind::InternalFailure, .message = std::string{what}, .os_errno = 0});
}

void OtlpLogExporter::DrainQueue(std::unique_lock<std::mutex>& lock) noexcept
{
    while (!m_queue.empty())
    {
        std::vector<internal::LogBatchHandle> batches;
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
        // The batches are gone either way — the worker is `noexcept` and there
        // is nowhere to put them — but a swallowed failure that nothing counts
        // leaves GetExporterHealth() reporting a clean pipeline (issue #224).
        catch (const std::exception& e)
        {
            RecordDrainFailure(e.what());
        }
        lock.lock();
    }
}

void OtlpLogExporter::WorkerLoop() noexcept
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
