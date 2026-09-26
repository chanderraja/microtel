// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/internal/batch_group_exporter.hpp"
#include "microtel/internal/clock.hpp"
#include "microtel/internal/diagnostics_sink.hpp"
#include "microtel/internal/exporter.hpp"
#include "microtel/internal/otlp_encoder.hpp"
#include "microtel/internal/wire_codec.hpp"

#include "exporter/retry_engine.hpp"
#include "exporter/retry_policy.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string_view>
#include <thread>
#include <vector>

namespace microtel::exporter
{

/// @brief Configuration for `OtlpExporter`.
struct OtlpExporterConfig
{
    /// @brief Maximum number of batches held in the worker queue.
    /// `Export` returns `Dropped` when the queue is at capacity.
    std::size_t max_queue_size = 256;
    /// @brief Per-export deadline passed to `IWireCodec::Send`.
    std::chrono::milliseconds export_deadline{std::chrono::seconds(10)};
    /// @brief Retry / backoff policy.
    RetryPolicyConfig retry_policy{};
    /// @brief Most spans one export request may carry when the worker joins
    /// several drained batches into one request
    /// (`docs/leaf-concentrator-design.md` §3.6.1). A batch is never split: a
    /// batch larger than this goes as a request of its own. `SdkBuilder` sets
    /// it to `BatchOptions::max_export_batch_size`.
    std::size_t max_spans_per_request = 512;
};

/// @brief Protocol-agnostic OTLP export pipeline.
///
/// Owns a worker thread. `Export` is non-blocking — it enqueues the batch
/// and returns immediately. The worker drains the queue, encodes each batch
/// via `IOtlpEncoder`, and submits the encoded bytes to `IWireCodec::Send`.
///
/// A retryable failure is retried by `RetryEngine`, the engine the metric
/// and log exporters share (`RetryPolicyConfig`).
///
/// **Multi-Resource requests.** The batches the worker drains together are
/// sent as one request, up to `max_spans_per_request` spans, by concatenating
/// their encodings (`wire::ConcatenateTraceRequests`,
/// `docs/leaf-concentrator-design.md` §3.6.1). A request is sent, retried and
/// classified as one unit; its outcome is counted once per batch it carries.
/// `ExportGroup` queues all the batches of one processor drain under one lock,
/// so they are always drained together.
///
/// **Dependencies (all non-owning):**
/// - `IOtlpEncoder` — required.
/// - `IWireCodec` — required; must be connected before first `Export` call.
/// - `IDiagnosticsSink` — optional; used from M3-C onward.
/// - `ISteadyClock` — optional; used for the retry budget.
///
/// @threadsafety `Export` is thread-safe. `ForceFlush` and `Shutdown` are
///   caller-thread-safe and idempotent.
/// @see docs/interfaces.md §4.4
class OtlpExporter final : public internal::IExporter, public internal::IBatchGroupExporter
{
public:
    explicit OtlpExporter(internal::IOtlpEncoder* encoder,
                          internal::IWireCodec* codec,
                          OtlpExporterConfig config = {},
                          internal::IDiagnosticsSink* diag = nullptr,
                          internal::ISteadyClock* clock = nullptr) noexcept;

    ~OtlpExporter() noexcept override;

    OtlpExporter(const OtlpExporter&) = delete;
    OtlpExporter& operator=(const OtlpExporter&) = delete;
    OtlpExporter(OtlpExporter&&) = delete;
    OtlpExporter& operator=(OtlpExporter&&) = delete;

    [[nodiscard]] internal::ExportResult Export(internal::BatchHandle&& batch) noexcept override;

    void ExportGroup(std::vector<internal::BatchHandle>&& batches) noexcept override;

    [[nodiscard]] microtel::Status ForceFlush(std::chrono::milliseconds timeout) noexcept override;

    [[nodiscard]] microtel::Status Shutdown(std::chrono::milliseconds timeout) noexcept override;

private:
    void WorkerLoop() noexcept;
    void DrainQueue(std::unique_lock<std::mutex>& lock) noexcept;
    void FanOutAndProcess(const std::vector<internal::BatchHandle>& batches);
    /// @brief Queue one batch, or count why not. Caller must hold `m_mu`.
    [[nodiscard]] internal::ExportResult EnqueueLocked(internal::BatchHandle&& batch) noexcept;
    /// @brief Encode the batches `[first, first + count)` of @p batches and
    ///        join them into one request.
    [[nodiscard]] internal::EncodedPayload EncodeRequest(
        const std::vector<internal::BatchHandle>& batches, std::size_t first, std::size_t count);
    /// @brief Add `n` to the counter for `reason`. No-op when no sink was
    ///        supplied. Lock-free, so it is safe under `m_mu`.
    void RecordDropped(DropReason reason, std::uint64_t n) noexcept;
    /// @brief Account for a batch lost to an exception escaping
    ///        `FanOutAndProcess`. No-op when no sink was supplied.
    ///
    /// The worker is `noexcept` and holds nowhere to put the batch, so the
    /// loss is unavoidable — but it is recorded rather than swallowed, which
    /// is what `error-model.md` §5.1 requires of the worker's top-level catch
    /// (issue #224). Counted as a failed batch, not a `DropReason`: no
    /// existing reason names this, and adding one is an ICP
    /// (`docs/interfaces.md` §3.5).
    ///
    /// @param what the exception's `what()`. Borrowed; copied into the error.
    void RecordDrainFailure(std::string_view what) noexcept;
    /// @brief Publish the current queue depth. Caller must hold `m_mu`.
    void PublishQueueDepth() noexcept;

    internal::IOtlpEncoder* m_encoder;
    internal::IWireCodec* m_codec;
    OtlpExporterConfig m_config;
    // NOLINTNEXTLINE(clang-diagnostic-unused-private-field) — used from M3-C onward
    internal::IDiagnosticsSink* m_diag;
    /// @brief Retry loop and final-outcome accounting. Before `m_worker`,
    ///        which uses it from the moment it starts.
    RetryEngine m_retry;

    std::deque<internal::BatchHandle> m_queue;
    std::mutex m_mu;
    std::condition_variable m_cv;
    std::uint64_t m_flush_seq = 0;
    std::uint64_t m_flush_done_seq = 0;
    std::atomic<bool> m_shutdown{false};
    std::thread m_worker;
};

}  // namespace microtel::exporter
