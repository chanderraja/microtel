// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/attribute.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace microtel
{

/// OTel-specified default explicit-bucket boundaries (15 boundaries → 16 buckets).
inline constexpr std::array<double, 15> kDefaultHistogramBoundaries = {0.0,
                                                                       5.0,
                                                                       10.0,
                                                                       25.0,
                                                                       50.0,
                                                                       75.0,
                                                                       100.0,
                                                                       250.0,
                                                                       500.0,
                                                                       750.0,
                                                                       1000.0,
                                                                       2500.0,
                                                                       5000.0,
                                                                       7500.0,
                                                                       10000.0};

/// @brief Abstract monotonic Sum instrument.
///
/// Obtained via `Meter::CreateCounter<T>()`. Hot-path `Add()` is `noexcept`.
///
/// @tparam T Value type: `std::int64_t` or `double`.
template <typename T>
class Counter
{
public:
    Counter() noexcept = default;
    virtual ~Counter() noexcept = default;

    Counter(const Counter&) = delete;
    Counter& operator=(const Counter&) = delete;
    Counter(Counter&&) = delete;
    Counter& operator=(Counter&&) = delete;

    /// @brief Record a measurement. Values below zero are silently dropped.
    virtual void Add(T value, AttributeSpan attrs) noexcept = 0;
};

/// @brief Abstract non-monotonic Sum instrument.
///
/// Obtained via `Meter::CreateUpDownCounter<T>()`. Accepts negative values.
///
/// @tparam T Value type: `std::int64_t` or `double`.
template <typename T>
class UpDownCounter
{
public:
    UpDownCounter() noexcept = default;
    virtual ~UpDownCounter() noexcept = default;

    UpDownCounter(const UpDownCounter&) = delete;
    UpDownCounter& operator=(const UpDownCounter&) = delete;
    UpDownCounter(UpDownCounter&&) = delete;
    UpDownCounter& operator=(UpDownCounter&&) = delete;

    /// @brief Record a measurement (may be negative).
    virtual void Add(T value, AttributeSpan attrs) noexcept = 0;
};

/// @brief Abstract synchronous Gauge instrument (last-write-wins).
///
/// Obtained via `Meter::CreateGauge<T>()`.
///
/// @tparam T Value type: `std::int64_t` or `double`.
template <typename T>
class Gauge
{
public:
    Gauge() noexcept = default;
    virtual ~Gauge() noexcept = default;

    Gauge(const Gauge&) = delete;
    Gauge& operator=(const Gauge&) = delete;
    Gauge(Gauge&&) = delete;
    Gauge& operator=(Gauge&&) = delete;

    /// @brief Record an observation; last call wins per attribute set.
    virtual void Record(T value, AttributeSpan attrs) noexcept = 0;
};

/// @brief Abstract explicit-bucket Histogram instrument.
///
/// Obtained via `Meter::CreateHistogram<T>()`.
///
/// @tparam T Value type: `std::int64_t` or `double`.
template <typename T>
class Histogram
{
public:
    Histogram() noexcept = default;
    virtual ~Histogram() noexcept = default;

    Histogram(const Histogram&) = delete;
    Histogram& operator=(const Histogram&) = delete;
    Histogram(Histogram&&) = delete;
    Histogram& operator=(Histogram&&) = delete;

    /// @brief Record an observation into the histogram.
    virtual void Record(T value, AttributeSpan attrs) noexcept = 0;
};

/// @brief Factory for metric instruments bound to one instrumentation scope.
///
/// Obtained via `Provider::GetMeter(name, version)`. Each `Create*<T>()` call
/// registers a metric stream with the shared producer and returns a
/// `shared_ptr` to an abstract instrument whose `Add()`/`Record()` methods
/// route directly to the underlying storage.
///
/// Supported value types: `std::int64_t` and `double`. Other types produce a
/// link error via the un-defined primary template specialization.
///
/// @threadsafety Thread-safe.
class Meter
{
public:
    Meter() noexcept = default;
    virtual ~Meter() noexcept = default;

    Meter(const Meter&) = delete;
    Meter& operator=(const Meter&) = delete;
    Meter(Meter&&) = delete;
    Meter& operator=(Meter&&) = delete;

    // ── Synchronous instruments ────────────────────────────────────────────

    /// @brief Create a monotonic Sum instrument (Counter).
    template <typename T>
    std::shared_ptr<Counter<T>> CreateCounter(std::string name,
                                              std::string description = {},
                                              std::string unit = {});

    /// @brief Create a non-monotonic Sum instrument (UpDownCounter).
    template <typename T>
    std::shared_ptr<UpDownCounter<T>> CreateUpDownCounter(std::string name,
                                                          std::string description = {},
                                                          std::string unit = {});

    /// @brief Create a synchronous last-write-wins Gauge instrument.
    template <typename T>
    std::shared_ptr<Gauge<T>> CreateGauge(std::string name,
                                          std::string description = {},
                                          std::string unit = {});

    /// @brief Create a Histogram with explicit bucket @p boundaries.
    template <typename T>
    std::shared_ptr<Histogram<T>> CreateHistogram(std::string name,
                                                  std::string description,
                                                  std::string unit,
                                                  std::vector<double> boundaries);

    /// @brief Create a Histogram with the OTel default 15-boundary ladder.
    template <typename T>
    std::shared_ptr<Histogram<T>> CreateHistogram(std::string name,
                                                  std::string description = {},
                                                  std::string unit = {})
    {
        return CreateHistogram<T>(std::move(name),
                                  std::move(description),
                                  std::move(unit),
                                  std::vector<double>(kDefaultHistogramBoundaries.begin(),
                                                      kDefaultHistogramBoundaries.end()));
    }

private:
    virtual std::shared_ptr<Counter<std::int64_t>> DoCreateCounterI64(std::string name,
                                                                      std::string description,
                                                                      std::string unit) = 0;
    virtual std::shared_ptr<Counter<double>> DoCreateCounterDouble(std::string name,
                                                                   std::string description,
                                                                   std::string unit) = 0;

    virtual std::shared_ptr<UpDownCounter<std::int64_t>> DoCreateUpDownCounterI64(
        std::string name, std::string description, std::string unit) = 0;
    virtual std::shared_ptr<UpDownCounter<double>> DoCreateUpDownCounterDouble(
        std::string name, std::string description, std::string unit) = 0;

    virtual std::shared_ptr<Gauge<std::int64_t>> DoCreateGaugeI64(std::string name,
                                                                  std::string description,
                                                                  std::string unit) = 0;
    virtual std::shared_ptr<Gauge<double>> DoCreateGaugeDouble(std::string name,
                                                               std::string description,
                                                               std::string unit) = 0;

    virtual std::shared_ptr<Histogram<std::int64_t>> DoCreateHistogramI64(
        std::string name,
        std::string description,
        std::string unit,
        std::vector<double> boundaries) = 0;
    virtual std::shared_ptr<Histogram<double>> DoCreateHistogramDouble(
        std::string name,
        std::string description,
        std::string unit,
        std::vector<double> boundaries) = 0;
};

// ── Explicit specializations — Counter ────────────────────────────────────────

template <>
inline std::shared_ptr<Counter<std::int64_t>> Meter::CreateCounter<std::int64_t>(
    std::string name, std::string description, std::string unit)
{
    return DoCreateCounterI64(std::move(name), std::move(description), std::move(unit));
}

template <>
inline std::shared_ptr<Counter<double>> Meter::CreateCounter<double>(std::string name,
                                                                     std::string description,
                                                                     std::string unit)
{
    return DoCreateCounterDouble(std::move(name), std::move(description), std::move(unit));
}

// ── Explicit specializations — UpDownCounter ──────────────────────────────────

template <>
inline std::shared_ptr<UpDownCounter<std::int64_t>> Meter::CreateUpDownCounter<std::int64_t>(
    std::string name, std::string description, std::string unit)
{
    return DoCreateUpDownCounterI64(std::move(name), std::move(description), std::move(unit));
}

template <>
inline std::shared_ptr<UpDownCounter<double>> Meter::CreateUpDownCounter<double>(
    std::string name, std::string description, std::string unit)
{
    return DoCreateUpDownCounterDouble(std::move(name), std::move(description), std::move(unit));
}

// ── Explicit specializations — Gauge ──────────────────────────────────────────

template <>
inline std::shared_ptr<Gauge<std::int64_t>> Meter::CreateGauge<std::int64_t>(
    std::string name, std::string description, std::string unit)
{
    return DoCreateGaugeI64(std::move(name), std::move(description), std::move(unit));
}

template <>
inline std::shared_ptr<Gauge<double>> Meter::CreateGauge<double>(std::string name,
                                                                 std::string description,
                                                                 std::string unit)
{
    return DoCreateGaugeDouble(std::move(name), std::move(description), std::move(unit));
}

// ── Explicit specializations — Histogram (4-arg) ──────────────────────────────

template <>
inline std::shared_ptr<Histogram<std::int64_t>> Meter::CreateHistogram<std::int64_t>(
    std::string name, std::string description, std::string unit, std::vector<double> boundaries)
{
    return DoCreateHistogramI64(
        std::move(name), std::move(description), std::move(unit), std::move(boundaries));
}

template <>
inline std::shared_ptr<Histogram<double>> Meter::CreateHistogram<double>(
    std::string name, std::string description, std::string unit, std::vector<double> boundaries)
{
    return DoCreateHistogramDouble(
        std::move(name), std::move(description), std::move(unit), std::move(boundaries));
}

}  // namespace microtel
