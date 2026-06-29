// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#include "backend.hpp"

#include "microtel/meter.hpp"
#include "microtel/provider.hpp"
#include "microtel/sdk_builder.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <numeric>
#include <stdexcept>
#include <string>
#include <vector>

namespace bench
{

namespace
{

class MicrotelBackend final : public IBackend
{
public:
    MicrotelBackend() = default;

    MicrotelBackend(const MicrotelBackend&) = delete;
    MicrotelBackend& operator=(const MicrotelBackend&) = delete;
    MicrotelBackend(MicrotelBackend&&) = delete;
    MicrotelBackend& operator=(MicrotelBackend&&) = delete;

    ~MicrotelBackend() override = default;

    void Init(const BackendOptions& opts) override
    {
        const auto metric_interval =
            opts.metric_interval_ms > 0
                ? std::chrono::milliseconds(opts.metric_interval_ms)
                : std::chrono::milliseconds(60'000);

        auto result = microtel::SdkBuilder{}
                          .WithEndpoint(opts.endpoint)
#if defined(BENCH_MICROTEL_GRPC)
                          .WithProtocol(microtel::Protocol::Grpc)
#else
                          .WithProtocol(microtel::Protocol::Http)
#endif
                          .WithServiceName(opts.service_name)
                          .WithServiceVersion(opts.service_version)
                          .WithCompressionGzip(opts.compression_gzip)
                          .WithMetricInterval(metric_interval)
                          .Build();

        if (!result)
        {
            throw std::runtime_error("microtel::SdkBuilder::Build() failed: " +
                                     result.error().message);
        }

        m_provider = std::move(*result);

        if (auto conn = m_provider->Connect(); !conn)
        {
            throw std::runtime_error("microtel::Provider::Connect() failed: " +
                                     conn.error().message);
        }

        m_tracer = m_provider->GetTracer("bench");

        auto meter = m_provider->GetMeter("bench");
        m_counter   = meter->CreateCounter<int64_t>("bench.records",
                                                    "Records emitted", "{record}");
        m_histogram = meter->CreateHistogram<double>("bench.record_latency_ns",
                                                     "Record hot-path latency", "ns");

        m_attrs_per_span = opts.attributes_per_span;
        m_attr_keys.resize(opts.attributes_per_span);
        for (int i = 0; i < opts.attributes_per_span; ++i)
        {
            m_attr_keys[i] = "bench.attr." + std::to_string(i);
        }
        m_attr_value = std::string(
            static_cast<std::size_t>(opts.attribute_value_bytes), 'x');
    }

    void EmitRecord() override
    {
        m_counter->Add(1, {});
        m_histogram->Record(1.0, {});
        m_emit_count.fetch_add(1, std::memory_order_relaxed);
    }

    void EmitSpan() override
    {
        auto span = m_tracer->StartSpan("bench.span");
        for (int i = 0; i < m_attrs_per_span; ++i)
        {
            span->SetAttribute(m_attr_keys[i], m_attr_value);
        }
        span->End();
        m_emit_count.fetch_add(1, std::memory_order_relaxed);
    }

    void EmitRequest() override
    {
        auto parent = m_tracer->StartSpan("bench.request");
        auto child1 = m_tracer->StartSpan("bench.request.op1",
                                          {.parent = parent->GetContext()});
        child1->End();
        auto child2 = m_tracer->StartSpan("bench.request.op2",
                                          {.parent = parent->GetContext()});
        child2->End();
        parent->End();
        m_emit_count.fetch_add(1, std::memory_order_relaxed);
    }

    [[nodiscard]] uint64_t ForceFlush() override
    {
        using Clock = std::chrono::steady_clock;
        const auto t0 = Clock::now();
        m_provider->ForceFlush(std::chrono::milliseconds(30'000));
        const auto t1 = Clock::now();
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
    }

    void Shutdown() override
    {
        m_provider->Shutdown(std::chrono::seconds(30));
    }

    [[nodiscard]] BackendStats Stats() const override
    {
        const auto health = m_provider->GetExporterHealth();

        auto dc = [&health](microtel::DropReason r) -> uint64_t {
            return health.drop_counters[static_cast<std::size_t>(r)];
        };

        DroppedCounts dropped;
        dropped.queue_full             = dc(microtel::DropReason::QueueFull);
        dropped.record_too_large       = dc(microtel::DropReason::RecordTooLarge);
        dropped.span_attribute_limit   = dc(microtel::DropReason::SpanAttributeLimit);
        dropped.attribute_value_truncated = dc(microtel::DropReason::AttributeValueTruncated);
        dropped.total                  = std::accumulate(health.drop_counters.begin(),
                                                         health.drop_counters.end(),
                                                         uint64_t{0});
        dropped.other = dropped.total - dropped.queue_full - dropped.record_too_large
                        - dropped.span_attribute_limit - dropped.attribute_value_truncated;

        const uint64_t emitted = m_emit_count.load(std::memory_order_relaxed);
        const uint64_t exported = emitted > dropped.total ? emitted - dropped.total : 0;

        return BackendStats{
            .spans_exported_total = exported,
            .spans_dropped        = dropped,
            .bytes_sent_total     = 0,  // not exposed by HealthSnapshot; driver reads from sink
        };
    }

private:
    std::shared_ptr<microtel::Provider>          m_provider;
    std::shared_ptr<microtel::Tracer>            m_tracer;
    std::shared_ptr<microtel::Counter<int64_t>>  m_counter;
    std::shared_ptr<microtel::Histogram<double>> m_histogram;
    std::atomic<uint64_t>                        m_emit_count{0};
    int                                          m_attrs_per_span{0};
    std::vector<std::string>                     m_attr_keys;
    std::string                                  m_attr_value;
};

}  // namespace

IBackend* CreateBackend()
{
    return new MicrotelBackend();  // NOLINT(cppcoreguidelines-owning-memory)
}

}  // namespace bench
