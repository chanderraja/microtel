// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#include "backend.hpp"

#include <opentelemetry/exporters/otlp/otlp_grpc_exporter_factory.h>
#include <opentelemetry/exporters/otlp/otlp_grpc_exporter_options.h>
#include <opentelemetry/exporters/otlp/otlp_http_exporter_factory.h>
#include <opentelemetry/exporters/otlp/otlp_http_exporter_options.h>
#include <opentelemetry/sdk/trace/batch_span_processor_factory.h>
#include <opentelemetry/sdk/trace/batch_span_processor_options.h>
#include <opentelemetry/sdk/trace/tracer_provider_factory.h>
#include <opentelemetry/trace/provider.h>
#include <opentelemetry/trace/tracer.h>

#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace bench
{

namespace
{

namespace otlp = opentelemetry::exporter::otlp;
namespace sdktrace = opentelemetry::sdk::trace;
namespace trace_api = opentelemetry::trace;

class OtelCppBackend final : public IBackend
{
public:
    OtelCppBackend() = default;

    OtelCppBackend(const OtelCppBackend&) = delete;
    OtelCppBackend& operator=(const OtelCppBackend&) = delete;
    OtelCppBackend(OtelCppBackend&&) = delete;
    OtelCppBackend& operator=(OtelCppBackend&&) = delete;

    ~OtelCppBackend() override = default;

    void Init(const BackendOptions& opts) override
    {
        std::unique_ptr<opentelemetry::sdk::trace::SpanExporter> exporter;

#if defined(BENCH_BACKEND_OTELCPP_GRPC)
        otlp::OtlpGrpcExporterOptions grpc_opts;
        grpc_opts.endpoint = opts.endpoint;
        grpc_opts.use_ssl_credentials = false;
        exporter = otlp::OtlpGrpcExporterFactory::Create(grpc_opts);
#elif defined(BENCH_BACKEND_OTELCPP_HTTP)
        otlp::OtlpHttpExporterOptions http_opts;
        http_opts.url = opts.endpoint + "/v1/traces";
        exporter = otlp::OtlpHttpExporterFactory::Create(http_opts);
#else
        static_assert(false, "BENCH_BACKEND_OTELCPP_GRPC or BENCH_BACKEND_OTELCPP_HTTP required");
#endif

        if (!exporter)
        {
            throw std::runtime_error("opentelemetry-cpp exporter creation failed");
        }

        sdktrace::BatchSpanProcessorOptions proc_opts;
        auto processor = sdktrace::BatchSpanProcessorFactory::Create(std::move(exporter),
                                                                     proc_opts);

        auto provider = sdktrace::TracerProviderFactory::Create(std::move(processor));
        m_provider = std::move(provider);
        m_tracer = m_provider->GetTracer(opts.service_name, opts.service_version);

        m_attrs_per_span = opts.attributes_per_span;
        m_attr_keys.resize(opts.attributes_per_span);
        for (int i = 0; i < opts.attributes_per_span; ++i)
        {
            m_attr_keys[i] = "bench.attr." + std::to_string(i);
        }
        m_attr_value = std::string(
            static_cast<std::size_t>(opts.attribute_value_bytes), 'x');
    }

    void EmitSpan() override
    {
        auto span = m_tracer->StartSpan("bench.span");
        for (int i = 0; i < m_attrs_per_span; ++i)
        {
            span->SetAttribute(
                opentelemetry::nostd::string_view(m_attr_keys[i]),
                opentelemetry::nostd::string_view(m_attr_value));
        }
        span->End();
        m_emit_count++;
    }

    void Shutdown() override
    {
        m_provider->ForceFlush();
        m_provider->Shutdown();
    }

    [[nodiscard]] BackendStats Stats() const override
    {
        // opentelemetry-cpp does not expose drop counters via a stable public API.
        // All DroppedCounts fields are 0.  See bench/docs/methodology.md.
        return BackendStats{
            .spans_exported_total = m_emit_count,
            .spans_dropped        = {},
            .bytes_sent_total     = 0,
        };
    }

private:
    std::shared_ptr<opentelemetry::trace::TracerProvider>          m_provider;
    opentelemetry::nostd::shared_ptr<opentelemetry::trace::Tracer> m_tracer;
    uint64_t                                                        m_emit_count{0};
    int                                                             m_attrs_per_span{0};
    std::vector<std::string>                                        m_attr_keys;
    std::string                                                     m_attr_value;
};

}  // namespace

IBackend* CreateBackend()
{
    return new OtelCppBackend();  // NOLINT(cppcoreguidelines-owning-memory)
}

}  // namespace bench
