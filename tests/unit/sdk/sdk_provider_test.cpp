// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for SdkProvider: ForceFlush two-stage pipeline behaviour.

#include "sdk/sdk_provider.hpp"

#include "microtel/internal/sampler.hpp"
#include "microtel/provider.hpp"
#include "microtel/resource.hpp"
#include "microtel/sampler.hpp"
#include "microtel/status.hpp"

#include "mocks/mock_exporter.hpp"
#include "mocks/mock_span_processor.hpp"
#include "mocks/mock_transport.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace mt = microtel;
namespace mts = microtel::sdk;
namespace mtm = microtel::testing;

namespace
{

constexpr auto kTimeout = std::chrono::milliseconds(500);

// Build an SdkProvider with mock processor, exporter, and transport.
// Returns the provider; raw non-owning pointers to the mocks are written
// through the out-params so callers can observe call counts after the call.
std::unique_ptr<mts::SdkProvider> MakeProvider(mtm::MockSpanProcessor** out_proc,
                                               mtm::MockExporter** out_exp,
                                               mtm::MockTransport** out_transport)
{
    auto proc = std::make_unique<mtm::MockSpanProcessor>();
    auto exp = std::make_unique<mtm::MockExporter>();
    auto transport = std::make_unique<mtm::MockTransport>();

    *out_proc = proc.get();
    *out_exp = exp.get();
    *out_transport = transport.get();

    return std::make_unique<mts::SdkProvider>(mts::SdkProviderArgs{
        .diagnostics = std::make_unique<mts::DiagnosticsCounters>(),
        .encoder = nullptr,
        .auth = nullptr,
        .transport = std::move(transport),
        .codec = nullptr,
        .exporter = std::move(exp),
        .processor = std::move(proc),
        .resource = std::make_shared<mt::Resource>(),
        .sampler = mt::MakeAlwaysOnSampler(),
        .span_limits = {},
        .connect_opts = {},
    });
}

}  // namespace

// ---------------------------------------------------------------------------
// ForceFlush — two-stage pipeline
// ---------------------------------------------------------------------------

// Both processor and exporter ForceFlush must be called on the happy path.
TEST(SdkProviderTest, ForceFlush_CallsBothProcessorAndExporter)
{
    mtm::MockSpanProcessor* proc = nullptr;   // NOLINT(misc-const-correctness)
    mtm::MockExporter* exp = nullptr;         // NOLINT(misc-const-correctness)
    mtm::MockTransport* transport = nullptr;  // NOLINT(misc-const-correctness)
    auto provider = MakeProvider(&proc, &exp, &transport);

    const auto status = provider->ForceFlush(kTimeout);

    EXPECT_EQ(status, mt::Status::Completed);
    EXPECT_EQ(proc->force_flush_call_count, 1);
    EXPECT_EQ(exp->force_flush_call_count, 1);
}

// If the processor times out the exporter must NOT be flushed — its queue
// may still contain in-flight data, so calling ForceFlush on it would give
// a misleading Completed result.
TEST(SdkProviderTest, ForceFlush_ProcessorTimedOut_ExporterNotFlushed)
{
    mtm::MockSpanProcessor* proc = nullptr;
    mtm::MockExporter* exp = nullptr;         // NOLINT(misc-const-correctness)
    mtm::MockTransport* transport = nullptr;  // NOLINT(misc-const-correctness)
    auto provider = MakeProvider(&proc, &exp, &transport);

    proc->force_flush_result = mt::Status::TimedOut;

    const auto status = provider->ForceFlush(kTimeout);

    EXPECT_EQ(status, mt::Status::TimedOut);
    EXPECT_EQ(proc->force_flush_call_count, 1);
    EXPECT_EQ(exp->force_flush_call_count, 0);
}

// ---------------------------------------------------------------------------
// GetExporterHealth — diagnostics counters
// ---------------------------------------------------------------------------

// Drops recorded on the provider-owned diagnostics sink surface in the
// health snapshot; connection state is still read live from the transport.
TEST(SdkProviderTest, GetExporterHealthReflectsRecordedDrops)
{
    mtm::MockSpanProcessor* proc = nullptr;   // NOLINT(misc-const-correctness)
    mtm::MockExporter* exp = nullptr;         // NOLINT(misc-const-correctness)
    mtm::MockTransport* transport = nullptr;  // NOLINT(misc-const-correctness)
    auto provider = MakeProvider(&proc, &exp, &transport);

    constexpr std::uint64_t kOverflowDrops = 4;
    provider->DiagnosticsSink().RecordDrop(mt::DropReason::QueueFull);
    provider->DiagnosticsSink().RecordDrop(mt::DropReason::CardinalityOverflow, kOverflowDrops);

    const mt::HealthSnapshot health = provider->GetExporterHealth();

    EXPECT_EQ(health.drop_counters[static_cast<std::size_t>(mt::DropReason::QueueFull)], 1U);
    EXPECT_EQ(health.drop_counters[static_cast<std::size_t>(mt::DropReason::CardinalityOverflow)],
              kOverflowDrops);
    EXPECT_EQ(health.connection_state, mt::ConnectionState::Connected);
}

// If the exporter times out the caller receives TimedOut.
TEST(SdkProviderTest, ForceFlush_ExporterTimedOut_ReturnTimedOut)
{
    mtm::MockSpanProcessor* proc = nullptr;  // NOLINT(misc-const-correctness)
    mtm::MockExporter* exp = nullptr;
    mtm::MockTransport* transport = nullptr;  // NOLINT(misc-const-correctness)
    auto provider = MakeProvider(&proc, &exp, &transport);

    exp->force_flush_result = mt::Status::TimedOut;

    const auto status = provider->ForceFlush(kTimeout);

    EXPECT_EQ(status, mt::Status::TimedOut);
    EXPECT_EQ(proc->force_flush_call_count, 1);
    EXPECT_EQ(exp->force_flush_call_count, 1);
}
