// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// The concentrator ingest path end to end, short of a network
// (docs/leaf-concentrator-design.md §3.6, §3.6.1, §4.4, §7.4): leaf payloads
// encoded by the real OtlpEncoder → Provider::GetLeafReceiver()->Ingest → the
// real upb decoder → BatchSpanProcessor → OtlpExporter → a recording
// FakeWireCodec. The assertions decode what would have gone on the wire.

#include "sdk/sdk_provider.hpp"
#include "wire/encoder/otlp_encoder.hpp"
#include "wire/encoder/otlp_trace_decoder.hpp"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#include "opentelemetry/proto/collector/logs/v1/logs_service.upb.h"
#include "opentelemetry/proto/collector/metrics/v1/metrics_service.upb.h"
#include "opentelemetry/proto/collector/trace/v1/trace_service.upb.h"
#include "opentelemetry/proto/common/v1/common.upb.h"
#include "opentelemetry/proto/logs/v1/logs.upb.h"
#include "opentelemetry/proto/metrics/v1/metrics.upb.h"
#include "opentelemetry/proto/resource/v1/resource.upb.h"
#include "opentelemetry/proto/trace/v1/trace.upb.h"
#include "upb/mem/arena.h"
#pragma GCC diagnostic pop

#include "microtel/attribute.hpp"
#include "microtel/internal/batch.hpp"
#include "microtel/leaf_receiver.hpp"
#include "microtel/provider.hpp"
#include "microtel/resource.hpp"
#include "microtel/sampler.hpp"
#include "microtel/sdk_builder.hpp"
#include "microtel/status.hpp"
#include "microtel/tracer.hpp"

#include "exporter/otlp_exporter.hpp"
#include "fakes/fake_wire_codec.hpp"
#include "mocks/mock_exporter.hpp"
#include "mocks/mock_span_processor.hpp"
#include "mocks/mock_transport.hpp"
#include "sdk/batch_span_processor.hpp"
#include "sdk/diagnostics_counters.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mt = microtel;
namespace mti = microtel::internal;
namespace mts = microtel::sdk;
namespace mtm = microtel::testing;

