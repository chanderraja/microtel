// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/attribute.hpp"

#include "sdk/metric_gauge_storage.hpp"
#include "sdk/metric_histogram_storage.hpp"
#include "sdk/metric_sum_storage.hpp"

namespace microtel::sdk
{

/// @brief Handle to a monotonic Sum instrument (Counter).
///
/// Thin value type; holds a non-owning pointer to the `SumStorage<T>` owned by
/// the `MetricStreamSum<T>` registered with the `MetricProducer`. The caller
/// must not use this handle after the owning `Meter` (and thus its producer)
/// has been destroyed.
///
/// @tparam T Value type: `std::int64_t` or `double`.
template <typename T>
class Counter
{
public:
    explicit Counter(SumStorage<T>* storage) noexcept : m_storage(storage) {}

    /// @brief Record a measurement on the hot path.
    void Add(T value, AttributeSpan attrs)
    {
        m_storage->Add(value, attrs);
    }

private:
    SumStorage<T>* m_storage;
};

/// @brief Handle to a non-monotonic Sum instrument (UpDownCounter).
///
/// Identical to `Counter<T>` but backed by a `SumStorage` with
/// `monotonic = false`; negative `Add()` values are accepted.
///
/// @tparam T Value type: `std::int64_t` or `double`.
template <typename T>
class UpDownCounter
{
public:
    explicit UpDownCounter(SumStorage<T>* storage) noexcept : m_storage(storage) {}

    /// @brief Record a measurement (may be negative).
    void Add(T value, AttributeSpan attrs)
    {
        m_storage->Add(value, attrs);
    }

private:
    SumStorage<T>* m_storage;
};

/// @brief Handle to a synchronous Gauge instrument (last-write-wins).
///
/// @tparam T Value type: `std::int64_t` or `double`.
template <typename T>
class Gauge
{
public:
    explicit Gauge(GaugeStorage<T>* storage) noexcept : m_storage(storage) {}

    /// @brief Record an observation; last call wins per attribute set.
    void Record(T value, AttributeSpan attrs)
    {
        m_storage->Record(value, attrs);
    }

private:
    GaugeStorage<T>* m_storage;
};

/// @brief Handle to an explicit-bucket Histogram instrument.
///
/// @tparam T Value type: `std::int64_t` or `double`.
template <typename T>
class Histogram
{
public:
    explicit Histogram(HistogramStorage<T>* storage) noexcept : m_storage(storage) {}

    /// @brief Record an observation into the histogram.
    void Record(T value, AttributeSpan attrs)
    {
        m_storage->Record(value, attrs);
    }

private:
    HistogramStorage<T>* m_storage;
};

}  // namespace microtel::sdk
