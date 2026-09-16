// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// The v1.1 ships-when gate, clause 2 (ICP 0024): "the hot-reload setters pass
// a TSAN-built concurrent hammer test — all four setters racing span/log
// emission, the BSP/BLRP workers, and metric collection".
//
// Why a hammer rather than a unit test. The risk the setters carry is not a
// wrong return value — `provider_setters_test.cpp` owns that — it is a **data
// race between a setter writer and a hot-path reader**. ICP 0024 says so in
// as many words: "with setters it is data races between setter writers and
// hot-path readers, which a fuzzer does not find and TSAN under contention
// does." So this test asserts almost nothing. Its output is the sanitizer's.
//
// Every reader the four setters can race is running at once:
//
//   - `BatchSpanProcessor::m_opts`  — read by OnEnd on the emitter thread and
//     by WaitAndCollect on the BSP worker; written by SetBatchOptions.
//   - `BatchLogRecordProcessor::m_opts` — same shape on the log side.
//   - `PeriodicExportingMetricReader::m_interval` — read by RunLoop on the
//     reader thread; written by SetMetricInterval. The reader's interval is
//     short enough here that RunLoop really is looping.
//   - `TraceIdRatioSampler::m_threshold` and its description — read by
//     `ShouldSample` on every StartSpan and by `Description()`; written by
//     SetSamplerRatio through the ParentBased composite, which regenerates its
//     own description at the same time.
//   - `g_min_level` — read by every LogImpl; written by SetLogLevel.
//
// Duration. The default is a few seconds so the sanitizer CI job stays a CI
// job. `MICROTEL_HAMMER_SECONDS` extends it for the minutes-scale local run
// the gate asks for:
//
//     MICROTEL_HAMMER_SECONDS=300 ./build-tsan/tests/unit/sdk/hot_reload_hammer_test

#include "microtel/internal/sampler.hpp"
#include "microtel/log_record.hpp"
#include "microtel/log_sink.hpp"
#include "microtel/logger.hpp"
#include "microtel/meter.hpp"
#include "microtel/provider.hpp"
#include "microtel/resource.hpp"
#include "microtel/sampler.hpp"
#include "microtel/sdk_builder.hpp"
#include "microtel/status.hpp"
#include "microtel/tracer.hpp"

#include "common/internal_log.hpp"
#include "mocks/mock_exporter.hpp"
#include "mocks/mock_log_exporter.hpp"
#include "mocks/mock_metric_exporter.hpp"
#include "mocks/mock_transport.hpp"
#include "sdk/batch_span_processor.hpp"
#include "sdk/diagnostics_counters.hpp"
#include "sdk/sdk_provider.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace mt = microtel;
namespace mts = microtel::sdk;
namespace mtmk = microtel::testing;

using namespace std::chrono_literals;

