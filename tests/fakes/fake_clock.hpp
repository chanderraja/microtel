// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/internal/clock.hpp"

#include <chrono>

namespace microtel::testing
{

/// @brief Wall-clock fake. Tests advance time via `Advance()`; `Now()`
/// returns the configured time.
///
/// Used by retry-timing, last-error-timestamp, and any other code that
/// reads `IClock`. Distinct from `FakeSteadyClock` because the two
/// abstractions answer different questions; in tests they typically
/// advance together but don't have to.
class FakeClock : public internal::IClock
{
public:
    /// @brief Current configured time. Tests may set this directly or
    /// drive it via `Advance()`.
    internal::TimePointWall now{};

    [[nodiscard]] internal::TimePointWall Now() const noexcept override
    {
        return now;
    }

    /// @brief Advance the clock forward by `d`.
    void Advance(std::chrono::nanoseconds d) noexcept
    {
        now += d;
    }
};

}  // namespace microtel::testing
