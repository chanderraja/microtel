// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/meter.hpp"

#include "adapters/otelcpp/attribute_conversion.hpp"

#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include <opentelemetry/context/context.h>
#include <opentelemetry/metrics/async_instruments.h>
#include <opentelemetry/metrics/observer_result.h>
#include <opentelemetry/metrics/sync_instruments.h>
#include <opentelemetry/nostd/unique_ptr.h>

/// @file
/// Instrument shims: otel-cpp sync and observable metric instruments (ABI v1)
/// over microtel's.
///
/// **Value mapping.** otel-cpp's Counter and Histogram measure in `uint64_t`;
/// microtel's in `int64_t`. Values up to `INT64_MAX` cast exactly; above it
/// the measurement is **dropped** — per ICP 0015's principle, preserve or
/// omit, never invent, and unlike attributes a measurement has no degraded
/// type to preserve into. (A single increment above 9.2 × 10¹⁸ does not occur
/// in practice.) `int64_t` / `double` instruments map exactly.
///
/// **Context parameters** on `Add`/`Record` carry exemplar correlation in
/// otel-cpp; microtel has no exemplar surface in v1, so they are ignored.

namespace microtel::adapters::otelcpp
{

namespace detail
{

/// @brief True when a `uint64_t` measurement is representable as `int64_t`.
[[nodiscard]] constexpr bool FitsInt64(std::uint64_t value) noexcept
{
    return value <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
}

/// @brief otel `Counter<OtelT>` over `microtel::Counter<MicroT>`.
template <typename OtelT, typename MicroT>
class CounterShim final : public opentelemetry::metrics::Counter<OtelT>
{
public:
    explicit CounterShim(std::shared_ptr<microtel::Counter<MicroT>> counter) noexcept
        : m_counter{std::move(counter)}
    {
    }

    void Add(OtelT value) noexcept override
    {
        Forward(value, {});
    }

    void Add(OtelT value, const opentelemetry::context::Context& /*context*/) noexcept override
    {
        Forward(value, {});
    }

    void Add(OtelT value, const otel_common::KeyValueIterable& attributes) noexcept override
    {
        const std::vector<microtel::KeyValue> converted = ConvertKeyValues(attributes);
        Forward(value, microtel::AttributeSpan{converted});
    }

    void Add(OtelT value,
             const otel_common::KeyValueIterable& attributes,
             const opentelemetry::context::Context& /*context*/) noexcept override
    {
        Add(value, attributes);
    }

private:
    void Forward(OtelT value, microtel::AttributeSpan attrs) noexcept
    {
        if constexpr (std::is_same_v<OtelT, std::uint64_t> && std::is_same_v<MicroT, std::int64_t>)
        {
            if (!FitsInt64(value))
            {
                return;  // omit, never invent (see @file)
            }
        }
        m_counter->Add(static_cast<MicroT>(value), attrs);
    }

    std::shared_ptr<microtel::Counter<MicroT>> m_counter;
};

/// @brief otel `UpDownCounter<OtelT>` over `microtel::UpDownCounter<MicroT>`.
///
/// Both sides are signed (`int64_t` / `double`), so values map exactly.
template <typename OtelT, typename MicroT>
class UpDownCounterShim final : public opentelemetry::metrics::UpDownCounter<OtelT>
{
public:
    explicit UpDownCounterShim(std::shared_ptr<microtel::UpDownCounter<MicroT>> counter) noexcept
        : m_counter{std::move(counter)}
    {
    }

    void Add(OtelT value) noexcept override
    {
        m_counter->Add(static_cast<MicroT>(value), {});
    }

    void Add(OtelT value, const opentelemetry::context::Context& /*context*/) noexcept override
    {
        m_counter->Add(static_cast<MicroT>(value), {});
    }

    void Add(OtelT value, const otel_common::KeyValueIterable& attributes) noexcept override
    {
        const std::vector<microtel::KeyValue> converted = ConvertKeyValues(attributes);
        m_counter->Add(static_cast<MicroT>(value), microtel::AttributeSpan{converted});
    }

    void Add(OtelT value,
             const otel_common::KeyValueIterable& attributes,
             const opentelemetry::context::Context& /*context*/) noexcept override
    {
        Add(value, attributes);
    }

private:
    std::shared_ptr<microtel::UpDownCounter<MicroT>> m_counter;
};

/// @brief otel `Histogram<OtelT>` over `microtel::Histogram<MicroT>`.
template <typename OtelT, typename MicroT>
class HistogramShim final : public opentelemetry::metrics::Histogram<OtelT>
{
public:
    explicit HistogramShim(std::shared_ptr<microtel::Histogram<MicroT>> histogram) noexcept
        : m_histogram{std::move(histogram)}
    {
    }

