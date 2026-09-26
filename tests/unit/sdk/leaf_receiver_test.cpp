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

#include "fakes/fake_clock.hpp"
#include "fakes/fake_diagnostics_sink.hpp"
#include "fakes/fake_otlp_trace_decoder.hpp"
#include "fakes/fake_span_processor.hpp"
#include "fakes/fake_steady_clock.hpp"
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
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <stdexcept>
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
    mtm::FakeClock wall;
    mtm::FakeSteadyClock steady;

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
                                                          .clock = &wall,
                                                          .steady_clock = &steady,
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

TEST_F(LeafReceiverTest, IngestAfterShutdownCountsPayloadsPostShutdownNotADropReason)
{
    auto rx = Make();
    Decodes(Payload(Reserved(), {LeafSpan(1, 1), LeafSpan(1, 2), LeafSpan(1, 3)}));
    rx->MarkShutDown();

    const mt::IngestResult r = rx->Ingest(Request());
    EXPECT_EQ(r.status, mt::IngestStatus::ShutDown);
    EXPECT_EQ(std::uint64_t{r.spans_accepted} + r.spans_sampled_out + r.spans_dropped, 0U);
    EXPECT_EQ(decoder->decode_call_count, 0) << "the payload is not decoded after shutdown";
    EXPECT_TRUE(processor.received_spans.empty());

    const mt::LeafReceiverStats stats = rx->Stats();
    EXPECT_EQ(stats.payloads_post_shutdown, 1U);
    EXPECT_EQ(stats.payloads_rejected, 0U) << "the two payload counters are disjoint (ICP 0035)";
    EXPECT_EQ(Drops(sink, mt::DropReason::PostShutdown), 0U)
        << "post_shutdown counts records only (ICP 0035)";
    EXPECT_EQ(TotalDrops(sink), 0U);
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

// ---------------------------------------------------------------------------
// Time modes (§5)
// ---------------------------------------------------------------------------

/// The reserved attributes of a payload in @p mode with encode time @p e and
/// the mode's own attribute (`sync_age` or `boot_id`) set to @p extra.
std::vector<mt::KeyValue> TimeReserved(mt::LeafTimeMode mode,
                                       std::optional<std::int64_t> e,
                                       std::int64_t extra = 0)
{
    std::vector<mt::KeyValue> attrs{
        {.key = "microtel.leaf.proto", .value = std::int64_t{1}},
        {.key = "microtel.leaf.time_mode", .value = static_cast<std::int64_t>(mode)},
    };
    if (e.has_value())
    {
        attrs.push_back({.key = "microtel.leaf.encode_time", .value = *e});
    }
    if (mode == mt::LeafTimeMode::SyncRelative)
    {
        attrs.push_back({.key = "microtel.leaf.sync_age", .value = extra});
    }
    if (mode == mt::LeafTimeMode::BootRelative)
    {
        attrs.push_back({.key = "microtel.leaf.boot_id", .value = extra});
    }
    return attrs;
}

/// A span at [@p start, @p end] in the leaf's clock, with one event at @p event.
mti::SpanRecord TimedSpan(std::int64_t start, std::int64_t end, std::int64_t event)
{
    auto span = LeafSpan(1, 1);
    span.start_time = Ns(start);
    span.end_time = Ns(end);
    mti::SpanEvent ev;
    ev.name = "e";
    ev.timestamp = Ns(event);
    span.events.push_back(std::move(ev));
    return span;
}

std::int64_t NsOf(std::chrono::system_clock::time_point tp)
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(tp.time_since_epoch()).count();
}

mt::IngestRequest RequestAt(std::int64_t received_ns, std::string_view leaf_id = kLeaf)
{
    auto r = Request(leaf_id);
    r.received_at = Ns(received_ns);
    return r;
}

constexpr std::int64_t kSecond = 1'000'000'000;
/// A receive time in the right century, so R - E is a realistic Unix offset.
constexpr std::int64_t kR = 1'700'000'000 * kSecond;

