// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// The four hot-reload `Provider` setters — ICP 0026.
//
// What each one owes the caller is the same shape: read `m_shut_down` before
// any mutex, validate, change nothing when it rejects, and answer with the
// `Status` the rest of the lifecycle surface answers with. These tests are
// that contract, one section per setter, plus the two enumerators the ICP adds
// to `Status`.
//
// `SetBatchOptions` validates with the same rule set as `SdkBuilder::Build`
// (issue #267, closing ICP 0026 Discrepancy 1); the agreement between the two
// is asserted in batch_options_validation_test.cpp.

#include "microtel/internal/sampler.hpp"
#include "microtel/log_sink.hpp"
#include "microtel/provider.hpp"
#include "microtel/resource.hpp"
#include "microtel/sampler.hpp"
#include "microtel/sdk_builder.hpp"
#include "microtel/status.hpp"

#include "common/internal_log.hpp"
#include "fakes/fake_exporter.hpp"
#include "mocks/mock_exporter.hpp"
#include "mocks/mock_log_exporter.hpp"
#include "mocks/mock_metric_exporter.hpp"
#include "mocks/mock_span_processor.hpp"
#include "mocks/mock_transport.hpp"
#include "sdk/batch_span_processor.hpp"
#include "sdk/diagnostics_counters.hpp"
#include "sdk/sdk_provider.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <limits>
#include <memory>
#include <string>
#include <utility>

namespace mt = microtel;
namespace mts = microtel::sdk;
// One alias: the mocks and the fakes share `microtel::testing`.
namespace mtmk = microtel::testing;

using namespace std::chrono_literals;

