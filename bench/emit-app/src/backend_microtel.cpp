// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#include "backend.hpp"

#include "microtel/provider.hpp"
#include "microtel/sdk_builder.hpp"

#include <atomic>
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
        auto result = microtel::SdkBuilder{}
                          .WithEndpoint(opts.endpoint)
                          .WithProtocol(microtel::Protocol::Http)
                          .WithServiceName(opts.service_name)
                          .WithServiceVersion(opts.service_version)
                          .WithCompressionGzip(opts.compression_gzip)
                          .Build();

        if (!result)
        {
            throw std::runtime_error("microtel::SdkBuilder::Build() failed: " +
                                     result.error().message);
        }

        m_provider = std::move(*result);
        m_tracer = m_provider->GetTracer("bench");

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
            span->SetAttribute(m_attr_keys[i], m_attr_value);
        }
        span->End();
        m_emit_count.fetch_add(1, std::memory_order_relaxed);
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
    std::shared_ptr<microtel::Provider> m_provider;
    std::shared_ptr<microtel::Tracer>   m_tracer;
    std::atomic<uint64_t>               m_emit_count{0};
    int                                 m_attrs_per_span{0};
    std::vector<std::string>            m_attr_keys;
    std::string                         m_attr_value;
};

}  // namespace

IBackend* CreateBackend()
{
    return new MicrotelBackend();  // NOLINT(cppcoreguidelines-owning-memory)
}

}  // namespace bench
