// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/error.hpp"
#include "microtel/internal/diagnostics_sink.hpp"
#include "microtel/provider.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>

namespace microtel::sdk
{

/// @brief Production `internal::IDiagnosticsSink`: atomic counters plus a
/// mutex-guarded last-error slot.
///
/// The single sink for drop counters, batch counters, queue depth, and
/// connection state — the backing store for `Provider::GetExporterHealth()`
/// (`docs/interfaces.md` §4.11). Counter and gauge recording is `noexcept`,
/// lock-free, and allocation-free (relaxed atomics); only `RecordBatchFailed`
/// takes the leaf-lock `m_error_mu` to copy the capped error message
/// (`docs/threading-model.md` §4).
///
/// @threadsafety Thread-safe. Callable from caller threads, the exporter
/// worker, and the I/O thread.
class DiagnosticsCounters final : public internal::IDiagnosticsSink
{
public:
    /// @brief Cap applied to the stored last-error message, in bytes
    /// (`HealthSnapshot::last_error_message` is documented as capped).
    static constexpr std::size_t kMaxErrorMessageLength = 256;

    DiagnosticsCounters() noexcept = default;
    ~DiagnosticsCounters() noexcept override = default;

    DiagnosticsCounters(const DiagnosticsCounters&) = delete;
    DiagnosticsCounters& operator=(const DiagnosticsCounters&) = delete;
    DiagnosticsCounters(DiagnosticsCounters&&) = delete;
    DiagnosticsCounters& operator=(DiagnosticsCounters&&) = delete;

    /// @brief Add `n` to the counter for `reason` (relaxed `fetch_add`).
    void RecordDrop(DropReason reason, std::uint64_t n = 1) noexcept override;

    /// @brief Increment the sent-batch counter.
    void RecordBatchSent() noexcept override;

    /// @brief Increment the failed-batch counter and store `err` in the
    /// last-error slot (message capped at `kMaxErrorMessageLength`).
    void RecordBatchFailed(const Error& err) noexcept override;

    /// @brief Publish the current export-queue depth (relaxed store).
    void SetQueueDepth(std::uint64_t depth) noexcept override;

    /// @brief Publish the transport connection state (relaxed store).
    void SetConnectionState(ConnectionState state) noexcept override;

    /// @brief Copy all counters and the last-error slot into a
    /// consistent-at-a-moment `HealthSnapshot` (relaxed loads).
    [[nodiscard]] HealthSnapshot Snapshot() const noexcept override;

private:
    std::array<std::atomic<std::uint64_t>, kDropReasonCount> m_counters{};
    std::atomic<std::uint64_t> m_batches_sent{0};
    std::atomic<std::uint64_t> m_batches_failed{0};
    std::atomic<std::uint64_t> m_queue_depth{0};
    std::atomic<ConnectionState> m_connection_state{ConnectionState::Disconnected};

    /// Leaf-lock guarding the last-error slot below; never held while calling
    /// out (`docs/threading-model.md` §4).
    mutable std::mutex m_error_mu;
    std::optional<std::chrono::system_clock::time_point> m_last_error_time;
    std::string m_last_error_message;
};

}  // namespace microtel::sdk