class LeafTimeTest : public LeafReceiverTest
{
public:
    /// Ingest one boot-relative span at leaf time 0 with encode time @p e,
    /// received at @p r; return its corrected start, the anchor B.
    std::int64_t BootAnchorAfter(mts::SdkLeafReceiver& rx,
                                 std::int64_t e,
                                 std::int64_t r,
                                 std::int64_t boot_id = 7,
                                 std::string_view leaf = kLeaf)
    {
        Decodes(Payload(TimeReserved(mt::LeafTimeMode::BootRelative, e, boot_id),
                        {TimedSpan(0, 0, 0)}));
        processor.received_spans.clear();
        const auto result = rx.Ingest(RequestAt(r, leaf));
        EXPECT_EQ(result.status, mt::IngestStatus::Accepted);
        if (processor.received_spans.size() != 1)
        {
            ADD_FAILURE() << "expected one span";
            return -1;
        }
        return NsOf(processor.received_spans[0].start_time);
    }
};

TEST_F(LeafTimeTest, ConcentratorStampedShiftsEveryTimestampByReceiveMinusEncode)
{
    auto rx = Make();
    Decodes(Payload(TimeReserved(mt::LeafTimeMode::ConcentratorStamped, 1000),
                    {TimedSpan(100, 200, 150)}));

    ASSERT_EQ(rx->Ingest(RequestAt(kR)).status, mt::IngestStatus::Accepted);

    ASSERT_EQ(processor.received_spans.size(), 1U);
    const auto& span = processor.received_spans[0];
    EXPECT_EQ(NsOf(span.start_time), kR - 900) << "t' = t + (R - E)";
    EXPECT_EQ(NsOf(span.end_time), kR - 800);
    ASSERT_EQ(span.events.size(), 1U);
    EXPECT_EQ(NsOf(span.events[0].timestamp), kR - 850);
    EXPECT_EQ(rx->Stats().time_fallbacks, 0U);
}

TEST_F(LeafTimeTest, ConcentratorStampedWithoutAClockStampsEverythingAtReceiveTime)
{
    auto rx = Make();
    Decodes(Payload(TimeReserved(mt::LeafTimeMode::ConcentratorStamped, std::nullopt),
                    {TimedSpan(0, 0, 0)}));

    ASSERT_EQ(rx->Ingest(RequestAt(kR)).status, mt::IngestStatus::Accepted);

    const auto& span = processor.received_spans.at(0);
    EXPECT_EQ(NsOf(span.start_time), kR);
    EXPECT_EQ(NsOf(span.end_time), kR);
    EXPECT_EQ(NsOf(span.events.at(0).timestamp), kR);
}

TEST_F(LeafTimeTest, AnUnsetReceivedAtReadsTheProvidersClock)
{
    wall.now = Ns(kR);
    auto rx = Make();
    Decodes(Payload(TimeReserved(mt::LeafTimeMode::ConcentratorStamped, 1000),
                    {TimedSpan(100, 200, 150)}));

    ASSERT_EQ(rx->Ingest(Request()).status, mt::IngestStatus::Accepted);

    EXPECT_EQ(NsOf(processor.received_spans.at(0).start_time), kR - 900);
}

TEST_F(LeafTimeTest, ACorrectionThatWouldGoBelowZeroIsClampedToZero)
{
    auto rx = Make();
    Decodes(
        Payload(TimeReserved(mt::LeafTimeMode::ConcentratorStamped, kR), {TimedSpan(0, 10, 5)}));

    ASSERT_EQ(rx->Ingest(RequestAt(100)).status, mt::IngestStatus::Accepted);

    const auto& span = processor.received_spans.at(0);
    EXPECT_EQ(NsOf(span.start_time), 0);
    EXPECT_EQ(NsOf(span.end_time), 0);
}

TEST_F(LeafTimeTest, ExtremeLeafValuesSaturateInsteadOfOverflowing)
{
    auto rx = Make();
    constexpr std::int64_t kMax = std::numeric_limits<std::int64_t>::max();
    constexpr std::int64_t kMin = std::numeric_limits<std::int64_t>::min();
    Decodes(Payload(TimeReserved(mt::LeafTimeMode::ConcentratorStamped, kMin),
                    {TimedSpan(kMax - 1, kMax, kMax)}));

    ASSERT_EQ(rx->Ingest(RequestAt(kR)).status, mt::IngestStatus::Accepted);

    const auto& span = processor.received_spans.at(0);
    EXPECT_EQ(NsOf(span.start_time), kMax);
    EXPECT_EQ(NsOf(span.end_time), kMax);
}

