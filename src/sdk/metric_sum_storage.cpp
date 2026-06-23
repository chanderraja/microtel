// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#include "sdk/metric_sum_storage.hpp"

#include <cstdint>
#include <mutex>
#include <utility>

namespace microtel::sdk
{

template <typename T>
void SumStorage<T>::Add(T value, AttributeSpan attrs)
{
    const std::scoped_lock lock{m_mu};
    // Materialises an AttributeSet for the probe (allocates only on first
    // sight of a set — try_emplace inserts the key once, then accumulates).
    auto [it, inserted] = m_points.try_emplace(AttributeSet{attrs}, T{0});
    it->second += value;
}

template <typename T>
internal::SumData SumStorage<T>::Collect(internal::AggregationTemporality temporality)
{
    const std::scoped_lock lock{m_mu};

    internal::SumData data;
    data.temporality = temporality;
    data.is_monotonic = m_monotonic;
    data.points.reserve(m_points.size());

    for (const auto& [key, sum] : m_points)
    {
        internal::NumberPoint point;
        const auto pairs = key.Pairs();
        point.attributes.assign(pairs.begin(), pairs.end());
        point.value = sum;  // T (int64_t/double) selects the MetricValue alternative
        data.points.push_back(std::move(point));
    }

    // Delta reports the increment since the last collect, so clear live state.
    if (temporality == internal::AggregationTemporality::Delta)
    {
        m_points.clear();
    }
    return data;
}

template class SumStorage<std::int64_t>;
template class SumStorage<double>;

}  // namespace microtel::sdk
