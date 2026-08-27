// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/meter.hpp"
#include "microtel/provider.hpp"

#include "adapters/otelcpp/metrics_instruments_shim.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>

#include <opentelemetry/metrics/meter.h>
#include <opentelemetry/metrics/meter_provider.h>
#include <opentelemetry/nostd/shared_ptr.h>
#include <opentelemetry/nostd/unique_ptr.h>

/// @file
/// `MeterShim` / `MeterProviderShim` — implement otel-cpp's `metrics::Meter`
/// and `metrics::MeterProvider` (ABI v1) over `microtel::Meter` and
/// `microtel::Provider`. M17 L3.
///
/// Sync gauges do not appear: their create methods are ABI v2, and the shim
/// pins ABI v1 (observable gauges are v1 and are covered). microtel's
/// exponential histograms have no otel-cpp API surface to arrive through —
/// applications wanting them use microtel's API directly.

namespace microtel::adapters::otelcpp
{

/// @brief An otel-cpp meter backed by a microtel meter.
///
/// Sync instruments forward per `metrics_instruments_shim.hpp` (uint64
/// measurements above `INT64_MAX` are omitted, exactly-representable values
/// cast exactly). Observable instruments register **one** microtel callback
/// at creation whose collection-cycle invocation fans out to the otel
/// callbacks registered on the returned instrument — see
/// `detail::ObservableCallbackRegistry`.
///
/// @threadsafety Thread-safe, matching `microtel::Meter`.
class MeterShim final : public opentelemetry::metrics::Meter
{
public:
    /// @param meter the microtel meter to create instruments on. Must be
    ///              non-null.
    explicit MeterShim(std::shared_ptr<microtel::Meter> meter) noexcept : m_meter{std::move(meter)}
    {
    }

    [[nodiscard]] opentelemetry::nostd::unique_ptr<opentelemetry::metrics::Counter<std::uint64_t>>
    CreateUInt64Counter(opentelemetry::nostd::string_view name,
                        opentelemetry::nostd::string_view description = "",
                        opentelemetry::nostd::string_view unit = "") noexcept override
    {
        return opentelemetry::nostd::unique_ptr<opentelemetry::metrics::Counter<std::uint64_t>>{
            std::make_unique<detail::CounterShim<std::uint64_t, std::int64_t>>(
                m_meter->CreateCounter<std::int64_t>(
                    ToString(name), ToString(description), ToString(unit)))};
    }

    [[nodiscard]] opentelemetry::nostd::unique_ptr<opentelemetry::metrics::Counter<double>>
    CreateDoubleCounter(opentelemetry::nostd::string_view name,
                        opentelemetry::nostd::string_view description = "",
                        opentelemetry::nostd::string_view unit = "") noexcept override
    {
        return opentelemetry::nostd::unique_ptr<opentelemetry::metrics::Counter<double>>{
            std::make_unique<detail::CounterShim<double, double>>(m_meter->CreateCounter<double>(
                ToString(name), ToString(description), ToString(unit)))};
    }

    [[nodiscard]] opentelemetry::nostd::shared_ptr<opentelemetry::metrics::ObservableInstrument>
    CreateInt64ObservableCounter(opentelemetry::nostd::string_view name,
                                 opentelemetry::nostd::string_view description = "",
                                 opentelemetry::nostd::string_view unit = "") noexcept override
    {
        return MakeObservable<std::int64_t>(
            [this](std::string n,
                   std::string d,
                   std::string u,
                   microtel::ObservableCallback<std::int64_t> cb)
            {
                std::ignore = m_meter->CreateObservableCounter<std::int64_t>(
                    std::move(n), std::move(d), std::move(u), std::move(cb));
            },
            name,
            description,
            unit);
    }

    [[nodiscard]] opentelemetry::nostd::shared_ptr<opentelemetry::metrics::ObservableInstrument>
    CreateDoubleObservableCounter(opentelemetry::nostd::string_view name,
                                  opentelemetry::nostd::string_view description = "",
                                  opentelemetry::nostd::string_view unit = "") noexcept override
    {
        return MakeObservable<double>(
            [this](std::string n,
                   std::string d,
                   std::string u,
                   microtel::ObservableCallback<double> cb)
            {
                std::ignore = m_meter->CreateObservableCounter<double>(
                    std::move(n), std::move(d), std::move(u), std::move(cb));
            },
            name,
            description,
            unit);
    }