TEST_F(LeafTimeTest, SyncRelativeWithinBothLimitsIsTrusted)
{
    options.max_sync_age = std::chrono::seconds{10};
    options.max_clock_skew = std::chrono::seconds{5};
    auto rx = Make();
    // At the limits exactly: sync_age == max_sync_age, |R - E| == max_clock_skew.
    for (const std::int64_t e : {kR - (5 * kSecond), kR + (5 * kSecond)})
    {
        processor.received_spans.clear();
        Decodes(Payload(TimeReserved(mt::LeafTimeMode::SyncRelative, e, 10 * kSecond),
                        {TimedSpan(kR - 300, kR - 200, kR - 250)}));

        ASSERT_EQ(rx->Ingest(RequestAt(kR)).status, mt::IngestStatus::Accepted);

        const auto& span = processor.received_spans.at(0);
        EXPECT_EQ(NsOf(span.start_time), kR - 300) << "t' = t";
        EXPECT_EQ(NsOf(span.end_time), kR - 200);
        EXPECT_EQ(NsOf(span.events.at(0).timestamp), kR - 250);
    }
    EXPECT_EQ(rx->Stats().time_fallbacks, 0U);
}

TEST_F(LeafTimeTest, SyncRelativeWithAStaleSyncFallsBackToConcentratorStamped)
{
    options.max_sync_age = std::chrono::seconds{10};
    auto rx = Make();
    const std::int64_t e = kR - 1000;
    Decodes(Payload(TimeReserved(mt::LeafTimeMode::SyncRelative, e, (10 * kSecond) + 1),
                    {TimedSpan(e - 300, e - 200, e - 250)}));

    ASSERT_EQ(rx->Ingest(RequestAt(kR)).status, mt::IngestStatus::Accepted);

    EXPECT_EQ(NsOf(processor.received_spans.at(0).start_time), kR - 300) << "t' = t + (R - E)";
    EXPECT_EQ(rx->Stats().time_fallbacks, 1U);
}

TEST_F(LeafTimeTest, SyncRelativeWithANegativeSyncAgeFallsBack)
{
    auto rx = Make();
    Decodes(Payload(TimeReserved(mt::LeafTimeMode::SyncRelative, kR, -1),
                    {TimedSpan(kR - 300, kR - 200, kR - 250)}));

    ASSERT_EQ(rx->Ingest(RequestAt(kR + 1000)).status, mt::IngestStatus::Accepted);

    EXPECT_EQ(NsOf(processor.received_spans.at(0).start_time), kR + 700);
    EXPECT_EQ(rx->Stats().time_fallbacks, 1U);
}

TEST_F(LeafTimeTest, SyncRelativeWithASkewedClockFallsBackEitherWay)
{
    options.max_clock_skew = std::chrono::seconds{5};
    auto rx = Make();
    // The leaf's wall clock is ahead of the concentrator's, then behind it.
    for (const std::int64_t e : {kR + (5 * kSecond) + 1, kR - (5 * kSecond) - 1})
    {
        processor.received_spans.clear();
        Decodes(Payload(TimeReserved(mt::LeafTimeMode::SyncRelative, e, 0),
                        {TimedSpan(e - 300, e - 200, e - 250)}));

        ASSERT_EQ(rx->Ingest(RequestAt(kR)).status, mt::IngestStatus::Accepted);

        EXPECT_EQ(NsOf(processor.received_spans.at(0).start_time), kR - 300);
    }
    EXPECT_EQ(rx->Stats().time_fallbacks, 2U);
}

TEST_F(LeafTimeTest, BootRelativeFirstPayloadAnchorsProvisionallyOnItsOwnSample)
{
    auto rx = Make();
    Decodes(
        Payload(TimeReserved(mt::LeafTimeMode::BootRelative, 5000, 7), {TimedSpan(100, 200, 150)}));

    ASSERT_EQ(rx->Ingest(RequestAt(kR)).status, mt::IngestStatus::Accepted);

    const auto& span = processor.received_spans.at(0);
    const std::int64_t b = kR - 5000;
    EXPECT_EQ(NsOf(span.start_time), b + 100) << "t' = t + B";
    EXPECT_EQ(NsOf(span.end_time), b + 200);
    EXPECT_EQ(NsOf(span.events.at(0).timestamp), b + 150);
}

