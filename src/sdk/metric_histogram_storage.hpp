// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/attribute.hpp"
#include "microtel/internal/diagnostics_sink.hpp"
#include "microtel/internal/icurrent_span_source.hpp"
#include "microtel/internal/metric_batch.hpp"

#include "sdk/metric_attribute_set.hpp"

#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace microtel::sdk
{

/// @brief Live aggregation state for an explicit-bucket Histogram instrument,
/// per `docs/metrics-design.md` §3.
///
/// Per attribute set, tracks count / sum / min / max and per-bucket counts
/// against a shared, immutable boundary ladder. Bucket semantics are
/// OTLP-standard upper-inclusive: a value `v` falls in bucket `i` = the
/// smallest `i` with `v <= bounds[i]`, or bucket `n` if `v > bounds[n-1]`
/// (so `bucket_counts.size() == bounds.size() + 1`). `Collect()` snapshots
/// cumulative `internal::HistogramData`. `T` is the measurement value type
/// (`std::int64_t` or `double`); values are bucketed as `double`.
///
/// v1.2 scope note: cumulative-only; exponential histograms, delta/reset,
/// cardinality limits, and exemplars land in their own increments. Lookup
/// materialises an `AttributeSet` per `Record` (no-alloc probe is a follow-up).
///
/// @threadsafety Thread-safe — `Record` and `Collect` may be called concurrently.
template <typename T>
class HistogramStorage
{
public:
    /// @param diag Non-owning, borrowed diagnostics sink for overflow drop
    ///        accounting; null disables it. Lifetime: the owning provider
    ///        outlives this storage.
    explicit HistogramStorage(std::vector<double> boundaries,
                              std::size_t max_cardinality = kDefaultMaxCardinality,
                              const internal::ICurrentSpanSource* span_source = nullptr,
                              internal::IDiagnosticsSink* diag = nullptr)
        : m_boundaries(std::move(boundaries)),
          m_max_cardinality(max_cardinality),
          m_span_source(span_source),
          m_diag(diag)
    {
    }

    /// @brief Record `value` into the point for `attrs` (hot path).
    void Record(T value, AttributeSpan attrs);

    /// @brief Snapshot the buckets as `HistogramData`. Cumulative (default)
    /// retains state; Delta reports the buckets accumulated since the previous
    /// collect and then clears the live state.
    [[nodiscard]] internal::HistogramData Collect(internal::AggregationTemporality temporality =
                                                      internal::AggregationTemporality::Cumulative);

private:
    struct Point
    {
        std::uint64_t count = 0;
        double sum = 0.0;
        double min = 0.0;
        double max = 0.0;
        std::vector<std::uint64_t> bucket_counts;
    };

    mutable std::mutex m_mu;
    std::vector<double> m_boundaries;  ///< shared, immutable upper bounds (sorted)
    std::size_t m_max_cardinality;
    const internal::ICurrentSpanSource* m_span_source;  ///< non-owning; null disables exemplars
    internal::IDiagnosticsSink* m_diag;  ///< non-owning; null disables drop accounting
    std::unordered_map<AttributeSet, Point, AttributeSetHash> m_points;
    std::unordered_map<AttributeSet, internal::Exemplar, AttributeSetHash> m_exemplars;
};

}  // namespace microtel::sdk
