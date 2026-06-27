// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/attribute.hpp"
#include "microtel/internal/metric_batch.hpp"

#include "sdk/metric_attribute_set.hpp"

#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace microtel::sdk
{

/// @brief Live aggregation state for an explicit-bucket Histogram instrument,
/// per `docs/metrics-design.md` §3.
///
/// Per attribute set, tracks count / sum / min / max and per-bucket counts
/// against a shared, immutable boundary ladder. Bucket semantics are
/// OTLP-standard upper-inclusive: a value `v` falls in bucket `i` = the
/// smallest `i` with `v <= bounds[i]`, or bucket `n` if `v > bounds[n-1]`
/// (so `bucket_counts.size() == bounds.size() + 1`). `Collect()` snapshots
/// cumulative `internal::HistogramData`. `T` is the measurement value type
/// (`std::int64_t` or `double`); values are bucketed as `double`.
///
/// v1.2 scope note: cumulative-only; exponential histograms, delta/reset,
/// cardinality limits, and exemplars land in their own increments. Lookup
/// materialises an `AttributeSet` per `Record` (no-alloc probe is a follow-up).
///
/// @threadsafety Thread-safe — `Record` and `Collect` may be called concurrently.
template <typename T>
class HistogramStorage
{
public:
    explicit HistogramStorage(std::vector<double> boundaries) : m_boundaries(std::move(boundaries))
    {
    }

    /// @brief Record `value` into the point for `attrs` (hot path).
    void Record(T value, AttributeSpan attrs);

    /// @brief Snapshot the buckets as `HistogramData`. Cumulative (default)
    /// retains state; Delta reports the buckets accumulated since the previous
    /// collect and then clears the live state.
    [[nodiscard]] internal::HistogramData Collect(internal::AggregationTemporality temporality =
                                                      internal::AggregationTemporality::Cumulative);

private:
    struct Point
    {
        std::uint64_t count = 0;
        double sum = 0.0;
        double min = 0.0;
        double max = 0.0;
        std::vector<std::uint64_t> bucket_counts;
    };

    mutable std::mutex m_mu;
    std::vector<double> m_boundaries;  ///< shared, immutable upper bounds (sorted)
    std::unordered_map<AttributeSet, Point, AttributeSetHash> m_points;
};

}  // namespace microtel::sdk
