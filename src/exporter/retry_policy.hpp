// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <chrono>
#include <cstdint>
#include <optional>

namespace microtel::exporter
{

/// @brief Retry / backoff configuration for `OtlpExporter`.
///
/// OTLP-spec recommended defaults: 5 attempts, 1 s initial backoff,
/// 32 s ceiling, 1.5× multiplier, ±20 % jitter, 5-minute retry budget.
struct RetryPolicyConfig
{
    /// @brief Total attempts (first try + retries). Must be ≥ 1.
    std::uint32_t max_attempts = 5;
    /// @brief Initial backoff before the second attempt.
    std::chrono::milliseconds initial_backoff{1000};
    /// @brief Backoff ceiling — exponential growth is clamped here.
    std::chrono::milliseconds max_backoff{32000};
    /// @brief Multiplier applied to the backoff on each successive attempt.
    double backoff_multiplier = 1.5;
    /// @brief Symmetric jitter fraction, e.g. 0.2 ⇒ ±20 % around the base.
    double jitter_fraction = 0.2;
    /// @brief Maximum elapsed time across all retry attempts for a single batch.
    std::chrono::milliseconds retry_budget{std::chrono::minutes(5)};
};

/// @brief Compute the sleep duration before re-trying a failed attempt.
///
/// @param attempt      0-indexed after the first failure (0 = wait before 2nd try).
/// @param config       Active retry policy.
/// @param retry_after  Server-supplied minimum wait; overrides backoff when larger.
/// @param jitter_01    Uniform random sample in [0, 1). Pass 0.5 for zero net jitter.
///
/// @return Milliseconds to sleep. Never negative; honours `max_backoff` and `retry_after`.
[[nodiscard]] std::chrono::milliseconds ComputeBackoff(
    std::uint32_t attempt,
    const RetryPolicyConfig& config,
    std::optional<std::chrono::milliseconds> retry_after,
    double jitter_01) noexcept;

}  // namespace microtel::exporter
