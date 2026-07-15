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

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace microtel::sdk
{

namespace
{

// ── Attribute allowlist types and helpers ─────────────────────────────────────

using Allowlist = std::vector<std::string>;

// Filters attrs to only keys present in the (sorted) allowlist, writing
// results into buf. buf is cleared before use; capacity is preserved across
// calls to amortise allocation in the multi-slot fan-out case.
void FilterAttrs(microtel::AttributeSpan attrs,
                 const Allowlist& allowlist,
                 std::vector<microtel::KeyValue>& buf) noexcept
{
    buf.clear();
    for (const auto& kv : attrs)
    {
        if (std::ranges::binary_search(allowlist, kv.key))
        {
            buf.push_back(kv);
        }
    }
}

// ── Stream specification ──────────────────────────────────────────────────────
// Carries the resolved stream name and optional sorted allowlist for one view
// match. The allowlist is pre-sorted so FilterAttrs can use binary_search.

struct StreamSpec
{
    std::string name;
    std::optional<Allowlist> allowlist;  // sorted; nullopt = no filtering
};

// ── Storage slot ──────────────────────────────────────────────────────────────
// Pairs a non-owning storage pointer with the allowlist for its stream.
// An empty allowlist vector (not nullopt) means all keys are stripped;
// nullopt means no filtering is applied.

template <typename StorageT>
struct StorageSlot
{
    StorageT* storage = nullptr;
    std::optional<Allowlist> allowlist;
};

// ── View stream resolution ────────────────────────────────────────────────────
// Returns one StreamSpec per non-dropped view that matches desc, in
// registration order. An empty result means all views have drop=true;
// the caller creates no stream (instrument is a no-op). A single entry
// with the original name and nullopt allowlist is the default (no registry,
// no match, or a match with no rename / no allowlist).

std::vector<StreamSpec> ResolveStreamSpecs(const std::string& instrument_name,
                                           const ViewRegistry* registry,
                                           const InstrumentDescriptor& desc)
{
    if (registry == nullptr)
    {
        return {{.name = instrument_name, .allowlist = std::nullopt}};
    }

    const auto matches = registry->Match(desc);
    if (matches.empty())
    {
        return {{.name = instrument_name, .allowlist = std::nullopt}};
    }

    std::vector<StreamSpec> specs;
    for (const auto* const view : matches)
    {
        if (view->transform.drop)
        {
            continue;
        }
        auto al = view->transform.attribute_allowlist;
        if (al.has_value())
        {
            std::ranges::sort(*al);
        }
        specs.push_back({
            .name = view->transform.name.value_or(instrument_name),
            .allowlist = std::move(al),
        });
    }
    return specs;
}

// ── Concrete instrument adapters ──────────────────────────────────────────────
// Each adapter holds one StorageSlot per matching view. An empty slot list
// makes the instrument a no-op (all views dropped). When a slot has an
// allowlist, attrs are filtered before forwarding to storage; otherwise the
// original span is used directly (zero-copy hot path).
// Hot-path methods are noexcept: OOM in FilterAttrs → std::terminate per policy.

template <typename T>
class SdkCounter final : public microtel::Counter<T>
{
public:
    explicit SdkCounter(std::vector<StorageSlot<SumStorage<T>>> slots) noexcept
        : m_slots(std::move(slots))
    {
    }

    void Add(T value, microtel::AttributeSpan attrs) noexcept override
    {
        std::vector<microtel::KeyValue> filter_buf;
        for (const auto& slot : m_slots)
        {
            if (slot.allowlist.has_value())
            {
                FilterAttrs(attrs, *slot.allowlist, filter_buf);
                slot.storage->Add(value, microtel::AttributeSpan{filter_buf});
            }
            else
            {
                slot.storage->Add(value, attrs);
            }
        }
    }

private:
    std::vector<StorageSlot<SumStorage<T>>> m_slots;
};

template <typename T>
class SdkUpDownCounter final : public microtel::UpDownCounter<T>
{
public:
    explicit SdkUpDownCounter(std::vector<StorageSlot<SumStorage<T>>> slots) noexcept
        : m_slots(std::move(slots))
    {
    }

    void Add(T value, microtel::AttributeSpan attrs) noexcept override
    {
        std::vector<microtel::KeyValue> filter_buf;
        for (const auto& slot : m_slots)
        {
            if (slot.allowlist.has_value())
            {
                FilterAttrs(attrs, *slot.allowlist, filter_buf);
                slot.storage->Add(value, microtel::AttributeSpan{filter_buf});
            }
            else
            {
                slot.storage->Add(value, attrs);
            }
        }
    }

private:
    std::vector<StorageSlot<SumStorage<T>>> m_slots;
};

template <typename T>
class SdkGauge final : public microtel::Gauge<T>
{
public:
    explicit SdkGauge(std::vector<StorageSlot<GaugeStorage<T>>> slots) noexcept
        : m_slots(std::move(slots))
    {
    }

    void Record(T value, microtel::AttributeSpan attrs) noexcept override
    {
        std::vector<microtel::KeyValue> filter_buf;
        for (const auto& slot : m_slots)
        {
            if (slot.allowlist.has_value())
            {
                FilterAttrs(attrs, *slot.allowlist, filter_buf);
                slot.storage->Record(value, microtel::AttributeSpan{filter_buf});
            }
            else
            {
                slot.storage->Record(value, attrs);
            }
        }
    }

private:
    std::vector<StorageSlot<GaugeStorage<T>>> m_slots;
};

template <typename T>
class SdkHistogram final : public microtel::Histogram<T>
{
public:
    explicit SdkHistogram(std::vector<StorageSlot<HistogramStorage<T>>> slots) noexcept
        : m_slots(std::move(slots))
    {
    }

    void Record(T value, microtel::AttributeSpan attrs) noexcept override
    {
        std::vector<microtel::KeyValue> filter_buf;
        for (const auto& slot : m_slots)
        {
            if (slot.allowlist.has_value())
            {
                FilterAttrs(attrs, *slot.allowlist, filter_buf);
                slot.storage->Record(value, microtel::AttributeSpan{filter_buf});
            }
            else
            {
                slot.storage->Record(value, attrs);
            }
        }
    }

private:
    std::vector<StorageSlot<HistogramStorage<T>>> m_slots;
};

template <typename T>
class SdkExponentialHistogram final : public microtel::ExponentialHistogram<T>
{
public:
    explicit SdkExponentialHistogram(
        std::vector<StorageSlot<ExponentialHistogramStorage<T>>> slots) noexcept
        : m_slots(std::move(slots))
    {
    }

    void Record(T value, microtel::AttributeSpan attrs) noexcept override
    {
        std::vector<microtel::KeyValue> filter_buf;
        for (const auto& slot : m_slots)
        {
            if (slot.allowlist.has_value())
            {
                FilterAttrs(attrs, *slot.allowlist, filter_buf);
                slot.storage->Record(value, microtel::AttributeSpan{filter_buf});
            }
            else
            {
                slot.storage->Record(value, attrs);
            }
        }
    }

private:
    std::vector<StorageSlot<ExponentialHistogramStorage<T>>> m_slots;
};

// ── Public-to-internal ObservableResult bridge ────────────────────────────────
// Adapts microtel::ObservableResult<T> (public abstract) to sdk::ObservableResult<T>
// (internal concrete). The adapter is stack-allocated inside the bridge lambda that
// wraps the user's public callback for each collection cycle.

template <typename T>
class SdkObservableResultAdapter final : public microtel::ObservableResult<T>
{
public:
    SdkObservableResultAdapter(ObservableResult<T>& target,
                               const std::optional<Allowlist>& allowlist) noexcept
        : m_target(target), m_allowlist(&allowlist)
    {
    }

    SdkObservableResultAdapter(const SdkObservableResultAdapter&) = delete;
    SdkObservableResultAdapter& operator=(const SdkObservableResultAdapter&) = delete;
    SdkObservableResultAdapter(SdkObservableResultAdapter&&) = delete;
    SdkObservableResultAdapter& operator=(SdkObservableResultAdapter&&) = delete;
    ~SdkObservableResultAdapter() noexcept override = default;

    void Observe(T value, microtel::AttributeSpan attrs) override
    {
        if (m_allowlist->has_value())
        {
            FilterAttrs(attrs, **m_allowlist, m_filter_buf);
            m_target.Observe(value, microtel::AttributeSpan{m_filter_buf});
        }
        else
        {
            m_target.Observe(value, attrs);
        }
    }

private:
    ObservableResult<T>& m_target;
    const std::optional<Allowlist>* m_allowlist;
    std::vector<microtel::KeyValue> m_filter_buf;
};

}  // namespace

// ── SdkMeter ──────────────────────────────────────────────────────────────────

SdkMeter::SdkMeter(internal::InstrumentationScope scope,
                   std::shared_ptr<MetricProducer> producer,
                   std::size_t max_cardinality,
                   internal::IDiagnosticsSink* diag,
                   std::shared_ptr<const ViewRegistry> registry) noexcept
    : m_scope(std::move(scope)),
      m_producer(std::move(producer)),
      m_max_cardinality(max_cardinality),
      m_diag(diag),
      m_registry(std::move(registry))
{
}

std::shared_ptr<microtel::Counter<std::int64_t>> SdkMeter::DoCreateCounterI64(
    std::string name, std::string description, std::string unit)
{
    const InstrumentDescriptor desc{
        .name = name, .kind = InstrumentKind::Counter, .meter_name = m_scope.name};
    const auto specs = ResolveStreamSpecs(name, m_registry.get(), desc);
    const StorageOptions opts{.max_cardinality = m_max_cardinality, .diag = m_diag};
    std::vector<StorageSlot<SumStorage<std::int64_t>>> slots;
    slots.reserve(specs.size());
    for (const auto& spec : specs)
    {
        auto stream = std::make_unique<MetricStreamSum<std::int64_t>>(
            spec.name, description, unit, /*monotonic=*/true, opts);
        slots.push_back({.storage = &stream->Storage(), .allowlist = spec.allowlist});
        m_producer->AddStream(m_scope, std::move(stream));
    }
    return std::make_shared<SdkCounter<std::int64_t>>(std::move(slots));
}

std::shared_ptr<microtel::Counter<double>> SdkMeter::DoCreateCounterDouble(std::string name,
                                                                           std::string description,
                                                                           std::string unit)
{
    const InstrumentDescriptor desc{
        .name = name, .kind = InstrumentKind::Counter, .meter_name = m_scope.name};
    const auto specs = ResolveStreamSpecs(name, m_registry.get(), desc);
    const StorageOptions opts{.max_cardinality = m_max_cardinality, .diag = m_diag};
    std::vector<StorageSlot<SumStorage<double>>> slots;
    slots.reserve(specs.size());
    for (const auto& spec : specs)
    {
        auto stream = std::make_unique<MetricStreamSum<double>>(
            spec.name, description, unit, /*monotonic=*/true, opts);
        slots.push_back({.storage = &stream->Storage(), .allowlist = spec.allowlist});
        m_producer->AddStream(m_scope, std::move(stream));
    }
    return std::make_shared<SdkCounter<double>>(std::move(slots));
}

std::shared_ptr<microtel::UpDownCounter<std::int64_t>> SdkMeter::DoCreateUpDownCounterI64(
    std::string name, std::string description, std::string unit)
{
    const InstrumentDescriptor desc{
        .name = name, .kind = InstrumentKind::UpDownCounter, .meter_name = m_scope.name};
    const auto specs = ResolveStreamSpecs(name, m_registry.get(), desc);
    const StorageOptions opts{.max_cardinality = m_max_cardinality, .diag = m_diag};
    std::vector<StorageSlot<SumStorage<std::int64_t>>> slots;
    slots.reserve(specs.size());
    for (const auto& spec : specs)
    {
        auto stream = std::make_unique<MetricStreamSum<std::int64_t>>(
            spec.name, description, unit, /*monotonic=*/false, opts);
        slots.push_back({.storage = &stream->Storage(), .allowlist = spec.allowlist});
        m_producer->AddStream(m_scope, std::move(stream));
    }
    return std::make_shared<SdkUpDownCounter<std::int64_t>>(std::move(slots));
}

std::shared_ptr<microtel::UpDownCounter<double>> SdkMeter::DoCreateUpDownCounterDouble(
    std::string name, std::string description, std::string unit)
{
    const InstrumentDescriptor desc{
        .name = name, .kind = InstrumentKind::UpDownCounter, .meter_name = m_scope.name};
    const auto specs = ResolveStreamSpecs(name, m_registry.get(), desc);
    const StorageOptions opts{.max_cardinality = m_max_cardinality, .diag = m_diag};
    std::vector<StorageSlot<SumStorage<double>>> slots;
    slots.reserve(specs.size());
    for (const auto& spec : specs)
    {
        auto stream = std::make_unique<MetricStreamSum<double>>(
            spec.name, description, unit, /*monotonic=*/false, opts);
        slots.push_back({.storage = &stream->Storage(), .allowlist = spec.allowlist});
        m_producer->AddStream(m_scope, std::move(stream));
    }
    return std::make_shared<SdkUpDownCounter<double>>(std::move(slots));
}

std::shared_ptr<microtel::Gauge<std::int64_t>> SdkMeter::DoCreateGaugeI64(std::string name,
                                                                          std::string description,
                                                                          std::string unit)
{
    const InstrumentDescriptor desc{
        .name = name, .kind = InstrumentKind::Gauge, .meter_name = m_scope.name};
    const auto specs = ResolveStreamSpecs(name, m_registry.get(), desc);
    const StorageOptions opts{.max_cardinality = m_max_cardinality, .diag = m_diag};
    std::vector<StorageSlot<GaugeStorage<std::int64_t>>> slots;
    slots.reserve(specs.size());
    for (const auto& spec : specs)
    {
        auto stream =
            std::make_unique<MetricStreamGauge<std::int64_t>>(spec.name, description, unit, opts);
        slots.push_back({.storage = &stream->Storage(), .allowlist = spec.allowlist});
        m_producer->AddStream(m_scope, std::move(stream));
    }
    return std::make_shared<SdkGauge<std::int64_t>>(std::move(slots));
}

std::shared_ptr<microtel::Gauge<double>> SdkMeter::DoCreateGaugeDouble(std::string name,
                                                                       std::string description,
                                                                       std::string unit)
{
    const InstrumentDescriptor desc{
        .name = name, .kind = InstrumentKind::Gauge, .meter_name = m_scope.name};
    const auto specs = ResolveStreamSpecs(name, m_registry.get(), desc);
    const StorageOptions opts{.max_cardinality = m_max_cardinality, .diag = m_diag};
    std::vector<StorageSlot<GaugeStorage<double>>> slots;
    slots.reserve(specs.size());
    for (const auto& spec : specs)
    {
        auto stream =
            std::make_unique<MetricStreamGauge<double>>(spec.name, description, unit, opts);
        slots.push_back({.storage = &stream->Storage(), .allowlist = spec.allowlist});
        m_producer->AddStream(m_scope, std::move(stream));
    }
    return std::make_shared<SdkGauge<double>>(std::move(slots));
}

std::shared_ptr<microtel::Histogram<std::int64_t>> SdkMeter::DoCreateHistogramI64(
    std::string name, std::string description, std::string unit, std::vector<double> boundaries)
{
    const InstrumentDescriptor desc{
        .name = name, .kind = InstrumentKind::Histogram, .meter_name = m_scope.name};
    const auto specs = ResolveStreamSpecs(name, m_registry.get(), desc);
    const StorageOptions opts{.max_cardinality = m_max_cardinality, .diag = m_diag};
    std::vector<StorageSlot<HistogramStorage<std::int64_t>>> slots;
    slots.reserve(specs.size());
    for (const auto& spec : specs)
    {
        auto stream = std::make_unique<MetricStreamHistogram<std::int64_t>>(
            spec.name, description, unit, boundaries, opts);
        slots.push_back({.storage = &stream->Storage(), .allowlist = spec.allowlist});
        m_producer->AddStream(m_scope, std::move(stream));
    }
    return std::make_shared<SdkHistogram<std::int64_t>>(std::move(slots));
}

std::shared_ptr<microtel::Histogram<double>> SdkMeter::DoCreateHistogramDouble(
    std::string name, std::string description, std::string unit, std::vector<double> boundaries)
{
    const InstrumentDescriptor desc{
        .name = name, .kind = InstrumentKind::Histogram, .meter_name = m_scope.name};
    const auto specs = ResolveStreamSpecs(name, m_registry.get(), desc);
    const StorageOptions opts{.max_cardinality = m_max_cardinality, .diag = m_diag};
    std::vector<StorageSlot<HistogramStorage<double>>> slots;
    slots.reserve(specs.size());
    for (const auto& spec : specs)
    {
        auto stream = std::make_unique<MetricStreamHistogram<double>>(
            spec.name, description, unit, boundaries, opts);
        slots.push_back({.storage = &stream->Storage(), .allowlist = spec.allowlist});
        m_producer->AddStream(m_scope, std::move(stream));
    }
    return std::make_shared<SdkHistogram<double>>(std::move(slots));
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
    const auto specs = ResolveStreamSpecs(name, m_registry.get(), desc);
    const StorageOptions opts{.max_cardinality = m_max_cardinality, .diag = m_diag};
    std::vector<StorageSlot<ExponentialHistogramStorage<std::int64_t>>> slots;
    slots.reserve(specs.size());
    for (const auto& spec : specs)
    {
        auto stream = std::make_unique<MetricStreamExpHistogram<std::int64_t>>(
            spec.name, description, unit, max_scale, max_buckets, opts);
        slots.push_back({.storage = &stream->Storage(), .allowlist = spec.allowlist});
        m_producer->AddStream(m_scope, std::move(stream));
    }
    return std::make_shared<SdkExponentialHistogram<std::int64_t>>(std::move(slots));
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
    const auto specs = ResolveStreamSpecs(name, m_registry.get(), desc);
    const StorageOptions opts{.max_cardinality = m_max_cardinality, .diag = m_diag};
    std::vector<StorageSlot<ExponentialHistogramStorage<double>>> slots;
    slots.reserve(specs.size());
    for (const auto& spec : specs)
    {
        auto stream = std::make_unique<MetricStreamExpHistogram<double>>(
            spec.name, description, unit, max_scale, max_buckets, opts);
        slots.push_back({.storage = &stream->Storage(), .allowlist = spec.allowlist});
        m_producer->AddStream(m_scope, std::move(stream));
    }
    return std::make_shared<SdkExponentialHistogram<double>>(std::move(slots));
}

microtel::ObservableCounter<std::int64_t> SdkMeter::DoCreateObservableCounterI64(
    std::string name,
    std::string description,
    std::string unit,
    microtel::ObservableCallback<std::int64_t> callback)
{
    const InstrumentDescriptor desc{
        .name = name, .kind = InstrumentKind::ObservableCounter, .meter_name = m_scope.name};
    const auto specs = ResolveStreamSpecs(name, m_registry.get(), desc);
    for (const auto& spec : specs)
    {
        ObservableCallback<std::int64_t> bridge{
            [pub_cb = callback, al = spec.allowlist](ObservableResult<std::int64_t>& sdk_result)
            {
                SdkObservableResultAdapter<std::int64_t> adapter{sdk_result, al};
                pub_cb(adapter);
            }};
        m_producer->AddStream(
            m_scope,
            std::make_unique<MetricStreamObservableSum<std::int64_t>>(spec.name,
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
    const auto specs = ResolveStreamSpecs(name, m_registry.get(), desc);
    for (const auto& spec : specs)
    {
        ObservableCallback<double> bridge{
            [pub_cb = callback, al = spec.allowlist](ObservableResult<double>& sdk_result)
            {
                SdkObservableResultAdapter<double> adapter{sdk_result, al};
                pub_cb(adapter);
            }};
        m_producer->AddStream(
            m_scope,
            std::make_unique<MetricStreamObservableSum<double>>(spec.name,
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
    const auto specs = ResolveStreamSpecs(name, m_registry.get(), desc);
    for (const auto& spec : specs)
    {
        ObservableCallback<std::int64_t> bridge{
            [pub_cb = callback, al = spec.allowlist](ObservableResult<std::int64_t>& sdk_result)
            {
                SdkObservableResultAdapter<std::int64_t> adapter{sdk_result, al};
                pub_cb(adapter);
            }};
        m_producer->AddStream(
            m_scope,
            std::make_unique<MetricStreamObservableSum<std::int64_t>>(spec.name,
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
    const auto specs = ResolveStreamSpecs(name, m_registry.get(), desc);
    for (const auto& spec : specs)
    {
        ObservableCallback<double> bridge{
            [pub_cb = callback, al = spec.allowlist](ObservableResult<double>& sdk_result)
            {
                SdkObservableResultAdapter<double> adapter{sdk_result, al};
                pub_cb(adapter);
            }};
        m_producer->AddStream(
            m_scope,
            std::make_unique<MetricStreamObservableSum<double>>(spec.name,
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
    const auto specs = ResolveStreamSpecs(name, m_registry.get(), desc);
    for (const auto& spec : specs)
    {
        ObservableCallback<std::int64_t> bridge{
            [pub_cb = callback, al = spec.allowlist](ObservableResult<std::int64_t>& sdk_result)
            {
                SdkObservableResultAdapter<std::int64_t> adapter{sdk_result, al};
                pub_cb(adapter);
            }};
        m_producer->AddStream(
            m_scope,
            std::make_unique<MetricStreamObservableGauge<std::int64_t>>(
                spec.name, description, unit, std::move(bridge), m_max_cardinality, m_diag));
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
    const auto specs = ResolveStreamSpecs(name, m_registry.get(), desc);
    for (const auto& spec : specs)
    {
        ObservableCallback<double> bridge{
            [pub_cb = callback, al = spec.allowlist](ObservableResult<double>& sdk_result)
            {
                SdkObservableResultAdapter<double> adapter{sdk_result, al};
                pub_cb(adapter);
            }};
        m_producer->AddStream(
            m_scope,
            std::make_unique<MetricStreamObservableGauge<double>>(
                spec.name, description, unit, std::move(bridge), m_max_cardinality, m_diag));
    }
    return {};
}

}  // namespace microtel::sdk