namespace
{

constexpr auto kFlushTimeout = std::chrono::seconds(10);

mti::SpanRecord LeafSpan(std::uint8_t trace_fill, std::uint8_t span_fill, std::string name)
{
    mt::TraceId::Bytes t{};
    t.fill(trace_fill);
    mt::SpanId::Bytes s{};
    s.fill(span_fill);
    mti::SpanRecord r;
    r.context = mt::SpanContext{.trace_id = mt::TraceId{t}, .span_id = mt::SpanId{s}};
    r.name = std::move(name);
    r.start_time = std::chrono::system_clock::time_point{std::chrono::nanoseconds{100}};
    r.end_time = std::chrono::system_clock::time_point{std::chrono::nanoseconds{200}};
    return r;
}

/// What a microtel leaf would put on its link: an OTLP request whose Resource
/// carries the reserved wire attributes (§3.8).
std::vector<std::byte> LeafPayload(std::vector<mt::KeyValue> extra_resource,
                                   std::vector<mti::SpanRecord> spans)
{
    std::vector<mt::KeyValue> resource{
        {.key = "microtel.leaf.proto", .value = std::int64_t{1}},
        {.key = "microtel.leaf.time_mode", .value = std::int64_t{0}},
        {.key = "microtel.leaf.encode_time", .value = std::int64_t{1000}},
    };
    for (auto& kv : extra_resource)
    {
        resource.push_back(std::move(kv));
    }
    const mti::BatchHandle batch{std::move(spans),
                                 std::make_shared<const mt::Resource>(std::move(resource)),
                                 mti::InstrumentationScope{.name = "leaf-lib", .version = "1"}};
    mt::wire::OtlpEncoder encoder;
    const auto encoded = encoder.Encode(batch);
    return {encoded.Bytes().begin(), encoded.Bytes().end()};
}

/// One ResourceSpans as it arrived on the wire.
struct WireResourceSpans
{
    std::map<std::string, std::string> string_attrs;
    std::size_t attr_count = 0;
    std::vector<std::string> span_names;
    std::vector<std::uint64_t> start_times;
};

std::string View(upb_StringView v)
{
    return std::string{v.data, v.size};
}

WireResourceSpans ReadResourceSpans(const opentelemetry_proto_trace_v1_ResourceSpans* rs)
{
    WireResourceSpans out;
    std::size_t n_attrs = 0;
    const auto* const* attrs = opentelemetry_proto_resource_v1_Resource_attributes(
        opentelemetry_proto_trace_v1_ResourceSpans_resource(rs), &n_attrs);
    out.attr_count = n_attrs;
    for (std::size_t a = 0; a < n_attrs; ++a)
    {
        const auto* const value = opentelemetry_proto_common_v1_KeyValue_value(attrs[a]);
        out.string_attrs[View(opentelemetry_proto_common_v1_KeyValue_key(attrs[a]))] =
            View(opentelemetry_proto_common_v1_AnyValue_string_value(value));
    }
    std::size_t n_ss = 0;
    const auto* const* ss = opentelemetry_proto_trace_v1_ResourceSpans_scope_spans(rs, &n_ss);
    for (std::size_t i = 0; i < n_ss; ++i)
    {
        std::size_t n_spans = 0;
        const auto* const* spans = opentelemetry_proto_trace_v1_ScopeSpans_spans(ss[i], &n_spans);
        for (std::size_t j = 0; j < n_spans; ++j)
        {
            out.span_names.push_back(View(opentelemetry_proto_trace_v1_Span_name(spans[j])));
            out.start_times.push_back(
                opentelemetry_proto_trace_v1_Span_start_time_unix_nano(spans[j]));
        }
    }
    return out;
}

std::vector<WireResourceSpans> DecodeRequest(const std::vector<std::byte>& bytes)
{
    upb_Arena* const arena = upb_Arena_New();
    std::vector<WireResourceSpans> out;
    const auto* const req = opentelemetry_proto_collector_trace_v1_ExportTraceServiceRequest_parse(
        reinterpret_cast<const char*>(bytes.data()), bytes.size(), arena);
    EXPECT_NE(req, nullptr) << "the request on the wire must parse";
    if (req != nullptr)
    {
        std::size_t n = 0;
        const auto* const* rss =
            opentelemetry_proto_collector_trace_v1_ExportTraceServiceRequest_resource_spans(req,
                                                                                            &n);
        for (std::size_t i = 0; i < n; ++i)
        {
            out.push_back(ReadResourceSpans(rss[i]));
        }
    }
    upb_Arena_Free(arena);
    return out;
}

/// A provider with the real trace pipeline, a leaf receiver, and a recording
/// codec in place of the network.
struct Concentrator
{
    mtm::FakeWireCodec* codec = nullptr;  // owned by provider
    std::unique_ptr<mts::SdkProvider> provider;
};

Concentrator MakeConcentrator(mt::LeafReceiverOptions options)
{
    {
        auto diagnostics = std::make_unique<mts::DiagnosticsCounters>();
        auto encoder = std::make_unique<mt::wire::OtlpEncoder>();
        auto owned_codec = std::make_unique<mtm::FakeWireCodec>();
        owned_codec->default_result = mti::WireResult{.success = true};
        auto* const codec = owned_codec.get();
        auto exporter =
            std::make_unique<mt::exporter::OtlpExporter>(encoder.get(),
                                                         owned_codec.get(),
                                                         mt::exporter::OtlpExporterConfig{},
                                                         diagnostics.get());
        auto resource = std::make_shared<const mt::Resource>(
            std::vector<mt::KeyValue>{{.key = "service.name", .value = std::string{"gateway"}}});
        auto processor = std::make_unique<mts::BatchSpanProcessor>(
            exporter.get(),
            resource,
            mt::BatchOptions{.schedule_delay = std::chrono::hours(1)},
            mt::MemoryLimitOptions{}.max_record_bytes,
            mt::MemoryLimitOptions{}.max_total_queue_bytes,
            diagnostics.get(),
            exporter.get());
        auto* const bsp = processor.get();
        auto provider = std::make_unique<mts::SdkProvider>(mts::SdkProviderArgs{
            .diagnostics = std::move(diagnostics),
            .encoder = std::move(encoder),
            .auth = nullptr,
            .transport = std::make_unique<mtm::MockTransport>(),
            .codec = std::move(owned_codec),
            .exporter = std::move(exporter),
            .batch_span_processor = bsp,
            .processor = std::move(processor),
            .resource = std::move(resource),
            .sampler = mt::MakeAlwaysOnSampler(),
            .span_limits = {},
            .connect_opts = {},
            .leaf_receiver = std::move(options),
            .leaf_decoder = std::make_unique<mt::wire::OtlpTraceDecoder>(),
        });
        return Concentrator{.codec = codec, .provider = std::move(provider)};
    }
}

}  // namespace

