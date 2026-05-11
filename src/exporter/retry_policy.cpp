// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#include "exporter/retry_policy.hpp"

#include <algorithm>
#include <cmath>

namespace microtel::exporter
{

namespace
{
// Jitter formula maps uniform [0, 1] random to [-1, +1]:
//   sign = kJitterScale * jitter_01 - kJitterOrigin
constexpr double kJitterScale = 2.0;
constexpr double kJitterOrigin = 1.0;
constexpr double kBackoffFloor = 0.0;
}  // namespace

std::chrono::milliseconds ComputeBackoff(std::uint32_t attempt,
                                         const RetryPolicyConfig& config,
                                         std::optional<std::chrono::milliseconds> retry_after,
                                         double jitter_01) noexcept
{
    const auto initial_ms = static_cast<double>(config.initial_backoff.count());
    const auto max_ms = static_cast<double>(config.max_backoff.count());
    const double scaled =
        initial_ms * std::pow(config.backoff_multiplier, static_cast<double>(attempt));
    const double base_ms = std::min(scaled, max_ms);
    const double jitter =
        base_ms * config.jitter_fraction * ((kJitterScale * jitter_01) - kJitterOrigin);
    const double result_ms = std::max(kBackoffFloor, base_ms + jitter);
    auto backoff =
        std::chrono::milliseconds{static_cast<std::chrono::milliseconds::rep>(result_ms)};
    if (retry_after.has_value() && *retry_after > backoff)
    {
        backoff = *retry_after;
    }
    return backoff;
}

}  // namespace microtel::exporter