    [[nodiscard]] opentelemetry::nostd::unique_ptr<opentelemetry::metrics::Histogram<std::uint64_t>>
    CreateUInt64Histogram(opentelemetry::nostd::string_view name,
                          opentelemetry::nostd::string_view description = "",
                          opentelemetry::nostd::string_view unit = "") noexcept override
    {
        return opentelemetry::nostd::unique_ptr<opentelemetry::metrics::Histogram<std::uint64_t>>{
            std::make_unique<detail::HistogramShim<std::uint64_t, std::int64_t>>(
                m_meter->CreateHistogram<std::int64_t>(
                    ToString(name), ToString(description), ToString(unit)))};
    }

    [[nodiscard]] opentelemetry::nostd::unique_ptr<opentelemetry::metrics::Histogram<double>>
    CreateDoubleHistogram(opentelemetry::nostd::string_view name,
                          opentelemetry::nostd::string_view description = "",
                          opentelemetry::nostd::string_view unit = "") noexcept override
    {
        return opentelemetry::nostd::unique_ptr<opentelemetry::metrics::Histogram<double>>{
            std::make_unique<detail::HistogramShim<double, double>>(
                m_meter->CreateHistogram<double>(
                    ToString(name), ToString(description), ToString(unit)))};
    }

    [[nodiscard]] opentelemetry::nostd::shared_ptr<opentelemetry::metrics::ObservableInstrument>
    CreateInt64ObservableGauge(opentelemetry::nostd::string_view name,
                               opentelemetry::nostd::string_view description = "",
                               opentelemetry::nostd::string_view unit = "") noexcept override
    {
        return MakeObservable<std::int64_t>(
            [this](std::string n,
                   std::string d,
                   std::string u,
                   microtel::ObservableCallback<std::int64_t> cb)
            {
                std::ignore = m_meter->CreateObservableGauge<std::int64_t>(
                    std::move(n), std::move(d), std::move(u), std::move(cb));
            },
            name,
            description,
            unit);
    }

    [[nodiscard]] opentelemetry::nostd::shared_ptr<opentelemetry::metrics::ObservableInstrument>
    CreateDoubleObservableGauge(opentelemetry::nostd::string_view name,
                                opentelemetry::nostd::string_view description = "",
                                opentelemetry::nostd::string_view unit = "") noexcept override
    {
        return MakeObservable<double>(
            [this](std::string n,
                   std::string d,
                   std::string u,
                   microtel::ObservableCallback<double> cb)
            {
                std::ignore = m_meter->CreateObservableGauge<double>(
                    std::move(n), std::move(d), std::move(u), std::move(cb));
            },
            name,
            description,
            unit);
    }

    [[nodiscard]] opentelemetry::nostd::unique_ptr<
        opentelemetry::metrics::UpDownCounter<std::int64_t>>
    CreateInt64UpDownCounter(opentelemetry::nostd::string_view name,
                             opentelemetry::nostd::string_view description = "",
                             opentelemetry::nostd::string_view unit = "") noexcept override
    {
        return opentelemetry::nostd::unique_ptr<
            opentelemetry::metrics::UpDownCounter<std::int64_t>>{
            std::make_unique<detail::UpDownCounterShim<std::int64_t, std::int64_t>>(
                m_meter->CreateUpDownCounter<std::int64_t>(
                    ToString(name), ToString(description), ToString(unit)))};
    }

    [[nodiscard]] opentelemetry::nostd::unique_ptr<opentelemetry::metrics::UpDownCounter<double>>
    CreateDoubleUpDownCounter(opentelemetry::nostd::string_view name,
                              opentelemetry::nostd::string_view description = "",
                              opentelemetry::nostd::string_view unit = "") noexcept override
    {
        return opentelemetry::nostd::unique_ptr<opentelemetry::metrics::UpDownCounter<double>>{
            std::make_unique<detail::UpDownCounterShim<double, double>>(
                m_meter->CreateUpDownCounter<double>(
                    ToString(name), ToString(description), ToString(unit)))};
    }

