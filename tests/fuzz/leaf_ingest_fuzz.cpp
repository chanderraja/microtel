// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

// Fuzz harness for the concentrator's ingest path —
// docs/leaf-concentrator-design.md §7.3 (ICP 0031 ship gate 2).
//
// Drives `Provider::GetLeafReceiver()->Ingest` on a live `SdkProvider` with
// the real upb decoder and a recording span processor. Byte 0 of the input
// selects the configuration; the rest is the payload:
//
//   bits 0-1  the transport leaf id: "", "can0:0x1a4", "boiler-7" or "x"
//   bits 2-3  the time mode configured for the two named leaves:
//             auto, concentrator-stamped, sync-relative or boot-relative
//   bit  4    unknown_leaf = reject (the two named leaves are configured)
//
// Beyond the standing invariants in tests/fuzz/README.md it asserts, for
// every input:
//
//   1. An accepted payload's `spans_accepted + spans_sampled_out +
//      spans_dropped` is the number of spans the payload decodes to, and every
//      accepted span reached the processor carrying a Resource.
//   2. A rejected payload reports all three counts as zero and increments
//      exactly one of `leaf_payload_malformed`, `leaf_payload_too_large` and
//      `leaf_unknown`, by exactly one, and the one that matches its status.
//   3. No reserved `microtel.leaf.*` key reaches a Resource.
//
// Seeds under corpus/leaf_ingest_fuzz/ are well-formed leaf payloads plus
// truncations and single-byte corruptions of each.
//
// Repro:
//   ./build-fuzz/tests/fuzz/leaf_ingest_fuzz <crash_file>

#include "microtel/attribute.hpp"
#include "microtel/internal/otlp_trace_decoder.hpp"
#include "microtel/leaf_receiver.hpp"
#include "microtel/provider.hpp"
#include "microtel/resource.hpp"
#include "microtel/sampler.hpp"
#include "microtel/sdk_builder.hpp"

#include "fakes/fake_span_processor.hpp"
#include "mocks/mock_exporter.hpp"
#include "mocks/mock_transport.hpp"
#include "sdk/diagnostics_counters.hpp"
#include "sdk/sdk_provider.hpp"
#include "wire/encoder/otlp_trace_decoder.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

namespace mt = microtel;
namespace mti = microtel::internal;
namespace mts = microtel::sdk;
namespace mtm = microtel::testing;