TEST(LeafIngestIntegrationTest, ManyLeavesInOneBatchLeaveInOneExportRequest)
{
    const Concentrator c = MakeConcentrator(mt::LeafReceiverOptions{
        .leaf_defaults_resource = {{.key = "deployment.environment", .value = std::string{"prod"}}},
    });
    const auto receiver = c.provider->GetLeafReceiver();
    constexpr int kLeaves = 5;
    for (int i = 0; i < kLeaves; ++i)
    {
        const std::string id = "can0:" + std::to_string(i);
        // Leaf 0 claims to be someone else; the transport id wins.
        std::vector<mt::KeyValue> declared;
        if (i == 0)
        {
            declared.push_back({.key = "device.id", .value = std::string{"cloned"}});
        }
        const auto payload = LeafPayload(
            std::move(declared), {LeafSpan(static_cast<std::uint8_t>(i + 1), 1, "leaf-" + id)});
        const auto r = receiver->Ingest(mt::IngestRequest{.leaf_id = id, .payload = payload});
        ASSERT_EQ(r.status, mt::IngestStatus::Accepted) << id;
        ASSERT_EQ(r.spans_accepted, 1U);
    }
    // An in-process span in the same batch keeps the Provider's Resource.
    {
        auto tracer = c.provider->GetTracer("gateway-lib", "1");
        auto span = tracer->StartSpan("in-process");
        span->End();
    }

    ASSERT_EQ(c.provider->ForceFlush(kFlushTimeout), mt::Status::Completed);

    const auto sent = c.codec->SentPayloads();
    ASSERT_EQ(sent.size(), 1U) << "one batch of six Resources is one export request (§3.6.1)";
    const auto request = DecodeRequest(sent[0]);
    ASSERT_EQ(request.size(), static_cast<std::size_t>(kLeaves + 1))
        << "one ResourceSpans per leaf, plus the Provider's own";

    std::map<std::string, const WireResourceSpans*> by_device;
    const WireResourceSpans* gateway = nullptr;
    for (const auto& rs : request)
    {
        const auto it = rs.string_attrs.find("device.id");
        if (it == rs.string_attrs.end())
        {
            gateway = &rs;
            continue;
        }
        by_device[it->second] = &rs;
        for (const auto& [key, value] : rs.string_attrs)
        {
            EXPECT_FALSE(key.starts_with("microtel.leaf.")) << key << " reached the collector";
        }
        EXPECT_EQ(rs.string_attrs.at("deployment.environment"), "prod");
        EXPECT_EQ(rs.string_attrs.at("service.name"), "unknown_service");
    }
    ASSERT_NE(gateway, nullptr);
    EXPECT_EQ(gateway->string_attrs.at("service.name"), "gateway");
    ASSERT_EQ(gateway->span_names.size(), 1U);
    EXPECT_EQ(gateway->span_names[0], "in-process");
    ASSERT_EQ(by_device.size(), static_cast<std::size_t>(kLeaves));
    EXPECT_EQ(by_device.count("cloned"), 0U) << "the transport id beat the leaf's own device.id";
    ASSERT_EQ(by_device.count("can0:0"), 1U);
    EXPECT_EQ(by_device.at("can0:0")->span_names.at(0), "leaf-can0:0");
    EXPECT_EQ(receiver->Stats().leaf_id_conflicts, 1U);

    const auto health = c.provider->GetExporterHealth();
    EXPECT_EQ(health.batches_sent, static_cast<std::uint64_t>(kLeaves + 1));
}

