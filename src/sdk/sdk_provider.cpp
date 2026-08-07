// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#include "sdk/sdk_provider.hpp"

#include "microtel/internal/batch.hpp"
#include "microtel/internal/metric_batch.hpp"
#include "microtel/provider.hpp"
#include "microtel/status.hpp"
#include "microtel/tracer.hpp"

#include "sdk/batch_log_record_processor.hpp"
#include "sdk/metric_producer.hpp"
#include "sdk/noop_logger.hpp"
#include "sdk/periodic_exporting_metric_reader.hpp"
#include "sdk/sdk_logger.hpp"
#include "sdk/sdk_meter.hpp"
#include "sdk/sdk_tracer.hpp"

#include <chrono>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace microtel::sdk
{

namespace
{

constexpr auto kProviderDestructorTimeout = std::chrono::milliseconds(5000);

[[nodiscard]] internal::AggregationTemporality ToAggregationTemporality(
    microtel::TemporalityPreference pref) noexcept
{
    switch (pref)
    {
        case microtel::TemporalityPreference::Delta:
        case microtel::TemporalityPreference::LowMemory:
            // LowMemory uses delta for all instruments; per-kind mapping deferred to v1.3.
            return internal::AggregationTemporality::Delta;
        case microtel::TemporalityPreference::Cumulative:
        default:
            return internal::AggregationTemporality::Cumulative;
    }
}

}  // namespace

SdkProvider::SdkProvider(SdkProviderArgs args) noexcept
    : m_encoder(std::move(args.encoder)),
      m_auth(std::move(args.auth)),
      m_transport(std::move(args.transport)),
      m_codec(std::move(args.codec)),
      m_metric_codec(std::move(args.metric_codec)),
      m_log_codec(std::move(args.log_codec)),
      m_exporter(std::move(args.exporter)),
      m_metric_exporter(std::move(args.metric_exporter)),
      m_log_exporter(std::move(args.log_exporter)),
      m_metric_interval(args.metric_interval),
      m_metric_temporality(args.metric_temporality),
      m_metric_max_cardinality(args.metric_max_cardinality),
      m_log_batch_opts(args.log_batch_opts),
      m_processor(std::move(args.processor)),
      m_resource(std::move(args.resource)),
      m_sampler(std::move(args.sampler)),
      m_span_limits(args.span_limits),
      m_connect_opts(std::move(args.connect_opts)),
      m_view_registry(std::make_shared<ViewRegistry>(std::move(args.view_registry))),
      m_noop_logger(std::make_shared<NoopLogger>())
{
}

SdkProvider::~SdkProvider() noexcept
{
    (void)Shutdown(kProviderDestructorTimeout);
}

std::shared_ptr<Tracer> SdkProvider::GetTracer(std::string_view name, std::string_view version)
{
    return std::make_shared<SdkTracer>(
        m_sampler.Get(),
        m_processor.get(),
        m_resource,
        internal::InstrumentationScope{.name = std::string{name}, .version = std::string{version}},
        m_span_limits);
}

Expected<void, Error> SdkProvider::Connect()
{
    return m_transport->Connect(m_connect_opts);
}

Status SdkProvider::ForceFlush(std::chrono::milliseconds timeout) noexcept
{
    // Two-stage flush: drain the BSP queue into the exporter queue first,
    // then drain the exporter queue (actual HTTP sends). Both are async
    // workers; flushing only the processor leaves batches undelivered.
    const Status s = m_processor->ForceFlush(timeout);
    if (s != Status::Completed)
    {
        return s;
    }
    const Status s2 = m_exporter->ForceFlush(timeout);
    if (s2 != Status::Completed)
    {
        return s2;
    }
    // Metric reader ForceFlush: collect a snapshot then flush the exporter.
    if (m_metric_reader != nullptr)
    {
        const Status ms = m_metric_reader->ForceFlush(timeout);
        if (ms != Status::Completed)
        {
            return ms;
        }
    }
    // Log pipeline: drain the processor queue into the exporter, then flush it.
    if (m_log_processor != nullptr)
    {
        const Status ls = m_log_processor->ForceFlush(timeout);
        if (ls != Status::Completed)
        {
            return ls;
        }
    }
    if (m_log_exporter != nullptr)
    {
        return m_log_exporter->ForceFlush(timeout);
    }
    return Status::Completed;
}

Status SdkProvider::Shutdown(std::chrono::milliseconds timeout) noexcept
{
    const Status status = m_processor->Shutdown(timeout);
    // Metric reader shutdown (also shuts down the metric exporter internally).
    if (m_metric_reader != nullptr)
    {
        (void)m_metric_reader->Shutdown(timeout);
    }
    else if (m_metric_exporter != nullptr)
    {
        (void)m_metric_exporter->Shutdown(timeout);
    }
    // Log pipeline: stop the processor (halts emits to the exporter), then the
    // exporter, before the shared transport is closed.
    if (m_log_processor != nullptr)
    {
        (void)m_log_processor->Shutdown(timeout);
    }
    if (m_log_exporter != nullptr)
    {
        (void)m_log_exporter->Shutdown(timeout);
    }
    (void)m_exporter->Shutdown(timeout);
    (void)m_transport->Close(timeout);
    return status;
}

HealthSnapshot SdkProvider::GetExporterHealth() const noexcept
{
    HealthSnapshot health = m_diagnostics.Snapshot();
    // Connection state is read live from the transport; the sink's
    // SetConnectionState channel is wired up in increment 26.
    health.connection_state = m_transport->GetState();
    return health;
}

internal::IDiagnosticsSink& SdkProvider::DiagnosticsSink() noexcept
{
    return m_diagnostics;
}

std::shared_ptr<microtel::Meter> SdkProvider::GetMeter(std::string_view name,
                                                       std::string_view version,
                                                       std::string_view /*schema_url*/)
{
    const std::scoped_lock lk{m_meter_mu};
    if (!m_metric_producer)
    {
        m_metric_producer = std::make_shared<MetricProducer>(m_resource);
        if (m_metric_exporter != nullptr)
        {
            m_metric_reader = std::make_unique<PeriodicExportingMetricReader>(
                *m_metric_producer,
                *m_metric_exporter,
                m_metric_interval,
                ToAggregationTemporality(m_metric_temporality));
        }
    }
    std::string key;
    key.reserve(name.size() + 1 + version.size());
    key.append(name);
    key += '\0';
    key.append(version);
    auto& entry = m_meters[key];
    if (!entry)
    {
        entry = std::make_shared<SdkMeter>(
            internal::InstrumentationScope{.name = std::string{name},
                                           .version = std::string{version}},
            m_metric_producer,
            m_metric_max_cardinality,
            &m_diagnostics,
            m_view_registry);
    }
    return entry;
}

std::shared_ptr<microtel::Logger> SdkProvider::GetLogger(std::string_view name,
                                                         std::string_view version)
{
    if (m_log_exporter == nullptr)
    {
        return m_noop_logger;
    }
    const std::scoped_lock lk{m_logger_mu};
    if (!m_log_processor)
    {
        m_log_processor = std::make_unique<BatchLogRecordProcessor>(
            m_log_exporter.get(), m_resource, m_log_batch_opts);
    }
    std::string key;
    key.reserve(name.size() + 1 + version.size());
    key.append(name);
    key += '\0';
    key.append(version);
    auto& entry = m_loggers[key];
    if (!entry)
    {
        entry = std::make_shared<SdkLogger>(
            m_log_processor.get(),
            internal::InstrumentationScope{.name = std::string{name},
                                           .version = std::string{version}},
            nullptr,  // ICurrentSpanSource — trace-correlation seam, wired later
            &m_diagnostics,
            LogLimitOptions{});
    }
    return entry;
}

}  // namespace microtel::sdk