    void Record(OtelT value, const opentelemetry::context::Context& /*context*/) noexcept override
    {
        Forward(value, {});
    }

    void Record(OtelT value,
                const otel_common::KeyValueIterable& attributes,
                const opentelemetry::context::Context& /*context*/) noexcept override
    {
        const std::vector<microtel::KeyValue> converted = ConvertKeyValues(attributes);
        Forward(value, microtel::AttributeSpan{converted});
    }

private:
    void Forward(OtelT value, microtel::AttributeSpan attrs) noexcept
    {
        if constexpr (std::is_same_v<OtelT, std::uint64_t> && std::is_same_v<MicroT, std::int64_t>)
        {
            if (!FitsInt64(value))
            {
                return;  // omit, never invent (see @file)
            }
        }
        m_histogram->Record(static_cast<MicroT>(value), attrs);
    }

    std::shared_ptr<microtel::Histogram<MicroT>> m_histogram;
};

/// @brief otel `ObserverResultT<T>` writing into a microtel
///        `ObservableResult<T>` during one collection cycle.
///
/// Valid only for the duration of the callback invocation that received it —
/// otel-cpp's own SDK observer results have the same lifetime.
template <typename T>
class ObserverResultAdapter final : public opentelemetry::metrics::ObserverResultT<T>
{
public:
    explicit ObserverResultAdapter(microtel::ObservableResult<T>& sink) noexcept : m_sink{sink} {}

    void Observe(T value) noexcept override
    {
        m_sink.Observe(value, {});
    }

    void Observe(T value, const otel_common::KeyValueIterable& attributes) noexcept override
    {
        const std::vector<microtel::KeyValue> converted = ConvertKeyValues(attributes);
        m_sink.Observe(value, microtel::AttributeSpan{converted});
    }

private:
    microtel::ObservableResult<T>& m_sink;
};

/// @brief Shared list of otel observer callbacks for one observable
///        instrument.
///
/// Bridges the callback-model mismatch: otel-cpp adds and removes callbacks
/// after creation; microtel binds exactly one callback at creation. The one
/// microtel callback fans out to whatever this registry holds at collection
/// time. Shared between the `ObservableShim` (which mutates it) and the
/// microtel callback (which reads it), so it outlives whichever goes first.
///
/// @threadsafety Thread-safe: the app thread mutates while microtel's
///               collection cycle reads.
template <typename T>
class ObservableCallbackRegistry
{
public:
    void Add(opentelemetry::metrics::ObservableCallbackPtr callback, void* state)
    {
        const std::scoped_lock lock{m_mu};
        m_callbacks.push_back({.callback = callback, .state = state});
    }

    void Remove(opentelemetry::metrics::ObservableCallbackPtr callback, void* state)
    {
        const std::scoped_lock lock{m_mu};
        std::erase_if(m_callbacks,
                      [callback, state](const Entry& entry)
                      { return entry.callback == callback && entry.state == state; });
    }

    /// @brief One collection cycle: hand every registered otel callback an
    ///        adapter writing into @p result.
    void Invoke(microtel::ObservableResult<T>& result)
    {
        const auto adapter = std::make_shared<ObserverResultAdapter<T>>(result);
        const std::scoped_lock lock{m_mu};
        for (const auto& entry : m_callbacks)
        {
            entry.callback(opentelemetry::metrics::ObserverResult{adapter}, entry.state);
        }
    }

private:
    struct Entry
    {
        opentelemetry::metrics::ObservableCallbackPtr callback = nullptr;
        void* state = nullptr;
    };

    std::mutex m_mu;
    std::vector<Entry> m_callbacks;
};

/// @brief otel `ObservableInstrument` fronting a registry; the paired
///        microtel observable holds the other reference to the registry.
template <typename T>
class ObservableShim final : public opentelemetry::metrics::ObservableInstrument
{
public:
    explicit ObservableShim(std::shared_ptr<ObservableCallbackRegistry<T>> registry) noexcept
        : m_registry{std::move(registry)}
    {
    }

    void AddCallback(opentelemetry::metrics::ObservableCallbackPtr callback,
                     void* state) noexcept override
    {
        m_registry->Add(callback, state);
    }

    void RemoveCallback(opentelemetry::metrics::ObservableCallbackPtr callback,
                        void* state) noexcept override
    {
        m_registry->Remove(callback, state);
    }

private:
    std::shared_ptr<ObservableCallbackRegistry<T>> m_registry;
};

}  // namespace detail

}  // namespace microtel::adapters::otelcpp
