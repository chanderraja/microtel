// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/internal/metric_batch.hpp"

#include <vector>

namespace microtel::internal
{

/// @brief The pull source the `IMetricReader` collects from — the metrics SDK
/// core (meter registry + per-instrument aggregation state).
///
/// `Collect()` snapshots the current aggregation state into one
/// `MetricBatchHandle` per instrumentation scope and runs any registered
/// async-instrument callbacks. It is invoked on the reader thread
/// (`metrics-design.md` §4/§9), never on a caller hot path.
///
/// Not `noexcept`: it performs collection work and runs user callbacks. The
/// reader invokes it inside a `try`/`catch (const std::exception&)` boundary —
/// mirroring how `OtlpExporter::DrainQueue` wraps `FanOutAndProcess` — so a
/// throwing callback is caught and counted, not propagated.
///
/// @threadsafety Single-caller (the reader thread).
/// @see docs/metrics-design.md §5
class IMetricProducer
{
public:
    virtual ~IMetricProducer() noexcept = default;

    [[nodiscard]] virtual std::vector<MetricBatchHandle> Collect(
        AggregationTemporality temporality = AggregationTemporality::Cumulative) = 0;
};

}  // namespace microtel::internal
