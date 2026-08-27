// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/meter.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace microtel::testing
{

/// @brief Recording fake for any single-value sync instrument.
///
/// One template serves Counter / UpDownCounter / Gauge / Histogram — they all
/// reduce to "one value plus attributes per call". Attribute spans are copied
/// at call time (borrowed storage). Single-threaded use only.
template <typename T, template <typename> class Instrument>
class FakeSyncInstrument : public Instrument<T>
{
public:
    struct Recorded
    {
        T value;
        std::vector<microtel::KeyValue> attributes;
    };

    std::vector<Recorded> calls;

protected:
    void Capture(T value, microtel::AttributeSpan attrs)
    {
        calls.push_back({.value = value, .attributes = {attrs.begin(), attrs.end()}});
    }
};

template <typename T>
class FakeCounter final : public FakeSyncInstrument<T, microtel::Counter>
{
public:
    void Add(T value, microtel::AttributeSpan attrs) noexcept override
    {
        this->Capture(value, attrs);
    }
};

template <typename T>
class FakeUpDownCounter final : public FakeSyncInstrument<T, microtel::UpDownCounter>
{
public:
    void Add(T value, microtel::AttributeSpan attrs) noexcept override
    {
        this->Capture(value, attrs);
    }
};

template <typename T>
class FakeHistogram final : public FakeSyncInstrument<T, microtel::Histogram>
{
public:
    void Record(T value, microtel::AttributeSpan attrs) noexcept override
    {
        this->Capture(value, attrs);
    }
};

/// @brief Recording `microtel::ObservableResult` handed to captured
///        observable callbacks by tests.
template <typename T>
class FakeObservableResult final : public microtel::ObservableResult<T>
{
public:
    struct Observed
    {
        T value;
        std::vector<microtel::KeyValue> attributes;
    };

    std::vector<Observed> observations;

    void Observe(T value, microtel::AttributeSpan attrs) override
    {
        observations.push_back({.value = value, .attributes = {attrs.begin(), attrs.end()}});
    }
};

/// @brief Fake `microtel::Meter` recording every instrument creation.
///
/// Sync creates return owned recording fakes (`counters_i64`, …). Observable
/// creates capture the registered callback so tests can drive a collection
/// cycle by invoking it with a `FakeObservableResult`. Single-threaded use
/// only.
class FakeMeter : public microtel::Meter
{
public:
    struct CreatedInstrument
    {
        std::string kind;
        std::string name;
        std::string description;
        std::string unit;
        std::vector<double> boundaries;
    };

    std::vector<CreatedInstrument> created;

    std::vector<std::shared_ptr<FakeCounter<std::int64_t>>> counters_i64;
    std::vector<std::shared_ptr<FakeCounter<double>>> counters_double;
    std::vector<std::shared_ptr<FakeUpDownCounter<std::int64_t>>> updowns_i64;
    std::vector<std::shared_ptr<FakeUpDownCounter<double>>> updowns_double;
    std::vector<std::shared_ptr<FakeHistogram<std::int64_t>>> histograms_i64;
    std::vector<std::shared_ptr<FakeHistogram<double>>> histograms_double;

    std::vector<microtel::ObservableCallback<std::int64_t>> observable_callbacks_i64;
    std::vector<microtel::ObservableCallback<double>> observable_callbacks_double;

private:
    std::shared_ptr<microtel::Counter<std::int64_t>> DoCreateCounterI64(std::string name,
                                                                        std::string description,
                                                                        std::string unit) override
    {
        Note("counter_i64", name, description, unit);
        counters_i64.push_back(std::make_shared<FakeCounter<std::int64_t>>());
        return counters_i64.back();
    }

    std::shared_ptr<microtel::Counter<double>> DoCreateCounterDouble(std::string name,
                                                                     std::string description,
                                                                     std::string unit) override
    {
        Note("counter_double", name, description, unit);
        counters_double.push_back(std::make_shared<FakeCounter<double>>());
        return counters_double.back();
    }

    std::shared_ptr<microtel::UpDownCounter<std::int64_t>> DoCreateUpDownCounterI64(
        std::string name, std::string description, std::string unit) override
    {
        Note("updown_i64", name, description, unit);
        updowns_i64.push_back(std::make_shared<FakeUpDownCounter<std::int64_t>>());
        return updowns_i64.back();
    }

    std::shared_ptr<microtel::UpDownCounter<double>> DoCreateUpDownCounterDouble(
        std::string name, std::string description, std::string unit) override
    {
        Note("updown_double", name, description, unit);
        updowns_double.push_back(std::make_shared<FakeUpDownCounter<double>>());
        return updowns_double.back();
    }

    std::shared_ptr<microtel::Gauge<std::int64_t>> DoCreateGaugeI64(std::string name,
                                                                    std::string description,
                                                                    std::string unit) override
    {
        Note("gauge_i64", name, description, unit);
        return nullptr;  // sync gauges are ABI v2; unreachable from the shim
    }

    std::shared_ptr<microtel::Gauge<double>> DoCreateGaugeDouble(std::string name,
                                                                 std::string description,
                                                                 std::string unit) override
    {
        Note("gauge_double", name, description, unit);
        return nullptr;  // sync gauges are ABI v2; unreachable from the shim
    }

    std::shared_ptr<microtel::Histogram<std::int64_t>> DoCreateHistogramI64(
        std::string name,
        std::string description,
        std::string unit,
        std::vector<double> boundaries) override
    {
        Note("histogram_i64", name, description, unit, boundaries);
        histograms_i64.push_back(std::make_shared<FakeHistogram<std::int64_t>>());
        return histograms_i64.back();
    }

    std::shared_ptr<microtel::Histogram<double>> DoCreateHistogramDouble(
        std::string name,
        std::string description,
        std::string unit,
        std::vector<double> boundaries) override
    {
        Note("histogram_double", name, description, unit, boundaries);
        histograms_double.push_back(std::make_shared<FakeHistogram<double>>());
        return histograms_double.back();
    }

    std::shared_ptr<microtel::ExponentialHistogram<std::int64_t>> DoCreateExponentialHistogramI64(
        std::string name,
        std::string description,
        std::string unit,
        std::int32_t /*max_scale*/,
        std::int32_t /*max_buckets*/) override
    {
        Note("exp_histogram_i64", name, description, unit);
        return nullptr;  // no otel-cpp API surface maps here
    }

    std::shared_ptr<microtel::ExponentialHistogram<double>> DoCreateExponentialHistogramDouble(
        std::string name,
        std::string description,
        std::string unit,
        std::int32_t /*max_scale*/,
        std::int32_t /*max_buckets*/) override
    {
        Note("exp_histogram_double", name, description, unit);
        return nullptr;  // no otel-cpp API surface maps here
    }

    microtel::ObservableCounter<std::int64_t> DoCreateObservableCounterI64(
        std::string name,
        std::string description,
        std::string unit,
        microtel::ObservableCallback<std::int64_t> callback) override
    {
        Note("observable_counter_i64", name, description, unit);
        observable_callbacks_i64.push_back(std::move(callback));
        return {};
    }

    microtel::ObservableCounter<double> DoCreateObservableCounterDouble(
        std::string name,
        std::string description,
        std::string unit,
        microtel::ObservableCallback<double> callback) override
    {
        Note("observable_counter_double", name, description, unit);
        observable_callbacks_double.push_back(std::move(callback));
        return {};
    }

    microtel::ObservableUpDownCounter<std::int64_t> DoCreateObservableUpDownCounterI64(
        std::string name,
        std::string description,
        std::string unit,
        microtel::ObservableCallback<std::int64_t> callback) override
    {
        Note("observable_updown_i64", name, description, unit);
        observable_callbacks_i64.push_back(std::move(callback));
        return {};
    }

    microtel::ObservableUpDownCounter<double> DoCreateObservableUpDownCounterDouble(
        std::string name,
        std::string description,
        std::string unit,
        microtel::ObservableCallback<double> callback) override
    {
        Note("observable_updown_double", name, description, unit);
        observable_callbacks_double.push_back(std::move(callback));
        return {};
    }

    microtel::ObservableGauge<std::int64_t> DoCreateObservableGaugeI64(
        std::string name,
        std::string description,
        std::string unit,
        microtel::ObservableCallback<std::int64_t> callback) override
    {
        Note("observable_gauge_i64", name, description, unit);
        observable_callbacks_i64.push_back(std::move(callback));
        return {};
    }

    microtel::ObservableGauge<double> DoCreateObservableGaugeDouble(
        std::string name,
        std::string description,
        std::string unit,
        microtel::ObservableCallback<double> callback) override
    {
        Note("observable_gauge_double", name, description, unit);
        observable_callbacks_double.push_back(std::move(callback));
        return {};
    }

    void Note(std::string kind,
              std::string name,
              std::string description,
              std::string unit,
              std::vector<double> boundaries = {})
    {
        created.push_back({.kind = std::move(kind),
                           .name = std::move(name),
                           .description = std::move(description),
                           .unit = std::move(unit),
                           .boundaries = std::move(boundaries)});
    }
};

}  // namespace microtel::testing
