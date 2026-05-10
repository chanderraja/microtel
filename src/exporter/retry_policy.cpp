// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#include "exporter/retry_policy.hpp"

#include <algorithm>
#include <cmath>

namespace microtel::exporter
{

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
    const double jitter = base_ms * config.jitter_fraction * ((2.0 * jitter_01) - 1.0);
    const double result_ms = std::max(0.0, base_ms + jitter);
    auto backoff =
        std::chrono::milliseconds{static_cast<std::chrono::milliseconds::rep>(result_ms)};
    if (retry_after.has_value() && *retry_after > backoff)
    {
        backoff = *retry_after;
    }
    return backoff;
}

}  // namespace microtel::exporter
