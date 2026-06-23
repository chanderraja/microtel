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

/// @brief One sign's bucket array for an exponential histogram point.
/// `counts[k]` is the count for absolute bucket index `offset + k`.
struct ExpBuckets
{
    std::int32_t offset = 0;
    std::vector<std::uint64_t> counts;
};

/// @brief Live exponential-histogram state for one attribute set. Scalars are
/// `T`-independent so the heavy logic lives outside the storage template.
struct ExpHistoPoint
{
    std::int32_t scale = 0;
    std::uint64_t zero_count = 0;
    std::uint64_t count = 0;
    double sum = 0.0;
    double min = 0.0;
    double max = 0.0;
    ExpBuckets positive;
    ExpBuckets negative;
};

/// @brief Live aggregation state for a base-2 exponential-histogram instrument,
/// per `docs/metrics-design.md` §3.
///
/// Per attribute set: a `scale` (starting at `max_scale`), a zero-count, and
/// positive/negative bucket arrays. For value `v > 0` at scale `s` the bucket
/// index is `ceil(log2(v) * 2^s) - 1`. When a value would push a bucket array
/// past `max_buckets`, the point is **downscaled** (scale reduced by `k`,
/// indices floor-divided by `2^k`, buckets merged) until it fits. Negative
/// values go to the negative array (by magnitude); zeros to the zero count.
/// `Collect()` snapshots cumulative `internal::ExponentialHistogramData`.
/// `T` is the measurement value type (`std::int64_t` / `double`).
///
/// Known caveat: the mapping uses `std::log2`, which has the floating-point
/// boundary subtlety at exact powers of two shared with the OTel reference.
///
/// v1.2 scope note: cumulative-only; delta/reset, cardinality, and exemplars
/// land in their own increments.
///
/// @threadsafety Thread-safe — `Record` and `Collect` may be called concurrently.
template <typename T>
class ExponentialHistogramStorage
{
public:
    ExponentialHistogramStorage(std::int32_t max_scale, std::int32_t max_buckets)
        : m_max_scale(max_scale), m_max_buckets(max_buckets)
    {
    }

    /// @brief Record `value` into the point for `attrs` (hot path).
    void Record(T value, AttributeSpan attrs);

    /// @brief Snapshot the buckets as cumulative `ExponentialHistogramData`.
    [[nodiscard]] internal::ExponentialHistogramData Collect() const;

private:
    mutable std::mutex m_mu;
    std::int32_t m_max_scale;
    std::int32_t m_max_buckets;  ///< per-sign bucket-count cap (downscale trigger)
    std::unordered_map<AttributeSet, ExpHistoPoint, AttributeSetHash> m_points;
};

}  // namespace microtel::sdk