    [[nodiscard]] opentelemetry::nostd::shared_ptr<opentelemetry::metrics::ObservableInstrument>
    CreateInt64ObservableUpDownCounter(
        opentelemetry::nostd::string_view name,
        opentelemetry::nostd::string_view description = "",
        opentelemetry::nostd::string_view unit = "") noexcept override
    {
        return MakeObservable<std::int64_t>(
            [this](std::string n,
                   std::string d,
                   std::string u,
                   microtel::ObservableCallback<std::int64_t> cb)
            {
                std::ignore = m_meter->CreateObservableUpDownCounter<std::int64_t>(
                    std::move(n), std::move(d), std::move(u), std::move(cb));
            },
            name,
            description,
            unit);
    }

    [[nodiscard]] opentelemetry::nostd::shared_ptr<opentelemetry::metrics::ObservableInstrument>
    CreateDoubleObservableUpDownCounter(
        opentelemetry::nostd::string_view name,
        opentelemetry::nostd::string_view description = "",
        opentelemetry::nostd::string_view unit = "") noexcept override
    {
        return MakeObservable<double>(
            [this](std::string n,
                   std::string d,
                   std::string u,
                   microtel::ObservableCallback<double> cb)
            {
                std::ignore = m_meter->CreateObservableUpDownCounter<double>(
                    std::move(n), std::move(d), std::move(u), std::move(cb));
            },
            name,
            description,
            unit);
    }

private:
    [[nodiscard]] static std::string ToString(opentelemetry::nostd::string_view view)
    {
        return {view.data(), view.size()};
    }

    /// @brief Common observable wiring: build the shared registry, register
    ///        the fan-out callback with microtel via @p create, and front the
    ///        registry with an `ObservableShim`.
    template <typename T, typename CreateFn>
    [[nodiscard]] opentelemetry::nostd::shared_ptr<opentelemetry::metrics::ObservableInstrument>
    MakeObservable(const CreateFn& create,
                   opentelemetry::nostd::string_view name,
                   opentelemetry::nostd::string_view description,
                   opentelemetry::nostd::string_view unit) noexcept
    {
        auto registry = std::make_shared<detail::ObservableCallbackRegistry<T>>();
        create(ToString(name),
               ToString(description),
               ToString(unit),
               [registry](microtel::ObservableResult<T>& result) { registry->Invoke(result); });
        return opentelemetry::nostd::shared_ptr<opentelemetry::metrics::ObservableInstrument>{
            std::make_shared<detail::ObservableShim<T>>(std::move(registry))};
    }

    std::shared_ptr<microtel::Meter> m_meter;
};

/// @brief An otel-cpp meter provider backed by a microtel provider.
///
/// Unlike the tracer path, `schema_url` passes through — microtel's
/// `Provider::GetMeter` carries it.
///
/// @threadsafety Thread-safe, matching `microtel::Provider`.
class MeterProviderShim final : public opentelemetry::metrics::MeterProvider
{
public:
    /// @param provider the microtel provider to adapt. Must be non-null.
    explicit MeterProviderShim(std::shared_ptr<microtel::Provider> provider) noexcept
        : m_provider{std::move(provider)}
    {
    }

    // Default arguments mirror the base declaration so direct calls through
    // the shim type keep otel-cpp's one-argument form.
    [[nodiscard]] opentelemetry::nostd::shared_ptr<opentelemetry::metrics::Meter> GetMeter(
        opentelemetry::nostd::string_view name,
        opentelemetry::nostd::string_view version = "",
        opentelemetry::nostd::string_view schema_url = "") noexcept override
    {
        return opentelemetry::nostd::shared_ptr<opentelemetry::metrics::Meter>{
            std::make_shared<MeterShim>(
                m_provider->GetMeter({name.data(), name.size()},
                                     {version.data(), version.size()},
                                     {schema_url.data(), schema_url.size()}))};
    }

private:
    std::shared_ptr<microtel::Provider> m_provider;
};

/// @brief Build an otel-cpp meter provider over a microtel provider, ready
///        for `opentelemetry::metrics::Provider::SetMeterProvider`.
[[nodiscard]] inline opentelemetry::nostd::shared_ptr<opentelemetry::metrics::MeterProvider>
MakeMeterProvider(std::shared_ptr<microtel::Provider> provider)
{
    return opentelemetry::nostd::shared_ptr<opentelemetry::metrics::MeterProvider>{
        std::make_shared<MeterProviderShim>(std::move(provider))};
}

}  // namespace microtel::adapters::otelcpp
