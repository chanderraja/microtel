// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/attribute.hpp"
#include "microtel/internal/batch.hpp"  // InstrumentationScope (reused)
#include "microtel/resource.hpp"
#include "microtel/trace.hpp"  // SpanContext (exemplar linkage)

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace microtel::internal
{

/// @brief A single measurement value. OTLP NumberDataPoint / Exemplar carry a
/// `oneof { as_int; as_double }`; this mirrors it.
using MetricValue = std::variant<std::int64_t, double>;

/// @brief Aggregation temporality (OTLP). Default cumulative per
/// `docs/metrics-design.md` §1.
enum class AggregationTemporality : std::uint8_t
{
    Cumulative = 0,
    Delta = 1,
};

/// @brief A sampled raw measurement attached to an aggregated point, carrying
/// the trace context of the span active when it was recorded (`metrics-design.md`
/// §7). `span_context.IsValid()` is false when no span was active.
struct Exemplar
{
    SpanContext span_context{};
    std::chrono::system_clock::time_point time{};
    MetricValue value{std::int64_t{0}};
    std::vector<KeyValue> filtered_attributes;
};

/// @brief A Sum / Gauge data point (OTLP NumberDataPoint).
struct NumberPoint
{
    std::vector<KeyValue> attributes;
    std::chrono::system_clock::time_point start_time{};
    std::chrono::system_clock::time_point time{};
    MetricValue value{std::int64_t{0}};
    std::vector<Exemplar> exemplars;
};

/// @brief An explicit-bucket histogram data point (OTLP HistogramDataPoint).
/// `bucket_counts.size() == explicit_bounds.size() + 1`.
struct HistogramPoint
{
    std::vector<KeyValue> attributes;
    std::chrono::system_clock::time_point start_time{};
    std::chrono::system_clock::time_point time{};
    std::uint64_t count = 0;
    double sum = 0.0;
    std::optional<double> min;
    std::optional<double> max;
    std::vector<std::uint64_t> bucket_counts;
    std::vector<double> explicit_bounds;
    std::vector<Exemplar> exemplars;
};

/// @brief One sign's buckets for an exponential histogram (OTLP
/// ExponentialHistogramDataPoint.Buckets).
struct ExponentialHistogramBuckets
{
    std::int32_t offset = 0;
    std::vector<std::uint64_t> bucket_counts;
};

/// @brief A base-2 exponential histogram data point
/// (OTLP ExponentialHistogramDataPoint), per `metrics-design.md` §3.
struct ExponentialHistogramPoint
{
    std::vector<KeyValue> attributes;
    std::chrono::system_clock::time_point start_time{};
    std::chrono::system_clock::time_point time{};
    std::uint64_t count = 0;
    double sum = 0.0;
    std::optional<double> min;
    std::optional<double> max;
    std::int32_t scale = 0;
    std::uint64_t zero_count = 0;
    ExponentialHistogramBuckets positive;
    ExponentialHistogramBuckets negative;
    std::vector<Exemplar> exemplars;
};

/// @brief Sum aggregation (Counter / UpDownCounter / ObservableCounter / ...).
struct SumData
{
    AggregationTemporality temporality = AggregationTemporality::Cumulative;
    bool is_monotonic = false;
    std::vector<NumberPoint> points;
};

/// @brief Gauge aggregation (synchronous Gauge / ObservableGauge). No
/// temporality — a gauge is always the last value.
struct GaugeData
{
    std::vector<NumberPoint> points;
};

/// @brief Explicit-bucket histogram aggregation.
struct HistogramData
{
    AggregationTemporality temporality = AggregationTemporality::Cumulative;
    std::vector<HistogramPoint> points;
};

/// @brief Base-2 exponential histogram aggregation.
struct ExponentialHistogramData
{
    AggregationTemporality temporality = AggregationTemporality::Cumulative;
    std::vector<ExponentialHistogramPoint> points;
};

/// @brief The aggregation carried by one metric stream. Maps to the OTLP
/// `Metric` `oneof data`.
using MetricData = std::variant<SumData, GaugeData, HistogramData, ExponentialHistogramData>;

/// @brief One metric stream: identity (name/description/unit) plus its
/// aggregated data points. The collection-time analogue of `SpanRecord`.
struct MetricRecord
{
    std::string name;
    std::string description;
    std::string unit;
    MetricData data;
};

/// @brief A move-only owning snapshot of collected metric streams sharing one
/// `Resource` and one `InstrumentationScope`.
///
/// Produced by `IMetricProducer::Collect()` (one handle per scope), consumed by
/// `IMetricEncoder::Encode` / `IMetricExporter::Export`. The metrics analogue of
/// `BatchHandle`.
class MetricBatchHandle
{
public:
    MetricBatchHandle() noexcept = default;

    MetricBatchHandle(std::vector<MetricRecord> metrics,
                      std::shared_ptr<const Resource> resource,
                      InstrumentationScope scope) noexcept
        : m_metrics(std::move(metrics)), m_resource(std::move(resource)), m_scope(std::move(scope))
    {
    }

    MetricBatchHandle(const MetricBatchHandle&) = delete;
    MetricBatchHandle& operator=(const MetricBatchHandle&) = delete;
    MetricBatchHandle(MetricBatchHandle&&) noexcept = default;
    MetricBatchHandle& operator=(MetricBatchHandle&&) noexcept = default;
    ~MetricBatchHandle() noexcept = default;

    [[nodiscard]] std::span<const MetricRecord> Metrics() const noexcept
    {
        return {m_metrics.data(), m_metrics.size()};
    }
    [[nodiscard]] const Resource& ResourceRef() const noexcept
    {
        return *m_resource;
    }
    [[nodiscard]] const InstrumentationScope& Scope() const noexcept
    {
        return m_scope;
    }

private:
    std::vector<MetricRecord> m_metrics;
    std::shared_ptr<const Resource> m_resource;
    InstrumentationScope m_scope;
};

}  // namespace microtel::internal
