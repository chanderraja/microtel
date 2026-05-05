// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/internal/clock.hpp"

#include <chrono>

namespace microtel::testing
{

/// @brief Steady-clock fake. Tests advance time via `Advance()`; `Now()`
/// returns the configured time.
///
/// Used by retry-budget arithmetic, batch-deadline timing, and timeout
/// enforcement. The fake's monotonic invariant is the same as the real
/// `std::chrono::steady_clock` — `Advance` can only move forward.
class FakeSteadyClock : public internal::ISteadyClock
{
public:
    internal::TimePointSteady now {};

    [[nodiscard]] internal::TimePointSteady Now() const noexcept override
    {
        return now;
    }

    /// @brief Advance the clock forward by `d`. Argument must be
    /// non-negative; runtime check kept out for spike-grade code, M3's
    /// production code can add an assertion.
    void Advance(std::chrono::nanoseconds d) noexcept
    {
        now += d;
    }
};

}  // namespace microtel::testing
