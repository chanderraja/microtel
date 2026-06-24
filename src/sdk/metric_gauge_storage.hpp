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

/// @brief Live aggregation state for a Gauge instrument (synchronous Gauge /
/// ObservableGauge), per `docs/metrics-design.md` §8/§9.
///
/// Last-write-wins per attribute set behind a per-instrument mutex; `Collect()`
/// snapshots the latest values into `internal::GaugeData`. A gauge has no
/// temporality — it is always the most recent value. `T` is the measurement
/// value type (`std::int64_t` or `double`; the only instantiations defined in
/// the .cpp).
///
/// v1.2 scope note: same as `SumStorage` — cardinality limits and value
/// validation land in later increments; lookup materialises an `AttributeSet`
/// per `Record` (no-allocation heterogeneous probe is a follow-up).
///
/// @threadsafety Thread-safe — `Record` and `Collect` may be called concurrently.
template <typename T>
class GaugeStorage
{
public:
    explicit GaugeStorage(std::size_t max_cardinality = kDefaultMaxCardinality) noexcept
        : m_max_cardinality(max_cardinality)
    {
    }

    /// @brief Store `value` as the latest reading for `attrs` (hot path).
    void Record(T value, AttributeSpan attrs);

    /// @brief Snapshot the latest readings as `GaugeData`.
    [[nodiscard]] internal::GaugeData Collect() const;

private:
    mutable std::mutex m_mu;
    std::unordered_map<AttributeSet, T, AttributeSetHash> m_points;
    std::size_t m_max_cardinality;
};

}  // namespace microtel::sdk
