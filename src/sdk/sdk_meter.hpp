// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/internal/batch.hpp"
#include "microtel/meter.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace microtel::sdk
{

class MetricProducer;

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
/// @threadsafety Thread-safe for concurrent `Add()`/`Record()` calls.
///               `Create*()` calls are guarded by the provider's meter mutex.
class SdkMeter final : public microtel::Meter
{
public:
    /// @brief Construct a meter bound to @p scope and @p producer.
    explicit SdkMeter(internal::InstrumentationScope scope,
                      std::shared_ptr<MetricProducer> producer) noexcept;

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

    internal::InstrumentationScope m_scope;
    std::shared_ptr<MetricProducer> m_producer;
};

}  // namespace microtel::sdk
