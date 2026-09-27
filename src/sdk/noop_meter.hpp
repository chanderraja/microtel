// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/attribute.hpp"
#include "microtel/meter.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace microtel::sdk
{

/// @brief A Counter or UpDownCounter whose every `Add` is dropped.
///
/// @tparam Base the instrument interface, `microtel::Counter` or
///         `microtel::UpDownCounter`.
/// @threadsafety Thread-safe (stateless).
template <typename T, template <typename> class Base>
class NoopAddInstrument final : public Base<T>
{
public:
    void Add(T /*value*/, AttributeSpan /*attrs*/) noexcept override
    {
        // Intentionally empty: there is no metrics pipeline to record into.
    }
};

/// @brief A Gauge or histogram whose every `Record` is dropped.
///
/// @tparam Base the instrument interface, `microtel::Gauge`,
///         `microtel::Histogram` or `microtel::ExponentialHistogram`.
/// @threadsafety Thread-safe (stateless).
template <typename T, template <typename> class Base>
class NoopRecordInstrument final : public Base<T>
{
public:
    void Record(T /*value*/, AttributeSpan /*attrs*/) noexcept override
    {
        // Intentionally empty: there is no metrics pipeline to record into.
    }
};

/// @brief A `Meter` whose instruments drop every measurement.
///
/// Returned by `SdkProvider::GetMeter` when the metrics pipeline is switched
/// off, which today is a Provider built with `SdkBuilder::WithExportTransport`
/// and `ExportTransportOptions::metrics == false` (ICP 0036 Decision 3).
/// Callers get valid, non-null instruments; observable callbacks are never
/// invoked. The metric-side twin of `NoopLogger`. ICP 0030 names a no-op meter
/// for its own disabled-signal case; this is the minimal one both need.
///
/// @threadsafety Thread-safe (stateless).
class NoopMeter final : public microtel::Meter
{
private:
    template <template <typename> class Base, typename T>
    [[nodiscard]] static std::shared_ptr<Base<T>> MakeAdder()
    {
        return std::make_shared<NoopAddInstrument<T, Base>>();
    }

    template <template <typename> class Base, typename T>
    [[nodiscard]] static std::shared_ptr<Base<T>> MakeRecorder()
    {
        return std::make_shared<NoopRecordInstrument<T, Base>>();
    }

    std::shared_ptr<Counter<std::int64_t>> DoCreateCounterI64(std::string /*name*/,
                                                              std::string /*description*/,
                                                              std::string /*unit*/) override
    {
        return MakeAdder<Counter, std::int64_t>();
    }
    std::shared_ptr<Counter<double>> DoCreateCounterDouble(std::string /*name*/,
                                                           std::string /*description*/,
                                                           std::string /*unit*/) override
    {
        return MakeAdder<Counter, double>();
    }
    std::shared_ptr<UpDownCounter<std::int64_t>> DoCreateUpDownCounterI64(
        std::string /*name*/, std::string /*description*/, std::string /*unit*/) override
    {
        return MakeAdder<UpDownCounter, std::int64_t>();
    }
    std::shared_ptr<UpDownCounter<double>> DoCreateUpDownCounterDouble(
        std::string /*name*/, std::string /*description*/, std::string /*unit*/) override
    {
        return MakeAdder<UpDownCounter, double>();
    }
    std::shared_ptr<Gauge<std::int64_t>> DoCreateGaugeI64(std::string /*name*/,
                                                          std::string /*description*/,
                                                          std::string /*unit*/) override
    {
        return MakeRecorder<Gauge, std::int64_t>();
    }
    std::shared_ptr<Gauge<double>> DoCreateGaugeDouble(std::string /*name*/,
                                                       std::string /*description*/,
                                                       std::string /*unit*/) override
    {
        return MakeRecorder<Gauge, double>();
    }
    std::shared_ptr<Histogram<std::int64_t>> DoCreateHistogramI64(
        std::string /*name*/,
        std::string /*description*/,
        std::string /*unit*/,
        std::vector<double> /*boundaries*/) override
    {
        return MakeRecorder<Histogram, std::int64_t>();
    }
    std::shared_ptr<Histogram<double>> DoCreateHistogramDouble(
        std::string /*name*/,
        std::string /*description*/,
        std::string /*unit*/,
        std::vector<double> /*boundaries*/) override
    {
        return MakeRecorder<Histogram, double>();
    }
    std::shared_ptr<ExponentialHistogram<std::int64_t>> DoCreateExponentialHistogramI64(
        std::string /*name*/,
        std::string /*description*/,
        std::string /*unit*/,
        std::int32_t /*max_scale*/,
        std::int32_t /*max_buckets*/) override
    {
        return MakeRecorder<ExponentialHistogram, std::int64_t>();
    }
    std::shared_ptr<ExponentialHistogram<double>> DoCreateExponentialHistogramDouble(
        std::string /*name*/,
        std::string /*description*/,
        std::string /*unit*/,
        std::int32_t /*max_scale*/,
        std::int32_t /*max_buckets*/) override
    {
        return MakeRecorder<ExponentialHistogram, double>();
    }
    ObservableCounter<std::int64_t> DoCreateObservableCounterI64(
        std::string /*name*/,
        std::string /*description*/,
        std::string /*unit*/,
        ObservableCallback<std::int64_t> /*callback*/) override
    {
        return {};
    }
    ObservableCounter<double> DoCreateObservableCounterDouble(
        std::string /*name*/,
        std::string /*description*/,
        std::string /*unit*/,
        ObservableCallback<double> /*callback*/) override
    {
        return {};
    }
    ObservableUpDownCounter<std::int64_t> DoCreateObservableUpDownCounterI64(
        std::string /*name*/,
        std::string /*description*/,
        std::string /*unit*/,
        ObservableCallback<std::int64_t> /*callback*/) override
    {
        return {};
    }
    ObservableUpDownCounter<double> DoCreateObservableUpDownCounterDouble(
        std::string /*name*/,
        std::string /*description*/,
        std::string /*unit*/,
        ObservableCallback<double> /*callback*/) override
    {
        return {};
    }
    ObservableGauge<std::int64_t> DoCreateObservableGaugeI64(
        std::string /*name*/,
        std::string /*description*/,
        std::string /*unit*/,
        ObservableCallback<std::int64_t> /*callback*/) override
    {
        return {};
    }
    ObservableGauge<double> DoCreateObservableGaugeDouble(
        std::string /*name*/,
        std::string /*description*/,
        std::string /*unit*/,
        ObservableCallback<double> /*callback*/) override
    {
        return {};
    }
};

}  // namespace microtel::sdk
