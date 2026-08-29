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
#include "mocks/mock_log_exporter.hpp"
#include "mocks/mock_metric_exporter.hpp"
#include "mocks/mock_span_processor.hpp"
#include "mocks/mock_transport.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <memory>
#include <string_view>

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

// Variants that actually wire a log / metric exporter. Without one, GetLogger
// returns the noop logger and GetMeter never builds a reader, so a
// post-shutdown test against the plain fixture would pass while proving
// nothing.
std::unique_ptr<mts::SdkProvider> MakeProviderWithLogExporter(mtm::MockSpanProcessor** out_proc,
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
        .log_exporter = std::make_unique<mtm::MockLogExporter>(),
    });
}

std::unique_ptr<mts::SdkProvider> MakeProviderWithMetricExporter(mtm::MockSpanProcessor** out_proc,
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
        .metric_exporter = std::make_unique<mtm::MockMetricExporter>(),
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

// MakeProvider reports its mocks through T** out-params, so these locals must
// be non-const pointers even in tests that only read the mock afterwards.
// NOLINTBEGIN(misc-const-correctness)
// ---------------------------------------------------------------------------
// Shutdown status aggregation.
//
// Provider::Shutdown drove six components and returned only the span
// processor's status, discarding the other five with (void). A transport or
// exporter that timed out was invisible to the caller — which made
// Http2Transport::Close's timeout unobservable even after it was honoured.
// ---------------------------------------------------------------------------

TEST(SdkProviderTest, Shutdown_TransportTimedOut_IsReportedToTheCaller)
{
    mtm::MockSpanProcessor* proc = nullptr;
    mtm::MockExporter* exp = nullptr;
    mtm::MockTransport* transport = nullptr;
    auto provider = MakeProvider(&proc, &exp, &transport);
    ASSERT_NE(transport, nullptr);

    ASSERT_NE(proc, nullptr);
    ASSERT_NE(exp, nullptr);
    transport->close_result = mt::Status::TimedOut;

    EXPECT_EQ(provider->Shutdown(std::chrono::milliseconds(10)), mt::Status::TimedOut);
    // The timeout must not stop the rest of the teardown.
    EXPECT_GT(proc->shutdown_call_count, 0);
    EXPECT_GT(exp->shutdown_call_count, 0);
}

TEST(SdkProviderTest, Shutdown_AllComponentsComplete_ReportsCompleted)
{
    mtm::MockSpanProcessor* proc = nullptr;
    mtm::MockExporter* exp = nullptr;
    mtm::MockTransport* transport = nullptr;
    auto provider = MakeProvider(&proc, &exp, &transport);
    ASSERT_NE(proc, nullptr);
    ASSERT_NE(exp, nullptr);
    ASSERT_NE(transport, nullptr);

    EXPECT_EQ(provider->Shutdown(std::chrono::milliseconds(10)), mt::Status::Completed);
    EXPECT_GT(proc->shutdown_call_count, 0);
    EXPECT_GT(exp->shutdown_call_count, 0);
    EXPECT_GT(transport->close_call_count, 0);
}

TEST(SdkProviderTest, Shutdown_TransportStillClosesWhenProcessorTimedOut)
{
    mtm::MockSpanProcessor* proc = nullptr;
    mtm::MockExporter* exp = nullptr;
    mtm::MockTransport* transport = nullptr;
    auto provider = MakeProvider(&proc, &exp, &transport);
    ASSERT_NE(proc, nullptr);
    ASSERT_NE(transport, nullptr);

    ASSERT_NE(exp, nullptr);
    proc->shutdown_result = mt::Status::TimedOut;

    EXPECT_EQ(provider->Shutdown(std::chrono::milliseconds(10)), mt::Status::TimedOut);
    EXPECT_GT(exp->shutdown_call_count, 0);
    // A partial teardown would leak the I/O thread and socket, so an early
    // timeout must not short-circuit the components after it.
    EXPECT_GT(transport->close_call_count, 0);
}

// ---------------------------------------------------------------------------
// Post-shutdown component construction.
//
// GetMeter and GetLogger had no shutdown check, so either could build a
// PeriodicExportingMetricReader or a BatchLogRecordProcessor -- and spawn its
// thread -- after Shutdown() returned. threading-model.md §6.2 says no further
// records are accepted after Shutdown; the new thread was also joined only at
// destruction. Thread count is the assertion because the thread is the bug.
// ---------------------------------------------------------------------------

// Live threads in this process, via /proc. Cheap and Linux-only, which matches
// the project's target platform.
namespace
{
[[nodiscard]] int LiveThreadCount()
{
    std::ifstream status{"/proc/self/status"};
    std::string line;
    while (std::getline(status, line))
    {
        constexpr std::string_view kPrefix = "Threads:";
        if (std::string_view{line}.starts_with(kPrefix))
        {
            return std::stoi(line.substr(kPrefix.size()));
        }
    }
    return -1;
}
}  // namespace

TEST(SdkProviderTest, GetLoggerAfterShutdown_SpawnsNoThread)
{
    mtm::MockSpanProcessor* proc = nullptr;
    mtm::MockExporter* exp = nullptr;
    mtm::MockTransport* transport = nullptr;
    auto provider = MakeProviderWithLogExporter(&proc, &exp, &transport);

    ASSERT_EQ(provider->Shutdown(std::chrono::milliseconds(50)), mt::Status::Completed);

    const int before = LiveThreadCount();
    ASSERT_GT(before, 0);
    auto logger = provider->GetLogger("after", "1.0");
    EXPECT_EQ(LiveThreadCount(), before);
    EXPECT_NE(logger, nullptr);
}

TEST(SdkProviderTest, GetMeterAfterShutdown_SpawnsNoThread)
{
    mtm::MockSpanProcessor* proc = nullptr;
    mtm::MockExporter* exp = nullptr;
    mtm::MockTransport* transport = nullptr;
    auto provider = MakeProviderWithMetricExporter(&proc, &exp, &transport);

    ASSERT_EQ(provider->Shutdown(std::chrono::milliseconds(50)), mt::Status::Completed);

    const int before = LiveThreadCount();
    ASSERT_GT(before, 0);
    auto meter = provider->GetMeter("after", "1.0");
    EXPECT_EQ(LiveThreadCount(), before);
    EXPECT_NE(meter, nullptr);
}

// The guard must not break the ordinary path.
TEST(SdkProviderTest, GetLoggerBeforeShutdown_StillBuildsThePipeline)
{
    mtm::MockSpanProcessor* proc = nullptr;
    mtm::MockExporter* exp = nullptr;
    mtm::MockTransport* transport = nullptr;
    auto provider = MakeProviderWithLogExporter(&proc, &exp, &transport);

    const int before = LiveThreadCount();
    auto logger = provider->GetLogger("before", "1.0");
    EXPECT_NE(logger, nullptr);
    // The processor's worker thread is expected here.
    EXPECT_GT(LiveThreadCount(), before);
}

// NOLINTEND(misc-const-correctness)
