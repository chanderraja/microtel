// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/internal/auth_provider.hpp"
#include "microtel/internal/diagnostics_sink.hpp"
#include "microtel/internal/exporter.hpp"
#include "microtel/internal/log_exporter.hpp"
#include "microtel/internal/log_record_processor.hpp"
#include "microtel/internal/metric_exporter.hpp"
#include "microtel/internal/otlp_encoder.hpp"
#include "microtel/internal/processor.hpp"
#include "microtel/internal/transport.hpp"
#include "microtel/internal/wire_codec.hpp"
#include "microtel/provider.hpp"
#include "microtel/resource.hpp"
#include "microtel/sampler.hpp"
#include "microtel/sdk_builder.hpp"

#include "sdk/diagnostics_counters.hpp"
#include "sdk/metric_attribute_set.hpp"
#include "sdk/view_registry.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

namespace microtel::sdk
{

// Forward-declared to keep implementation headers out of this header's transitive closure.
class SdkMeter;
class MetricProducer;
class PeriodicExportingMetricReader;

/// @brief All owned objects passed to SdkProvider at construction.
///
/// Bundles the ten pipeline components so the constructor stays within the
/// seven-parameter tidy threshold. Declaration order matches the member
/// declaration order in SdkProvider (reverse-destruction semantics).
struct SdkProviderArgs
{
    /// @brief Diagnostics sink backing `GetExporterHealth()`. Created by
    /// `SdkBuilder::Build` before the exporters, which borrow it. Must be
    /// non-null.
    std::unique_ptr<DiagnosticsCounters> diagnostics;
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
    /// @brief Aggregation temporality preference for the metrics reader.
    microtel::TemporalityPreference metric_temporality{microtel::TemporalityPreference::Cumulative};
    /// @brief Per-instrument cardinality cap forwarded to every SdkMeter.
    std::size_t metric_max_cardinality{kDefaultMaxCardinality};
    /// @brief View configurations registered via `SdkBuilder::WithView()`.
    ViewRegistry view_registry;
    /// @brief Optional wire codec for the logs endpoint. Must outlive
    /// `log_exporter`.
    std::unique_ptr<internal::IWireCodec> log_codec;
    /// @brief Optional logs export pipeline. When non-null, the first
    /// `GetLogger()` builds a `BatchLogRecordProcessor` around it; when null,
    /// `GetLogger()` returns a no-op logger.
    std::unique_ptr<internal::ILogExporter> log_exporter;
    /// @brief Batch options for the log record processor (default BSP knobs).
    BatchOptions log_batch_opts;
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
    /// instance. `schema_url` is stored in the scope but does not affect
    /// caching in v1 (deferred to M12-hardening).
    [[nodiscard]] std::shared_ptr<microtel::Meter> GetMeter(
        std::string_view name,
        std::string_view version = {},
        std::string_view schema_url = {}) override;

    /// @brief Acquire (or create) the `Logger` for one instrumentation scope.
    ///
    /// Lazily builds the log pipeline (a `BatchLogRecordProcessor` around the
    /// configured log exporter) on the first call, and caches an `SdkLogger`
    /// per `(name, version)`. Returns a shared no-op logger when no log
    /// exporter is configured.
    [[nodiscard]] std::shared_ptr<microtel::Logger> GetLogger(
        std::string_view name, std::string_view version = {}) override;

    /// @brief Borrow the provider-owned diagnostics sink.
    ///
    /// Non-owning reference, valid for the provider's lifetime. The seam
    /// through which pipeline components record drops (wired in increment 26)
    /// and tests assert on health counters.
    [[nodiscard]] internal::IDiagnosticsSink& DiagnosticsSink() noexcept;

