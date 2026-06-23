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
    explicit SumStorage(bool monotonic) noexcept : m_monotonic(monotonic) {}

    /// @brief Accumulate `value` into the point for `attrs` (hot path).
    void Add(T value, AttributeSpan attrs);

    /// @brief Snapshot the running totals as cumulative `SumData`.
    [[nodiscard]] internal::SumData Collect() const;

private:
    mutable std::mutex m_mu;
    std::unordered_map<AttributeSet, T, AttributeSetHash> m_points;
    bool m_monotonic;
};

}  // namespace microtel::sdk