namespace
{

constexpr std::array<std::string_view, 4> kLeafIds{"", "can0:0x1a4", "boiler-7", "x"};
constexpr unsigned kLeafIdMask = 0x03U;
constexpr unsigned kModeShift = 2U;
constexpr unsigned kModeMask = 0x03U;
constexpr unsigned kRejectBit = 0x10U;
// Small enough that the fuzzer reaches the size limits.
constexpr std::uint32_t kMaxPayloadBytes = 4096;
constexpr std::uint32_t kMaxSpans = 16;

void Require(bool condition)
{
    if (!condition)
    {
        std::abort();
    }
}

std::uint64_t Drops(const mt::HealthSnapshot& health, mt::DropReason reason)
{
    return health.drop_counters.at(static_cast<std::size_t>(reason));
}

mt::LeafReceiverOptions OptionsFor(unsigned selector)
{
    const unsigned mode_bits = (selector >> kModeShift) & kModeMask;
    const std::optional<mt::LeafTimeMode> mode =
        mode_bits == 0 ? std::nullopt : std::optional{static_cast<mt::LeafTimeMode>(mode_bits - 1)};
    mt::LeafReceiverOptions options;
    options.max_payload_bytes = kMaxPayloadBytes;
    options.max_spans_per_payload = kMaxSpans;
    options.unknown_leaf = (selector & kRejectBit) != 0 ? mt::UnknownLeafPolicy::Reject
                                                        : mt::UnknownLeafPolicy::Accept;
    for (const std::string_view id : {kLeafIds[1], kLeafIds[2]})
    {
        options.leaves.emplace_back(std::string{id},
                                    mt::LeafConfig{.time_mode = mode, .resource = {}});
    }
    return options;
}

/// The span count an independent decode with the receiver's limits finds, or
/// nullopt if it does not decode.
std::optional<std::size_t> DecodedSpans(std::span<const std::byte> payload)
{
    const mt::wire::OtlpTraceDecoder decoder;
    const auto decoded = decoder.Decode(
        payload,
        mti::DecodeLimits{.max_spans = kMaxSpans, .max_depth = 16, .max_arena_bytes = 1U << 24U});
    if (!decoded.has_value())
    {
        return std::nullopt;
    }
    std::size_t n = 0;
    for (const auto& rs : *decoded)
    {
        for (const auto& ss : rs.scopes)
        {
            n += ss.spans.size();
        }
    }
    return n;
}

void CheckRejected(const mt::IngestResult& r, const mt::HealthSnapshot& health)
{
    Require(r.spans_accepted == 0 && r.spans_sampled_out == 0 && r.spans_dropped == 0);
    const std::uint64_t malformed = Drops(health, mt::DropReason::LeafPayloadMalformed);
    const std::uint64_t too_large = Drops(health, mt::DropReason::LeafPayloadTooLarge);
    const std::uint64_t unknown = Drops(health, mt::DropReason::LeafUnknown);
    Require(malformed + too_large + unknown == 1);
    Require((r.status == mt::IngestStatus::Malformed) == (malformed == 1));
    Require((r.status == mt::IngestStatus::TooLarge) == (too_large == 1));
    Require((r.status == mt::IngestStatus::UnknownLeaf) == (unknown == 1));
}

void CheckAccepted(const mt::IngestResult& r,
                   std::span<const std::byte> payload,
                   const mtm::FakeSpanProcessor& processor)
{
    const auto decoded = DecodedSpans(payload);
    Require(decoded.has_value());
    const std::uint64_t counted =
        std::uint64_t{r.spans_accepted} + r.spans_sampled_out + r.spans_dropped;
    Require(counted == decoded.value_or(0));
    Require(processor.received_spans.size() == r.spans_accepted);
    for (const auto& span : processor.received_spans)
    {
        Require(span.resource != nullptr);
        for (const auto& kv : span.resource->Attributes())
        {
            Require(!kv.key.starts_with("microtel.leaf."));
        }
    }
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size)
{
    if (size == 0)
    {
        return 0;
    }
    const unsigned selector = data[0];
    // libFuzzer hands over bytes as uint8_t; the receiver takes std::byte.
    const auto* const bytes = reinterpret_cast<const std::byte*>(data);
    const std::span<const std::byte> payload = std::span{bytes, size}.subspan(1);

    auto processor = std::make_unique<mtm::FakeSpanProcessor>();
    const auto* const recorded = processor.get();
    auto provider = std::make_unique<mts::SdkProvider>(mts::SdkProviderArgs{
        .diagnostics = std::make_unique<mts::DiagnosticsCounters>(),
        .encoder = nullptr,
        .auth = nullptr,
        .transport = std::make_unique<mtm::MockTransport>(),
        .codec = nullptr,
        .exporter = std::make_unique<mtm::MockExporter>(),
        .processor = std::move(processor),
        .resource = std::make_shared<mt::Resource>(),
        .sampler = mt::MakeAlwaysOnSampler(),
        .span_limits = {},
        .connect_opts = {},
        .leaf_receiver = OptionsFor(selector),
        .leaf_decoder = std::make_unique<mt::wire::OtlpTraceDecoder>(),
    });

    const auto receiver = provider->GetLeafReceiver();
    const auto r = receiver->Ingest(
        mt::IngestRequest{.leaf_id = kLeafIds.at(selector & kLeafIdMask), .payload = payload});
    const mt::HealthSnapshot health = provider->GetExporterHealth();

    switch (r.status)
    {
        case mt::IngestStatus::Accepted:
        case mt::IngestStatus::PartiallyAccepted:
            CheckAccepted(r, payload, *recorded);
            break;
        case mt::IngestStatus::Malformed:
        case mt::IngestStatus::TooLarge:
        case mt::IngestStatus::UnknownLeaf:
            CheckRejected(r, health);
            break;
        case mt::IngestStatus::OutOfMemory:
            break;
        case mt::IngestStatus::ShutDown:
        case mt::IngestStatus::Disabled:
            Require(false);  // a live, running receiver never answers these
            break;
    }
    return 0;
}
