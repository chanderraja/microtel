// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#include "backend.hpp"

#include "microtel/logger.hpp"
#include "microtel/meter.hpp"
#include "microtel/provider.hpp"
#include "microtel/sdk_builder.hpp"

#if defined(BENCH_LEAF_FANIN)
#include "microtel/leaf.h"
#include "microtel/leaf_receiver.hpp"
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <numeric>
#include <optional>
#include <random>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace bench
{

namespace
{

constexpr std::string_view kLogBody = "bench log record";
constexpr std::string_view kSeverityText = "INFO";

#if defined(BENCH_LEAF_FANIN)
// Record buffer for one simulated leaf's payload: generous, since the leaf is
// only a payload generator here and runs once per leaf at start-up.
constexpr std::size_t kLeafRecordBytes = 256U * 1024U;
constexpr std::uint64_t kLeafTickNs = 1000;
constexpr std::uint32_t kMinLeafTable = 1024;

std::uint64_t LeafTick(void* ctx)
{
    auto* const now = static_cast<std::uint64_t*>(ctx);
    *now += kLeafTickNs;
    return *now;
}

void LeafRandom(void* ctx, std::uint8_t* out, std::size_t len)
{
    auto* const engine = static_cast<std::mt19937_64*>(ctx);
    for (std::size_t i = 0; i < len; ++i)
    {
        out[i] = static_cast<std::uint8_t>((*engine)());
    }
}

microtel_leaf_kv_t LeafKv(std::string_view key, std::int64_t value)
{
    microtel_leaf_kv_t kv{};
    kv.key = key.data();
    kv.key_len = key.size();
    kv.type = MICROTEL_LEAF_VALUE_INT64;
    kv.value.i = value;
    return kv;
}

/// One payload as simulated leaf @p index would send it: @p spans spans with
/// one attribute each, encoded by the real C leaf library (concentrator-
/// stamped time). Throws on a leaf error, which is a harness bug.
std::vector<std::byte> EncodeLeafPayload(int index, int spans)
{
    constexpr std::string_view kService = "bench-leaf";
    constexpr std::string_view kSpan = "bench.leaf.span";
    constexpr std::string_view kServiceKey = "service.name";
    const microtel_leaf_kv_t service{.key = kServiceKey.data(),
                                     .key_len = kServiceKey.size(),
                                     .type = MICROTEL_LEAF_VALUE_STRING,
                                     .value = {.s = {.ptr = kService.data(), .len = kService.size()}}};
    const std::array<microtel_leaf_kv_t, 2> resource{service, LeafKv("leaf.index", index)};
    std::uint64_t clock = 0;
    std::mt19937_64 engine(static_cast<std::uint64_t>(index) + 1U);
    microtel_leaf_config_t config{};
    config.struct_size = sizeof(config);
    config.now_ns = &LeafTick;
    config.clock_ctx = &clock;
    config.random_bytes = &LeafRandom;
    config.random_ctx = &engine;
    config.resource = resource.data();
    config.resource_count = resource.size();
    config.max_attributes_per_span = 1;

    microtel_leaf_t leaf{};
    std::vector<std::uint8_t> records(kLeafRecordBytes);
    if (microtel_leaf_init(&leaf, sizeof(leaf), &config, records.data(), records.size()) !=
        MICROTEL_LEAF_OK)
    {
        throw std::runtime_error("microtel_leaf_init failed");
    }
    const microtel_leaf_kv_t attr = LeafKv("bench.seq", 0);
    for (int i = 0; i < spans; ++i)
    {
        microtel_leaf_span_t span = 0;
        microtel_leaf_span_start(&leaf, &span, kSpan.data(), kSpan.size(),
                                 MICROTEL_LEAF_SPAN_KIND_INTERNAL, nullptr);
        microtel_leaf_span_set_attribute(&leaf, span, &attr);
        microtel_leaf_span_end(&leaf, span);
    }
    std::vector<std::uint8_t> out(microtel_leaf_encoded_size(&leaf));
    std::size_t written = 0;
    const microtel_leaf_status_t st = microtel_leaf_encode(&leaf, out.data(), out.size(), &written);
    microtel_leaf_free(&leaf);
    if (st != MICROTEL_LEAF_OK)
    {
        throw std::runtime_error("microtel_leaf_encode failed: " + std::to_string(st));
    }
    const auto* const first = reinterpret_cast<const std::byte*>(out.data());
    return {first, first + written};
}
#endif

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

        microtel::SdkBuilder builder;
        builder.WithEndpoint(opts.endpoint)
#if defined(BENCH_MICROTEL_GRPC)
            .WithProtocol(microtel::Protocol::Grpc)
#else
            .WithProtocol(microtel::Protocol::Http)
#endif
            .WithServiceName(opts.service_name)
            .WithServiceVersion(opts.service_version)
            .WithCompressionGzip(opts.compression_gzip)
            .WithMetricInterval(metric_interval);
#if defined(BENCH_LEAF_FANIN)
        if (opts.leaf_count > 0)
        {
            microtel::LeafReceiverOptions leaf_opts;
            leaf_opts.max_leaves =
                std::max(kMinLeafTable, static_cast<std::uint32_t>(opts.leaf_count));
            builder.WithLeafReceiver(std::move(leaf_opts));
        }
#endif
        auto result = builder.Build();

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
        m_logger = m_provider->GetLogger("bench");

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

#if defined(BENCH_LEAF_FANIN)
        if (opts.leaf_count > 0)
        {
            m_receiver = m_provider->GetLeafReceiver();
            m_leaf_spans = static_cast<std::uint64_t>(opts.leaf_spans_per_payload);
            for (int i = 0; i < opts.leaf_count; ++i)
            {
                m_leaf_ids.push_back("bench-leaf-" + std::to_string(i));
                m_leaf_payloads.push_back(EncodeLeafPayload(i, opts.leaf_spans_per_payload));
            }
        }
#endif
    }

#if defined(BENCH_LEAF_FANIN)
    void EmitLeafPayload() override
    {
        const std::size_t i =
            m_next_leaf.fetch_add(1, std::memory_order_relaxed) % m_leaf_payloads.size();
        const microtel::IngestResult r = m_receiver->Ingest(microtel::IngestRequest{
            .leaf_id = m_leaf_ids[i],
            .payload = std::span<const std::byte>(m_leaf_payloads[i]),
            .received_at = std::nullopt,
        });
        static_cast<void>(r);
        m_emit_count.fetch_add(m_leaf_spans, std::memory_order_relaxed);
    }
#endif

    void EmitRecord() override
    {
        m_counter->Add(1, {});
        m_histogram->Record(1.0, {});
        m_emit_count.fetch_add(1, std::memory_order_relaxed);
    }

    void EmitLog() override
    {
        // Minimal record with the same wire content the otel-cpp backend
        // sends: severity INFO (otel-cpp's OTLP recordable always fills
        // severity_text from the number) and a short string body. The SDK
        // stamps observed_time.
        m_logger->Emit(microtel::LogRecord{
            .severity_number = microtel::SeverityNumber::Info,
            .severity_text = std::string{kSeverityText},
            .body = std::string{kLogBody},
        });
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
    std::shared_ptr<microtel::Logger>            m_logger;
    std::shared_ptr<microtel::Counter<int64_t>>  m_counter;
    std::shared_ptr<microtel::Histogram<double>> m_histogram;
    std::atomic<uint64_t>                        m_emit_count{0};
    int                                          m_attrs_per_span{0};
    std::vector<std::string>                     m_attr_keys;
    std::string                                  m_attr_value;
#if defined(BENCH_LEAF_FANIN)
    std::shared_ptr<microtel::LeafReceiver>      m_receiver;
    std::vector<std::string>                     m_leaf_ids;
    std::vector<std::vector<std::byte>>          m_leaf_payloads;
    std::atomic<std::size_t>                     m_next_leaf{0};
    std::uint64_t                                m_leaf_spans{0};
#endif
};

}  // namespace

IBackend* CreateBackend()
{
    return new MicrotelBackend();  // NOLINT(cppcoreguidelines-owning-memory)
}

}  // namespace bench