TEST(LeafIngestIntegrationTest, RejectedPayloadsShowInExporterHealth)
{
    const Concentrator c = MakeConcentrator(mt::LeafReceiverOptions{.max_payload_bytes = 64});
    const auto receiver = c.provider->GetLeafReceiver();

    const std::vector<std::byte> garbage{std::byte{0xff}, std::byte{0xff}};
    EXPECT_EQ(receiver->Ingest(mt::IngestRequest{.leaf_id = "a", .payload = garbage}).status,
              mt::IngestStatus::Malformed);
    const std::vector<std::byte> big(65);
    EXPECT_EQ(receiver->Ingest(mt::IngestRequest{.leaf_id = "a", .payload = big}).status,
              mt::IngestStatus::TooLarge);

    const auto health = c.provider->GetExporterHealth();
    EXPECT_EQ(
        health.drop_counters.at(static_cast<std::size_t>(mt::DropReason::LeafPayloadMalformed)),
        1U);
    EXPECT_EQ(
        health.drop_counters.at(static_cast<std::size_t>(mt::DropReason::LeafPayloadTooLarge)), 1U);
}

TEST(LeafIngestIntegrationTest, IngestAfterProviderShutdownIsShutDown)
{
    const Concentrator c = MakeConcentrator(mt::LeafReceiverOptions{});
    const auto receiver = c.provider->GetLeafReceiver();
    const auto tracer = c.provider->GetTracer("gateway-lib", "1");
    ASSERT_EQ(c.provider->Shutdown(kFlushTimeout), mt::Status::Completed);

    const auto payload = LeafPayload({}, {LeafSpan(1, 1, "late"), LeafSpan(1, 2, "late")});
    EXPECT_EQ(receiver->Ingest(mt::IngestRequest{.leaf_id = "a", .payload = payload}).status,
              mt::IngestStatus::ShutDown);
    EXPECT_EQ(receiver->Stats().payloads_post_shutdown, 1U);
    EXPECT_EQ(receiver->Stats().payloads_rejected, 0U);
    EXPECT_EQ(c.provider->GetExporterHealth().drop_counters.at(
                  static_cast<std::size_t>(mt::DropReason::PostShutdown)),
              0U)
        << "a late leaf payload is not a post_shutdown record (ICP 0035)";

    // An in-process span after Shutdown is still one post_shutdown record.
    tracer->StartSpan("late-in-process")->End();
    EXPECT_EQ(c.provider->GetExporterHealth().drop_counters.at(
                  static_cast<std::size_t>(mt::DropReason::PostShutdown)),
              1U);
    EXPECT_EQ(receiver->Stats().payloads_post_shutdown, 1U);
}

TEST(LeafIngestIntegrationTest, ReceiverOutlivesItsProvider)
{
    std::shared_ptr<mt::LeafReceiver> receiver;
    {
        const Concentrator c = MakeConcentrator(mt::LeafReceiverOptions{});
        receiver = c.provider->GetLeafReceiver();
    }
    const auto payload = LeafPayload({}, {LeafSpan(1, 1, "orphan")});
    EXPECT_EQ(receiver->Ingest(mt::IngestRequest{.leaf_id = "a", .payload = payload}).status,
              mt::IngestStatus::ShutDown);
}

