// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#include "sdk/metric_histogram_storage.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <utility>

namespace microtel::sdk
{

template <typename T>
void HistogramStorage<T>::Record(T value, AttributeSpan attrs)
{
    const auto observation = static_cast<double>(value);
    if (!std::isfinite(observation))
    {
        return;
    }

    const std::scoped_lock lock{m_mu};
    AttributeSet key{attrs};
    auto it = m_points.find(key);
    if (it == m_points.end())
    {
        if (m_points.size() >= m_max_cardinality)
        {
            key = OverflowAttributeSet();
        }
        it = m_points.try_emplace(std::move(key)).first;
    }
    Point& point = it->second;
    if (point.bucket_counts.empty())  // newly inserted entry (normal or first overflow use)
    {
        point.bucket_counts.assign(m_boundaries.size() + 1, 0);
        point.min = observation;
        point.max = observation;
    }
    else
    {
        point.min = std::min(point.min, observation);
        point.max = std::max(point.max, observation);
    }

    // Upper-inclusive bucket: first boundary >= value, else the overflow bucket.
    const auto bound = std::ranges::lower_bound(m_boundaries, observation);
    const auto bucket = static_cast<std::size_t>(bound - m_boundaries.begin());
    ++point.bucket_counts[bucket];
    ++point.count;
    point.sum += observation;
}

template <typename T>
internal::HistogramData HistogramStorage<T>::Collect(internal::AggregationTemporality temporality)
{
    const std::scoped_lock lock{m_mu};

    internal::HistogramData data;
    data.temporality = temporality;
    data.points.reserve(m_points.size());

    for (const auto& [key, point] : m_points)
    {
        internal::HistogramPoint out;
        const auto pairs = key.Pairs();
        out.attributes.assign(pairs.begin(), pairs.end());
        out.count = point.count;
        out.sum = point.sum;
        out.min = point.min;
        out.max = point.max;
        out.bucket_counts = point.bucket_counts;
        out.explicit_bounds = m_boundaries;
        data.points.push_back(std::move(out));
    }

    if (temporality == internal::AggregationTemporality::Delta)
    {
        m_points.clear();
    }
    return data;
}

template class HistogramStorage<std::int64_t>;
template class HistogramStorage<double>;

}  // namespace microtel::sdk
