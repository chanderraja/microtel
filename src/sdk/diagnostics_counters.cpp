// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#include "sdk/diagnostics_counters.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <new>

namespace microtel::sdk
{

void DiagnosticsCounters::RecordDrop(DropReason reason, std::uint64_t n) noexcept
{
    // Every DropReason enumerator indexes inside the kDropReasonCount array.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-constant-array-index)
    m_counters[static_cast<std::size_t>(reason)].fetch_add(n, std::memory_order_relaxed);
}

void DiagnosticsCounters::RecordBatchSent() noexcept
{
    m_batches_sent.fetch_add(1, std::memory_order_relaxed);
}

void DiagnosticsCounters::RecordBatchFailed(const Error& err) noexcept
{
    m_batches_failed.fetch_add(1, std::memory_order_relaxed);
    const std::scoped_lock lk{m_error_mu};
    m_last_error_time = std::chrono::system_clock::now();
    try
    {
        m_last_error_message.assign(err.message, 0, kMaxErrorMessageLength);
    }
    catch (const std::bad_alloc&)
    {
        // Recording a diagnostic must never fail the caller's operation;
        // on allocation failure keep the counter bump and drop the text.
        m_last_error_message.clear();
    }
}

void DiagnosticsCounters::SetQueueDepth(std::uint64_t depth) noexcept
{
    m_queue_depth.store(depth, std::memory_order_relaxed);
}

void DiagnosticsCounters::SetConnectionState(ConnectionState state) noexcept
{
    m_connection_state.store(state, std::memory_order_relaxed);
}

HealthSnapshot DiagnosticsCounters::Snapshot() const noexcept
{
    HealthSnapshot snap;
    std::ranges::transform(m_counters,
                           snap.drop_counters.begin(),
                           [](const std::atomic<std::uint64_t>& counter)
                           { return counter.load(std::memory_order_relaxed); });
    snap.batches_sent = m_batches_sent.load(std::memory_order_relaxed);
    snap.batches_failed = m_batches_failed.load(std::memory_order_relaxed);
    snap.queue_depth_now = m_queue_depth.load(std::memory_order_relaxed);
    snap.connection_state = m_connection_state.load(std::memory_order_relaxed);
    const std::scoped_lock lk{m_error_mu};
    snap.last_error_time = m_last_error_time;
    // One bounded allocation per snapshot — accepted by interfaces.md §4.11
    // ("Snapshot allocates one HealthSnapshot value").
    snap.last_error_message = m_last_error_message;
    return snap;
}

}  // namespace microtel::sdk