TEST(LeafIngestIntegrationTest, ProviderWithoutLeafOptionsHandsOutADisabledReceiver)
{
    auto provider = std::make_unique<mts::SdkProvider>(mts::SdkProviderArgs{
        .diagnostics = std::make_unique<mts::DiagnosticsCounters>(),
        .encoder = nullptr,
        .auth = nullptr,
        .transport = std::make_unique<mtm::MockTransport>(),
        .codec = nullptr,
        .exporter = std::make_unique<mtm::MockExporter>(),
        .processor = std::make_unique<mtm::MockSpanProcessor>(),
        .resource = std::make_shared<mt::Resource>(),
        .sampler = mt::MakeAlwaysOnSampler(),
        .span_limits = {},
        .connect_opts = {},
    });
    const auto receiver = provider->GetLeafReceiver();
    ASSERT_NE(receiver, nullptr);
    EXPECT_EQ(receiver, provider->GetLeafReceiver()) << "the same receiver every call";
    const std::vector<std::byte> payload(4);
    const auto r = receiver->Ingest(mt::IngestRequest{.leaf_id = "a", .payload = payload});
    EXPECT_EQ(r.status, mt::IngestStatus::Disabled);
    EXPECT_EQ(receiver->Stats().payloads_rejected, 0U);
}

// ---------------------------------------------------------------------------
// Full nodes and empty payloads (ICP 0036)
// ---------------------------------------------------------------------------

