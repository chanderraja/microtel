// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#include "sdk/sdk_meter.hpp"

#include "microtel/attribute.hpp"
#include "microtel/meter.hpp"
#include "microtel/view.hpp"

#include "sdk/metric_gauge_storage.hpp"
#include "sdk/metric_histogram_storage.hpp"
#include "sdk/metric_observable_instruments.hpp"
#include "sdk/metric_producer.hpp"
#include "sdk/metric_stream_impls.hpp"
#include "sdk/metric_sum_storage.hpp"
#include "sdk/view_registry.hpp"

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace microtel::sdk
{

namespace
{

// ── View name resolution ──────────────────────────────────────────────────────
// Returns the list of stream names to register for an instrument, applying
// view rename and drop rules. Each entry in the returned vector → one stream.
// An empty result means all matching views have drop=true; the caller creates
// no stream (instrument is a no-op). A single entry with the original name
// means either no registry, no matching view, or a match with no rename.

std::vector<std::string> ResolveStreamNames(const std::string& instrument_name,
                                            const ViewRegistry* registry,
                                            const InstrumentDescriptor& desc)
{
    if (registry == nullptr)
    {
        return {instrument_name};
    }

    const auto matches = registry->Match(desc);
    if (matches.empty())
    {
        return {instrument_name};
    }

    std::vector<std::string> names;
    for (const auto* const view : matches)
    {
        if (!view->transform.drop)
        {
            names.push_back(view->transform.name.value_or(instrument_name));
        }
    }
    return names;
}

// ── Concrete instrument adapters ──────────────────────────────────────────────
// Each adapter holds non-owning pointers to storages owned by MetricProducer.
// Multiple storages support fan-out (one per matching view). An empty storage
// list makes the instrument a no-op (all views dropped).
// Hot-path methods are noexcept: storage errors call std::terminate per policy.

template <typename T>
class SdkCounter final : public microtel::Counter<T>
{
public:
    explicit SdkCounter(std::vector<SumStorage<T>*> storages) noexcept
        : m_storages(std::move(storages))
    {
    }

    void Add(T value, microtel::AttributeSpan attrs) noexcept override
    {
        for (auto* const storage : m_storages)
        {
            storage->Add(value, attrs);
        }
    }

private:
    std::vector<SumStorage<T>*> m_storages;
};

template <typename T>
class SdkUpDownCounter final : public microtel::UpDownCounter<T>
{
public:
    explicit SdkUpDownCounter(std::vector<SumStorage<T>*> storages) noexcept
        : m_storages(std::move(storages))
    {
    }

    void Add(T value, microtel::AttributeSpan attrs) noexcept override
    {
        for (auto* const storage : m_storages)
        {
            storage->Add(value, attrs);
        }
    }

private:
    std::vector<SumStorage<T>*> m_storages;
};

template <typename T>
class SdkGauge final : public microtel::Gauge<T>
{
public:
    explicit SdkGauge(std::vector<GaugeStorage<T>*> storages) noexcept
        : m_storages(std::move(storages))
    {
    }

    void Record(T value, microtel::AttributeSpan attrs) noexcept override
    {
        for (auto* const storage : m_storages)
        {
            storage->Record(value, attrs);
        }
    }

private:
    std::vector<GaugeStorage<T>*> m_storages;
};

template <typename T>
class SdkHistogram final : public microtel::Histogram<T>
{
public:
    explicit SdkHistogram(std::vector<HistogramStorage<T>*> storages) noexcept
        : m_storages(std::move(storages))
    {
    }

    void Record(T value, microtel::AttributeSpan attrs) noexcept override
    {
        for (auto* const storage : m_storages)
        {
            storage->Record(value, attrs);
        }
    }

private:
    std::vector<HistogramStorage<T>*> m_storages;
};

template <typename T>
class SdkExponentialHistogram final : public microtel::ExponentialHistogram<T>
{
public:
    explicit SdkExponentialHistogram(std::vector<ExponentialHistogramStorage<T>*> storages) noexcept
        : m_storages(std::move(storages))
    {
    }

    void Record(T value, microtel::AttributeSpan attrs) noexcept override
    {
        for (auto* const storage : m_storages)
        {
            storage->Record(value, attrs);
        }
    }

private:
    std::vector<ExponentialHistogramStorage<T>*> m_storages;
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
                   internal::IDiagnosticsSink* diag,
                   const ViewRegistry* registry) noexcept
    : m_scope(std::move(scope)),
      m_producer(std::move(producer)),
      m_max_cardinality(max_cardinality),
      m_diag(diag),
      m_registry(registry)
{
}

std::shared_ptr<microtel::Counter<std::int64_t>> SdkMeter::DoCreateCounterI64(
    std::string name, std::string description, std::string unit)
{
    const InstrumentDescriptor desc{
        .name = name, .kind = InstrumentKind::Counter, .meter_name = m_scope.name};
    const auto names = ResolveStreamNames(name, m_registry, desc);
    const StorageOptions opts{.max_cardinality = m_max_cardinality, .diag = m_diag};
    std::vector<SumStorage<std::int64_t>*> storages;
    storages.reserve(names.size());
    for (const auto& stream_name : names)
    {
        auto stream = std::make_unique<MetricStreamSum<std::int64_t>>(
            stream_name, description, unit, /*monotonic=*/true, opts);
        storages.push_back(&stream->Storage());
        m_producer->AddStream(m_scope, std::move(stream));
    }
    return std::make_shared<SdkCounter<std::int64_t>>(std::move(storages));
}

std::shared_ptr<microtel::Counter<double>> SdkMeter::DoCreateCounterDouble(std::string name,
                                                                           std::string description,
                                                                           std::string unit)
{
    const InstrumentDescriptor desc{
        .name = name, .kind = InstrumentKind::Counter, .meter_name = m_scope.name};
    const auto names = ResolveStreamNames(name, m_registry, desc);
    const StorageOptions opts{.max_cardinality = m_max_cardinality, .diag = m_diag};
    std::vector<SumStorage<double>*> storages;
    storages.reserve(names.size());
    for (const auto& stream_name : names)
    {
        auto stream = std::make_unique<MetricStreamSum<double>>(
            stream_name, description, unit, /*monotonic=*/true, opts);
        storages.push_back(&stream->Storage());
        m_producer->AddStream(m_scope, std::move(stream));
    }
    return std::make_shared<SdkCounter<double>>(std::move(storages));
}

std::shared_ptr<microtel::UpDownCounter<std::int64_t>> SdkMeter::DoCreateUpDownCounterI64(
    std::string name, std::string description, std::string unit)
{
    const InstrumentDescriptor desc{
        .name = name, .kind = InstrumentKind::UpDownCounter, .meter_name = m_scope.name};
    const auto names = ResolveStreamNames(name, m_registry, desc);
    const StorageOptions opts{.max_cardinality = m_max_cardinality, .diag = m_diag};
    std::vector<SumStorage<std::int64_t>*> storages;
    storages.reserve(names.size());
    for (const auto& stream_name : names)
    {
        auto stream = std::make_unique<MetricStreamSum<std::int64_t>>(
            stream_name, description, unit, /*monotonic=*/false, opts);
        storages.push_back(&stream->Storage());
        m_producer->AddStream(m_scope, std::move(stream));
    }
    return std::make_shared<SdkUpDownCounter<std::int64_t>>(std::move(storages));
}

std::shared_ptr<microtel::UpDownCounter<double>> SdkMeter::DoCreateUpDownCounterDouble(
    std::string name, std::string description, std::string unit)
{
    const InstrumentDescriptor desc{
        .name = name, .kind = InstrumentKind::UpDownCounter, .meter_name = m_scope.name};
    const auto names = ResolveStreamNames(name, m_registry, desc);
    const StorageOptions opts{.max_cardinality = m_max_cardinality, .diag = m_diag};
    std::vector<SumStorage<double>*> storages;
    storages.reserve(names.size());
    for (const auto& stream_name : names)
    {
        auto stream = std::make_unique<MetricStreamSum<double>>(
            stream_name, description, unit, /*monotonic=*/false, opts);
        storages.push_back(&stream->Storage());
        m_producer->AddStream(m_scope, std::move(stream));
    }
    return std::make_shared<SdkUpDownCounter<double>>(std::move(storages));
}

std::shared_ptr<microtel::Gauge<std::int64_t>> SdkMeter::DoCreateGaugeI64(std::string name,
                                                                          std::string description,
                                                                          std::string unit)
{
    const InstrumentDescriptor desc{
        .name = name, .kind = InstrumentKind::Gauge, .meter_name = m_scope.name};
    const auto names = ResolveStreamNames(name, m_registry, desc);
    const StorageOptions opts{.max_cardinality = m_max_cardinality, .diag = m_diag};
    std::vector<GaugeStorage<std::int64_t>*> storages;
    storages.reserve(names.size());
    for (const auto& stream_name : names)
    {
        auto stream =
            std::make_unique<MetricStreamGauge<std::int64_t>>(stream_name, description, unit, opts);
        storages.push_back(&stream->Storage());
        m_producer->AddStream(m_scope, std::move(stream));
    }
    return std::make_shared<SdkGauge<std::int64_t>>(std::move(storages));
}

std::shared_ptr<microtel::Gauge<double>> SdkMeter::DoCreateGaugeDouble(std::string name,
                                                                       std::string description,
                                                                       std::string unit)
{
    const InstrumentDescriptor desc{
        .name = name, .kind = InstrumentKind::Gauge, .meter_name = m_scope.name};
    const auto names = ResolveStreamNames(name, m_registry, desc);
    const StorageOptions opts{.max_cardinality = m_max_cardinality, .diag = m_diag};
    std::vector<GaugeStorage<double>*> storages;
    storages.reserve(names.size());
    for (const auto& stream_name : names)
    {
        auto stream =
            std::make_unique<MetricStreamGauge<double>>(stream_name, description, unit, opts);
        storages.push_back(&stream->Storage());
        m_producer->AddStream(m_scope, std::move(stream));
    }
    return std::make_shared<SdkGauge<double>>(std::move(storages));
}

std::shared_ptr<microtel::Histogram<std::int64_t>> SdkMeter::DoCreateHistogramI64(
    std::string name, std::string description, std::string unit, std::vector<double> boundaries)
{
    const InstrumentDescriptor desc{
        .name = name, .kind = InstrumentKind::Histogram, .meter_name = m_scope.name};
    const auto names = ResolveStreamNames(name, m_registry, desc);
    const StorageOptions opts{.max_cardinality = m_max_cardinality, .diag = m_diag};
    std::vector<HistogramStorage<std::int64_t>*> storages;
    storages.reserve(names.size());
    for (const auto& stream_name : names)
    {
        auto stream = std::make_unique<MetricStreamHistogram<std::int64_t>>(
            stream_name, description, unit, boundaries, opts);
        storages.push_back(&stream->Storage());
        m_producer->AddStream(m_scope, std::move(stream));
    }
    return std::make_shared<SdkHistogram<std::int64_t>>(std::move(storages));
}

std::shared_ptr<microtel::Histogram<double>> SdkMeter::DoCreateHistogramDouble(
    std::string name, std::string description, std::string unit, std::vector<double> boundaries)
{
    const InstrumentDescriptor desc{
        .name = name, .kind = InstrumentKind::Histogram, .meter_name = m_scope.name};
    const auto names = ResolveStreamNames(name, m_registry, desc);
    const StorageOptions opts{.max_cardinality = m_max_cardinality, .diag = m_diag};
    std::vector<HistogramStorage<double>*> storages;
    storages.reserve(names.size());
    for (const auto& stream_name : names)
    {
        auto stream = std::make_unique<MetricStreamHistogram<double>>(
            stream_name, description, unit, boundaries, opts);
        storages.push_back(&stream->Storage());
        m_producer->AddStream(m_scope, std::move(stream));
    }
    return std::make_shared<SdkHistogram<double>>(std::move(storages));
}

std::shared_ptr<microtel::ExponentialHistogram<std::int64_t>>
SdkMeter::DoCreateExponentialHistogramI64(std::string name,
                                          std::string description,
                                          std::string unit,
                                          std::int32_t max_scale,
                                          std::int32_t max_buckets)
{
    const InstrumentDescriptor desc{
        .name = name, .kind = InstrumentKind::ExponentialHistogram, .meter_name = m_scope.name};
    const auto names = ResolveStreamNames(name, m_registry, desc);
    const StorageOptions opts{.max_cardinality = m_max_cardinality, .diag = m_diag};
    std::vector<ExponentialHistogramStorage<std::int64_t>*> storages;
    storages.reserve(names.size());
    for (const auto& stream_name : names)
    {
        auto stream = std::make_unique<MetricStreamExpHistogram<std::int64_t>>(
            stream_name, description, unit, max_scale, max_buckets, opts);
        storages.push_back(&stream->Storage());
        m_producer->AddStream(m_scope, std::move(stream));
    }
    return std::make_shared<SdkExponentialHistogram<std::int64_t>>(std::move(storages));
}

std::shared_ptr<microtel::ExponentialHistogram<double>>
SdkMeter::DoCreateExponentialHistogramDouble(std::string name,
                                             std::string description,
                                             std::string unit,
                                             std::int32_t max_scale,
                                             std::int32_t max_buckets)
{
    const InstrumentDescriptor desc{
        .name = name, .kind = InstrumentKind::ExponentialHistogram, .meter_name = m_scope.name};
    const auto names = ResolveStreamNames(name, m_registry, desc);
    const StorageOptions opts{.max_cardinality = m_max_cardinality, .diag = m_diag};
    std::vector<ExponentialHistogramStorage<double>*> storages;
    storages.reserve(names.size());
    for (const auto& stream_name : names)
    {
        auto stream = std::make_unique<MetricStreamExpHistogram<double>>(
            stream_name, description, unit, max_scale, max_buckets, opts);
        storages.push_back(&stream->Storage());
        m_producer->AddStream(m_scope, std::move(stream));
    }
    return std::make_shared<SdkExponentialHistogram<double>>(std::move(storages));
}

microtel::ObservableCounter<std::int64_t> SdkMeter::DoCreateObservableCounterI64(
    std::string name,
    std::string description,
    std::string unit,
    microtel::ObservableCallback<std::int64_t> callback)
{
    const InstrumentDescriptor desc{
        .name = name, .kind = InstrumentKind::ObservableCounter, .meter_name = m_scope.name};
    const auto names = ResolveStreamNames(name, m_registry, desc);
    for (const auto& stream_name : names)
    {
        ObservableCallback<std::int64_t> bridge{
            [pub_cb = callback](ObservableResult<std::int64_t>& sdk_result)
            {
                SdkObservableResultAdapter<std::int64_t> adapter{sdk_result};
                pub_cb(adapter);
            }};
        m_producer->AddStream(
            m_scope,
            std::make_unique<MetricStreamObservableSum<std::int64_t>>(stream_name,
                                                                      description,
                                                                      unit,
                                                                      /*monotonic=*/true,
                                                                      std::move(bridge),
                                                                      m_max_cardinality,
                                                                      m_diag));
    }
    return {};
}

microtel::ObservableCounter<double> SdkMeter::DoCreateObservableCounterDouble(
    std::string name,
    std::string description,
    std::string unit,
    microtel::ObservableCallback<double> callback)
{
    const InstrumentDescriptor desc{
        .name = name, .kind = InstrumentKind::ObservableCounter, .meter_name = m_scope.name};
    const auto names = ResolveStreamNames(name, m_registry, desc);
    for (const auto& stream_name : names)
    {
        ObservableCallback<double> bridge{
            [pub_cb = callback](ObservableResult<double>& sdk_result)
            {
                SdkObservableResultAdapter<double> adapter{sdk_result};
                pub_cb(adapter);
            }};
        m_producer->AddStream(
            m_scope,
            std::make_unique<MetricStreamObservableSum<double>>(stream_name,
                                                                description,
                                                                unit,
                                                                /*monotonic=*/true,
                                                                std::move(bridge),
                                                                m_max_cardinality,
                                                                m_diag));
    }
    return {};
}

microtel::ObservableUpDownCounter<std::int64_t> SdkMeter::DoCreateObservableUpDownCounterI64(
    std::string name,
    std::string description,
    std::string unit,
    microtel::ObservableCallback<std::int64_t> callback)
{
    const InstrumentDescriptor desc{
        .name = name, .kind = InstrumentKind::ObservableUpDownCounter, .meter_name = m_scope.name};
    const auto names = ResolveStreamNames(name, m_registry, desc);
    for (const auto& stream_name : names)
    {
        ObservableCallback<std::int64_t> bridge{
            [pub_cb = callback](ObservableResult<std::int64_t>& sdk_result)
            {
                SdkObservableResultAdapter<std::int64_t> adapter{sdk_result};
                pub_cb(adapter);
            }};
        m_producer->AddStream(
            m_scope,
            std::make_unique<MetricStreamObservableSum<std::int64_t>>(stream_name,
                                                                      description,
                                                                      unit,
                                                                      /*monotonic=*/false,
                                                                      std::move(bridge),
                                                                      m_max_cardinality,
                                                                      m_diag));
    }
    return {};
}

microtel::ObservableUpDownCounter<double> SdkMeter::DoCreateObservableUpDownCounterDouble(
    std::string name,
    std::string description,
    std::string unit,
    microtel::ObservableCallback<double> callback)
{
    const InstrumentDescriptor desc{
        .name = name, .kind = InstrumentKind::ObservableUpDownCounter, .meter_name = m_scope.name};
    const auto names = ResolveStreamNames(name, m_registry, desc);
    for (const auto& stream_name : names)
    {
        ObservableCallback<double> bridge{
            [pub_cb = callback](ObservableResult<double>& sdk_result)
            {
                SdkObservableResultAdapter<double> adapter{sdk_result};
                pub_cb(adapter);
            }};
        m_producer->AddStream(
            m_scope,
            std::make_unique<MetricStreamObservableSum<double>>(stream_name,
                                                                description,
                                                                unit,
                                                                /*monotonic=*/false,
                                                                std::move(bridge),
                                                                m_max_cardinality,
                                                                m_diag));
    }
    return {};
}

microtel::ObservableGauge<std::int64_t> SdkMeter::DoCreateObservableGaugeI64(
    std::string name,
    std::string description,
    std::string unit,
    microtel::ObservableCallback<std::int64_t> callback)
{
    const InstrumentDescriptor desc{
        .name = name, .kind = InstrumentKind::ObservableGauge, .meter_name = m_scope.name};
    const auto names = ResolveStreamNames(name, m_registry, desc);
    for (const auto& stream_name : names)
    {
        ObservableCallback<std::int64_t> bridge{
            [pub_cb = callback](ObservableResult<std::int64_t>& sdk_result)
            {
                SdkObservableResultAdapter<std::int64_t> adapter{sdk_result};
                pub_cb(adapter);
            }};
        m_producer->AddStream(
            m_scope,
            std::make_unique<MetricStreamObservableGauge<std::int64_t>>(
                stream_name, description, unit, std::move(bridge), m_max_cardinality, m_diag));
    }
    return {};
}

microtel::ObservableGauge<double> SdkMeter::DoCreateObservableGaugeDouble(
    std::string name,
    std::string description,
    std::string unit,
    microtel::ObservableCallback<double> callback)
{
    const InstrumentDescriptor desc{
        .name = name, .kind = InstrumentKind::ObservableGauge, .meter_name = m_scope.name};
    const auto names = ResolveStreamNames(name, m_registry, desc);
    for (const auto& stream_name : names)
    {
        ObservableCallback<double> bridge{
            [pub_cb = callback](ObservableResult<double>& sdk_result)
            {
                SdkObservableResultAdapter<double> adapter{sdk_result};
                pub_cb(adapter);
            }};
        m_producer->AddStream(
            m_scope,
            std::make_unique<MetricStreamObservableGauge<double>>(
                stream_name, description, unit, std::move(bridge), m_max_cardinality, m_diag));
    }
    return {};
}

}  // namespace microtel::sdk
