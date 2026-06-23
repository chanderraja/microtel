// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#include "sdk/metric_gauge_storage.hpp"

#include <cstdint>
#include <mutex>
#include <utility>

namespace microtel::sdk
{

template <typename T>
void GaugeStorage<T>::Record(T value, AttributeSpan attrs)
{
    const std::scoped_lock lock{m_mu};
    // Last-write-wins: insert or overwrite the latest reading for this set.
    m_points[AttributeSet{attrs}] = value;
}

template <typename T>
internal::GaugeData GaugeStorage<T>::Collect() const
{
    const std::scoped_lock lock{m_mu};

    internal::GaugeData data;
    data.points.reserve(m_points.size());

    for (const auto& [key, value] : m_points)
    {
        internal::NumberPoint point;
        const auto pairs = key.Pairs();
        point.attributes.assign(pairs.begin(), pairs.end());
        point.value = value;  // T (int64_t/double) selects the MetricValue alternative
        data.points.push_back(std::move(point));
    }
    return data;
}

template class GaugeStorage<std::int64_t>;
template class GaugeStorage<double>;

}  // namespace microtel::sdk
