// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/internal/batch.hpp"
#include "microtel/internal/diagnostics_sink.hpp"
#include "microtel/meter.hpp"

#include "sdk/metric_attribute_set.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace microtel::sdk
{

class MetricProducer;
class ViewRegistry;

/// @brief Concrete `microtel::Meter` backed by the SDK metric pipeline.
///
/// Bridges the public abstract API to the internal stream-registration layer.
/// Each `DoCreate*()` call allocates a `MetricStream`, registers it with the
/// shared `MetricProducer` under this meter's `InstrumentationScope`, and
/// returns a `shared_ptr` to a concrete instrument adapter that forwards
/// `Add()`/`Record()` straight to the underlying storage.
///
/// Instrument lifetime: the `MetricProducer` owns the streams (and therefore
/// the storage); instruments hold non-owning raw pointers into that storage.
/// An instrument must not be used after the provider that owns it is destroyed.
///
/// @threadsafety Thread-safe for concurrent `Add()`/`Record()` calls, and for
///               concurrent `Create*()` calls. `Create*()` registers the new
///               stream through `MetricProducer::AddStream`, which takes the
///               producer's own mutex.
///
/// @note This previously claimed `Create*()` was "guarded by the provider's
///       meter mutex". It is not: `m_meter_mu` is held only inside
///       `SdkProvider::GetMeter`, never during instrument creation. The
///       synchronisation now lives in `MetricProducer`, where the contended
///       state actually is.
class SdkMeter final : public microtel::Meter
{
public:
    /// @brief Construct a meter bound to @p scope and @p producer.
    ///
    /// @param max_cardinality Per-instrument cardinality cap; defaults to
    ///        `kDefaultMaxCardinality` (2000).
    /// @param diag Non-owning pointer to the provider's diagnostics sink;
    ///        null disables overflow drop accounting. Lifetime: the owning
    ///        provider outlives every SdkMeter it creates.
    explicit SdkMeter(internal::InstrumentationScope scope,
                      std::shared_ptr<MetricProducer> producer,
                      std::size_t max_cardinality = kDefaultMaxCardinality,
                      internal::IDiagnosticsSink* diag = nullptr,
                      std::shared_ptr<const ViewRegistry> registry = nullptr) noexcept;

    SdkMeter(const SdkMeter&) = delete;
    SdkMeter& operator=(const SdkMeter&) = delete;
    SdkMeter(SdkMeter&&) = delete;
    SdkMeter& operator=(SdkMeter&&) = delete;
    ~SdkMeter() noexcept override = default;

private:
    std::shared_ptr<microtel::Counter<std::int64_t>> DoCreateCounterI64(std::string name,
                                                                        std::string description,
                                                                        std::string unit) override;
    std::shared_ptr<microtel::Counter<double>> DoCreateCounterDouble(std::string name,
                                                                     std::string description,
                                                                     std::string unit) override;

    std::shared_ptr<microtel::UpDownCounter<std::int64_t>> DoCreateUpDownCounterI64(
        std::string name, std::string description, std::string unit) override;
    std::shared_ptr<microtel::UpDownCounter<double>> DoCreateUpDownCounterDouble(
        std::string name, std::string description, std::string unit) override;

    std::shared_ptr<microtel::Gauge<std::int64_t>> DoCreateGaugeI64(std::string name,
                                                                    std::string description,
                                                                    std::string unit) override;
    std::shared_ptr<microtel::Gauge<double>> DoCreateGaugeDouble(std::string name,
                                                                 std::string description,
                                                                 std::string unit) override;

    std::shared_ptr<microtel::Histogram<std::int64_t>> DoCreateHistogramI64(
        std::string name,
        std::string description,
        std::string unit,
        std::vector<double> boundaries) override;
    std::shared_ptr<microtel::Histogram<double>> DoCreateHistogramDouble(
        std::string name,
        std::string description,
        std::string unit,
        std::vector<double> boundaries) override;

    std::shared_ptr<microtel::ExponentialHistogram<std::int64_t>> DoCreateExponentialHistogramI64(
        std::string name,
        std::string description,
        std::string unit,
        std::int32_t max_scale,
        std::int32_t max_buckets) override;
    std::shared_ptr<microtel::ExponentialHistogram<double>> DoCreateExponentialHistogramDouble(
        std::string name,
        std::string description,
        std::string unit,
        std::int32_t max_scale,
        std::int32_t max_buckets) override;

    microtel::ObservableCounter<std::int64_t> DoCreateObservableCounterI64(
        std::string name,
        std::string description,
        std::string unit,
        microtel::ObservableCallback<std::int64_t> callback) override;
    microtel::ObservableCounter<double> DoCreateObservableCounterDouble(
        std::string name,
        std::string description,
        std::string unit,
        microtel::ObservableCallback<double> callback) override;

    microtel::ObservableUpDownCounter<std::int64_t> DoCreateObservableUpDownCounterI64(
        std::string name,
        std::string description,
        std::string unit,
        microtel::ObservableCallback<std::int64_t> callback) override;
    microtel::ObservableUpDownCounter<double> DoCreateObservableUpDownCounterDouble(
        std::string name,
        std::string description,
        std::string unit,
        microtel::ObservableCallback<double> callback) override;

    microtel::ObservableGauge<std::int64_t> DoCreateObservableGaugeI64(
        std::string name,
        std::string description,
        std::string unit,
        microtel::ObservableCallback<std::int64_t> callback) override;
    microtel::ObservableGauge<double> DoCreateObservableGaugeDouble(
        std::string name,
        std::string description,
        std::string unit,
        microtel::ObservableCallback<double> callback) override;

    internal::InstrumentationScope m_scope;
    std::shared_ptr<MetricProducer> m_producer;
    std::size_t m_max_cardinality;
    internal::IDiagnosticsSink* m_diag;
    std::shared_ptr<const ViewRegistry> m_registry;
};

}  // namespace microtel::sdk