namespace
{

/// A Unix time in the right century, as a full node's clock would hold.
constexpr std::uint64_t kNodeStartNs = 1'700'000'000'000'000'000ULL;

upb_StringView Str(std::string_view s)
{
    return upb_StringView_FromDataAndSize(s.data(), s.size());
}

void AddServiceName(opentelemetry_proto_resource_v1_Resource* resource, upb_Arena* arena)
{
    auto* const kv = opentelemetry_proto_resource_v1_Resource_add_attributes(resource, arena);
    opentelemetry_proto_common_v1_KeyValue_set_key(kv, Str("service.name"));
    opentelemetry_proto_common_v1_AnyValue_set_string_value(
        opentelemetry_proto_common_v1_KeyValue_mutable_value(kv, arena), Str("node-svc"));
}

std::vector<std::byte> Serialized(const char* data, std::size_t size)
{
    const auto* const first = reinterpret_cast<const std::byte*>(data);
    return {first, first + size};
}

/// An ExportMetricsServiceRequest: one Resource, one scope and, when
/// @p with_metric, one gauge with one data point.
std::vector<std::byte> MetricsRequest(bool with_metric)
{
    upb_Arena* const arena = upb_Arena_New();
    auto* const req =
        opentelemetry_proto_collector_metrics_v1_ExportMetricsServiceRequest_new(arena);
    auto* const rm =
        opentelemetry_proto_collector_metrics_v1_ExportMetricsServiceRequest_add_resource_metrics(
            req, arena);
    AddServiceName(opentelemetry_proto_metrics_v1_ResourceMetrics_mutable_resource(rm, arena),
                   arena);
    auto* const sm = opentelemetry_proto_metrics_v1_ResourceMetrics_add_scope_metrics(rm, arena);
    opentelemetry_proto_common_v1_InstrumentationScope_set_name(
        opentelemetry_proto_metrics_v1_ScopeMetrics_mutable_scope(sm, arena), Str("node-lib"));
    if (with_metric)
    {
        auto* const metric = opentelemetry_proto_metrics_v1_ScopeMetrics_add_metrics(sm, arena);
        opentelemetry_proto_metrics_v1_Metric_set_name(metric, Str("cpu.utilization"));
        auto* const point = opentelemetry_proto_metrics_v1_Gauge_add_data_points(
            opentelemetry_proto_metrics_v1_Metric_mutable_gauge(metric, arena), arena);
        opentelemetry_proto_metrics_v1_NumberDataPoint_set_time_unix_nano(point, kNodeStartNs);
        opentelemetry_proto_metrics_v1_NumberDataPoint_set_as_double(point, 0.5);
    }
    std::size_t size = 0;
    const char* const data =
        opentelemetry_proto_collector_metrics_v1_ExportMetricsServiceRequest_serialize(
            req, arena, &size);
    auto bytes = Serialized(data, size);
    upb_Arena_Free(arena);
    return bytes;
}

/// An ExportLogsServiceRequest: one Resource, one scope and, when
/// @p with_record, one log record.
std::vector<std::byte> LogsRequest(bool with_record)
{
    upb_Arena* const arena = upb_Arena_New();
    auto* const req = opentelemetry_proto_collector_logs_v1_ExportLogsServiceRequest_new(arena);
    auto* const rl =
        opentelemetry_proto_collector_logs_v1_ExportLogsServiceRequest_add_resource_logs(req,
                                                                                         arena);
    AddServiceName(opentelemetry_proto_logs_v1_ResourceLogs_mutable_resource(rl, arena), arena);
    auto* const sl = opentelemetry_proto_logs_v1_ResourceLogs_add_scope_logs(rl, arena);
    opentelemetry_proto_common_v1_InstrumentationScope_set_name(
        opentelemetry_proto_logs_v1_ScopeLogs_mutable_scope(sl, arena), Str("node-lib"));
    if (with_record)
    {
        auto* const record = opentelemetry_proto_logs_v1_ScopeLogs_add_log_records(sl, arena);
        opentelemetry_proto_logs_v1_LogRecord_set_time_unix_nano(record, kNodeStartNs);
        opentelemetry_proto_common_v1_AnyValue_set_string_value(
            opentelemetry_proto_logs_v1_LogRecord_mutable_body(record, arena), Str("hello"));
    }
    std::size_t size = 0;
    const char* const data =
        opentelemetry_proto_collector_logs_v1_ExportLogsServiceRequest_serialize(req, arena, &size);
    auto bytes = Serialized(data, size);
    upb_Arena_Free(arena);
    return bytes;
}

/// What a full C++ node's exporter hands its ExportTransport: an ordinary
/// trace request with the node's own Resource and no `microtel.leaf.*` key.
std::vector<std::byte> NodeRequest(std::string name)
{
    auto span = LeafSpan(9, 1, std::move(name));
    span.start_time = std::chrono::system_clock::time_point{std::chrono::nanoseconds{kNodeStartNs}};
    span.end_time = span.start_time + std::chrono::milliseconds{3};
    std::vector<mti::SpanRecord> spans;
    spans.push_back(std::move(span));
    const mti::BatchHandle batch{std::move(spans),
                                 std::make_shared<const mt::Resource>(std::vector<mt::KeyValue>{
                                     {.key = "service.name", .value = std::string{"node-svc"}},
                                     {.key = "device.id", .value = std::string{"board-serial-1"}}}),
                                 mti::InstrumentationScope{.name = "node-lib", .version = "2"}};
    mt::wire::OtlpEncoder encoder;
    const auto encoded = encoder.Encode(batch);
    return {encoded.Bytes().begin(), encoded.Bytes().end()};
}

std::vector<std::byte> ReadVector(std::string_view name)
{
    const std::filesystem::path path =
        std::filesystem::path{MICROTEL_LEAF_VECTORS_DIR} / (std::string{name} + ".bin");
    std::ifstream in{path, std::ios::binary};
    EXPECT_TRUE(in.good()) << "missing golden vector " << path;
    const std::vector<char> chars{std::istreambuf_iterator<char>{in},
                                  std::istreambuf_iterator<char>{}};
    return Serialized(chars.data(), chars.size());
}

mt::IngestStatus IngestBytes(mt::LeafReceiver& receiver,
                             std::string_view leaf_id,
                             const std::vector<std::byte>& bytes)
{
    return receiver.Ingest(mt::IngestRequest{.leaf_id = leaf_id, .payload = bytes}).status;
}

}  // namespace

