// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/internal/clock.hpp"
#include "microtel/internal/diagnostics_sink.hpp"
#include "microtel/internal/exporter.hpp"
#include "microtel/internal/otlp_encoder.hpp"
#include "microtel/internal/wire_codec.hpp"

#include "exporter/retry_policy.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <random>
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
};

/// @brief Protocol-agnostic OTLP export pipeline.
///
/// Owns a worker thread. `Export` is non-blocking — it enqueues the batch
/// and returns immediately. The worker drains the queue, encodes each batch
/// via `IOtlpEncoder`, and submits the encoded bytes to `IWireCodec::Send`.
///
/// **M3 note:** retry / backoff / jitter are deferred to M5. One encode+send
/// attempt is made per batch; failures are dropped without retry.
///
/// **Dependencies (all non-owning):**
/// - `IOtlpEncoder` — required.
/// - `IWireCodec` — required; must be connected before first `Export` call.
/// - `IDiagnosticsSink` — optional; used from M3-C onward.
/// - `ISteadyClock` — optional; used for retry timing from M5 onward.
///
/// @threadsafety `Export` is thread-safe. `ForceFlush` and `Shutdown` are
///   caller-thread-safe and idempotent.
/// @see docs/interfaces.md §4.4
class OtlpExporter final : public internal::IExporter
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

    [[nodiscard]] microtel::Status ForceFlush(std::chrono::milliseconds timeout) noexcept override;

    [[nodiscard]] microtel::Status Shutdown(std::chrono::milliseconds timeout) noexcept override;

private:
    void WorkerLoop() noexcept;
    void DrainQueue(std::unique_lock<std::mutex>& lock) noexcept;
    void FanOutAndProcess(const std::vector<internal::BatchHandle>& batches);
    /// @brief Retry `batch` from `starting_attempt` until success, a
    ///        non-retryable result, or exhaustion of attempts / retry budget.
    /// @return The last `WireResult` observed, or `nullopt` when no further
    ///         attempt was made (retry budget already spent on entry) — in
    ///         which case the caller's own result is the batch's outcome.
    [[nodiscard]] std::optional<internal::WireResult> RunRetryLoop(
        const internal::BatchHandle& batch, std::uint32_t starting_attempt = 0U);
    /// @brief Report one batch's terminal outcome to the diagnostics sink.
    ///        No-op when no sink was supplied.
    void RecordOutcome(const internal::WireResult& result) noexcept;
    /// @brief Publish the current queue depth. Caller must hold `m_mu`.
    void PublishQueueDepth() noexcept;
    [[nodiscard]] internal::TimePointSteady ClockNow() const noexcept;
    [[nodiscard]] double DrawJitter01() noexcept;

    internal::IOtlpEncoder* m_encoder;
    internal::IWireCodec* m_codec;
    OtlpExporterConfig m_config;
    // NOLINTNEXTLINE(clang-diagnostic-unused-private-field) — used from M3-C onward
    internal::IDiagnosticsSink* m_diag;
    internal::ISteadyClock* m_clock;
    std::mt19937_64 m_rng;

    std::deque<internal::BatchHandle> m_queue;
    std::mutex m_mu;
    std::condition_variable m_cv;
    std::uint64_t m_flush_seq = 0;
    std::uint64_t m_flush_done_seq = 0;
    std::atomic<bool> m_shutdown{false};
    std::thread m_worker;
};

}  // namespace microtel::exporter
