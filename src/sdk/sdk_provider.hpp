// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/internal/auth_provider.hpp"
#include "microtel/internal/exporter.hpp"
#include "microtel/internal/metric_exporter.hpp"
#include "microtel/internal/otlp_encoder.hpp"
#include "microtel/internal/processor.hpp"
#include "microtel/internal/transport.hpp"
#include "microtel/internal/wire_codec.hpp"
#include "microtel/provider.hpp"
#include "microtel/resource.hpp"
#include "microtel/sampler.hpp"
#include "microtel/sdk_builder.hpp"

#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

namespace microtel::sdk
{

// Forward-declared to keep implementation headers out of this header's transitive closure.
class Meter;
class MetricProducer;
class PeriodicExportingMetricReader;

/// @brief All owned objects passed to SdkProvider at construction.
///
/// Bundles the ten pipeline components so the constructor stays within the
/// seven-parameter tidy threshold. Declaration order matches the member
/// declaration order in SdkProvider (reverse-destruction semantics).
struct SdkProviderArgs
{
    std::unique_ptr<internal::IOtlpEncoder> encoder;
    std::unique_ptr<internal::IAuthProvider> auth;
    std::unique_ptr<internal::ITransport> transport;
    std::unique_ptr<internal::IWireCodec> codec;
    std::unique_ptr<internal::IExporter> exporter;
    std::unique_ptr<internal::ISpanProcessor> processor;
    std::shared_ptr<const Resource> resource;
    SamplerHandle sampler;
    SpanLimitOptions span_limits;
    internal::ConnectOptions connect_opts;
    /// @brief Optional wire codec for the metrics endpoint. Must outlive
    /// `metric_exporter` (i.e. declared before it in this struct so the
    /// SdkProvider member initializer list initializes it first).
    std::unique_ptr<internal::IWireCodec> metric_codec;
    /// @brief Optional metrics export pipeline. When non-null, a
    /// `PeriodicExportingMetricReader` is created on the first `GetMeter()` call.
    std::unique_ptr<internal::IMetricExporter> metric_exporter;
    /// @brief Background export interval for the metrics reader (default 30 s).
    std::chrono::milliseconds metric_interval{30'000};
};

/// @brief Production `Provider` wiring the full export pipeline.
///
/// Owns the pipeline end-to-end: encoder → transport → codec → exporter →
/// processor. Members are declared so that reverse-destruction (i.e. the
/// compiler-generated destructor) tears down the processor first, then the
/// exporter, then the codec and transport, preserving the happens-before chain
/// required by TSAN and the threading model (interfaces.md §6).
///
/// @threadsafety Thread-safe. All methods may be called from any thread.
class SdkProvider final : public microtel::Provider
{
public:
    explicit SdkProvider(SdkProviderArgs args) noexcept;

    ~SdkProvider() noexcept override;

    SdkProvider(const SdkProvider&) = delete;
    SdkProvider& operator=(const SdkProvider&) = delete;
    SdkProvider(SdkProvider&&) = delete;
    SdkProvider& operator=(SdkProvider&&) = delete;

    [[nodiscard]] std::shared_ptr<Tracer> GetTracer(std::string_view name,
                                                    std::string_view version = {}) override;

    [[nodiscard]] Expected<void, Error> Connect() override;

    [[nodiscard]] Status ForceFlush(std::chrono::milliseconds timeout) noexcept override;

    [[nodiscard]] Status Shutdown(std::chrono::milliseconds timeout) noexcept override;

    [[nodiscard]] HealthSnapshot GetExporterHealth() const noexcept override;

    /// @brief Acquire (or create) the `Meter` for one instrumentation scope.
    ///
    /// Lazily initialises the shared `MetricProducer` on the first call.
    /// Subsequent calls with the same `(name, version)` return the cached
    /// `Meter` instance. `schema_url` is stored in the scope but does not
    /// affect caching in v1 (deferred to M12-hardening).
    [[nodiscard]] std::shared_ptr<Meter> GetMeter(std::string_view name,
                                                  std::string_view version = {},
                                                  std::string_view schema_url = {});

private:
    // Declared first → destroyed last. Encoder is stateless; no teardown order concern.
    std::unique_ptr<internal::IOtlpEncoder> m_encoder;
    std::unique_ptr<internal::IAuthProvider> m_auth;
    // Transport owns the I/O thread; must outlive all codecs and exporters.
    std::unique_ptr<internal::ITransport> m_transport;
    std::unique_ptr<internal::IWireCodec> m_codec;
    // Metric codec must outlive m_metric_exporter (which holds a raw pointer to it).
    std::unique_ptr<internal::IWireCodec> m_metric_codec;
    // Trace exporter thread; must outlive codec and transport.
    std::unique_ptr<internal::IExporter> m_exporter;
    // Metric exporter thread; must outlive m_metric_reader.
    std::unique_ptr<internal::IMetricExporter> m_metric_exporter;
    std::chrono::milliseconds m_metric_interval;
    // BSP thread — destroyed before trace exporter.
    std::unique_ptr<internal::ISpanProcessor> m_processor;
    // Metric reader thread — declared last → destroyed first (before metric exporter).
    std::unique_ptr<PeriodicExportingMetricReader> m_metric_reader;

    std::shared_ptr<const Resource> m_resource;
    SamplerHandle m_sampler;
    SpanLimitOptions m_span_limits;
    internal::ConnectOptions m_connect_opts;

    // Metrics pipeline: lazily initialised on first GetMeter() call.
    std::mutex m_meter_mu;
    std::shared_ptr<MetricProducer> m_metric_producer;
    std::unordered_map<std::string, std::shared_ptr<Meter>> m_meters;
};

}  // namespace microtel::sdk
