// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/attribute.hpp"
#include "microtel/internal/metric_batch.hpp"

#include "sdk/metric_attribute_set.hpp"
#include "sdk/metric_stream.hpp"

#include <functional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace microtel::sdk
{

/// @brief Observer API passed to an observable instrument callback.
///
/// The callback calls `Observe()` once per attribute set it wishes to report.
/// Values from a previous collection cycle are discarded; the map is cleared
/// before each callback invocation.
///
/// @tparam T Value type: `std::int64_t` or `double`.
template <typename T>
class ObservableResult
{
public:
    ObservableResult() noexcept = default;

    ObservableResult(const ObservableResult&) = delete;
    ObservableResult& operator=(const ObservableResult&) = delete;
    ObservableResult(ObservableResult&&) = delete;
    ObservableResult& operator=(ObservableResult&&) = delete;
    ~ObservableResult() noexcept = default;

    /// @brief Record one observation for @p attrs.
    /// Last call wins for a given attribute set within one callback cycle.
    void Observe(T value, AttributeSpan attrs)
    {
        m_map[AttributeSet{attrs}] = value;
    }

    /// @brief Discard all accumulated observations.
    void Clear() noexcept
    {
        m_map.clear();
    }

    /// @brief Read-only view of the current observations.
    [[nodiscard]] const std::unordered_map<AttributeSet, T, AttributeSetHash>& Map() const noexcept
    {
        return m_map;
    }

private:
    std::unordered_map<AttributeSet, T, AttributeSetHash> m_map;
};

/// @brief Signature of an observable instrument callback.
template <typename T>
using ObservableCallback = std::function<void(ObservableResult<T>&)>;

// ── ObservableCounter / ObservableUpDownCounter handle ───────────────────────

/// @brief RAII handle for a registered observable Sum instrument.
///
/// Holds no state in M12; in v1.2 it will own a callback-registration token
/// whose destruction deregisters the callback from the Meter.
template <typename T>
class ObservableCounter
{
};

/// @brief RAII handle for a registered observable UpDownCounter instrument.
template <typename T>
class ObservableUpDownCounter
{
};

/// @brief RAII handle for a registered observable Gauge instrument.
template <typename T>
class ObservableGauge
{
};

// ── Observable stream implementations ────────────────────────────────────────

/// @brief `IMetricStream` for ObservableCounter / ObservableUpDownCounter.
///
/// On `Collect()`: clears the `ObservableResult`, invokes the callback to
/// populate it, then converts the result map into a `SumData` with one
/// `NumberPoint` per attribute set the callback reported.
///
/// @tparam T Value type: `std::int64_t` or `double`.
template <typename T>
class MetricStreamObservableSum : public IMetricStream
{
public:
    MetricStreamObservableSum(std::string name,
                              std::string description,
                              std::string unit,
                              bool monotonic,
                              ObservableCallback<T> callback)
        : m_name(std::move(name)),
          m_description(std::move(description)),
          m_unit(std::move(unit)),
          m_monotonic(monotonic),
          m_callback(std::move(callback))
    {
    }

    MetricStreamObservableSum(const MetricStreamObservableSum&) = delete;
    MetricStreamObservableSum& operator=(const MetricStreamObservableSum&) = delete;
    MetricStreamObservableSum(MetricStreamObservableSum&&) = delete;
    MetricStreamObservableSum& operator=(MetricStreamObservableSum&&) = delete;
    ~MetricStreamObservableSum() noexcept override = default;

    [[nodiscard]] internal::MetricRecord Collect(
        internal::AggregationTemporality temporality) override
    {
        m_result.Clear();
        m_callback(m_result);

        internal::SumData data;
        data.is_monotonic = m_monotonic;
        data.temporality = temporality;
        for (const auto& [attr_set, value] : m_result.Map())
        {
            const auto pairs = attr_set.Pairs();
            data.points.push_back(internal::NumberPoint{
                .attributes = std::vector<KeyValue>(pairs.begin(), pairs.end()),
                .value = value,
                .exemplars = {},
            });
        }
        return internal::MetricRecord{
            .name = m_name,
            .description = m_description,
            .unit = m_unit,
            .data = std::move(data),
        };
    }

private:
    std::string m_name;
    std::string m_description;
    std::string m_unit;
    bool m_monotonic;
    ObservableCallback<T> m_callback;
    ObservableResult<T> m_result;
};

/// @brief `IMetricStream` for ObservableGauge.
///
/// Same as `MetricStreamObservableSum` but yields `GaugeData` (no temporality).
///
/// @tparam T Value type: `std::int64_t` or `double`.
template <typename T>
class MetricStreamObservableGauge : public IMetricStream
{
public:
    MetricStreamObservableGauge(std::string name,
                                std::string description,
                                std::string unit,
                                ObservableCallback<T> callback)
        : m_name(std::move(name)),
          m_description(std::move(description)),
          m_unit(std::move(unit)),
          m_callback(std::move(callback))
    {
    }

    MetricStreamObservableGauge(const MetricStreamObservableGauge&) = delete;
    MetricStreamObservableGauge& operator=(const MetricStreamObservableGauge&) = delete;
    MetricStreamObservableGauge(MetricStreamObservableGauge&&) = delete;
    MetricStreamObservableGauge& operator=(MetricStreamObservableGauge&&) = delete;
    ~MetricStreamObservableGauge() noexcept override = default;

    [[nodiscard]] internal::MetricRecord Collect(
        internal::AggregationTemporality /*temporality*/) override
    {
        m_result.Clear();
        m_callback(m_result);

        internal::GaugeData data;
        for (const auto& [attr_set, value] : m_result.Map())
        {
            const auto pairs = attr_set.Pairs();
            data.points.push_back(internal::NumberPoint{
                .attributes = std::vector<KeyValue>(pairs.begin(), pairs.end()),
                .value = value,
                .exemplars = {},
            });
        }
        return internal::MetricRecord{
            .name = m_name,
            .description = m_description,
            .unit = m_unit,
            .data = std::move(data),
        };
    }

private:
    std::string m_name;
    std::string m_description;
    std::string m_unit;
    ObservableCallback<T> m_callback;
    ObservableResult<T> m_result;
};

}  // namespace microtel::sdk
