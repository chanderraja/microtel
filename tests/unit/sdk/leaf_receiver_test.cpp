// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for SdkLeafReceiver, the concentrator's ingest path
// (docs/leaf-concentrator-design.md §3, §4.1, §4.4; ICP 0034). The decoder is
// the dumb mock or the fake, so every payload here is a hand-built decode
// result and no wire bytes are involved; the upb decoder has its own tests in
// tests/unit/wire/.

#include "sdk/leaf_receiver.hpp"

#include "microtel/attribute.hpp"
#include "microtel/internal/batch.hpp"
#include "microtel/internal/otlp_trace_decoder.hpp"
#include "microtel/internal/sampler.hpp"
#include "microtel/leaf_receiver.hpp"
#include "microtel/provider.hpp"
#include "microtel/resource.hpp"
#include "microtel/sampler.hpp"
#include "microtel/sdk_builder.hpp"
#include "microtel/trace.hpp"

#include "fakes/fake_diagnostics_sink.hpp"
#include "fakes/fake_otlp_trace_decoder.hpp"
#include "fakes/fake_span_processor.hpp"
#include "mocks/mock_exporter.hpp"
#include "mocks/mock_otlp_trace_decoder.hpp"
#include "mocks/mock_sampler.hpp"
#include "sdk/batch_span_processor.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace mt = microtel;
namespace mti = microtel::internal;
namespace mts = microtel::sdk;
namespace mtm = microtel::testing;

namespace
{

constexpr std::string_view kLeaf = "can0:0x1a4";
constexpr std::int64_t kEncodeTime = 1000;

// ---------------------------------------------------------------------------
// Payload builders
// ---------------------------------------------------------------------------

mt::TraceId TraceIdOf(std::uint8_t fill)
{
    mt::TraceId::Bytes b{};
    b.fill(fill);
    return mt::TraceId{b};
}

mt::SpanId SpanIdOf(std::uint8_t fill)
{
    mt::SpanId::Bytes b{};
    b.fill(fill);
    return mt::SpanId{b};
}

std::chrono::system_clock::time_point Ns(std::int64_t ns)
{
    return std::chrono::system_clock::time_point{std::chrono::nanoseconds{ns}};
}

mti::SpanRecord LeafSpan(std::uint8_t trace_fill, std::uint8_t span_fill, std::string name = "op")
{
    mti::SpanRecord r;
    r.context = mt::SpanContext{.trace_id = TraceIdOf(trace_fill), .span_id = SpanIdOf(span_fill)};
    r.name = std::move(name);
    r.start_time = Ns(100);
    r.end_time = Ns(200);
    return r;
}

std::vector<mt::KeyValue> Reserved(mt::LeafTimeMode mode = mt::LeafTimeMode::ConcentratorStamped)
{
    std::vector<mt::KeyValue> attrs{
        {.key = "microtel.leaf.proto", .value = std::int64_t{1}},
        {.key = "microtel.leaf.time_mode", .value = static_cast<std::int64_t>(mode)},
        {.key = "microtel.leaf.encode_time", .value = kEncodeTime},
    };
    if (mode == mt::LeafTimeMode::SyncRelative)
    {
        attrs.push_back({.key = "microtel.leaf.sync_age", .value = std::int64_t{5}});
    }
    if (mode == mt::LeafTimeMode::BootRelative)
    {
        attrs.push_back({.key = "microtel.leaf.boot_id", .value = std::int64_t{7}});
    }
    return attrs;
}

mti::DecodedResourceSpans Payload(std::vector<mt::KeyValue> resource,
                                  std::vector<mti::SpanRecord> spans,
                                  std::string scope_name = "leaf-lib")
{
    mti::DecodedResourceSpans rs;
    rs.resource = std::move(resource);
    rs.scopes.push_back(mti::DecodedScopeSpans{
        .scope = mti::InstrumentationScope{.name = std::move(scope_name), .version = "1"},
        .spans = std::move(spans),
    });
    return rs;
}

std::vector<mt::KeyValue> With(std::vector<mt::KeyValue> base, std::vector<mt::KeyValue> extra)
{
    for (auto& kv : extra)
    {
        base.push_back(std::move(kv));
    }
    return base;
}

const mt::AttributeValue* Find(const mt::Resource& resource, std::string_view key)
{
    const auto& attrs = resource.Attributes();
    const auto it = std::ranges::find(attrs, key, &mt::KeyValue::key);
    return it == attrs.end() ? nullptr : &it->value;
}

std::string StringAt(const mt::Resource& resource, std::string_view key)
{
    const auto* const v = Find(resource, key);
    if (v == nullptr)
    {
        return "<absent>";
    }
    const auto* const s = std::get_if<std::string>(v);
    return s == nullptr ? "<not a string>" : *s;
}

std::uint64_t Drops(const mtm::FakeDiagnosticsSink& sink, mt::DropReason reason)
{
    return sink.drop_counters.at(static_cast<std::size_t>(reason));
}

std::uint64_t TotalDrops(const mtm::FakeDiagnosticsSink& sink)
{
    std::uint64_t total = 0;
    for (const auto n : sink.drop_counters)
    {
        total += n;
    }
    return total;
}

// One byte is enough: the mock and the fake decoders never read the payload.
constexpr std::byte kOneByte[1] = {std::byte{0}};

mt::IngestRequest Request(std::string_view leaf_id = kLeaf)
{
    return mt::IngestRequest{.leaf_id = leaf_id, .payload = std::span<const std::byte>{kOneByte}};
}

// ---------------------------------------------------------------------------
// Fixture
// ---------------------------------------------------------------------------

/// A sampler that records the context it was asked about. Local to this file:
/// the shared MockSampler records only a call count.
class RecordingSampler : public mti::ISampler
{
public:
    mti::SamplingDecision decision = mti::SamplingDecision::RecordAndSample;
    mutable std::vector<mt::SpanContext> parents;
    mutable std::vector<mt::TraceId> trace_ids;
    mutable std::vector<std::string> names;

    [[nodiscard]] mti::SamplingResult ShouldSample(
        const mti::SamplingContext& ctx) const noexcept override
    {
        parents.push_back(ctx.parent);
        trace_ids.push_back(ctx.trace_id);
        names.emplace_back(ctx.span_name);
        return mti::SamplingResult{
            .decision = decision, .additional_attributes = {}, .trace_state = {}};
    }

