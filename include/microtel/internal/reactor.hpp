// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/error.hpp"
#include "microtel/expected.hpp"
#include "microtel/internal/clock.hpp"

#include <cstdint>
#include <functional>

namespace microtel::internal
{

/// @brief Bitmask of reactor events of interest.
enum class EventMask : std::uint32_t
{
    None  = 0,
    Read  = 1U << 0,
    Write = 1U << 1,
    Error = 1U << 2,
};

/// @brief Compose two event masks (bitwise-OR).
[[nodiscard]] constexpr EventMask operator|(EventMask a, EventMask b) noexcept
{
    return static_cast<EventMask>(
        static_cast<std::uint32_t>(a) | static_cast<std::uint32_t>(b));
}

/// @brief Test event-mask membership.
[[nodiscard]] constexpr bool HasEvent(EventMask mask, EventMask flag) noexcept
{
    return (static_cast<std::uint32_t>(mask) & static_cast<std::uint32_t>(flag)) != 0U;
}

/// @brief Callback invoked when an event fires for a registered fd.
///
/// Runs on the I/O thread. Must not block. Must not call back into the
/// reactor that's invoking it (no Register / Modify / Unregister of the same
/// fd from inside its own callback).
using EventCallback = std::function<void(int fd, EventMask events)>;

/// @brief epoll/kqueue abstraction.
///
/// The transport's I/O loop uses this interface; tests inject a fake reactor
/// that delivers events on test-driven schedules — useful for verifying
/// GOAWAY mid-batch, RST_STREAM mid-stream, and partial-frame edge cases
/// without an actual socket.
///
/// @threadsafety All methods other than `Wake` are I/O-thread-only (LOCKED).
///               `Wake` is the explicit cross-thread wakeup primitive.
/// @see docs/interfaces.md §4.8
class IReactor
{
public:
    virtual ~IReactor() noexcept = default;

    [[nodiscard]] virtual microtel::Expected<void, microtel::Error>
        Register(int fd, EventMask mask, EventCallback cb) = 0;

    virtual void Modify(int fd, EventMask mask) = 0;

    virtual void Unregister(int fd) noexcept = 0;

    /// @brief Block up to `deadline` waiting for events; dispatch each via its
    /// registered callback. Returns the number of events dispatched.
    virtual std::size_t WaitAndDispatch(TimePointSteady deadline) = 0;

    /// @brief Wake the loop if currently in `WaitAndDispatch` on another thread.
    virtual void Wake() noexcept = 0;
};

}  // namespace microtel::internal