TEST_F(LeafTimeTest, BootRelativeAnchorIsTheSecondSmallestSample)
{
    auto rx = Make();
    // Samples b = R - E of 3000, 1000 and 2000.
    EXPECT_EQ(BootAnchorAfter(*rx, 1000, 4000), 3000) << "{3000}: provisional";
    EXPECT_EQ(BootAnchorAfter(*rx, 2000, 3000), 3000) << "{3000, 1000}: second-smallest";
    EXPECT_EQ(BootAnchorAfter(*rx, 3000, 5000), 2000) << "{3000, 1000, 2000}";
}

TEST_F(LeafTimeTest, OneLowOutlierDoesNotMoveTheBootAnchorButASecondOneDoes)
{
    auto rx = Make();
    // A steady link: every sample is kR.
    for (std::int64_t i = 0; i < 4; ++i)
    {
        EXPECT_EQ(BootAnchorAfter(*rx, i * kSecond, kR + (i * kSecond)), kR);
    }
    // The leaf clock glitches forward by 1 s once: its sample is 1 s too low.
    EXPECT_EQ(BootAnchorAfter(*rx, (5 * kSecond) + kSecond, kR + (5 * kSecond)), kR)
        << "a single low outlier must not move the anchor";
    // A second sample at or below the current anchor does move it.
    EXPECT_EQ(BootAnchorAfter(*rx, (6 * kSecond) + 400, kR + (6 * kSecond)), kR - 400);
}

TEST_F(LeafTimeTest, ANewBootIdReplacesTheAnchor)
{
    auto rx = Make();
    EXPECT_EQ(BootAnchorAfter(*rx, 1000, kR, 7), kR - 1000);
    EXPECT_EQ(BootAnchorAfter(*rx, 1000, kR, 7), kR - 1000);
    // The leaf restarted: its clock counts from 0 again, from a later boot.
    EXPECT_EQ(BootAnchorAfter(*rx, 10, kR + kSecond, 8), kR + kSecond - 10)
        << "the old boot's samples are gone, so the new boot anchors on its own";
    EXPECT_EQ(BootAnchorAfter(*rx, 20, kR + kSecond + 30, 8), kR + kSecond + 10)
        << "{-10, +10} of the new boot: the second-smallest";
}

TEST_F(LeafTimeTest, BootSamplesOlderThanTheWindowAgeOut)
{
    options.boot_anchor_window = std::chrono::seconds{600};
    auto rx = Make();
    constexpr std::int64_t kWindow = 600 * kSecond;
    // Two low samples early on, then later ones 500 ns higher.
    EXPECT_EQ(BootAnchorAfter(*rx, 1000, kR), kR - 1000);
    EXPECT_EQ(BootAnchorAfter(*rx, 1000, kR), kR - 1000);
    EXPECT_EQ(BootAnchorAfter(*rx, 500 + kWindow, kR + kWindow), kR - 1000)
        << "at exactly the window's age the early samples still count";
    EXPECT_EQ(BootAnchorAfter(*rx, 501 + kWindow, kR + kWindow + 1), kR - 500)
        << "one nanosecond later they have aged out; two samples of kR - 500 remain";
}

TEST_F(LeafTimeTest, TheBootAnchorRingHoldsSixteenSamples)
{
    auto rx = Make();
    EXPECT_EQ(BootAnchorAfter(*rx, 1000, kR), kR - 1000);
    EXPECT_EQ(BootAnchorAfter(*rx, 1000, kR), kR - 1000);
    // Fourteen later samples, each of kR + 500: the ring is full and still
    // holds both low ones.
    for (std::int64_t i = 1; i <= 14; ++i)
    {
        EXPECT_EQ(BootAnchorAfter(*rx, 1000 + i, kR + i + 1500), kR - 1000) << i;
    }
    // The 17th sample pushes the first low one out, leaving one: no longer
    // corroborated, so the anchor moves up to the second-smallest.
    EXPECT_EQ(BootAnchorAfter(*rx, 1015, kR + 1015 + 1500), kR + 500);
}