namespace
{

constexpr auto kTimeout = std::chrono::milliseconds(500);

/// A provider and the borrowed handles a test needs to observe it.
struct Built
{
    std::unique_ptr<mts::SdkProvider> provider;
    mts::BatchSpanProcessor* bsp = nullptr;
    /// Records the batches the processor actually cut, which is how a test
    /// tells "the setter returned Completed" apart from "the setter took
    /// effect". A `MockExporter` only counts calls.
    mtmk::FakeExporter* exporter = nullptr;
};

mt::BatchOptions ManualDrainOpts()
{
    mt::BatchOptions opts;
    opts.schedule_delay = std::chrono::hours(1);
    opts.max_export_batch_size = 512;
    return opts;
}

/// Build a provider whose span pipeline really batches, so `SetBatchOptions`
/// has something to retune. `with_metrics` / `with_logs` decide whether the
/// other two pipelines exist at all, which is what separates `Completed` from
/// `Unsupported`.
Built MakeProvider(mt::SamplerHandle sampler = mt::MakeAlwaysOnSampler(),
                   bool with_metrics = true,
                   bool with_logs = true)
{
    auto exporter = std::make_unique<mtmk::FakeExporter>();
    auto* const exporter_ptr = exporter.get();
    auto processor = std::make_unique<mts::BatchSpanProcessor>(
        exporter_ptr, std::make_shared<const mt::Resource>(), ManualDrainOpts());
    auto* const bsp = processor.get();

    std::unique_ptr<mt::internal::IMetricExporter> metric_exporter;
    if (with_metrics)
    {
        metric_exporter = std::make_unique<mtmk::MockMetricExporter>();
    }
    std::unique_ptr<mt::internal::ILogExporter> log_exporter;
    if (with_logs)
    {
        log_exporter = std::make_unique<mtmk::MockLogExporter>();
    }

    return Built{
        .provider = std::make_unique<mts::SdkProvider>(mts::SdkProviderArgs{
            .diagnostics = std::make_unique<mts::DiagnosticsCounters>(),
            .encoder = nullptr,
            .auth = nullptr,
            .transport = std::make_unique<mtmk::MockTransport>(),
            .codec = nullptr,
            .exporter = std::move(exporter),
            .batch_span_processor = bsp,
            .processor = std::move(processor),
            .resource = std::make_shared<mt::Resource>(),
            .sampler = std::move(sampler),
            .span_limits = {},
            .connect_opts = {},
            .metric_exporter = std::move(metric_exporter),
            .metric_interval = 1h,
            .log_exporter = std::move(log_exporter),
            .log_batch_opts = ManualDrainOpts(),
        }),
        .bsp = bsp,
        .exporter = exporter_ptr,
    };
}

/// A provider whose span processor does not batch. `SimpleSpanProcessor` is
/// not reachable from `SdkBuilder` (ICP 0026 Discrepancy 4), so this stands in
/// for the shape a hand-assembled provider can still have.
std::unique_ptr<mts::SdkProvider> MakeNonBatchingProvider()
{
    return std::make_unique<mts::SdkProvider>(mts::SdkProviderArgs{
        .diagnostics = std::make_unique<mts::DiagnosticsCounters>(),
        .encoder = nullptr,
        .auth = nullptr,
        .transport = std::make_unique<mtmk::MockTransport>(),
        .codec = nullptr,
        .exporter = std::make_unique<mtmk::MockExporter>(),
        .processor = std::make_unique<mtmk::MockSpanProcessor>(),
        .resource = std::make_shared<mt::Resource>(),
        .sampler = mt::MakeAlwaysOnSampler(),
        .span_limits = {},
        .connect_opts = {},
    });
}

// ---------------------------------------------------------------------------
// Status — ICP 0026 §2
// ---------------------------------------------------------------------------

TEST(ProviderSetters, StatusCarriesTheTwoSetterEnumerators)
{
    EXPECT_EQ(static_cast<int>(mt::Status::InvalidArgument), 4);
    EXPECT_EQ(static_cast<int>(mt::Status::Unsupported), 5);
}

// ---------------------------------------------------------------------------
// SetBatchOptions
// ---------------------------------------------------------------------------

/// Queue three spans on @p built's span processor and drain them.
///
/// Three, so a `max_export_batch_size` of 2 shows up as two batches rather
/// than one — which is what tells an applied retune apart from a `Completed`
/// that changed nothing.
void QueueThreeAndFlush(Built& built)
{
    const mt::internal::InstrumentationScope scope{.name = "s", .version = ""};
    for (const char* name : {"a", "b", "c"})
    {
        mt::internal::SpanRecord record;
        record.name = name;
        built.bsp->OnEnd(std::move(record), scope);
    }
    ASSERT_EQ(built.bsp->ForceFlush(2s), mt::Status::Completed);
}

TEST(SetBatchOptions, RetunesTheSpanPipeline)
{
    auto built = MakeProvider();
    mt::BatchOptions opts = ManualDrainOpts();
    opts.max_export_batch_size = 2;
    EXPECT_EQ(built.provider->SetBatchOptions(opts), mt::Status::Completed);

    // Under the build-time 512 this would be one batch of three; under the
    // retuned 2 the drain takes min(queue, 2) per turn.
    QueueThreeAndFlush(built);
    ASSERT_EQ(built.exporter->received_batches.size(), std::size_t{2});
    EXPECT_EQ(built.exporter->received_batches[0].Spans().size(), std::size_t{2});
    EXPECT_EQ(built.exporter->received_batches[1].Spans().size(), std::size_t{1});

    EXPECT_EQ(built.provider->Shutdown(kTimeout), mt::Status::Completed);
}

TEST(SetBatchOptions, SeedsALogProcessorNotYetBuilt)
{
    auto built = MakeProvider();
    mt::BatchOptions opts = ManualDrainOpts();
    opts.max_export_batch_size = 3;
    ASSERT_EQ(built.provider->SetBatchOptions(opts), mt::Status::Completed);

    // The first GetLogger builds the BatchLogRecordProcessor from the seed the
    // setter stored, so a logger acquired afterwards sees the new knobs.
    EXPECT_NE(built.provider->GetLogger("late", "1.0"), nullptr);
    EXPECT_EQ(built.provider->Shutdown(kTimeout), mt::Status::Completed);
}

TEST(SetBatchOptions, RetunesALogProcessorAlreadyBuilt)
{
    auto built = MakeProvider();
    ASSERT_NE(built.provider->GetLogger("early", "1.0"), nullptr);

    mt::BatchOptions opts = ManualDrainOpts();
    opts.max_export_batch_size = 4;
    EXPECT_EQ(built.provider->SetBatchOptions(opts), mt::Status::Completed);
    EXPECT_EQ(built.provider->Shutdown(kTimeout), mt::Status::Completed);
}

TEST(SetBatchOptions, RejectsAZeroQueueSize)
{
    auto built = MakeProvider();
    mt::BatchOptions opts = ManualDrainOpts();
    opts.max_queue_size = 0;
    EXPECT_EQ(built.provider->SetBatchOptions(opts), mt::Status::InvalidArgument);
    EXPECT_EQ(built.provider->Shutdown(kTimeout), mt::Status::Completed);
}

TEST(SetBatchOptions, RejectsAZeroBatchSize)
{
    auto built = MakeProvider();
    mt::BatchOptions opts = ManualDrainOpts();
    opts.max_export_batch_size = 0;
    EXPECT_EQ(built.provider->SetBatchOptions(opts), mt::Status::InvalidArgument);
    EXPECT_EQ(built.provider->Shutdown(kTimeout), mt::Status::Completed);
}

TEST(SetBatchOptions, RejectsABatchLargerThanTheQueue)
{
    auto built = MakeProvider();
    mt::BatchOptions opts = ManualDrainOpts();
    opts.max_queue_size = 10;
    opts.max_export_batch_size = 11;
    EXPECT_EQ(built.provider->SetBatchOptions(opts), mt::Status::InvalidArgument);
    EXPECT_EQ(built.provider->Shutdown(kTimeout), mt::Status::Completed);
}

TEST(SetBatchOptions, RejectsANonPositiveScheduleDelay)
{
    auto built = MakeProvider();
    mt::BatchOptions opts = ManualDrainOpts();
    opts.schedule_delay = 0ms;
    EXPECT_EQ(built.provider->SetBatchOptions(opts), mt::Status::InvalidArgument);
    opts.schedule_delay = -1ms;
    EXPECT_EQ(built.provider->SetBatchOptions(opts), mt::Status::InvalidArgument);
    EXPECT_EQ(built.provider->Shutdown(kTimeout), mt::Status::Completed);
}

TEST(SetBatchOptions, RejectionChangesNothing)
{
    auto built = MakeProvider();
    mt::BatchOptions good = ManualDrainOpts();
    good.max_export_batch_size = 2;
    ASSERT_EQ(built.provider->SetBatchOptions(good), mt::Status::Completed);

    mt::BatchOptions bad = ManualDrainOpts();
    bad.max_export_batch_size = 0;
    ASSERT_EQ(built.provider->SetBatchOptions(bad), mt::Status::InvalidArgument);

    // Still draining in twos. A zero batch size would have made the drain
    // count min(queue, 0) — the queue would never empty and ForceFlush would
    // time out — so this is the assertion that the rejected call really did
    // change nothing rather than merely returning a bad status.
    QueueThreeAndFlush(built);
    ASSERT_EQ(built.exporter->received_batches.size(), std::size_t{2});
    EXPECT_EQ(built.exporter->received_batches[0].Spans().size(), std::size_t{2});
    EXPECT_EQ(built.exporter->received_batches[1].Spans().size(), std::size_t{1});

    EXPECT_EQ(built.provider->Shutdown(kTimeout), mt::Status::Completed);
}

TEST(SetBatchOptions, UnsupportedWithoutABatchingSpanProcessor)
{
    const auto provider = MakeNonBatchingProvider();
    EXPECT_EQ(provider->SetBatchOptions(ManualDrainOpts()), mt::Status::Unsupported);
    EXPECT_EQ(provider->Shutdown(kTimeout), mt::Status::Completed);
}

TEST(SetBatchOptions, AlreadyShutDownAfterShutdown)
{
    auto built = MakeProvider();
    ASSERT_EQ(built.provider->Shutdown(kTimeout), mt::Status::Completed);
    EXPECT_EQ(built.provider->SetBatchOptions(ManualDrainOpts()), mt::Status::AlreadyShutDown);
}

// ---------------------------------------------------------------------------
// SetMetricInterval
// ---------------------------------------------------------------------------

TEST(SetMetricInterval, RetunesAReaderAlreadyBuilt)
{
    auto built = MakeProvider();
    ASSERT_NE(built.provider->GetMeter("m", "1.0"), nullptr);
    EXPECT_EQ(built.provider->SetMetricInterval(250ms), mt::Status::Completed);
    EXPECT_EQ(built.provider->Shutdown(kTimeout), mt::Status::Completed);
}

TEST(SetMetricInterval, AppliesToAReaderNotYetBuilt)
{
    auto built = MakeProvider();
    ASSERT_EQ(built.provider->SetMetricInterval(250ms), mt::Status::Completed);
    // The reader is built by the first GetMeter, from the value the setter
    // stored (ICP 0026 §1).
    EXPECT_NE(built.provider->GetMeter("m", "1.0"), nullptr);
    EXPECT_EQ(built.provider->Shutdown(kTimeout), mt::Status::Completed);
}

TEST(SetMetricInterval, RejectsANonPositiveInterval)
{
    auto built = MakeProvider();
    EXPECT_EQ(built.provider->SetMetricInterval(0ms), mt::Status::InvalidArgument);
    EXPECT_EQ(built.provider->SetMetricInterval(-5ms), mt::Status::InvalidArgument);
    EXPECT_EQ(built.provider->Shutdown(kTimeout), mt::Status::Completed);
}

TEST(SetMetricInterval, UnsupportedWithoutAMetricsPipeline)
{
    auto built = MakeProvider(mt::MakeAlwaysOnSampler(), /*with_metrics=*/false);
    EXPECT_EQ(built.provider->SetMetricInterval(250ms), mt::Status::Unsupported);
    EXPECT_EQ(built.provider->Shutdown(kTimeout), mt::Status::Completed);
}

TEST(SetMetricInterval, AlreadyShutDownAfterShutdown)
{
    auto built = MakeProvider();
    ASSERT_EQ(built.provider->Shutdown(kTimeout), mt::Status::Completed);
    EXPECT_EQ(built.provider->SetMetricInterval(250ms), mt::Status::AlreadyShutDown);
}

// ---------------------------------------------------------------------------
// SetSamplerRatio
// ---------------------------------------------------------------------------

TEST(SetSamplerRatio, RetunesARatioSampler)
{
    auto built = MakeProvider(mt::MakeTraceIdRatioSampler(0.25));
    EXPECT_EQ(built.provider->SetSamplerRatio(0.01), mt::Status::Completed);
    EXPECT_EQ(built.provider->Shutdown(kTimeout), mt::Status::Completed);
}

TEST(SetSamplerRatio, RetunesThroughAParentBasedSampler)
{
    auto built = MakeProvider(mt::MakeParentBasedSampler(mt::MakeTraceIdRatioSampler(0.25)));
    EXPECT_EQ(built.provider->SetSamplerRatio(0.5), mt::Status::Completed);
    EXPECT_EQ(built.provider->Shutdown(kTimeout), mt::Status::Completed);
}

TEST(SetSamplerRatio, UnsupportedForAlwaysOn)
{
    auto built = MakeProvider(mt::MakeAlwaysOnSampler());
    EXPECT_EQ(built.provider->SetSamplerRatio(0.5), mt::Status::Unsupported);
    EXPECT_EQ(built.provider->Shutdown(kTimeout), mt::Status::Completed);
}

TEST(SetSamplerRatio, UnsupportedForAlwaysOff)
{
    auto built = MakeProvider(mt::MakeAlwaysOffSampler());
    EXPECT_EQ(built.provider->SetSamplerRatio(0.5), mt::Status::Unsupported);
    EXPECT_EQ(built.provider->Shutdown(kTimeout), mt::Status::Completed);
}

TEST(SetSamplerRatio, RejectsNaN)
{
    // The factory normalises NaN to 0.0; the setter rejects it. The asymmetry
    // is deliberate — ICP 0026 Decision 3.
    auto built = MakeProvider(mt::MakeTraceIdRatioSampler(0.25));
    EXPECT_EQ(built.provider->SetSamplerRatio(std::numeric_limits<double>::quiet_NaN()),
              mt::Status::InvalidArgument);
    EXPECT_EQ(built.provider->Shutdown(kTimeout), mt::Status::Completed);
}

TEST(SetSamplerRatio, RejectsOutOfRangeValues)
{
    auto built = MakeProvider(mt::MakeTraceIdRatioSampler(0.25));
    EXPECT_EQ(built.provider->SetSamplerRatio(-0.001), mt::Status::InvalidArgument);
    EXPECT_EQ(built.provider->SetSamplerRatio(1.5), mt::Status::InvalidArgument);
    EXPECT_EQ(built.provider->SetSamplerRatio(std::numeric_limits<double>::infinity()),
              mt::Status::InvalidArgument);
    EXPECT_EQ(built.provider->Shutdown(kTimeout), mt::Status::Completed);
}

TEST(SetSamplerRatio, AcceptsBothEndpoints)
{
    auto built = MakeProvider(mt::MakeTraceIdRatioSampler(0.25));
    EXPECT_EQ(built.provider->SetSamplerRatio(0.0), mt::Status::Completed);
    EXPECT_EQ(built.provider->SetSamplerRatio(1.0), mt::Status::Completed);
    EXPECT_EQ(built.provider->Shutdown(kTimeout), mt::Status::Completed);
}

TEST(SetSamplerRatio, AlreadyShutDownAfterShutdown)
{
    auto built = MakeProvider(mt::MakeTraceIdRatioSampler(0.25));
    ASSERT_EQ(built.provider->Shutdown(kTimeout), mt::Status::Completed);
    EXPECT_EQ(built.provider->SetSamplerRatio(0.5), mt::Status::AlreadyShutDown);
}

// ---------------------------------------------------------------------------
// SetLogLevel
// ---------------------------------------------------------------------------

class SetLogLevelTest : public ::testing::Test
{
protected:
    void TearDown() override
    {
        // Process-global knob (ICP 0026 §6): restore the shipped default so no
        // later test in this binary inherits it.
        EXPECT_TRUE(mt::internal::SetMinLogLevel(mt::LogLevel::Info));
    }
};

TEST_F(SetLogLevelTest, SetsTheProcessWideMinimum)
{
    auto built = MakeProvider();
    EXPECT_EQ(built.provider->SetLogLevel(mt::LogLevel::Error), mt::Status::Completed);
    EXPECT_EQ(mt::internal::MinLogLevel(), mt::LogLevel::Error);
    EXPECT_EQ(built.provider->Shutdown(kTimeout), mt::Status::Completed);
}

TEST_F(SetLogLevelTest, RejectsAValueOutsideTheEnumerators)
{
    auto built = MakeProvider();
    ASSERT_EQ(built.provider->SetLogLevel(mt::LogLevel::Warn), mt::Status::Completed);
    // NOLINTNEXTLINE(clang-analyzer-optin.core.EnumCastOutOfRange)
    EXPECT_EQ(built.provider->SetLogLevel(static_cast<mt::LogLevel>(200)),
              mt::Status::InvalidArgument);
    EXPECT_EQ(mt::internal::MinLogLevel(), mt::LogLevel::Warn);
    EXPECT_EQ(built.provider->Shutdown(kTimeout), mt::Status::Completed);
}

TEST_F(SetLogLevelTest, LastWriterWinsAcrossProviders)
{
    // The knob is process-global, so two providers share it. Stated, not
    // worked around (ICP 0026 §6).
    auto first = MakeProvider();
    ASSERT_EQ(first.provider->SetLogLevel(mt::LogLevel::Error), mt::Status::Completed);
    ASSERT_EQ(first.provider->Shutdown(kTimeout), mt::Status::Completed);
    first.provider.reset();

    auto second = MakeProvider();
    ASSERT_EQ(second.provider->SetLogLevel(mt::LogLevel::Debug), mt::Status::Completed);
    EXPECT_EQ(mt::internal::MinLogLevel(), mt::LogLevel::Debug);
    EXPECT_EQ(second.provider->Shutdown(kTimeout), mt::Status::Completed);
}

TEST_F(SetLogLevelTest, AlreadyShutDownAfterShutdown)
{
    auto built = MakeProvider();
    ASSERT_EQ(built.provider->Shutdown(kTimeout), mt::Status::Completed);
    EXPECT_EQ(built.provider->SetLogLevel(mt::LogLevel::Error), mt::Status::AlreadyShutDown);
}

}  // namespace
