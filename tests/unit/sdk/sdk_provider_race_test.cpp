// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Provider is documented `@threadsafety Thread-safe` (provider.hpp). It was
// not: GetMeter/GetLogger published m_metric_reader/m_log_processor under
// m_meter_mu/m_logger_mu, while ForceFlush and Shutdown read those same
// unique_ptrs with no lock at all. Concurrent GetMeter + ForceFlush was a data
// race, and a use-after-check if the pointer was published between the null
// test and the dereference.
//
// These tests are only meaningful under TSAN (-DMICROTEL_SANITIZER=tsan);
// without it they exercise the paths but cannot observe the race.

#include "microtel/internal/sampler.hpp"
#include "microtel/provider.hpp"
#include "microtel/resource.hpp"
#include "microtel/sampler.hpp"

#include "mocks/mock_exporter.hpp"
#include "mocks/mock_log_exporter.hpp"
#include "mocks/mock_metric_exporter.hpp"
#include "mocks/mock_span_processor.hpp"
#include "mocks/mock_transport.hpp"
#include "sdk/diagnostics_counters.hpp"
#include "sdk/sdk_provider.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

namespace mt = microtel;
namespace mts = microtel::sdk;
namespace mtmk = microtel::testing;

namespace
{

// The racing write happens exactly once per provider: GetMeter/GetLogger
// lazily construct the reader/processor on first call and never write again.
// So a long loop on one provider races nothing after iteration 1 -- the window
// is a single store. To make it observable, build a fresh provider per
// iteration and have both threads hit it at once, so the one write is
// contended by construction.
constexpr int kProviders = 300;

std::unique_ptr<mts::SdkProvider> MakeProvider()
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
        // Both exporters are required: GetMeter only builds m_metric_reader
        // when a metric exporter exists, and GetLogger only builds
        // m_log_processor when a log exporter exists. Without them the racing
        // write never happens and the test silently exercises nothing.
        .metric_exporter = std::make_unique<mtmk::MockMetricExporter>(),
        .log_exporter = std::make_unique<mtmk::MockLogExporter>(),
    });
}

// `create` performs the single lazy-init write; `flush` reads the same member.
// Before the fix the read was unsynchronised, so TSAN reports a write/read
// race on the unique_ptr.
void SpinThenRun(const std::atomic<bool>& go, const std::function<void()>& work)
{
    while (!go.load(std::memory_order_acquire))
    {
    }
    work();
}

void RaceOnce(const std::function<void(mts::SdkProvider&)>& create)
{
    auto provider = MakeProvider();
    std::atomic<bool> go{false};

    std::thread creator([&] { SpinThenRun(go, [&] { create(*provider); }); });
    std::thread flusher(
        [&]
        { SpinThenRun(go, [&] { (void)provider->ForceFlush(std::chrono::milliseconds(1)); }); });

    go.store(true, std::memory_order_release);
    creator.join();
    flusher.join();
}

void RaceCreateAgainstFlush(const std::function<void(mts::SdkProvider&)>& create)
{
    for (int i = 0; i < kProviders; ++i)
    {
        RaceOnce(create);
    }
}

TEST(SdkProviderRaceTest, GetLoggerConcurrentWithForceFlushIsRaceFree)
{
    RaceCreateAgainstFlush([](mts::SdkProvider& p) { (void)p.GetLogger("racy", "1.0"); });
    SUCCEED();
}

TEST(SdkProviderRaceTest, GetMeterConcurrentWithForceFlushIsRaceFree)
{
    RaceCreateAgainstFlush([](mts::SdkProvider& p) { (void)p.GetMeter("racy", "1.0"); });
    SUCCEED();
}

}  // namespace