TEST_F(LeafTimeTest, EachLeafHasItsOwnBootAnchor)
{
    auto rx = Make();
    EXPECT_EQ(BootAnchorAfter(*rx, 1000, kR, 7, "a"), kR - 1000);
    EXPECT_EQ(BootAnchorAfter(*rx, 5000, kR, 7, "b"), kR - 5000);
    EXPECT_EQ(BootAnchorAfter(*rx, 1000, kR, 7, "a"), kR - 1000);
}

TEST_F(LeafTimeTest, AnEvictedLeafReAnchorsFromItsNextPayload)
{
    options.max_leaves = 1;
    auto rx = Make();
    EXPECT_EQ(BootAnchorAfter(*rx, 1000, kR, 7, "a"), kR - 1000);
    EXPECT_EQ(BootAnchorAfter(*rx, 1000, kR, 7, "a"), kR - 1000);
    EXPECT_EQ(BootAnchorAfter(*rx, 1000, kR, 7, "b"), kR - 1000);  // evicts a
    EXPECT_EQ(BootAnchorAfter(*rx, 1000, kR + 700, 7, "a"), kR - 300)
        << "a's two earlier samples went with its entry";
}

TEST_F(LeafTimeTest, AConstrainedLeafMayStillSendConcentratorStampedAndIsCorrectedAsSuch)
{
    options.default_time_mode = mt::LeafTimeMode::SyncRelative;
    auto rx = Make();
    Decodes(Payload(TimeReserved(mt::LeafTimeMode::ConcentratorStamped, 1000),
                    {TimedSpan(100, 200, 150)}));

    ASSERT_EQ(rx->Ingest(RequestAt(kR)).status, mt::IngestStatus::Accepted);

    EXPECT_EQ(NsOf(processor.received_spans.at(0).start_time), kR - 900);
}

// ---------------------------------------------------------------------------
// Resolver (§4.3)
// ---------------------------------------------------------------------------

TEST_F(LeafReceiverTest, TheResolverIsCalledOncePerLeafAndItsAnswerIsUsed)
{
    std::vector<std::string> asked;
    options.resolver = [&asked](std::string_view id) -> std::optional<mt::LeafConfig>
    {
        asked.emplace_back(id);
        return mt::LeafConfig{.time_mode = std::nullopt,
                              .resource = {{.key = "service.name", .value = std::string{"res"}}}};
    };
    auto rx = Make();
    Decodes(Payload(Reserved(), {LeafSpan(1, 1)}));

    ASSERT_EQ(rx->Ingest(Request()).status, mt::IngestStatus::Accepted);
    ASSERT_EQ(rx->Ingest(Request()).status, mt::IngestStatus::Accepted);

    EXPECT_EQ(asked, std::vector<std::string>{std::string{kLeaf}});
    EXPECT_EQ(StringAt(*processor.received_spans.at(1).resource, "service.name"), "res");
}

TEST_F(LeafReceiverTest, TheResolversAnswerSitsAboveTheStaticEntryPerKey)
{
    options.leaves = {{std::string{kLeaf},
                       mt::LeafConfig{.time_mode = mt::LeafTimeMode::SyncRelative,
                                      .resource = {
                                          {.key = "service.name", .value = std::string{"static"}},
                                          {.key = "host.name", .value = std::string{"h-static"}},
                                      }}}};
    options.resolver = [](std::string_view) -> std::optional<mt::LeafConfig>
    {
        return mt::LeafConfig{.time_mode = std::nullopt,
                              .resource = {{.key = "service.name", .value = std::string{"res"}}}};
    };
    auto rx = Make();
    Decodes(Payload(Reserved(mt::LeafTimeMode::BootRelative), {LeafSpan(1, 1)}));

    EXPECT_EQ(rx->Ingest(Request()).status, mt::IngestStatus::Malformed)
        << "the resolver set no time mode, so the static entry's still constrains";

    Decodes(Payload(Reserved(), {LeafSpan(1, 1)}));
    ASSERT_EQ(rx->Ingest(Request()).status, mt::IngestStatus::Accepted);
    const auto& resource = *processor.received_spans.at(0).resource;
    EXPECT_EQ(StringAt(resource, "service.name"), "res");
    EXPECT_EQ(StringAt(resource, "host.name"), "h-static");
}

