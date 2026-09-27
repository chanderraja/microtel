// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/export_transport.hpp"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace microtel::testing
{

/// @brief Scripted fake for the public `microtel::ExportTransport` (ICP 0036).
///
/// Serves `SendResult`s from a FIFO script, then `default_result`, and keeps a
/// copy of every request. Optionally:
/// - `block_until_cancel`: every `Send` waits — past its deadline — until
///   `Cancel`, then returns `NonRetryable`. It models the transport that
///   ignores its deadline, the one `Cancel` exists for.
/// - `throw_std` / `throw_non_std`: `Send` throws instead of answering.
///
/// Thread-safe: a Provider with metrics and logs on calls `Send` from up to
/// three exporter workers at once.
class FakeExportTransport : public ExportTransport
{
public:
    /// @brief A copy of one `ExportRequest`.
    struct Recorded
    {
        ExportSignal signal = ExportSignal::Traces;
        std::vector<std::byte> bytes;
        std::chrono::steady_clock::time_point deadline;
    };

    /// @brief Thrown by `Send` when `throw_non_std` is set; not a `std::exception`.
    struct NonStdThrow
    {
    };

    // Configure before the transport is handed to a Provider.
    std::deque<SendResult> scripted_results;
    SendResult default_result{.outcome = SendOutcome::Success};
    bool block_until_cancel = false;
    bool throw_std = false;
    bool throw_non_std = false;

    [[nodiscard]] SendResult Send(const ExportRequest& request) override
    {
        std::unique_lock lock{m_mu};
        m_sent.push_back(Recorded{.signal = request.signal,
                                  .bytes = {request.bytes.begin(), request.bytes.end()},
                                  .deadline = request.deadline});
        m_cv.notify_all();
        if (throw_std)
        {
            throw std::runtime_error("link exploded");
        }
        if (throw_non_std)
        {
            throw NonStdThrow{};
        }
        if (block_until_cancel)
        {
            m_cv.wait(lock, [this] { return m_cancel_calls > 0; });
            return SendResult{.outcome = SendOutcome::NonRetryable, .message = "cancelled"};
        }
        if (!scripted_results.empty())
        {
            SendResult r = scripted_results.front();
            scripted_results.pop_front();
            return r;
        }
        return default_result;
    }

    void Cancel() noexcept override
    {
        const std::scoped_lock lock{m_mu};
        ++m_cancel_calls;
        m_cv.notify_all();
    }

    [[nodiscard]] std::vector<Recorded> Sent() const
    {
        const std::scoped_lock lock{m_mu};
        return m_sent;
    }

    [[nodiscard]] int CancelCalls() const
    {
        const std::scoped_lock lock{m_mu};
        return m_cancel_calls;
    }

    /// @brief Wait until at least @p n requests have arrived, or @p timeout.
    [[nodiscard]] bool WaitForSends(std::size_t n, std::chrono::milliseconds timeout) const
    {
        std::unique_lock lock{m_mu};
        return m_cv.wait_for(lock, timeout, [this, n] { return m_sent.size() >= n; });
    }

private:
    mutable std::mutex m_mu;
    mutable std::condition_variable m_cv;
    std::vector<Recorded> m_sent;
    int m_cancel_calls = 0;
};

}  // namespace microtel::testing
