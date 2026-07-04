// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#include "sdk/sdk_meter.hpp"

#include "microtel/attribute.hpp"
#include "microtel/meter.hpp"

#include "sdk/metric_gauge_storage.hpp"
#include "sdk/metric_histogram_storage.hpp"
#include "sdk/metric_observable_instruments.hpp"
#include "sdk/metric_producer.hpp"
#include "sdk/metric_stream_impls.hpp"
#include "sdk/metric_sum_storage.hpp"

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace microtel::sdk
{

namespace
{

// ── Concrete instrument adapters ──────────────────────────────────────────────
// Each adapter holds a non-owning pointer to storage owned by the MetricProducer.
// Hot-path methods are noexcept: storage errors call std::terminate per policy.

template <typename T>
class SdkCounter final : public microtel::Counter<T>
{
public:
    explicit SdkCounter(SumStorage<T>* storage) noexcept : m_storage(storage) {}

    void Add(T value, microtel::AttributeSpan attrs) noexcept override
    {
        m_storage->Add(value, attrs);
    }

private:
    SumStorage<T>* m_storage;
};

template <typename T>
class SdkUpDownCounter final : public microtel::UpDownCounter<T>
{
public:
    explicit SdkUpDownCounter(SumStorage<T>* storage) noexcept : m_storage(storage) {}

    void Add(T value, microtel::AttributeSpan attrs) noexcept override
    {
        m_storage->Add(value, attrs);
    }

private:
    SumStorage<T>* m_storage;
};

template <typename T>
class SdkGauge final : public microtel::Gauge<T>
{
public:
    explicit SdkGauge(GaugeStorage<T>* storage) noexcept : m_storage(storage) {}

    void Record(T value, microtel::AttributeSpan attrs) noexcept override
    {
        m_storage->Record(value, attrs);
    }

private:
    GaugeStorage<T>* m_storage;
};

template <typename T>
class SdkHistogram final : public microtel::Histogram<T>
{
public:
    explicit SdkHistogram(HistogramStorage<T>* storage) noexcept : m_storage(storage) {}

    void Record(T value, microtel::AttributeSpan attrs) noexcept override
    {
        m_storage->Record(value, attrs);
    }

private:
    HistogramStorage<T>* m_storage;
};

template <typename T>
class SdkExponentialHistogram final : public microtel::ExponentialHistogram<T>
{
public:
    explicit SdkExponentialHistogram(ExponentialHistogramStorage<T>* storage) noexcept
        : m_storage(storage)
    {
    }

    void Record(T value, microtel::AttributeSpan attrs) noexcept override
    {
        m_storage->Record(value, attrs);
    }

private:
    ExponentialHistogramStorage<T>* m_storage;
};

// ── Public-to-internal ObservableResult bridge ────────────────────────────────
// Adapts microtel::ObservableResult<T> (public abstract) to sdk::ObservableResult<T>
// (internal concrete). The adapter is stack-allocated inside the bridge lambda that
// wraps the user's public callback for each collection cycle.

template <typename T>
class SdkObservableResultAdapter final : public microtel::ObservableResult<T>
{
public:
    explicit SdkObservableResultAdapter(ObservableResult<T>& target) noexcept : m_target(target) {}

    SdkObservableResultAdapter(const SdkObservableResultAdapter&) = delete;
    SdkObservableResultAdapter& operator=(const SdkObservableResultAdapter&) = delete;
    SdkObservableResultAdapter(SdkObservableResultAdapter&&) = delete;
    SdkObservableResultAdapter& operator=(SdkObservableResultAdapter&&) = delete;
    ~SdkObservableResultAdapter() noexcept override = default;

    void Observe(T value, microtel::AttributeSpan attrs) override
    {
        m_target.Observe(value, attrs);
    }

private:
    ObservableResult<T>& m_target;
};

}  // namespace

// ── SdkMeter ──────────────────────────────────────────────────────────────────

SdkMeter::SdkMeter(internal::InstrumentationScope scope,
                   std::shared_ptr<MetricProducer> producer,
                   std::size_t max_cardinality,
                   internal::IDiagnosticsSink* diag) noexcept
    : m_scope(std::move(scope)),
      m_producer(std::move(producer)),
      m_max_cardinality(max_cardinality),
      m_diag(diag)
{
}

std::shared_ptr<microtel::Counter<std::int64_t>> SdkMeter::DoCreateCounterI64(
    std::string name, std::string description, std::string unit)
{
    auto stream = std::make_unique<MetricStreamSum<std::int64_t>>(
        std::move(name),
        std::move(description),
        std::move(unit),
        /*monotonic=*/true,
        StorageOptions{.max_cardinality = m_max_cardinality, .diag = m_diag});
    SumStorage<std::int64_t>& storage = stream->Storage();
    m_producer->AddStream(m_scope, std::move(stream));
    return std::make_shared<SdkCounter<std::int64_t>>(&storage);
}

std::shared_ptr<microtel::Counter<double>> SdkMeter::DoCreateCounterDouble(std::string name,
                                                                           std::string description,
                                                                           std::string unit)
{
    auto stream = std::make_unique<MetricStreamSum<double>>(
        std::move(name),
        std::move(description),
        std::move(unit),
        /*monotonic=*/true,
        StorageOptions{.max_cardinality = m_max_cardinality, .diag = m_diag});
    SumStorage<double>& storage = stream->Storage();
    m_producer->AddStream(m_scope, std::move(stream));
    return std::make_shared<SdkCounter<double>>(&storage);
}

std::shared_ptr<microtel::UpDownCounter<std::int64_t>> SdkMeter::DoCreateUpDownCounterI64(
    std::string name, std::string description, std::string unit)
{
    auto stream = std::make_unique<MetricStreamSum<std::int64_t>>(
        std::move(name),
        std::move(description),
        std::move(unit),
        /*monotonic=*/false,
        StorageOptions{.max_cardinality = m_max_cardinality, .diag = m_diag});
    SumStorage<std::int64_t>& storage = stream->Storage();
    m_producer->AddStream(m_scope, std::move(stream));
    return std::make_shared<SdkUpDownCounter<std::int64_t>>(&storage);
}

std::shared_ptr<microtel::UpDownCounter<double>> SdkMeter::DoCreateUpDownCounterDouble(
    std::string name, std::string description, std::string unit)
{
    auto stream = std::make_unique<MetricStreamSum<double>>(
        std::move(name),
        std::move(description),
        std::move(unit),
        /*monotonic=*/false,
        StorageOptions{.max_cardinality = m_max_cardinality, .diag = m_diag});
    SumStorage<double>& storage = stream->Storage();
    m_producer->AddStream(m_scope, std::move(stream));
    return std::make_shared<SdkUpDownCounter<double>>(&storage);
}

std::shared_ptr<microtel::Gauge<std::int64_t>> SdkMeter::DoCreateGaugeI64(std::string name,
                                                                          std::string description,
                                                                          std::string unit)
{
    auto stream = std::make_unique<MetricStreamGauge<std::int64_t>>(
        std::move(name),
        std::move(description),
        std::move(unit),
        StorageOptions{.max_cardinality = m_max_cardinality, .diag = m_diag});
    GaugeStorage<std::int64_t>& storage = stream->Storage();
    m_producer->AddStream(m_scope, std::move(stream));
    return std::make_shared<SdkGauge<std::int64_t>>(&storage);
}

std::shared_ptr<microtel::Gauge<double>> SdkMeter::DoCreateGaugeDouble(std::string name,
                                                                       std::string description,
                                                                       std::string unit)
{
    auto stream = std::make_unique<MetricStreamGauge<double>>(
        std::move(name),
        std::move(description),
        std::move(unit),
        StorageOptions{.max_cardinality = m_max_cardinality, .diag = m_diag});
    GaugeStorage<double>& storage = stream->Storage();
    m_producer->AddStream(m_scope, std::move(stream));
    return std::make_shared<SdkGauge<double>>(&storage);
}

std::shared_ptr<microtel::Histogram<std::int64_t>> SdkMeter::DoCreateHistogramI64(
    std::string name, std::string description, std::string unit, std::vector<double> boundaries)
{
    auto stream = std::make_unique<MetricStreamHistogram<std::int64_t>>(
        std::move(name),
        std::move(description),
        std::move(unit),
        std::move(boundaries),
        StorageOptions{.max_cardinality = m_max_cardinality, .diag = m_diag});
    HistogramStorage<std::int64_t>& storage = stream->Storage();
    m_producer->AddStream(m_scope, std::move(stream));
    return std::make_shared<SdkHistogram<std::int64_t>>(&storage);
}

std::shared_ptr<microtel::Histogram<double>> SdkMeter::DoCreateHistogramDouble(
    std::string name, std::string description, std::string unit, std::vector<double> boundaries)
{
    auto stream = std::make_unique<MetricStreamHistogram<double>>(
        std::move(name),
        std::move(description),
        std::move(unit),
        std::move(boundaries),
        StorageOptions{.max_cardinality = m_max_cardinality, .diag = m_diag});
    HistogramStorage<double>& storage = stream->Storage();
    m_producer->AddStream(m_scope, std::move(stream));
    return std::make_shared<SdkHistogram<double>>(&storage);
}

std::shared_ptr<microtel::ExponentialHistogram<std::int64_t>>
SdkMeter::DoCreateExponentialHistogramI64(std::string name,
                                          std::string description,
                                          std::string unit,
                                          std::int32_t max_scale,
                                          std::int32_t max_buckets)
{
    auto stream = std::make_unique<MetricStreamExpHistogram<std::int64_t>>(
        std::move(name),
        std::move(description),
        std::move(unit),
        max_scale,
        max_buckets,
        StorageOptions{.max_cardinality = m_max_cardinality, .diag = m_diag});
    ExponentialHistogramStorage<std::int64_t>& storage = stream->Storage();
    m_producer->AddStream(m_scope, std::move(stream));
    return std::make_shared<SdkExponentialHistogram<std::int64_t>>(&storage);
}

std::shared_ptr<microtel::ExponentialHistogram<double>>
SdkMeter::DoCreateExponentialHistogramDouble(std::string name,
                                             std::string description,
                                             std::string unit,
                                             std::int32_t max_scale,
                                             std::int32_t max_buckets)
{
    auto stream = std::make_unique<MetricStreamExpHistogram<double>>(
        std::move(name),
        std::move(description),
        std::move(unit),
        max_scale,
        max_buckets,
        StorageOptions{.max_cardinality = m_max_cardinality, .diag = m_diag});
    ExponentialHistogramStorage<double>& storage = stream->Storage();
    m_producer->AddStream(m_scope, std::move(stream));
    return std::make_shared<SdkExponentialHistogram<double>>(&storage);
}

microtel::ObservableCounter<std::int64_t> SdkMeter::DoCreateObservableCounterI64(
    std::string name,
    std::string description,
    std::string unit,
    microtel::ObservableCallback<std::int64_t> callback)
{
    ObservableCallback<std::int64_t> bridge{
        [pub_cb = std::move(callback)](ObservableResult<std::int64_t>& sdk_result)
        {
            SdkObservableResultAdapter<std::int64_t> adapter{sdk_result};
            pub_cb(adapter);
        }};
    auto stream = std::make_unique<MetricStreamObservableSum<std::int64_t>>(std::move(name),
                                                                            std::move(description),
                                                                            std::move(unit),
                                                                            /*monotonic=*/true,
                                                                            std::move(bridge),
                                                                            m_max_cardinality,
                                                                            m_diag);
    m_producer->AddStream(m_scope, std::move(stream));
    return {};
}

microtel::ObservableCounter<double> SdkMeter::DoCreateObservableCounterDouble(
    std::string name,
    std::string description,
    std::string unit,
    microtel::ObservableCallback<double> callback)
{
    ObservableCallback<double> bridge{
        [pub_cb = std::move(callback)](ObservableResult<double>& sdk_result)
        {
            SdkObservableResultAdapter<double> adapter{sdk_result};
            pub_cb(adapter);
        }};
    auto stream = std::make_unique<MetricStreamObservableSum<double>>(std::move(name),
                                                                      std::move(description),
                                                                      std::move(unit),
                                                                      /*monotonic=*/true,
                                                                      std::move(bridge),
                                                                      m_max_cardinality,
                                                                      m_diag);
    m_producer->AddStream(m_scope, std::move(stream));
    return {};
}

microtel::ObservableUpDownCounter<std::int64_t> SdkMeter::DoCreateObservableUpDownCounterI64(
    std::string name,
    std::string description,
    std::string unit,
    microtel::ObservableCallback<std::int64_t> callback)
{
    ObservableCallback<std::int64_t> bridge{
        [pub_cb = std::move(callback)](ObservableResult<std::int64_t>& sdk_result)
        {
            SdkObservableResultAdapter<std::int64_t> adapter{sdk_result};
            pub_cb(adapter);
        }};
    auto stream = std::make_unique<MetricStreamObservableSum<std::int64_t>>(std::move(name),
                                                                            std::move(description),
                                                                            std::move(unit),
                                                                            /*monotonic=*/false,
                                                                            std::move(bridge),
                                                                            m_max_cardinality,
                                                                            m_diag);
    m_producer->AddStream(m_scope, std::move(stream));
    return {};
}

microtel::ObservableUpDownCounter<double> SdkMeter::DoCreateObservableUpDownCounterDouble(
    std::string name,
    std::string description,
    std::string unit,
    microtel::ObservableCallback<double> callback)
{
    ObservableCallback<double> bridge{
        [pub_cb = std::move(callback)](ObservableResult<double>& sdk_result)
        {
            SdkObservableResultAdapter<double> adapter{sdk_result};
            pub_cb(adapter);
        }};
    auto stream = std::make_unique<MetricStreamObservableSum<double>>(std::move(name),
                                                                      std::move(description),
                                                                      std::move(unit),
                                                                      /*monotonic=*/false,
                                                                      std::move(bridge),
                                                                      m_max_cardinality,
                                                                      m_diag);
    m_producer->AddStream(m_scope, std::move(stream));
    return {};
}

microtel::ObservableGauge<std::int64_t> SdkMeter::DoCreateObservableGaugeI64(
    std::string name,
    std::string description,
    std::string unit,
    microtel::ObservableCallback<std::int64_t> callback)
{
    ObservableCallback<std::int64_t> bridge{
        [pub_cb = std::move(callback)](ObservableResult<std::int64_t>& sdk_result)
        {
            SdkObservableResultAdapter<std::int64_t> adapter{sdk_result};
            pub_cb(adapter);
        }};
    auto stream =
        std::make_unique<MetricStreamObservableGauge<std::int64_t>>(std::move(name),
                                                                    std::move(description),
                                                                    std::move(unit),
                                                                    std::move(bridge),
                                                                    m_max_cardinality,
                                                                    m_diag);
    m_producer->AddStream(m_scope, std::move(stream));
    return {};
}

microtel::ObservableGauge<double> SdkMeter::DoCreateObservableGaugeDouble(
    std::string name,
    std::string description,
    std::string unit,
    microtel::ObservableCallback<double> callback)
{
    ObservableCallback<double> bridge{
        [pub_cb = std::move(callback)](ObservableResult<double>& sdk_result)
        {
            SdkObservableResultAdapter<double> adapter{sdk_result};
            pub_cb(adapter);
        }};
    auto stream = std::make_unique<MetricStreamObservableGauge<double>>(std::move(name),
                                                                        std::move(description),
                                                                        std::move(unit),
                                                                        std::move(bridge),
                                                                        m_max_cardinality,
                                                                        m_diag);
    m_producer->AddStream(m_scope, std::move(stream));
    return {};
}

}  // namespace microtel::sdk