TEST_F(LeafReceiverTest, TheResolversTimeModeConstrainsTheLeaf)
{
    options.resolver = [](std::string_view) -> std::optional<mt::LeafConfig>
    { return mt::LeafConfig{.time_mode = mt::LeafTimeMode::SyncRelative, .resource = {}}; };
    auto rx = Make();
    Decodes(Payload(Reserved(mt::LeafTimeMode::BootRelative), {LeafSpan(1, 1)}));

    ExpectRejected(
        rx->Ingest(Request()), mt::IngestStatus::Malformed, mt::DropReason::LeafPayloadMalformed);
}

TEST_F(LeafReceiverTest, ANulloptResolverAnswerLeavesTheLeafUnknown)
{
    int calls = 0;
    options.unknown_leaf = mt::UnknownLeafPolicy::Reject;
    options.resolver = [&calls](std::string_view) -> std::optional<mt::LeafConfig>
    {
        ++calls;
        return std::nullopt;
    };
    auto rx = Make();
    Decodes(Payload(Reserved(), {LeafSpan(1, 1)}));

    ExpectRejected(
        rx->Ingest(Request()), mt::IngestStatus::UnknownLeaf, mt::DropReason::LeafUnknown);
    EXPECT_EQ(rx->Ingest(Request()).status, mt::IngestStatus::UnknownLeaf);
    EXPECT_EQ(calls, 1) << "a negative answer is cached like a positive one";
}

TEST_F(LeafReceiverTest, AResolverAnswerConfiguresTheLeafUnderReject)
{
    options.unknown_leaf = mt::UnknownLeafPolicy::Reject;
    options.resolver = [](std::string_view id) -> std::optional<mt::LeafConfig>
    {
        if (id == "known")
        {
            return mt::LeafConfig{};
        }
        return std::nullopt;
    };
    auto rx = Make();
    Decodes(Payload(Reserved(), {LeafSpan(1, 1)}));

    EXPECT_EQ(rx->Ingest(Request("known")).status, mt::IngestStatus::Accepted);
    EXPECT_EQ(rx->Ingest(Request("other")).status, mt::IngestStatus::UnknownLeaf);
}

TEST_F(LeafReceiverTest, TheResolverIsCalledAgainAfterEviction)
{
    options.max_leaves = 1;
    std::vector<std::string> asked;
    options.resolver = [&asked](std::string_view id) -> std::optional<mt::LeafConfig>
    {
        asked.emplace_back(id);
        return std::nullopt;
    };
    auto rx = Make();
    Decodes(Payload(Reserved(), {LeafSpan(1, 1)}));

    ASSERT_EQ(rx->Ingest(Request("a")).status, mt::IngestStatus::Accepted);
    ASSERT_EQ(rx->Ingest(Request("a")).status, mt::IngestStatus::Accepted);
    ASSERT_EQ(rx->Ingest(Request("b")).status, mt::IngestStatus::Accepted);  // evicts a
    ASSERT_EQ(rx->Ingest(Request("a")).status, mt::IngestStatus::Accepted);

    EXPECT_EQ(asked, (std::vector<std::string>{"a", "b", "a"}));
}

TEST_F(LeafReceiverTest, TheResolverRunsWithNoLockHeld)
{
    mts::SdkLeafReceiver* self = nullptr;
    std::uint64_t tracked_inside = 99;
    options.resolver = [&self, &tracked_inside](std::string_view) -> std::optional<mt::LeafConfig>
    {
        // Stats() takes the leaf table's lock: with it held, this deadlocks.
        tracked_inside = self->Stats().leaves_tracked;
        return std::nullopt;
    };
    auto rx = Make();
    self = rx.get();
    Decodes(Payload(Reserved(), {LeafSpan(1, 1)}));

    ASSERT_EQ(rx->Ingest(Request()).status, mt::IngestStatus::Accepted);
    EXPECT_EQ(tracked_inside, 0U);
}