    [[nodiscard]] std::string_view Description() const noexcept override
    {
        return "RecordingSampler";
    }
};

class LeafReceiverTest : public ::testing::Test
{
public:
    mtm::FakeSpanProcessor processor;
    mtm::FakeDiagnosticsSink sink;
    RecordingSampler sampler;
    mtm::MockOtlpTraceDecoder* decoder = nullptr;  // owned by the receiver
    mt::LeafReceiverOptions options;
    mt::SpanLimitOptions limits;

    std::unique_ptr<mts::SdkLeafReceiver> Make()
    {
        auto owned = std::make_unique<mtm::MockOtlpTraceDecoder>();
        decoder = owned.get();
        return std::make_unique<mts::SdkLeafReceiver>(options,
                                                      mts::LeafReceiverDeps{
                                                          .owner = nullptr,
                                                          .sampler = &sampler,
                                                          .processor = &processor,
                                                          .batch_processor = nullptr,
                                                          .diagnostics = &sink,
                                                          .decoder = std::move(owned),
                                                          .span_limits = limits,
                                                      });
    }

    void Decodes(std::vector<mti::DecodedResourceSpans> payload) const
    {
        decoder->result_to_return = std::move(payload);
    }

    void Decodes(mti::DecodedResourceSpans payload) const
    {
        std::vector<mti::DecodedResourceSpans> v;
        v.push_back(std::move(payload));
        Decodes(std::move(v));
    }

