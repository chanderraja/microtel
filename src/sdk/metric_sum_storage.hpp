// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/attribute.hpp"
#include "microtel/internal/metric_batch.hpp"

#include "sdk/metric_attribute_set.hpp"

#include <mutex>
#include <unordered_map>

namespace microtel::sdk
{

/// @brief Live aggregation state for a Sum instrument (Counter /
/// UpDownCounter / ObservableCounter), per `docs/metrics-design.md` §1/§9.
///
/// Accumulates a running total per attribute set behind a per-instrument mutex;
/// `Collect()` snapshots the totals into the OTLP-shaped `internal::SumData`.
/// `T` is the measurement value type — `std::int64_t` or `double` (the only
/// instantiations defined in the .cpp).
///
/// v1.2 scope note: temporality is cumulative (retain on collect); delta/reset,
/// cardinality limits, and value validation land in their own increments.
/// Lookup currently materialises an `AttributeSet` per `Add`; the no-allocation
/// heterogeneous probe (via `AttributeSet::HashOf`/`MatchesSpan`) is a
/// benchmark-driven follow-up.
///
/// @threadsafety Thread-safe — `Add` and `Collect` may be called concurrently.
template <typename T>
class SumStorage
{
public:
    explicit SumStorage(bool monotonic,
                        std::size_t max_cardinality = kDefaultMaxCardinality) noexcept
        : m_monotonic(monotonic), m_max_cardinality(max_cardinality)
    {
    }

    /// @brief Accumulate `value` into the point for `attrs` (hot path).
    void Add(T value, AttributeSpan attrs);

    /// @brief Snapshot the running totals as `SumData`.
    ///
    /// Cumulative (default) retains state across collects. Delta reports the
    /// totals accumulated since the previous collect and then clears the live
    /// state (so series with no new measurements are simply absent next cycle).
    [[nodiscard]] internal::SumData Collect(internal::AggregationTemporality temporality =
                                                internal::AggregationTemporality::Cumulative);

private:
    mutable std::mutex m_mu;
    std::unordered_map<AttributeSet, T, AttributeSetHash> m_points;
    bool m_monotonic;
    std::size_t m_max_cardinality;
};

}  // namespace microtel::sdk
