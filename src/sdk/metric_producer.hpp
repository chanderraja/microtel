// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/internal/metric_batch.hpp"
#include "microtel/internal/metric_producer.hpp"
#include "microtel/resource.hpp"

#include "sdk/metric_stream.hpp"

#include <memory>
#include <mutex>
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
/// Streams are registered via `AddStream` whenever the application creates an
/// instrument — **not** only at SDK-build time, as this comment previously
/// claimed. Every `SdkMeter::Create*` reaches it (25 call sites), so
/// registration races collection on the reader thread unless synchronised.
/// There is no removal API in v1.2, which is what makes the borrowed-pointer
/// snapshot in `Collect` safe: a stream, once added, lives as long as the
/// producer.
///
/// @threadsafety Thread-safe. `AddStream` and `Collect` may be called
/// concurrently from different threads. `Collect` snapshots the scope/stream
/// structure under `m_mu`, releases it, and only then calls into each stream —
/// so the producer's mutex is never held while a per-instrument mutex is
/// acquired (`docs/threading-model.md` §4).
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

    /// @brief One scope's borrowed streams, captured under `m_mu` so the
    ///        call-out to `IMetricStream::Collect` happens unlocked.
    struct ScopeSnapshot
    {
        internal::InstrumentationScope scope;
        std::vector<IMetricStream*> streams;  ///< borrowed; owned by m_scopes
    };

    /// @brief Copy the scope/stream structure under `m_mu`.
    [[nodiscard]] std::vector<ScopeSnapshot> SnapshotScopes() const;

    std::shared_ptr<const Resource> m_resource;
    /// Guards `m_scopes`. Application threads append through `AddStream` while
    /// the reader thread walks it in `Collect`; without this, a `push_back`
    /// that reallocates invalidates the iterators `Collect` is holding.
    mutable std::mutex m_mu;
    std::vector<ScopeEntry> m_scopes;
};

}  // namespace microtel::sdk