    void ExpectRejected(const mt::IngestResult& r,
                        mt::IngestStatus status,
                        mt::DropReason reason) const
    {
        EXPECT_EQ(r.status, status);
        EXPECT_EQ(std::uint64_t{r.spans_accepted} + r.spans_sampled_out + r.spans_dropped, 0U)
            << "a rejected payload reports no spans";
        EXPECT_EQ(Drops(sink, reason), 1U);
        EXPECT_EQ(TotalDrops(sink), 1U) << "a rejected payload counts one reason, once";
        EXPECT_TRUE(processor.received_spans.empty())
            << "nothing of a rejected payload is enqueued";
    }
};

// ---------------------------------------------------------------------------
// Accepted payloads and the merged Resource (§4.4)
// ---------------------------------------------------------------------------

TEST_F(LeafReceiverTest, ValidPayloadIsAcceptedWithTheMergedResource)
{
    options.leaf_defaults_resource = {
        {.key = "service.namespace", .value = std::string{"fleet"}},
        {.key = "deployment.environment", .value = std::string{"prod"}},
    };
    options.leaves = {
        {std::string{kLeaf},
         mt::LeafConfig{.time_mode = std::nullopt,
                        .resource = {{.key = "service.name", .value = std::string{"burner"}}}}}};
    auto rx = Make();
    Decodes(Payload(With(Reserved(),
                         {{.key = "service.namespace", .value = std::string{"own"}},
                          {.key = "service.name", .value = std::string{"firmware-name"}},
                          {.key = "host.name", .value = std::string{"boiler-7"}}}),
                    {LeafSpan(1, 1), LeafSpan(1, 2)}));

    const auto r = rx->Ingest(Request());

    EXPECT_EQ(r.status, mt::IngestStatus::Accepted);
    EXPECT_EQ(r.spans_accepted, 2U);
    EXPECT_EQ(r.spans_sampled_out, 0U);
    EXPECT_EQ(r.spans_dropped, 0U);
    ASSERT_EQ(processor.received_spans.size(), 2U);
    const auto& resource = processor.received_spans[0].resource;
    ASSERT_NE(resource, nullptr) << "a leaf span carries its leaf's Resource";
    EXPECT_EQ(resource, processor.received_spans[1].resource)
        << "both spans of one payload share one Resource object";
    EXPECT_EQ(StringAt(*resource, "service.namespace"), "own") << "the leaf sits above defaults";
    EXPECT_EQ(StringAt(*resource, "deployment.environment"), "prod") << "defaults fill gaps";
    EXPECT_EQ(StringAt(*resource, "service.name"), "burner")
        << "per-leaf config sits above the leaf";
    EXPECT_EQ(StringAt(*resource, "host.name"), "boiler-7");
    EXPECT_EQ(StringAt(*resource, "device.id"), kLeaf) << "the leaf id is the top layer";
    for (const auto& kv : resource->Attributes())
    {
        EXPECT_FALSE(kv.key.starts_with("microtel.leaf.")) << kv.key << " must be stripped";
    }
    EXPECT_EQ(processor.received_scopes[0].name, "leaf-lib");
    EXPECT_EQ(TotalDrops(sink), 0U);
    EXPECT_EQ(rx->Stats().payloads_accepted, 1U);
    EXPECT_EQ(rx->Stats().payloads_rejected, 0U);
}

TEST_F(LeafReceiverTest, TransportIdBeatsThePayloadsDeviceIdAndCountsAConflict)
{
    auto rx = Make();
    Decodes(Payload(With(Reserved(), {{.key = "device.id", .value = std::string{"boiler-7"}}}),
                    {LeafSpan(1, 1)}));

    const auto r = rx->Ingest(Request("can0:0x1a4"));

    ASSERT_EQ(r.status, mt::IngestStatus::Accepted);
    ASSERT_EQ(processor.received_spans.size(), 1U);
    EXPECT_EQ(StringAt(*processor.received_spans[0].resource, "device.id"), "can0:0x1a4");
    EXPECT_EQ(rx->Stats().leaf_id_conflicts, 1U);
    EXPECT_EQ(TotalDrops(sink), 0U) << "a conflict is a stat, not a drop";
}

TEST_F(LeafReceiverTest, MatchingPayloadDeviceIdIsNotAConflict)
{
    auto rx = Make();
    Decodes(Payload(With(Reserved(), {{.key = "device.id", .value = std::string{kLeaf}}}),
                    {LeafSpan(1, 1)}));

    ASSERT_EQ(rx->Ingest(Request()).status, mt::IngestStatus::Accepted);
    EXPECT_EQ(rx->Stats().leaf_id_conflicts, 0U);
}

TEST_F(LeafReceiverTest, EmptyTransportIdFallsBackToThePayloadsDeviceId)
{
    auto rx = Make();
    Decodes(Payload(With(Reserved(), {{.key = "device.id", .value = std::string{"boiler-7"}}}),
                    {LeafSpan(1, 1)}));

    const auto r = rx->Ingest(Request(""));

    ASSERT_EQ(r.status, mt::IngestStatus::Accepted);
    EXPECT_EQ(StringAt(*processor.received_spans[0].resource, "device.id"), "boiler-7");
    EXPECT_EQ(rx->Stats().leaf_id_conflicts, 0U);
}

TEST_F(LeafReceiverTest, NoTransportIdAndNoPayloadIdIsMalformed)
{
    auto rx = Make();
    Decodes(Payload(Reserved(), {LeafSpan(1, 1)}));

    ExpectRejected(
        rx->Ingest(Request("")), mt::IngestStatus::Malformed, mt::DropReason::LeafPayloadMalformed);
    EXPECT_EQ(rx->Stats().payloads_rejected, 1U);
}

TEST_F(LeafReceiverTest, EmptyLeafIdAttributeDisablesExportAndFallback)
{
    options.leaf_id_attribute = "";
    auto rx = Make();
    Decodes(Payload(With(Reserved(), {{.key = "device.id", .value = std::string{"boiler-7"}}}),
                    {LeafSpan(1, 1)}));

    ASSERT_EQ(rx->Ingest(Request()).status, mt::IngestStatus::Accepted);
    EXPECT_EQ(StringAt(*processor.received_spans[0].resource, "device.id"), "boiler-7")
        << "with the setting empty the id is not written, so the leaf's own value stands";
    EXPECT_EQ(rx->Stats().leaf_id_conflicts, 0U);

    processor.received_spans.clear();
    ExpectRejected(
        rx->Ingest(Request("")), mt::IngestStatus::Malformed, mt::DropReason::LeafPayloadMalformed);
}

TEST_F(LeafReceiverTest, MissingServiceNameBecomesUnknownService)
{
    auto rx = Make();
    Decodes(Payload(Reserved(), {LeafSpan(1, 1)}));

    ASSERT_EQ(rx->Ingest(Request()).status, mt::IngestStatus::Accepted);
    EXPECT_EQ(StringAt(*processor.received_spans[0].resource, "service.name"), "unknown_service");
}

TEST_F(LeafReceiverTest, TheProvidersOwnResourceIsNotMergedIn)
{
    auto rx = Make();
    Decodes(Payload(Reserved(), {LeafSpan(1, 1)}));

    ASSERT_EQ(rx->Ingest(Request()).status, mt::IngestStatus::Accepted);
    // Only the leaf's layers: service.name (unknown_service) and device.id.
    EXPECT_EQ(processor.received_spans[0].resource->Attributes().size(), 2U);
}

TEST_F(LeafReceiverTest, LeafReportedDropsAreSummedIntoStats)
{
    auto rx = Make();
    Decodes(Payload(With(Reserved(),
                         {{.key = "microtel.leaf.dropped_spans", .value = std::int64_t{3}},
                          {.key = "microtel.leaf.dropped_items", .value = std::int64_t{4}}}),
                    {LeafSpan(1, 1)}));

    ASSERT_EQ(rx->Ingest(Request()).status, mt::IngestStatus::Accepted);
    EXPECT_EQ(rx->Stats().leaf_reported_drops, 7U);
}

// ---------------------------------------------------------------------------
// Size limits (§3.7)
// ---------------------------------------------------------------------------

TEST_F(LeafReceiverTest, PayloadAtMaxPayloadBytesIsDecoded)
{
    options.max_payload_bytes = 4;
    auto rx = Make();
    Decodes(Payload(Reserved(), {LeafSpan(1, 1)}));
    const std::vector<std::byte> bytes(4);

    EXPECT_EQ(rx->Ingest(mt::IngestRequest{.leaf_id = kLeaf, .payload = bytes}).status,
              mt::IngestStatus::Accepted);
}

TEST_F(LeafReceiverTest, PayloadOverMaxPayloadBytesIsTooLargeAndNotDecoded)
{
    options.max_payload_bytes = 4;
    auto rx = Make();
    Decodes(Payload(Reserved(), {LeafSpan(1, 1)}));
    const std::vector<std::byte> bytes(5);

    ExpectRejected(rx->Ingest(mt::IngestRequest{.leaf_id = kLeaf, .payload = bytes}),
                   mt::IngestStatus::TooLarge,
                   mt::DropReason::LeafPayloadTooLarge);
    EXPECT_EQ(decoder->decode_call_count, 0);
}

TEST_F(LeafReceiverTest, DecoderIsGivenTheDesignedLimits)
{
    options.max_payload_bytes = 1000;
    options.max_spans_per_payload = 7;
    auto rx = Make();
    Decodes(Payload(Reserved(), {LeafSpan(1, 1)}));

    (void)rx->Ingest(Request());

    EXPECT_EQ(decoder->last_limits.max_spans, 7U);
    EXPECT_EQ(decoder->last_limits.max_depth, 16U);
    EXPECT_EQ(decoder->last_limits.max_arena_bytes, (16U * 1000U) + (16U * 1024U))
        << "16 x max_payload_bytes + 16 KiB, measured (see leaf_receiver.cpp)";
}

TEST_F(LeafReceiverTest, DecoderTooLargeIsTooLarge)
{
    auto rx = Make();
    decoder->result_to_return = mt::make_unexpected(mti::DecodeFailure::TooLarge);

    ExpectRejected(
        rx->Ingest(Request()), mt::IngestStatus::TooLarge, mt::DropReason::LeafPayloadTooLarge);
}

TEST_F(LeafReceiverTest, DecoderMalformedIsMalformed)
{
    auto rx = Make();
    decoder->result_to_return = mt::make_unexpected(mti::DecodeFailure::Malformed);

    ExpectRejected(
        rx->Ingest(Request()), mt::IngestStatus::Malformed, mt::DropReason::LeafPayloadMalformed);
}

TEST_F(LeafReceiverTest, LeafIdAtTheByteLimitIsAcceptedAndOnePastIsMalformed)
{
    auto rx = Make();
    Decodes(Payload(Reserved(), {LeafSpan(1, 1)}));
    const std::string at_limit(128, 'x');
    const std::string past_limit(129, 'x');

    EXPECT_EQ(rx->Ingest(Request(at_limit)).status, mt::IngestStatus::Accepted);
    processor.received_spans.clear();
    ExpectRejected(rx->Ingest(Request(past_limit)),
                   mt::IngestStatus::Malformed,
                   mt::DropReason::LeafPayloadMalformed);
}

TEST_F(LeafReceiverTest, SpanCountLimitAtTheBoundaryWithTheFakeDecoder)
{
    options.max_spans_per_payload = 2;
    auto fake = std::make_unique<mtm::FakeOtlpTraceDecoder>();
    fake->canned.push_back(Payload(Reserved(), {LeafSpan(1, 1), LeafSpan(1, 2)}));
    auto* const fake_ptr = fake.get();
    mts::SdkLeafReceiver rx{options,
                            mts::LeafReceiverDeps{.owner = nullptr,
                                                  .sampler = &sampler,
                                                  .processor = &processor,
                                                  .batch_processor = nullptr,
                                                  .diagnostics = &sink,
                                                  .decoder = std::move(fake),
                                                  .span_limits = limits}};
    EXPECT_EQ(rx.Ingest(Request()).status, mt::IngestStatus::Accepted);

    fake_ptr->canned[0].scopes[0].spans.push_back(LeafSpan(1, 3));
    const auto r = rx.Ingest(Request());
    EXPECT_EQ(r.status, mt::IngestStatus::TooLarge);
    EXPECT_EQ(Drops(sink, mt::DropReason::LeafPayloadTooLarge), 1U);
}

// ---------------------------------------------------------------------------
// Unknown leaves (§4.4)
// ---------------------------------------------------------------------------

TEST_F(LeafReceiverTest, UnknownLeafIsAcceptedByDefault)
{
    auto rx = Make();
    Decodes(Payload(Reserved(), {LeafSpan(1, 1)}));

    EXPECT_EQ(rx->Ingest(Request("never-configured")).status, mt::IngestStatus::Accepted);
}

TEST_F(LeafReceiverTest, UnknownLeafIsRejectedUnderRejectAndNotDecoded)
{
    options.unknown_leaf = mt::UnknownLeafPolicy::Reject;
    options.leaves = {{std::string{kLeaf}, mt::LeafConfig{}}};
    auto rx = Make();
    Decodes(Payload(Reserved(), {LeafSpan(1, 1)}));

    ExpectRejected(rx->Ingest(Request("never-configured")),
                   mt::IngestStatus::UnknownLeaf,
                   mt::DropReason::LeafUnknown);
    EXPECT_EQ(decoder->decode_call_count, 0) << "a transport id is enough to refuse it";
}

TEST_F(LeafReceiverTest, ConfiguredLeafIsAcceptedUnderReject)
{
    options.unknown_leaf = mt::UnknownLeafPolicy::Reject;
    options.leaves = {{std::string{kLeaf}, mt::LeafConfig{}}};
    auto rx = Make();
    Decodes(Payload(Reserved(), {LeafSpan(1, 1)}));

    EXPECT_EQ(rx->Ingest(Request()).status, mt::IngestStatus::Accepted);
}

TEST_F(LeafReceiverTest, UnknownPayloadDeclaredLeafIsRejectedUnderReject)
{
    options.unknown_leaf = mt::UnknownLeafPolicy::Reject;
    auto rx = Make();
    Decodes(Payload(With(Reserved(), {{.key = "device.id", .value = std::string{"boiler-7"}}}),
                    {LeafSpan(1, 1)}));

    ExpectRejected(
        rx->Ingest(Request("")), mt::IngestStatus::UnknownLeaf, mt::DropReason::LeafUnknown);
}

// ---------------------------------------------------------------------------
// Validation (§3.4): each rule rejects the whole payload as Malformed
// ---------------------------------------------------------------------------

TEST_F(LeafReceiverTest, MissingProtoVersionIsMalformed)
{
    auto rx = Make();
    auto attrs = Reserved();
    std::erase_if(attrs, [](const mt::KeyValue& kv) { return kv.key == "microtel.leaf.proto"; });
    Decodes(Payload(attrs, {LeafSpan(1, 1)}));

    ExpectRejected(
        rx->Ingest(Request()), mt::IngestStatus::Malformed, mt::DropReason::LeafPayloadMalformed);
}

TEST_F(LeafReceiverTest, UnsupportedProtoVersionIsMalformed)
{
    auto rx = Make();
    auto attrs = Reserved();
    attrs[0].value = std::int64_t{2};
    Decodes(Payload(attrs, {LeafSpan(1, 1)}));

    ExpectRejected(
        rx->Ingest(Request()), mt::IngestStatus::Malformed, mt::DropReason::LeafPayloadMalformed);
}

TEST_F(LeafReceiverTest, NonIntegerProtoVersionIsMalformed)
{
    auto rx = Make();
    auto attrs = Reserved();
    attrs[0].value = std::string{"1"};
    Decodes(Payload(attrs, {LeafSpan(1, 1)}));

    ExpectRejected(
        rx->Ingest(Request()), mt::IngestStatus::Malformed, mt::DropReason::LeafPayloadMalformed);
}

TEST_F(LeafReceiverTest, MissingTimeModeIsMalformed)
{
    auto rx = Make();
    auto attrs = Reserved();
    std::erase_if(attrs,
                  [](const mt::KeyValue& kv) { return kv.key == "microtel.leaf.time_mode"; });
    Decodes(Payload(attrs, {LeafSpan(1, 1)}));

    ExpectRejected(
        rx->Ingest(Request()), mt::IngestStatus::Malformed, mt::DropReason::LeafPayloadMalformed);
}

TEST_F(LeafReceiverTest, OutOfRangeTimeModeIsMalformed)
{
    auto rx = Make();
    auto attrs = Reserved();
    attrs[1].value = std::int64_t{3};
    Decodes(Payload(attrs, {LeafSpan(1, 1)}));

    ExpectRejected(
        rx->Ingest(Request()), mt::IngestStatus::Malformed, mt::DropReason::LeafPayloadMalformed);
}

TEST_F(LeafReceiverTest, SyncRelativeWithoutSyncAgeIsMalformed)
{
    auto rx = Make();
    auto attrs = Reserved(mt::LeafTimeMode::SyncRelative);
    std::erase_if(attrs, [](const mt::KeyValue& kv) { return kv.key == "microtel.leaf.sync_age"; });
    Decodes(Payload(attrs, {LeafSpan(1, 1)}));

    ExpectRejected(
        rx->Ingest(Request()), mt::IngestStatus::Malformed, mt::DropReason::LeafPayloadMalformed);
}

TEST_F(LeafReceiverTest, SyncRelativeWithoutEncodeTimeIsMalformed)
{
    auto rx = Make();
    auto attrs = Reserved(mt::LeafTimeMode::SyncRelative);
    std::erase_if(attrs,
                  [](const mt::KeyValue& kv) { return kv.key == "microtel.leaf.encode_time"; });
    Decodes(Payload(attrs, {LeafSpan(1, 1)}));

    ExpectRejected(
        rx->Ingest(Request()), mt::IngestStatus::Malformed, mt::DropReason::LeafPayloadMalformed);
}

TEST_F(LeafReceiverTest, BootRelativeWithoutBootIdIsMalformed)
{
    auto rx = Make();
    auto attrs = Reserved(mt::LeafTimeMode::BootRelative);
    std::erase_if(attrs, [](const mt::KeyValue& kv) { return kv.key == "microtel.leaf.boot_id"; });
    Decodes(Payload(attrs, {LeafSpan(1, 1)}));

    ExpectRejected(
        rx->Ingest(Request()), mt::IngestStatus::Malformed, mt::DropReason::LeafPayloadMalformed);
}

TEST_F(LeafReceiverTest, ConcentratorStampedWithoutEncodeTimeIsAccepted)
{
    auto rx = Make();
    auto attrs = Reserved();
    std::erase_if(attrs,
                  [](const mt::KeyValue& kv) { return kv.key == "microtel.leaf.encode_time"; });
    Decodes(Payload(attrs, {LeafSpan(1, 1)}));

    EXPECT_EQ(rx->Ingest(Request()).status, mt::IngestStatus::Accepted)
        << "a leaf with no clock sends no encode time (§5.2)";
}

TEST_F(LeafReceiverTest, ZeroTraceIdIsMalformed)
{
    auto rx = Make();
    Decodes(Payload(Reserved(), {LeafSpan(1, 1), LeafSpan(0, 2)}));

    ExpectRejected(
        rx->Ingest(Request()), mt::IngestStatus::Malformed, mt::DropReason::LeafPayloadMalformed);
}

TEST_F(LeafReceiverTest, ZeroSpanIdIsMalformed)
{
    auto rx = Make();
    Decodes(Payload(Reserved(), {LeafSpan(1, 0)}));

    ExpectRejected(
        rx->Ingest(Request()), mt::IngestStatus::Malformed, mt::DropReason::LeafPayloadMalformed);
}

TEST_F(LeafReceiverTest, EndBeforeStartIsMalformed)
{
    auto rx = Make();
    auto span = LeafSpan(1, 1);
    span.end_time = Ns(99);
    Decodes(Payload(Reserved(), {std::move(span)}));

    ExpectRejected(
        rx->Ingest(Request()), mt::IngestStatus::Malformed, mt::DropReason::LeafPayloadMalformed);
}

TEST_F(LeafReceiverTest, EndEqualToStartIsAccepted)
{
    auto rx = Make();
    auto span = LeafSpan(1, 1);
    span.end_time = span.start_time;
    Decodes(Payload(Reserved(), {std::move(span)}));

    EXPECT_EQ(rx->Ingest(Request()).status, mt::IngestStatus::Accepted);
}

TEST_F(LeafReceiverTest, AnEmptyRequestIsMalformed)
{
    auto rx = Make();
    Decodes(std::vector<mti::DecodedResourceSpans>{});

    ExpectRejected(
        rx->Ingest(Request()), mt::IngestStatus::Malformed, mt::DropReason::LeafPayloadMalformed);
}

TEST_F(LeafReceiverTest, AReservedKeyWithANonIntegerValueIsMalformed)
{
    auto rx = Make();
    Decodes(Payload(
        With(Reserved(), {{.key = "microtel.leaf.dropped_spans", .value = std::string{"3"}}}),
        {LeafSpan(1, 1)}));

    ExpectRejected(
        rx->Ingest(Request()), mt::IngestStatus::Malformed, mt::DropReason::LeafPayloadMalformed);
}

TEST_F(LeafReceiverTest, AnUnknownReservedKeyIsIgnoredAndStripped)
{
    auto rx = Make();
    Decodes(Payload(With(Reserved(), {{.key = "microtel.leaf.future", .value = std::string{"x"}}}),
                    {LeafSpan(1, 1)}));

    ASSERT_EQ(rx->Ingest(Request()).status, mt::IngestStatus::Accepted);
    EXPECT_EQ(Find(*processor.received_spans[0].resource, "microtel.leaf.future"), nullptr);
}

TEST_F(LeafReceiverTest, APayloadIdOverTheByteLimitIsMalformed)
{
    auto rx = Make();
    Decodes(Payload(With(Reserved(), {{.key = "device.id", .value = std::string(129, 'x')}}),
                    {LeafSpan(1, 1)}));

    ExpectRejected(
        rx->Ingest(Request("")), mt::IngestStatus::Malformed, mt::DropReason::LeafPayloadMalformed);
}

TEST_F(LeafReceiverTest, OneBadResourceSpansRejectsTheWholePayload)
{
    auto rx = Make();
    std::vector<mti::DecodedResourceSpans> payload;
    payload.push_back(Payload(Reserved(), {LeafSpan(1, 1)}));
    payload.push_back(Payload({}, {LeafSpan(1, 2)}));
    Decodes(std::move(payload));

    ExpectRejected(
        rx->Ingest(Request()), mt::IngestStatus::Malformed, mt::DropReason::LeafPayloadMalformed);
}

TEST_F(LeafReceiverTest, TimeModeNotAllowedByTheLeafsConfigIsMalformed)
{
    options.leaves = {
        {std::string{kLeaf}, mt::LeafConfig{.time_mode = mt::LeafTimeMode::BootRelative}}};
    auto rx = Make();
    Decodes(Payload(Reserved(mt::LeafTimeMode::SyncRelative), {LeafSpan(1, 1)}));

    ExpectRejected(
        rx->Ingest(Request()), mt::IngestStatus::Malformed, mt::DropReason::LeafPayloadMalformed);
}

TEST_F(LeafReceiverTest, ConfiguredModeAndConcentratorStampedAreBothAllowed)
{
    options.leaves = {
        {std::string{kLeaf}, mt::LeafConfig{.time_mode = mt::LeafTimeMode::BootRelative}}};
    auto rx = Make();

    Decodes(Payload(Reserved(mt::LeafTimeMode::BootRelative), {LeafSpan(1, 1)}));
    EXPECT_EQ(rx->Ingest(Request()).status, mt::IngestStatus::Accepted);

    Decodes(Payload(Reserved(mt::LeafTimeMode::ConcentratorStamped), {LeafSpan(1, 2)}));
    EXPECT_EQ(rx->Ingest(Request()).status, mt::IngestStatus::Accepted)
        << "every mode degrades to concentrator-stamped (§5.1)";
}

TEST_F(LeafReceiverTest, DefaultTimeModeConstrainsLeavesWithoutTheirOwn)
{
    options.default_time_mode = mt::LeafTimeMode::SyncRelative;
    auto rx = Make();
    Decodes(Payload(Reserved(mt::LeafTimeMode::BootRelative), {LeafSpan(1, 1)}));

    ExpectRejected(
        rx->Ingest(Request()), mt::IngestStatus::Malformed, mt::DropReason::LeafPayloadMalformed);
}

TEST_F(LeafReceiverTest, AutoAcceptsEveryDeclaredMode)
{
    auto rx = Make();
    for (const auto mode : {mt::LeafTimeMode::ConcentratorStamped,
                            mt::LeafTimeMode::SyncRelative,
                            mt::LeafTimeMode::BootRelative})
    {
        Decodes(Payload(Reserved(mode), {LeafSpan(1, 1)}));
        EXPECT_EQ(rx->Ingest(Request()).status, mt::IngestStatus::Accepted);
    }
}

// ---------------------------------------------------------------------------
// Out of memory (§3.3)
// ---------------------------------------------------------------------------

TEST_F(LeafReceiverTest, AllocationFailureIsOutOfMemoryAndNotTooLarge)
{
    auto fake = std::make_unique<mtm::FakeOtlpTraceDecoder>();
    fake->canned.push_back(Payload(Reserved(), {LeafSpan(1, 1)}));
    fake->throw_bad_alloc_on_call = 1;
    mts::SdkLeafReceiver rx{options,
                            mts::LeafReceiverDeps{.owner = nullptr,
                                                  .sampler = &sampler,
                                                  .processor = &processor,
                                                  .batch_processor = nullptr,
                                                  .diagnostics = &sink,
                                                  .decoder = std::move(fake),
                                                  .span_limits = limits}};

    const auto r = rx.Ingest(Request());

    EXPECT_EQ(r.status, mt::IngestStatus::OutOfMemory);
    EXPECT_EQ(rx.Stats().payloads_out_of_memory, 1U);
    EXPECT_EQ(Drops(sink, mt::DropReason::LeafPayloadTooLarge), 0U);
    EXPECT_EQ(TotalDrops(sink), 0U) << "out of memory is not a DropReason (ICP 0034)";

    EXPECT_EQ(rx.Ingest(Request()).status, mt::IngestStatus::Accepted)
        << "the receiver keeps working after an allocation failure";
}

// ---------------------------------------------------------------------------
// Shutdown
// ---------------------------------------------------------------------------

TEST_F(LeafReceiverTest, IngestAfterShutdownCountsOnePostShutdownPerPayload)
{
    auto rx = Make();
    Decodes(Payload(Reserved(), {LeafSpan(1, 1), LeafSpan(1, 2), LeafSpan(1, 3)}));
    rx->MarkShutDown();

    ExpectRejected(rx->Ingest(Request()), mt::IngestStatus::ShutDown, mt::DropReason::PostShutdown);
    EXPECT_EQ(decoder->decode_call_count, 0) << "the payload is not decoded after shutdown";
}

// ---------------------------------------------------------------------------
// Sampling (§3.6 step 3)
// ---------------------------------------------------------------------------

TEST_F(LeafReceiverTest, EverySpanIsSampledAsARootWithItsOwnTraceId)
{
    auto rx = Make();
    auto child = LeafSpan(9, 2, "child");
    child.parent_context = mt::SpanContext{.trace_id = TraceIdOf(9), .span_id = SpanIdOf(1)};
    Decodes(Payload(Reserved(), {LeafSpan(9, 1, "root"), std::move(child)}));

    ASSERT_EQ(rx->Ingest(Request()).status, mt::IngestStatus::Accepted);
    ASSERT_EQ(sampler.parents.size(), 2U);
    EXPECT_FALSE(sampler.parents[0].IsValid());
    EXPECT_FALSE(sampler.parents[1].IsValid()) << "even a span with a parent is sampled as a root";
    EXPECT_EQ(sampler.trace_ids[1].AsBytes(), TraceIdOf(9).AsBytes());
    EXPECT_EQ(sampler.names[1], "child");
    EXPECT_TRUE(processor.received_spans[1].parent_context.IsValid())
        << "the exported span keeps its parent";
}

TEST_F(LeafReceiverTest, DropAndRecordOnlyAreSampledOutNotDropped)
{
    auto rx = Make();
    Decodes(Payload(Reserved(), {LeafSpan(1, 1), LeafSpan(1, 2)}));

    sampler.decision = mti::SamplingDecision::Drop;
    const auto dropped = rx->Ingest(Request());
    sampler.decision = mti::SamplingDecision::RecordOnly;
    const auto record_only = rx->Ingest(Request());

    for (const auto& r : {dropped, record_only})
    {
        EXPECT_EQ(r.status, mt::IngestStatus::Accepted);
        EXPECT_EQ(r.spans_accepted, 0U);
        EXPECT_EQ(r.spans_sampled_out, 2U);
        EXPECT_EQ(r.spans_dropped, 0U);
    }
    EXPECT_TRUE(processor.received_spans.empty());
    EXPECT_EQ(TotalDrops(sink), 0U);
}

TEST_F(LeafReceiverTest, SampledSpansAreMarkedSampled)
{
    auto rx = Make();
    Decodes(Payload(Reserved(), {LeafSpan(1, 1)}));

    ASSERT_EQ(rx->Ingest(Request()).status, mt::IngestStatus::Accepted);
    EXPECT_TRUE(processor.received_spans[0].context.trace_flags.IsSampled());
}

// ---------------------------------------------------------------------------
// Span limits (§3.6 step 2) and unrepresentable attributes (§3.4)
// ---------------------------------------------------------------------------

TEST_F(LeafReceiverTest, TheProvidersSpanLimitsApplyToDecodedSpans)
{
    limits.attribute_count_limit = 1;
    limits.attribute_value_length_limit = 3;
    limits.event_count_limit = 1;
    limits.link_count_limit = 0;
    limits.event_attribute_count_limit = 0;
    auto rx = Make();
    auto span = LeafSpan(1, 1);
    span.attributes = {{.key = "a", .value = std::string{"abcdef"}},
                       {.key = "b", .value = std::int64_t{1}},
                       {.key = "c", .value = std::int64_t{2}}};
    span.events.push_back(mti::SpanEvent{
        .name = "e1", .timestamp = Ns(150), .attributes = {{.key = "x", .value = true}}});
    span.events.push_back(mti::SpanEvent{.name = "e2", .timestamp = Ns(160), .attributes = {}});
    span.links.push_back(mti::SpanLink{.linked_context = {}, .attributes = {}});
    Decodes(Payload(Reserved(), {std::move(span)}));

    ASSERT_EQ(rx->Ingest(Request()).status, mt::IngestStatus::Accepted);
    const auto& rec = processor.received_spans.at(0);
    ASSERT_EQ(rec.attributes.size(), 1U);
    EXPECT_EQ(std::get<std::string>(rec.attributes[0].value), "abc");
    ASSERT_EQ(rec.events.size(), 1U);
    EXPECT_TRUE(rec.events[0].attributes.empty());
    EXPECT_TRUE(rec.links.empty());
    EXPECT_EQ(Drops(sink, mt::DropReason::SpanAttributeLimit), 2U);
    EXPECT_EQ(Drops(sink, mt::DropReason::AttributeValueTruncated), 1U);
    EXPECT_EQ(Drops(sink, mt::DropReason::SpanEventLimit), 1U);
    EXPECT_EQ(Drops(sink, mt::DropReason::SpanLinkLimit), 1U);
    EXPECT_EQ(Drops(sink, mt::DropReason::EventAttributeLimit), 1U);
}

TEST_F(LeafReceiverTest, UnrepresentableAttributesAreCounted)
{
    auto rx = Make();
    auto payload = Payload(Reserved(), {LeafSpan(1, 1)});
    payload.dropped_span_attributes = 2;
    payload.dropped_resource_attributes = 3;
    Decodes(std::move(payload));

    ASSERT_EQ(rx->Ingest(Request()).status, mt::IngestStatus::Accepted);
    EXPECT_EQ(Drops(sink, mt::DropReason::SpanAttributeLimit), 2U);
    EXPECT_EQ(rx->Stats().resource_attributes_dropped, 3U);
}

// ---------------------------------------------------------------------------
// Resource budget (§4.5)
// ---------------------------------------------------------------------------

TEST_F(LeafReceiverTest, LeafAttributesOverTheResourceBudgetAreDroppedAndNotADropReason)
{
    // Budget: "service.name" + "unknown_service" (27) + "device.id" + kLeaf
    // (9 + 10) = 46, plus room for "a" + "1" (2) but not also "b" + "22" (3).
    options.max_leaf_resource_bytes = 48;
    auto rx = Make();
    Decodes(Payload(
        With(Reserved(),
             {{.key = "a", .value = std::string{"1"}}, {.key = "b", .value = std::string{"22"}}}),
        {LeafSpan(1, 1)}));

    ASSERT_EQ(rx->Ingest(Request()).status, mt::IngestStatus::Accepted);
    const auto& resource = *processor.received_spans[0].resource;
    EXPECT_EQ(StringAt(resource, "a"), "1");
    EXPECT_EQ(Find(resource, "b"), nullptr);
    EXPECT_EQ(StringAt(resource, "device.id"), kLeaf) << "the id is never the one dropped";
    EXPECT_EQ(rx->Stats().resource_attributes_dropped, 1U);
    EXPECT_EQ(TotalDrops(sink), 0U);
}

// ---------------------------------------------------------------------------
// The leaf table (§4.5)
// ---------------------------------------------------------------------------

TEST_F(LeafReceiverTest, RepeatPayloadsFromOneLeafShareOneResource)
{
    auto rx = Make();
    Decodes(Payload(Reserved(), {LeafSpan(1, 1)}));

    ASSERT_EQ(rx->Ingest(Request()).status, mt::IngestStatus::Accepted);
    ASSERT_EQ(rx->Ingest(Request()).status, mt::IngestStatus::Accepted);

    ASSERT_EQ(processor.received_spans.size(), 2U);
    EXPECT_EQ(processor.received_spans[0].resource, processor.received_spans[1].resource)
        << "one Resource per leaf keeps one ResourceSpans per leaf on the wire";
    EXPECT_EQ(rx->Stats().leaves_tracked, 1U);
}

TEST_F(LeafReceiverTest, AChangedLeafResourceIsResolvedAgain)
{
    auto rx = Make();
    Decodes(Payload(With(Reserved(), {{.key = "v", .value = std::string{"1"}}}), {LeafSpan(1, 1)}));
    ASSERT_EQ(rx->Ingest(Request()).status, mt::IngestStatus::Accepted);
    Decodes(Payload(With(Reserved(), {{.key = "v", .value = std::string{"2"}}}), {LeafSpan(1, 2)}));
    ASSERT_EQ(rx->Ingest(Request()).status, mt::IngestStatus::Accepted);

    EXPECT_NE(processor.received_spans[0].resource, processor.received_spans[1].resource);
    EXPECT_EQ(StringAt(*processor.received_spans[0].resource, "v"), "1")
        << "a queued span keeps the Resource it was enqueued with";
    EXPECT_EQ(StringAt(*processor.received_spans[1].resource, "v"), "2");
    EXPECT_EQ(rx->Stats().leaves_tracked, 1U);
}

TEST_F(LeafReceiverTest, TheLeafTableEvictsTheLeastRecentlySeenLeaf)
{
    options.max_leaves = 2;
    auto rx = Make();
    Decodes(Payload(Reserved(), {LeafSpan(1, 1)}));

    ASSERT_EQ(rx->Ingest(Request("a")).status, mt::IngestStatus::Accepted);
    ASSERT_EQ(rx->Ingest(Request("b")).status, mt::IngestStatus::Accepted);
    ASSERT_EQ(rx->Ingest(Request("a")).status, mt::IngestStatus::Accepted);  // a is now recent
    ASSERT_EQ(rx->Ingest(Request("c")).status, mt::IngestStatus::Accepted);  // evicts b

    EXPECT_EQ(rx->Stats().leaves_tracked, 2U);
    EXPECT_EQ(rx->Stats().leaves_evicted, 1U);
    const auto a_before = processor.received_spans[2].resource;
    ASSERT_EQ(rx->Ingest(Request("a")).status, mt::IngestStatus::Accepted);
    EXPECT_EQ(processor.received_spans[4].resource, a_before) << "a survived the eviction";
    ASSERT_EQ(rx->Ingest(Request("b")).status, mt::IngestStatus::Accepted);
    EXPECT_NE(processor.received_spans[5].resource, processor.received_spans[1].resource)
        << "b was evicted and is resolved afresh";
    EXPECT_EQ(rx->Stats().leaves_evicted, 2U);
}

// ---------------------------------------------------------------------------
// Pipeline (§3.5, §3.6 step 4)
// ---------------------------------------------------------------------------

TEST_F(LeafReceiverTest, SpansKeepTheirScopesAcrossResourceAndScopeSpans)
{
    auto rx = Make();
    auto first = Payload(Reserved(), {LeafSpan(1, 1)}, "scope-a");
    first.scopes.push_back(mti::DecodedScopeSpans{
        .scope = mti::InstrumentationScope{.name = "scope-b", .version = "2"},
        .spans = {LeafSpan(1, 2)},
    });
    std::vector<mti::DecodedResourceSpans> payload;
    payload.push_back(std::move(first));
    payload.push_back(Payload(Reserved(), {LeafSpan(1, 3)}, "scope-c"));
    Decodes(std::move(payload));

    const auto r = rx->Ingest(Request());

    EXPECT_EQ(r.spans_accepted, 3U);
    ASSERT_EQ(processor.received_scopes.size(), 3U);
    EXPECT_EQ(processor.received_scopes[0].name, "scope-a");
    EXPECT_EQ(processor.received_scopes[1].name, "scope-b");
    EXPECT_EQ(processor.received_scopes[1].version, "2");
    EXPECT_EQ(processor.received_scopes[2].name, "scope-c");
}

TEST(LeafReceiverPipelineTest, QueueFullDropsAreReportedInTheResultAndTheHealthCounters)
{
    mtm::MockExporter exporter;
    mtm::FakeDiagnosticsSink sink;
    mtm::MockSampler sampler;
    // Room for one estimated record and no more; a one-hour schedule so the
    // worker does not drain the queue mid-payload.
    const std::size_t one_record = mts::EstimateRecordBytes(LeafSpan(1, 1));
    mts::BatchSpanProcessor bsp{&exporter,
                                std::make_shared<mt::Resource>(),
                                mt::BatchOptions{.schedule_delay = std::chrono::hours(1)},
                                mt::MemoryLimitOptions{}.max_record_bytes,
                                one_record,
                                &sink};
    auto decoder = std::make_unique<mtm::MockOtlpTraceDecoder>();
    std::vector<mti::DecodedResourceSpans> decoded;
    decoded.push_back(Payload(Reserved(), {LeafSpan(1, 1), LeafSpan(1, 2), LeafSpan(1, 3)}));
    decoder->result_to_return = std::move(decoded);
    mts::SdkLeafReceiver rx{mt::LeafReceiverOptions{},
                            mts::LeafReceiverDeps{.owner = nullptr,
                                                  .sampler = &sampler,
                                                  .processor = &bsp,
                                                  .batch_processor = &bsp,
                                                  .diagnostics = &sink,
                                                  .decoder = std::move(decoder),
                                                  .span_limits = {}}};

    const auto r = rx.Ingest(Request());

    EXPECT_EQ(r.status, mt::IngestStatus::PartiallyAccepted);
    EXPECT_EQ(r.spans_accepted, 1U);
    EXPECT_EQ(r.spans_dropped, 2U);
    EXPECT_EQ(r.spans_accepted + r.spans_sampled_out + r.spans_dropped, 3U);
    EXPECT_EQ(Drops(sink, mt::DropReason::QueueFull), 2U);
    EXPECT_EQ(rx.Stats().payloads_accepted, 1U) << "a partial accept is still an accepted payload";
    (void)bsp.Shutdown(std::chrono::seconds(5));
}

constexpr int kPerThread = 200;

/// One thread's share of the concurrency test: payloads from leaves that
/// overlap with every other thread's, and a Stats read between them.
void IngestFromOneThread(mts::SdkLeafReceiver& rx, int thread, std::atomic<int>& accepted)
{
    const std::array<std::string_view, 5> ids{"a", "b", "c", "d", "e"};
    for (int i = 0; i < kPerThread; ++i)
    {
        const auto id = ids.at(static_cast<std::size_t>(thread + i) % ids.size());
        if (rx.Ingest(Request(id)).status == mt::IngestStatus::Accepted)
        {
            ++accepted;
        }
        (void)rx.Stats();
    }
}

TEST_F(LeafReceiverTest, ConcurrentIngestFromSeveralThreadsWithOverlappingLeaves)
{
    options.max_leaves = 3;
    auto fake = std::make_unique<mtm::FakeOtlpTraceDecoder>();
    fake->canned.push_back(Payload(Reserved(), {LeafSpan(1, 1)}));
    mtm::MockExporter exporter;
    mts::BatchSpanProcessor bsp{&exporter,
                                std::make_shared<mt::Resource>(),
                                mt::BatchOptions{},
                                1U << 20U,
                                1U << 30U,
                                &sink};
    // The real AlwaysOn sampler: the fixture's recording one is not meant to
    // be called from several threads.
    const auto always_on = mt::MakeAlwaysOnSampler();
    mts::SdkLeafReceiver rx{options,
                            mts::LeafReceiverDeps{.owner = nullptr,
                                                  .sampler = always_on.Get(),
                                                  .processor = &bsp,
                                                  .batch_processor = &bsp,
                                                  .diagnostics = nullptr,
                                                  .decoder = std::move(fake),
                                                  .span_limits = limits}};

    constexpr int kThreads = 4;
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    std::atomic<int> accepted{0};
    for (int t = 0; t < kThreads; ++t)
    {
        threads.emplace_back([&rx, &accepted, t] { IngestFromOneThread(rx, t, accepted); });
    }
    for (auto& th : threads)
    {
        th.join();
    }

    EXPECT_EQ(accepted.load(), kThreads * kPerThread);
    EXPECT_LE(rx.Stats().leaves_tracked, 3U);
    EXPECT_EQ(rx.Stats().payloads_accepted, static_cast<std::uint64_t>(kThreads * kPerThread));
    (void)bsp.Shutdown(std::chrono::seconds(5));
}

}  // namespace