namespace
{

/// Default wall-clock budget. Long enough that every worker wakes many times,
/// short enough for the sanitizer matrix.
constexpr auto kDefaultSeconds = 3;

/// Short enough that the BSP and BLRP workers and the metric reader are all
/// genuinely cycling for the whole run, which is what puts the readers and the
/// setter writers in contention.
constexpr auto kScheduleDelay = 2ms;
constexpr auto kMetricInterval = 2ms;

std::chrono::seconds HammerBudget()
{
    const char* const raw = std::getenv("MICROTEL_HAMMER_SECONDS");
    if (raw == nullptr)
    {
        return std::chrono::seconds{kDefaultSeconds};
    }
    const long parsed = std::strtol(raw, nullptr, 10);
    if (parsed <= 0)
    {
        return std::chrono::seconds{kDefaultSeconds};
    }
    return std::chrono::seconds{parsed};
}

mt::BatchOptions HammerBatchOpts(std::uint32_t batch_size)
{
    mt::BatchOptions opts;
    opts.max_queue_size = 4096;
    opts.max_export_batch_size = batch_size;
    opts.schedule_delay = kScheduleDelay;
    opts.drop_policy = mt::DropPolicy::DropOldest;
    return opts;
}

/// Everything under hammer, plus the borrowed BSP the provider needs to be
/// able to retune it.
struct Rig
{
    std::unique_ptr<mts::SdkProvider> provider;
};

Rig MakeRig()
{
    auto exporter = std::make_unique<mtmk::MockExporter>();
    auto* const exporter_ptr = exporter.get();
    auto processor = std::make_unique<mts::BatchSpanProcessor>(
        exporter_ptr, std::make_shared<const mt::Resource>(), HammerBatchOpts(64));
    auto* const bsp = processor.get();

    return Rig{.provider = std::make_unique<mts::SdkProvider>(mts::SdkProviderArgs{
                   .diagnostics = std::make_unique<mts::DiagnosticsCounters>(),
                   .encoder = nullptr,
                   .auth = nullptr,
                   .transport = std::make_unique<mtmk::MockTransport>(),
                   .codec = nullptr,
                   .exporter = std::move(exporter),
                   .processor = std::move(processor),
                   .batch_span_processor = bsp,
                   .resource = std::make_shared<mt::Resource>(),
                   // ParentBased over a ratio sampler: the shape SetSamplerRatio
                   // has to walk, and the one an operator actually runs.
                   .sampler = mt::MakeParentBasedSampler(mt::MakeTraceIdRatioSampler(0.5)),
                   .span_limits = {},
                   .connect_opts = {},
                   .metric_exporter = std::make_unique<mtmk::MockMetricExporter>(),
                   .metric_interval = kMetricInterval,
                   .log_exporter = std::make_unique<mtmk::MockLogExporter>(),
                   .log_batch_opts = HammerBatchOpts(64),
               })};
}

/// One worker: run `body` until `stop`, counting turns.
void RunUntil(const std::atomic<bool>& stop,
              std::atomic<std::uint64_t>& turns,
              const std::function<void()>& body)
{
    while (!stop.load(std::memory_order_relaxed))
    {
        body();
        turns.fetch_add(1, std::memory_order_relaxed);
    }
}

void EmitSpans(mt::Tracer& tracer, const std::atomic<bool>& stop, std::atomic<std::uint64_t>& turns)
{
    RunUntil(stop,
             turns,
             [&tracer]
             {
                 const mt::SpanHandle span = tracer.StartSpan("hammer.span");
                 span->SetAttribute("hammer.k", std::string{"v"});
                 span->End();
             });
}

void EmitLogs(mt::Logger& logger, const std::atomic<bool>& stop, std::atomic<std::uint64_t>& turns)
{
    RunUntil(stop,
             turns,
             [&logger]
             {
                 mt::LogRecord record;
                 record.body = std::string{"hammer"};
                 logger.Emit(std::move(record));
                 // The internal log path is a reader of the level knob, and
                 // nothing else in this test touches it.
                 mt::internal::LogImpl(mt::LogLevel::Warn, "hammer diagnostic");
             });
}

void RecordMetrics(mt::Counter<std::int64_t>& counter,
                   mt::Provider& provider,
                   const std::atomic<bool>& stop,
                   std::atomic<std::uint64_t>& turns)
{
    RunUntil(stop,
             turns,
             [&counter, &provider]
             {
                 counter.Add(1, {});
                 // Drives a collect+export cycle on the caller thread, racing
                 // the reader thread's own cycle and SetMetricInterval.
                 (void)provider.ForceFlush(50ms);
             });
}

void DriveBatchOptions(mt::Provider& provider,
                       const std::atomic<bool>& stop,
                       std::atomic<std::uint64_t>& turns)
{
    std::uint32_t size = 1;
    RunUntil(stop,
             turns,
             [&provider, &size]
             {
                 size = (size % 128U) + 1U;
                 (void)provider.SetBatchOptions(HammerBatchOpts(size));
             });
}

void DriveMetricInterval(mt::Provider& provider,
                         const std::atomic<bool>& stop,
                         std::atomic<std::uint64_t>& turns)
{
    int step = 0;
    RunUntil(stop,
             turns,
             [&provider, &step]
             {
                 step = (step % 8) + 1;
                 (void)provider.SetMetricInterval(std::chrono::milliseconds{step});
             });
}

void DriveSamplerRatio(mt::Provider& provider,
                       const std::atomic<bool>& stop,
                       std::atomic<std::uint64_t>& turns)
{
    int step = 0;
    RunUntil(stop,
             turns,
             [&provider, &step]
             {
                 step = (step + 1) % 101;
                 (void)provider.SetSamplerRatio(static_cast<double>(step) / 100.0);
             });
}

void DriveLogLevel(mt::Provider& provider,
                   const std::atomic<bool>& stop,
                   std::atomic<std::uint64_t>& turns)
{
    int step = 0;
    RunUntil(stop,
             turns,
             [&provider, &step]
             {
                 step = (step + 1) % 5;
                 (void)provider.SetLogLevel(static_cast<mt::LogLevel>(step));
             });
}

/// Reading the live sampler's description concurrently with a retune is the
/// half of the contract the append-only description store exists for: a view
/// handed out before a retune must stay readable after it (ICP 0026 §5).
void ReadSamplerDescription(const mt::internal::ISampler& sampler,
                            const std::atomic<bool>& stop,
                            std::atomic<std::uint64_t>& turns)
{
    RunUntil(stop,
             turns,
             [&sampler]
             {
                 const std::string_view view = sampler.Description();
                 volatile std::size_t sink = view.size();
                 (void)sink;
             });
}

TEST(HotReloadHammer, AllFourSettersRaceEveryHotPathReader)
{
    Rig rig = MakeRig();
    mt::Provider& provider = *rig.provider;

    const std::shared_ptr<mt::Tracer> tracer = provider.GetTracer("hammer", "1.0");
    const std::shared_ptr<mt::Logger> logger = provider.GetLogger("hammer", "1.0");
    const std::shared_ptr<mt::Meter> meter = provider.GetMeter("hammer", "1.0");
    ASSERT_NE(tracer, nullptr);
    ASSERT_NE(logger, nullptr);
    ASSERT_NE(meter, nullptr);
    const std::shared_ptr<mt::Counter<std::int64_t>> counter =
        meter->CreateCounter<std::int64_t>("hammer.counter");
    ASSERT_NE(counter, nullptr);

    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> spans{0};
    std::atomic<std::uint64_t> logs{0};
    std::atomic<std::uint64_t> metrics{0};
    std::atomic<std::uint64_t> batch_calls{0};
    std::atomic<std::uint64_t> interval_calls{0};
    std::atomic<std::uint64_t> ratio_calls{0};
    std::atomic<std::uint64_t> level_calls{0};

    std::vector<std::thread> threads;
    threads.emplace_back(EmitSpans, std::ref(*tracer), std::cref(stop), std::ref(spans));
    threads.emplace_back(EmitSpans, std::ref(*tracer), std::cref(stop), std::ref(spans));
    threads.emplace_back(EmitLogs, std::ref(*logger), std::cref(stop), std::ref(logs));
    threads.emplace_back(
        RecordMetrics, std::ref(*counter), std::ref(provider), std::cref(stop), std::ref(metrics));
    threads.emplace_back(
        DriveBatchOptions, std::ref(provider), std::cref(stop), std::ref(batch_calls));
    threads.emplace_back(
        DriveMetricInterval, std::ref(provider), std::cref(stop), std::ref(interval_calls));
    threads.emplace_back(
        DriveSamplerRatio, std::ref(provider), std::cref(stop), std::ref(ratio_calls));
    threads.emplace_back(DriveLogLevel, std::ref(provider), std::cref(stop), std::ref(level_calls));

    std::this_thread::sleep_for(HammerBudget());
    stop.store(true, std::memory_order_relaxed);
    for (std::thread& thread : threads)
    {
        thread.join();
    }

    // Not a behavioural assertion — a liveness one. A hammer whose threads
    // never ran would report a clean TSAN run and prove nothing.
    EXPECT_GT(spans.load(), std::uint64_t{0});
    EXPECT_GT(logs.load(), std::uint64_t{0});
    EXPECT_GT(metrics.load(), std::uint64_t{0});
    EXPECT_GT(batch_calls.load(), std::uint64_t{0});
    EXPECT_GT(interval_calls.load(), std::uint64_t{0});
    EXPECT_GT(ratio_calls.load(), std::uint64_t{0});
    EXPECT_GT(level_calls.load(), std::uint64_t{0});

    EXPECT_NE(provider.Shutdown(5s), mt::Status::Failed);
    EXPECT_TRUE(mt::internal::SetMinLogLevel(mt::LogLevel::Info));
}

TEST(HotReloadHammer, SamplerDescriptionReadsRaceRetunes)
{
    Rig rig = MakeRig();
    mt::Provider& provider = *rig.provider;
    const std::shared_ptr<mt::Tracer> tracer = provider.GetTracer("hammer", "1.0");
    ASSERT_NE(tracer, nullptr);

    // The live sampler object, reached the way SdkTracer reaches it: borrowed
    // and never reassigned, which is the whole reason the ratio-only design is
    // safe (ICP 0026 "Deferred").
    const mt::SamplerHandle probe = mt::MakeParentBasedSampler(mt::MakeTraceIdRatioSampler(0.5));

    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> reads{0};
    std::atomic<std::uint64_t> writes{0};

    std::vector<std::thread> threads;
    threads.emplace_back(
        ReadSamplerDescription, std::cref(*probe.Get()), std::cref(stop), std::ref(reads));
    threads.emplace_back(
        ReadSamplerDescription, std::cref(*probe.Get()), std::cref(stop), std::ref(reads));
    threads.emplace_back(
        [&probe, &stop, &writes]
        {
            int step = 0;
            while (!stop.load(std::memory_order_relaxed))
            {
                step = (step + 1) % 101;
                (void)probe.Get()->TrySetRatio(static_cast<double>(step) / 100.0);
                writes.fetch_add(1, std::memory_order_relaxed);
            }
        });

    std::this_thread::sleep_for(HammerBudget());
    stop.store(true, std::memory_order_relaxed);
    for (std::thread& thread : threads)
    {
        thread.join();
    }

    EXPECT_GT(reads.load(), std::uint64_t{0});
    EXPECT_GT(writes.load(), std::uint64_t{0});
    EXPECT_NE(provider.Shutdown(5s), mt::Status::Failed);
}

}  // namespace
