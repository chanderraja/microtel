// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/error.hpp"
#include "microtel/expected.hpp"
#include "microtel/internal/clock.hpp"
#include "microtel/internal/reactor.hpp"

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <utility>

namespace microtel::testing
{

/// @brief Scriptable `IReactor` for transport tests.
///
/// Tests register fds, queue events to fire on the next
/// `WaitAndDispatch`, and assert dispatch order. No real I/O. Used
/// pervasively in transport unit + integration tests; the reactor is
/// the seam that makes "GOAWAY mid-stream" / "RST_STREAM" / "partial
/// frame" reproducible without a real socket (per
/// `docs/sequences/goaway-handling.md`).
class FakeReactor : public internal::IReactor
{
public:
    struct ScriptedEvent
    {
        int fd;
        internal::EventMask mask;
    };

    /// @brief Events fired on the next `WaitAndDispatch`. Tests push
    /// onto this queue.
    std::deque<ScriptedEvent> scripted_events;

    /// @brief Recording: fds that were woken via `Wake()`.
    int wake_count = 0;
    /// @brief Recording: dispatch calls made.
    int wait_dispatch_count = 0;

    /// @brief Force `WaitAndDispatch` to return immediately even with
    /// no scripted events. Useful for shutdown-deadline tests.
    bool return_immediately = false;

    /// @brief Park every `WaitAndDispatch` call until `ReleaseDispatch()`.
    ///
    /// `Http2Transport`'s I/O thread is the only caller of `WaitAndDispatch`,
    /// so holding the dispatch holds the I/O thread: nothing is drained from
    /// the transport's request queue and nothing is submitted to nghttp2. That
    /// is what lets a test fill the request queue to an exact depth instead of
    /// racing the drain.
    ///
    /// `Wake()` deliberately does **not** release the hold: `Send` wakes the
    /// reactor on every call, and a hold any `Send` could lift would not be a
    /// hold. Engage it before `Http2Transport::Create` so the I/O thread parks
    /// on its first iteration.
    ///
    /// @warning Release before the transport is destroyed. `Close()` joins the
    ///          I/O thread, which cannot exit while it is parked here.
    void HoldDispatch() noexcept
    {
        const std::scoped_lock lk{m_hold_mu};
        m_held = true;
    }

    /// @brief Let a held `WaitAndDispatch` proceed, and stop holding.
    void ReleaseDispatch() noexcept
    {
        {
            const std::scoped_lock lk{m_hold_mu};
            m_held = false;
        }
        m_hold_cv.notify_all();
    }

    [[nodiscard]] microtel::Expected<void, microtel::Error> Register(
        int fd, internal::EventMask mask, internal::EventCallback cb) override
    {
        m_callbacks[fd] = Entry{mask, std::move(cb)};
        return {};
    }

    void Modify(int fd, internal::EventMask mask) override
    {
        auto it = m_callbacks.find(fd);
        if (it != m_callbacks.end())
        {
            it->second.mask = mask;
        }
    }

    void Unregister(int fd) noexcept override
    {
        m_callbacks.erase(fd);
    }

    std::size_t WaitAndDispatch(internal::TimePointSteady /*deadline*/) override
    {
        {
            std::unique_lock lk{m_hold_mu};
            m_hold_cv.wait(lk, [this] { return !m_held; });
        }
        ++wait_dispatch_count;
        if (scripted_events.empty() || return_immediately)
        {
            return 0;
        }
        std::size_t n = 0;
        while (!scripted_events.empty())
        {
            auto ev = scripted_events.front();
            scripted_events.pop_front();
            auto it = m_callbacks.find(ev.fd);
            if (it != m_callbacks.end())
            {
                it->second.callback(ev.fd, ev.mask);
                ++n;
            }
        }
        return n;
    }

    void Wake() noexcept override
    {
        ++wake_count;
    }

private:
    struct Entry
    {
        internal::EventMask mask;
        internal::EventCallback callback;
    };

    std::unordered_map<int, Entry> m_callbacks;

    std::mutex m_hold_mu;
    std::condition_variable m_hold_cv;
    bool m_held = false;
};

}  // namespace microtel::testing
