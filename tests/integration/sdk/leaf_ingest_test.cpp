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
#include "opentelemetry/proto/collector/trace/v1/trace_service.upb.h"
#include "opentelemetry/proto/common/v1/common.upb.h"
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
#include <map>
#include <memory>
#include <string>
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
    ASSERT_EQ(c.provider->Shutdown(kFlushTimeout), mt::Status::Completed);

    const auto payload = LeafPayload({}, {LeafSpan(1, 1, "late")});
    EXPECT_EQ(receiver->Ingest(mt::IngestRequest{.leaf_id = "a", .payload = payload}).status,
              mt::IngestStatus::ShutDown);
    EXPECT_EQ(c.provider->GetExporterHealth().drop_counters.at(
                  static_cast<std::size_t>(mt::DropReason::PostShutdown)),
              1U);
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