TEST_F(LeafReceiverTest, ReservedAndLeafIdKeysInAResolverAnswerAreIgnored)
{
    options.resolver = [](std::string_view) -> std::optional<mt::LeafConfig>
    {
        return mt::LeafConfig{.time_mode = std::nullopt,
                              .resource = {
                                  {.key = "device.id", .value = std::string{"spoofed"}},
                                  {.key = "microtel.leaf.proto", .value = std::int64_t{9}},
                                  {.key = "host.name", .value = std::string{"h"}},
                              }};
    };
    auto rx = Make();
    Decodes(Payload(Reserved(), {LeafSpan(1, 1)}));

    ASSERT_EQ(rx->Ingest(Request()).status, mt::IngestStatus::Accepted);
    const auto& resource = *processor.received_spans.at(0).resource;
    EXPECT_EQ(StringAt(resource, "device.id"), kLeaf);
    EXPECT_EQ(Find(resource, "microtel.leaf.proto"), nullptr);
    EXPECT_EQ(StringAt(resource, "host.name"), "h");
}

TEST_F(LeafReceiverTest, ResolverKeysOverTheResourceBudgetAreDroppedAndCounted)
{
    // device.id (9 + 10) + service.name (12 + 15) = 46 bytes are fixed; 10 more fit.
    options.max_leaf_resource_bytes = 56;
    options.resolver = [](std::string_view) -> std::optional<mt::LeafConfig>
    {
        return mt::LeafConfig{.time_mode = std::nullopt,
                              .resource = {
                                  {.key = "k", .value = std::string{"12345678"}},     // 9: fits
                                  {.key = "big", .value = std::string{"123456789"}},  // 12: over
                              }};
    };
    auto rx = Make();
    Decodes(Payload(Reserved(), {LeafSpan(1, 1)}));

    ASSERT_EQ(rx->Ingest(Request()).status, mt::IngestStatus::Accepted);
    const auto& resource = *processor.received_spans.at(0).resource;
    EXPECT_EQ(StringAt(resource, "k"), "12345678");
    EXPECT_EQ(Find(resource, "big"), nullptr);
    EXPECT_EQ(rx->Stats().resource_attributes_dropped, 1U);
    EXPECT_EQ(TotalDrops(sink), 0U) << "not a DropReason";
}

TEST_F(LeafReceiverTest, AThrowingResolverIsTreatedAsNoAnswer)
{
    options.unknown_leaf = mt::UnknownLeafPolicy::Reject;
    options.resolver = [](std::string_view) -> std::optional<mt::LeafConfig>
    { throw std::runtime_error{"inventory service down"}; };
    auto rx = Make();
    Decodes(Payload(Reserved(), {LeafSpan(1, 1)}));

    EXPECT_EQ(rx->Ingest(Request()).status, mt::IngestStatus::UnknownLeaf);
}

TEST_F(LeafReceiverTest, AResolverThatRunsOutOfMemoryIsOutOfMemory)
{
    options.resolver = [](std::string_view) -> std::optional<mt::LeafConfig>
    { throw std::bad_alloc{}; };
    auto rx = Make();
    Decodes(Payload(Reserved(), {LeafSpan(1, 1)}));

    EXPECT_EQ(rx->Ingest(Request()).status, mt::IngestStatus::OutOfMemory);
    EXPECT_EQ(rx->Stats().payloads_out_of_memory, 1U);
}

// ---------------------------------------------------------------------------
// Idle timeout (§4.5)
// ---------------------------------------------------------------------------

