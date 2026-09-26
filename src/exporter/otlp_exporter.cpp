// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#include "exporter/otlp_exporter.hpp"

#include "microtel/error.hpp"
#include "microtel/internal/batch.hpp"
#include "microtel/status.hpp"

#include "exporter/retry_engine.hpp"
#include "wire/encoder/trace_request_concat.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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
      m_retry(config.retry_policy, diag, clock),
      m_worker([this] { WorkerLoop(); })
{
}

// Maximum time the destructor waits for the worker to drain on implicit shutdown.
constexpr auto kDestructorShutdownTimeout = std::chrono::seconds(5);

// Named in the recorded error when a failed result carries no `Error`.
constexpr std::string_view kFailureStage = "export failed at wire codec";

OtlpExporter::~OtlpExporter() noexcept
{
    (void)Shutdown(kDestructorShutdownTimeout);
}

internal::ExportResult OtlpExporter::Export(internal::BatchHandle&& batch) noexcept
{
    const internal::ExportResult result = [this, &batch]
    {
        const std::scoped_lock lock{m_mu};
        return EnqueueLocked(std::move(batch));
    }();
    if (result == internal::ExportResult::Success)
    {
        m_cv.notify_one();
    }
    return result;
}

void OtlpExporter::ExportGroup(std::vector<internal::BatchHandle>&& batches) noexcept
{
    std::vector<internal::BatchHandle> group = std::move(batches);
    bool queued = false;
    {
        // One lock for the whole group, so the worker cannot wake between two
        // of its batches and send the first alone (design §3.6.1).
        const std::scoped_lock lock{m_mu};
        for (auto& batch : group)
        {
            queued = (EnqueueLocked(std::move(batch)) == internal::ExportResult::Success) || queued;
        }
    }
    if (queued)
    {
        m_cv.notify_one();
    }
}

internal::ExportResult OtlpExporter::EnqueueLocked(internal::BatchHandle&& batch) noexcept
{
    // Read before the move: a rejected batch is still counted in spans, and
    // after `push_back` consumes it there is nothing left to count.
    const auto span_count = static_cast<std::uint64_t>(batch.Spans().size());
    if (m_shutdown.load(std::memory_order_relaxed))
    {
        RecordDropped(DropReason::PostShutdown, span_count);
        return internal::ExportResult::AlreadyShutDown;
    }
    // Both bounds: batches, and the spans they carry (issue #345). The span
    // budget is the one a fan-in drain meets, since it arrives as one small
    // batch per leaf.
    if (m_queue.size() >= m_config.max_queue_size ||
        m_queued_spans + span_count > m_config.max_queued_spans)
    {
        RecordDropped(DropReason::QueueFull, span_count);
        return internal::ExportResult::Dropped;
    }
    try
    {
        m_queue.push_back(std::move(batch));
        m_queued_spans += span_count;
        PublishQueueDepth();
    }
    // std::exception, not std::bad_alloc: push_back can also throw
    // std::length_error, and Export is noexcept, so anything that escapes
    // terminates the host process rather than dropping one batch.
    catch (const std::exception&)
    {
        // Same loss as a full queue from the caller's side: the batch never
        // entered the pipeline because it would not fit.
        RecordDropped(DropReason::QueueFull, span_count);
        return internal::ExportResult::Dropped;
    }
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
    // Outside m_mu: the engine's lock is a leaf (threading-model.md §4).
    m_retry.Abort();
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

void OtlpExporter::RecordDropped(DropReason reason, std::uint64_t n) noexcept
{
    if (m_diag != nullptr)
    {
        m_diag->RecordDrop(reason, n);
    }
}

void OtlpExporter::RecordDrainFailure(std::string_view what) noexcept
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

void OtlpExporter::PublishQueueDepth() noexcept
{
    if (m_diag != nullptr)
    {
        m_diag->SetQueueDepth(static_cast<std::uint64_t>(m_queue.size()));
    }
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
        // The whole queue was taken, so the whole budget is free again.
        m_queued_spans = 0;
        lock.unlock();
        try
        {
            FanOutAndProcess(batches);
        }
        // The batches are gone either way — the worker is `noexcept` and there
        // is nowhere to put them — but a swallowed failure that nothing counts
        // leaves GetExporterHealth() reporting a clean pipeline, against
        // error-model.md §5.1 (issue #224).
        catch (const std::exception& e)
        {
            RecordDrainFailure(e.what());
        }
        lock.lock();
        PublishQueueDepth();
    }
}

internal::EncodedPayload OtlpExporter::EncodeRequest(
    const std::vector<internal::BatchHandle>& batches, std::size_t first, std::size_t count)
{
    std::vector<internal::EncodedPayload> parts;
    parts.reserve(count);
    for (std::size_t i = first; i < first + count; ++i)
    {
        parts.push_back(m_encoder->Encode(batches.at(i)));
    }
    return wire::ConcatenateTraceRequests(std::move(parts));
}

void OtlpExporter::FanOutAndProcess(const std::vector<internal::BatchHandle>& batches)
{
    if (batches.empty())
    {
        return;
    }

    // Split the drained batches into requests: consecutive batches while the
    // running span count stays within max_spans_per_request. A request holds
    // at least one batch, so a batch over the limit goes alone and whole.
    struct Request
    {
        std::size_t first = 0;
        std::size_t count = 0;
    };
    std::vector<Request> requests;
    std::size_t spans_in_request = 0;
    for (std::size_t i = 0; i < batches.size(); ++i)
    {
        const std::size_t spans = batches[i].Spans().size();
        if (requests.empty() || spans_in_request + spans > m_config.max_spans_per_request)
        {
            requests.push_back(Request{.first = i, .count = 0});
            spans_in_request = 0;
        }
        ++requests.back().count;
        spans_in_request += spans;
    }

    std::vector<internal::EncodedPayload> payloads;
    payloads.reserve(requests.size());
    for (const auto& request : requests)
    {
        payloads.push_back(EncodeRequest(batches, request.first, request.count));
    }

    // Fan-out: all requests submitted concurrently; SendAll collapses N
    // sequential round trips into one (see ICP 0007).
    const auto results = m_codec->SendAll(std::move(payloads), m_config.export_deadline);

    // The fan-out counts as attempt 0. Exactly one outcome per request, and
    // intermediate retryable failures are attempts, not failed batches. A
    // retry re-encodes the whole request: bytes do not survive an attempt
    // (memory-model.md §3.1). `at()`: a codec returning more results than
    // requests throws into DrainQueue's catch rather than reading past the end.
    for (std::size_t i = 0; i < results.size(); ++i)
    {
        const Request& request = requests.at(i);
        m_retry.Settle(
            results.at(i),
            [this, &batches, &request]
            {
                return m_codec->Send(EncodeRequest(batches, request.first, request.count),
                                     m_config.export_deadline);
            },
            kFailureStage,
            request.count);
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
