// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/internal/metric_batch.hpp"
#include "microtel/internal/metric_producer.hpp"
#include "microtel/resource.hpp"

#include "sdk/metric_stream.hpp"

#include <memory>
#include <vector>

namespace microtel::sdk
{

/// @brief Concrete `IMetricProducer` — the pull source for `IMetricReader`.
///
/// Holds one `ScopeEntry` per instrumentation scope (grouped by name+version).
/// Each entry owns its `IMetricStream` instances. `Collect()` loops over scopes
/// and streams, snapshots each stream's aggregation state, and assembles one
/// `MetricBatchHandle` per scope.
///
/// Streams are registered at SDK-build time via `AddStream`; no removal API
/// is needed in v1.2. The producer is move-only and single-owner.
///
/// @threadsafety Single-caller (the reader thread). Each stream's `Collect()`
/// acquires its own per-instrument mutex internally.
class MetricProducer : public internal::IMetricProducer
{
public:
    explicit MetricProducer(std::shared_ptr<const Resource> resource) noexcept
        : m_resource(std::move(resource))
    {
    }

    MetricProducer(const MetricProducer&) = delete;
    MetricProducer& operator=(const MetricProducer&) = delete;
    MetricProducer(MetricProducer&&) = delete;
    MetricProducer& operator=(MetricProducer&&) = delete;
    ~MetricProducer() noexcept override = default;

    /// @brief Register `stream` under `scope`. Creates the scope entry if new.
    void AddStream(internal::InstrumentationScope scope, std::unique_ptr<IMetricStream> stream);

    /// @brief Snapshot all streams. `temporality` defaults to `Cumulative` per the
    /// `IMetricProducer` contract; pass `Delta` to reset each stream after collection.
    [[nodiscard]] std::vector<internal::MetricBatchHandle> Collect(
        internal::AggregationTemporality temporality =
            internal::AggregationTemporality::Cumulative) override;

private:
    struct ScopeEntry
    {
        internal::InstrumentationScope scope;
        std::vector<std::unique_ptr<IMetricStream>> streams;
    };

    std::shared_ptr<const Resource> m_resource;
    std::vector<ScopeEntry> m_scopes;
};

}  // namespace microtel::sdk