TEST(LeafIngestIntegrationTest, AFullNodesTraceRequestArrivesWithItsResourceAndTimestamps)
{
    constexpr std::string_view kNode = "127.0.0.1:40001";
    const Concentrator c = MakeConcentrator(mt::LeafReceiverOptions{
        .leaves = {{std::string{kNode},
                    mt::LeafConfig{.time_mode = mt::LeafTimeMode::Unix, .resource = {}}}},
    });
    const auto receiver = c.provider->GetLeafReceiver();

    ASSERT_EQ(IngestBytes(*receiver, kNode, NodeRequest("node-op")), mt::IngestStatus::Accepted);
    ASSERT_EQ(IngestBytes(*receiver, "127.0.0.1:40002", NodeRequest("stranger")),
              mt::IngestStatus::Malformed)
        << "an undeclared payload under auto is still refused";
    ASSERT_EQ(c.provider->ForceFlush(kFlushTimeout), mt::Status::Completed);

    const auto sent = c.codec->SentPayloads();
    ASSERT_EQ(sent.size(), 1U);
    const auto request = DecodeRequest(sent[0]);
    ASSERT_EQ(request.size(), 1U);
    const auto& rs = request[0];
    EXPECT_EQ(rs.string_attrs.at("service.name"), "node-svc");
    EXPECT_EQ(rs.string_attrs.at("device.id"), kNode) << "the transport id wins (§4.1)";
    ASSERT_EQ(rs.span_names.size(), 1U);
    EXPECT_EQ(rs.span_names[0], "node-op");
    EXPECT_EQ(rs.start_times.at(0), kNodeStartNs) << "a unix node's timestamps are unchanged";
}

TEST(LeafIngestIntegrationTest, MetricsAndLogsRequestsHandedToIngestAreMalformed)
{
    // Unix everywhere: nothing about the missing leaf header refuses these, so
    // the span-id check and the at-least-one-span rule must (ICP 0036
    // Decision 3).
    const Concentrator c =
        MakeConcentrator(mt::LeafReceiverOptions{.default_time_mode = mt::LeafTimeMode::Unix});
    const auto receiver = c.provider->GetLeafReceiver();

    for (const bool with_data : {true, false})
    {
        EXPECT_EQ(IngestBytes(*receiver, "node", MetricsRequest(with_data)),
                  mt::IngestStatus::Malformed)
            << "metrics, with_data = " << with_data;
        EXPECT_EQ(IngestBytes(*receiver, "node", LogsRequest(with_data)),
                  mt::IngestStatus::Malformed)
            << "logs, with_data = " << with_data;
    }
    EXPECT_EQ(c.provider->GetExporterHealth().drop_counters.at(
                  static_cast<std::size_t>(mt::DropReason::LeafPayloadMalformed)),
              4U);
    EXPECT_EQ(receiver->Stats().payloads_accepted, 0U);
}

TEST(LeafIngestIntegrationTest, TheEmptyBatchGoldenVectorIsMalformedAndTheOthersAreAccepted)
{
    const Concentrator c = MakeConcentrator(mt::LeafReceiverOptions{});
    const auto receiver = c.provider->GetLeafReceiver();

    EXPECT_EQ(IngestBytes(*receiver, "a", ReadVector("empty_batch")), mt::IngestStatus::Malformed)
        << "a header-only payload with no drops (ICP 0036 Migration)";
    for (const std::string_view name : {"one_span",
                                        "max_strings",
                                        "attribute_types",
                                        "dropped_counters",
                                        "events",
                                        "remote_parent",
                                        "status",
                                        "time_boot_relative",
                                        "time_no_clock",
                                        "time_sync_relative",
                                        "time_sync_unsynced",
                                        "utf8"})
    {
        EXPECT_EQ(IngestBytes(*receiver, "a", ReadVector(name)), mt::IngestStatus::Accepted)
            << name;
    }
}
