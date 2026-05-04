// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <chrono>

namespace microtel::internal
{

using TimePointWall   = std::chrono::system_clock::time_point;
using TimePointSteady = std::chrono::steady_clock::time_point;

/// @brief Wall-clock abstraction. Production injects `std::chrono::system_clock`;
/// tests inject a fake that advances on demand.
///
/// @threadsafety Thread-safe.
/// @see docs/interfaces.md §4.7
class IClock
{
public:
    virtual ~IClock() noexcept                              = default;
    [[nodiscard]] virtual TimePointWall Now() const noexcept = 0;
};

/// @brief Steady (monotonic) clock abstraction.
///
/// Used for batch deadlines, retry backoff, and timeout enforcement.
/// `Now()` is monotonically non-decreasing.
///
/// @threadsafety Thread-safe.
class ISteadyClock
{
public:
    virtual ~ISteadyClock() noexcept                            = default;
    [[nodiscard]] virtual TimePointSteady Now() const noexcept = 0;
};

}  // namespace microtel::internal
