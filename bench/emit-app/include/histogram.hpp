// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Power-of-2 bucketed histogram for latency measurements.
// 64 buckets, nanosecond resolution.  No external dependencies.
// B1+ note: HdrHistogram deferred; this is sufficient for p50/p95/p99.

#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <limits>

namespace bench
{

/// Lock-free, append-only latency histogram with 64 power-of-2 buckets.
///
/// Bucket i covers [2^i, 2^(i+1)) nanoseconds.
/// Bucket 0 covers [0, 1) ns (sub-nanosecond — effectively zero).
/// Bucket 63 covers [2^63, inf) ns (> 290 years — overflow sentinel).
class Histogram
{
public:
    static constexpr int kBuckets = 64;

    Histogram() noexcept = default;

    Histogram(const Histogram&) = delete;
    Histogram& operator=(const Histogram&) = delete;
    Histogram(Histogram&&) = delete;
    Histogram& operator=(Histogram&&) = delete;

    ~Histogram() = default;

    /// Record one observation (nanoseconds).
    void Record(uint64_t ns) noexcept;

    /// Return the approximate p-th percentile in nanoseconds.
    /// p must be in [0.0, 1.0].  Returns 0 if no observations recorded.
    [[nodiscard]] uint64_t Percentile(double p) const noexcept;

    /// Total number of observations recorded.
    [[nodiscard]] uint64_t Count() const noexcept;

    /// Minimum observation recorded (UINT64_MAX if no observations).
    [[nodiscard]] uint64_t Min() const noexcept;

    /// Maximum observation recorded (0 if no observations).
    [[nodiscard]] uint64_t Max() const noexcept;

    /// Snapshot of all 64 bucket counts (index i = [2^i, 2^(i+1)) ns).
    [[nodiscard]] std::array<uint64_t, kBuckets> Buckets() const noexcept;

private:
    std::array<std::atomic<uint64_t>, kBuckets> m_buckets{};
    std::atomic<uint64_t> m_count{0};
    std::atomic<uint64_t> m_min{std::numeric_limits<uint64_t>::max()};
    std::atomic<uint64_t> m_max{0};
};

}  // namespace bench