    /// @brief Mark this provider dead after `fork()`, from the child.
    ///
    /// Called by the `pthread_atfork` child handler. Only the forking thread
    /// survives `fork`, so every worker and I/O thread this provider owns is
    /// gone in the child while their mutexes may still be held by threads that
    /// no longer exist. Flipping the shutdown flag makes the API entry points
    /// that check it drop instead of touching that state
    /// (`docs/threading-model.md` §7).
    ///
    /// @note Runs in the child of `fork()`, where only async-signal-safe
    ///       operations are permitted: this does nothing but a relaxed-release
    ///       store to an atomic. It takes no lock and allocates nothing.
    void MarkForkedChild() noexcept;

private:
    /// @brief Borrowed pointer to the lazily-built metric reader, read under
    ///        `m_meter_mu`. Returns nullptr when no meter has been created.
    /// @note Exists so `ForceFlush`/`Shutdown` can observe the pointer without
    ///       holding the mutex across the call into the reader — taking
    ///       `m_meter_mu` and then the reader's own lock would nest two
    ///       non-leaf locks, which `docs/threading-model.md` §4 forbids.
    [[nodiscard]] PeriodicExportingMetricReader* MetricReaderPtr() noexcept;
    /// @brief Borrowed pointer to the lazily-built log processor, read under
    ///        `m_logger_mu`. Same rationale as `MetricReaderPtr`.
    [[nodiscard]] internal::ILogRecordProcessor* LogProcessorPtr() noexcept;

public:
private:
    // Declared before every other member → destroyed last, so the sink stays
    // alive past any late RecordDrop from the metric machinery (or any other
    // pipeline component) during reverse-order teardown — same reasoning as
    // the m_metric_codec ordering note below.
    //
    // Heap-allocated rather than held by value because the eagerly-built
    // exporters need a pointer to it at *their* construction time, which is
    // before this Provider exists. SdkBuilder::Build creates it, hands the raw
    // pointer to the exporters, and moves ownership in here. DiagnosticsCounters
    // is neither copyable nor movable (it holds atomics), so a value member
    // could not be transferred in.
    std::unique_ptr<DiagnosticsCounters> m_diagnostics;
    /// Set by `Shutdown` before it tears anything down, so `GetMeter` and
    /// `GetLogger` stop building pipeline components afterwards. Without it
    /// either could construct a `PeriodicExportingMetricReader` or a
    /// `BatchLogRecordProcessor` — and spawn its thread — *after* `Shutdown`
    /// returned, contradicting `docs/threading-model.md` §6.2 and leaving the
    /// new thread joined only at destruction.
    ///
    /// Atomic because `Provider` is documented thread-safe: the check races
    /// a concurrent `Shutdown` on another thread. It narrows the window to
    /// the same one every other component here has (see `OtlpExporter::
    /// m_shutdown`); it does not eliminate it.
    std::atomic<bool> m_shut_down{false};
    // Encoder is stateless; no teardown order concern.
    std::unique_ptr<internal::IOtlpEncoder> m_encoder;
    std::unique_ptr<internal::IAuthProvider> m_auth;
    // Transport owns the I/O thread; must outlive all codecs and exporters.
    std::unique_ptr<internal::ITransport> m_transport;
    std::unique_ptr<internal::IWireCodec> m_codec;
    // Metric codec must outlive m_metric_exporter (which holds a raw pointer to it).
    std::unique_ptr<internal::IWireCodec> m_metric_codec;
    // Log codec must outlive m_log_exporter (which holds a raw pointer to it).
    std::unique_ptr<internal::IWireCodec> m_log_codec;
    // Trace exporter thread; must outlive codec and transport.
    std::unique_ptr<internal::IExporter> m_exporter;
    // Metric exporter thread; must outlive m_metric_reader.
    std::unique_ptr<internal::IMetricExporter> m_metric_exporter;
    // Log exporter thread; must outlive m_log_processor.
    std::unique_ptr<internal::ILogExporter> m_log_exporter;
    std::chrono::milliseconds m_metric_interval;
    microtel::TemporalityPreference m_metric_temporality;
    std::size_t m_metric_max_cardinality;
    BatchOptions m_log_batch_opts;
    // BSP thread — destroyed before trace exporter.
    std::unique_ptr<internal::ISpanProcessor> m_processor;
    // Metric reader thread — declared last → destroyed first (before metric exporter).
    std::unique_ptr<PeriodicExportingMetricReader> m_metric_reader;
    // Log processor thread — lazily created; destroyed before m_log_exporter.
    std::unique_ptr<internal::ILogRecordProcessor> m_log_processor;

    std::shared_ptr<const Resource> m_resource;
    SamplerHandle m_sampler;
    SpanLimitOptions m_span_limits;
    internal::ConnectOptions m_connect_opts;

    std::shared_ptr<ViewRegistry> m_view_registry;

    // Metrics pipeline: lazily initialised on first GetMeter() call.
    std::mutex m_meter_mu;
    std::shared_ptr<MetricProducer> m_metric_producer;
    std::unordered_map<std::string, std::shared_ptr<SdkMeter>> m_meters;

    // Logs pipeline: m_log_processor is lazily initialised on first GetLogger().
    std::mutex m_logger_mu;
    std::unordered_map<std::string, std::shared_ptr<microtel::Logger>> m_loggers;
    std::shared_ptr<microtel::Logger> m_noop_logger;
};

}  // namespace microtel::sdk
