// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#include "sdk/metric_producer.hpp"

#include <memory>
#include <utility>
#include <vector>

namespace microtel::sdk
{

void MetricProducer::AddStream(internal::InstrumentationScope scope,
                               std::unique_ptr<IMetricStream> stream)
{
    for (ScopeEntry& entry : m_scopes)
    {
        if (entry.scope.name == scope.name && entry.scope.version == scope.version)
        {
            entry.streams.push_back(std::move(stream));
            return;
        }
    }
    m_scopes.push_back({.scope = std::move(scope), .streams = {}});
    m_scopes.back().streams.push_back(std::move(stream));
}

std::vector<internal::MetricBatchHandle> MetricProducer::Collect()
{
    return Collect(internal::AggregationTemporality::Cumulative);
}

std::vector<internal::MetricBatchHandle> MetricProducer::Collect(
    internal::AggregationTemporality temporality)
{
    std::vector<internal::MetricBatchHandle> handles;
    handles.reserve(m_scopes.size());
    for (const ScopeEntry& entry : m_scopes)
    {
        std::vector<internal::MetricRecord> records;
        records.reserve(entry.streams.size());
        for (const auto& stream : entry.streams)
        {
            records.push_back(stream->Collect(temporality));
        }
        handles.emplace_back(std::move(records), m_resource, entry.scope);
    }
    return handles;
}

}  // namespace microtel::sdk