TEST_F(LeafReceiverTest, ALeafIdleForLongerThanTheTimeoutIsEvictedOnTheNextInsert)
{
    options.leaf_idle_timeout = std::chrono::seconds{60};
    std::vector<std::string> asked;
    options.resolver = [&asked](std::string_view id) -> std::optional<mt::LeafConfig>
    {
        asked.emplace_back(id);
        return std::nullopt;
    };
    auto rx = Make();
    Decodes(Payload(Reserved(), {LeafSpan(1, 1)}));

    ASSERT_EQ(rx->Ingest(Request("a")).status, mt::IngestStatus::Accepted);
    steady.Advance(std::chrono::seconds{60});
    ASSERT_EQ(rx->Ingest(Request("b")).status, mt::IngestStatus::Accepted);
    EXPECT_EQ(rx->Stats().leaves_tracked, 2U) << "idle for exactly the timeout: kept";
    EXPECT_EQ(rx->Stats().leaves_evicted, 0U);

    steady.Advance(std::chrono::seconds{1});
    ASSERT_EQ(rx->Ingest(Request("c")).status, mt::IngestStatus::Accepted);
    EXPECT_EQ(rx->Stats().leaves_tracked, 2U) << "a, idle for 61 s, is gone; b and c remain";
    EXPECT_EQ(rx->Stats().leaves_evicted, 1U);

    ASSERT_EQ(rx->Ingest(Request("b")).status, mt::IngestStatus::Accepted);
    ASSERT_EQ(rx->Ingest(Request("a")).status, mt::IngestStatus::Accepted);
    EXPECT_EQ(asked, (std::vector<std::string>{"a", "b", "c", "a"}))
        << "b was still cached; a was resolved again";
}

TEST_F(LeafReceiverTest, SeeingALeafResetsItsIdleClock)
{
    options.leaf_idle_timeout = std::chrono::seconds{60};
    auto rx = Make();
    Decodes(Payload(Reserved(), {LeafSpan(1, 1)}));

    ASSERT_EQ(rx->Ingest(Request("a")).status, mt::IngestStatus::Accepted);
    steady.Advance(std::chrono::seconds{50});
    ASSERT_EQ(rx->Ingest(Request("a")).status, mt::IngestStatus::Accepted);
    steady.Advance(std::chrono::seconds{50});
    ASSERT_EQ(rx->Ingest(Request("b")).status, mt::IngestStatus::Accepted);

    EXPECT_EQ(rx->Stats().leaves_evicted, 0U);
    EXPECT_EQ(rx->Stats().leaves_tracked, 2U);
}

/// One thread's share of the eviction race: boot-relative payloads from
/// leaves that overlap with every other thread's, through a table too small
/// for them, so inserts, evictions, anchor updates and resolver calls race.
void IngestAndEvictFromOneThread(mts::SdkLeafReceiver& rx, int thread, std::atomic<int>& accepted)
{
    const std::array<std::string_view, 6> ids{"a", "b", "c", "d", "e", "f"};
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

TEST_F(LeafReceiverTest, ConcurrentIngestWithEvictionAnchorsAndTheResolver)
{
    options.max_leaves = 2;
    options.leaf_idle_timeout = std::chrono::seconds{1};
    std::atomic<int> resolved{0};
    options.resolver = [&resolved](std::string_view) -> std::optional<mt::LeafConfig>
    {
        ++resolved;
        return mt::LeafConfig{.time_mode = mt::LeafTimeMode::BootRelative,
                              .resource = {{.key = "service.name", .value = std::string{"r"}}}};
    };
    auto fake = std::make_unique<mtm::FakeOtlpTraceDecoder>();
    fake->canned.push_back(Payload(Reserved(mt::LeafTimeMode::BootRelative), {LeafSpan(1, 1)}));
    mtm::MockExporter exporter;
    mts::BatchSpanProcessor bsp{&exporter,
                                std::make_shared<mt::Resource>(),
                                mt::BatchOptions{},
                                1U << 20U,
                                1U << 30U,
                                &sink};
    const auto always_on = mt::MakeAlwaysOnSampler();
    // Real clocks: the fakes are not meant to be read from several threads.
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
        threads.emplace_back([&rx, &accepted, t] { IngestAndEvictFromOneThread(rx, t, accepted); });
    }
    for (auto& th : threads)
    {
        th.join();
    }

    EXPECT_EQ(accepted.load(), kThreads * kPerThread);
    EXPECT_LE(rx.Stats().leaves_tracked, 2U);
    EXPECT_GT(rx.Stats().leaves_evicted, 0U);
    EXPECT_GE(resolved.load(), 6) << "every leaf was resolved at least once";
    (void)bsp.Shutdown(std::chrono::seconds(5));
}

}  // namespace
