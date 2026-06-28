// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Behavioural tests for PeriodicExportingMetricReader — the IMetricReader impl
// that drives collect+export on a background thread at a configurable interval.
//
// Contract under test:
//  - Collect() immediately does one synchronous collect+export cycle.
//  - ForceFlush() immediately does one synchronous collect+export cycle.
//  - Background thread fires at the configured interval.
//  - Shutdown() stops the background thread and delegates to the exporter.
//  - Collect() / ForceFlush() after Shutdown() return AlreadyShutDown.
//  - Double Shutdown() returns AlreadyShutDown on the second call.

#include "sdk/periodic_exporting_metric_reader.hpp"

#include "microtel/internal/metric_batch.hpp"
#include "microtel/internal/metric_exporter.hpp"
#include "microtel/internal/metric_producer.hpp"
#include "microtel/resource.hpp"
#include "microtel/status.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace mts = microtel::sdk;
namespace mti = microtel::internal;
namespace mt = microtel;

using namespace std::chrono_literals;

namespace
{

// ── Thread-safe fakes ─────────────────────────────────────────────────────────
// Counters are atomic so the background thread and test thread can read/write
// concurrently without a data race.

class FakeMetricProducer : public mti::IMetricProducer
{
public:
    explicit FakeMetricProducer(std::vector<mti::MetricBatchHandle> handles = {}) noexcept
        : m_handles(std::move(handles))
    {
    }

    [[nodiscard]] std::vector<mti::MetricBatchHandle> Collect(
        mti::AggregationTemporality temporality = mti::AggregationTemporality::Cumulative) override
    {
        m_collect_count.fetch_add(1, std::memory_order_relaxed);
        m_last_temporality.store(static_cast<int>(temporality), std::memory_order_relaxed);
        return std::move(m_handles);
    }

    [[nodiscard]] int CollectCount() const noexcept
    {
        return m_collect_count.load(std::memory_order_relaxed);
    }

    [[nodiscard]] mti::AggregationTemporality LastTemporality() const noexcept
    {
        return static_cast<mti::AggregationTemporality>(
            m_last_temporality.load(std::memory_order_relaxed));
    }

private:
    std::vector<mti::MetricBatchHandle> m_handles;
    std::atomic<int> m_collect_count{0};
    std::atomic<int> m_last_temporality{static_cast<int>(mti::AggregationTemporality::Cumulative)};
};

class FakeMetricExporter : public mti::IMetricExporter
{
public:
    explicit FakeMetricExporter(
        mti::ExportResult export_result = mti::ExportResult::Success) noexcept
        : m_export_result(export_result)
    {
    }

    // NOLINTNEXTLINE(cppcoreguidelines-rvalue-reference-param-not-moved)
    [[nodiscard]] mti::ExportResult Export(mti::MetricBatchHandle&& /*batch*/) noexcept override
    {
        m_export_count.fetch_add(1, std::memory_order_relaxed);
        return m_export_result;
    }

    [[nodiscard]] mt::Status ForceFlush(std::chrono::milliseconds /*timeout*/) noexcept override
    {
        m_flush_count.fetch_add(1, std::memory_order_relaxed);
        return m_flush_result;
    }

    [[nodiscard]] mt::Status Shutdown(std::chrono::milliseconds /*timeout*/) noexcept override
    {
        m_shutdown_count.fetch_add(1, std::memory_order_relaxed);
        return m_shutdown_result;
    }

    [[nodiscard]] int ExportCount() const noexcept
    {
        return m_export_count.load(std::memory_order_relaxed);
    }
    [[nodiscard]] int FlushCount() const noexcept
    {
        return m_flush_count.load(std::memory_order_relaxed);
    }
    [[nodiscard]] int ShutdownCount() const noexcept
    {
        return m_shutdown_count.load(std::memory_order_relaxed);
    }

    void SetExportResult(mti::ExportResult r) noexcept
    {
        m_export_result = r;
    }
    void SetFlushResult(mt::Status s) noexcept
    {
        m_flush_result = s;
    }
    void SetShutdownResult(mt::Status s) noexcept
    {
        m_shutdown_result = s;
    }

private:
    mti::ExportResult m_export_result;
    mt::Status m_flush_result{mt::Status::Completed};
    mt::Status m_shutdown_result{mt::Status::Completed};
    std::atomic<int> m_export_count{0};
    std::atomic<int> m_flush_count{0};
    std::atomic<int> m_shutdown_count{0};
};

}  // namespace

// ── Collect — happy path ──────────────────────────────────────────────────────

TEST(PeriodicExportingMetricReaderTest, CollectCallsProducerOnce)
{
    FakeMetricProducer producer;
    FakeMetricExporter exporter;
    mts::PeriodicExportingMetricReader reader{producer, exporter, 60'000ms};

    EXPECT_EQ(reader.Collect(100ms), mt::Status::Completed);
    // Background thread has not fired yet (60s interval).
    EXPECT_GE(producer.CollectCount(), 1);  // at least 1 from Collect()
}

TEST(PeriodicExportingMetricReaderTest, CollectReturnsCompleted)
{
    FakeMetricProducer producer;
    FakeMetricExporter exporter;
    mts::PeriodicExportingMetricReader reader{producer, exporter, 60'000ms};

    EXPECT_EQ(reader.Collect(100ms), mt::Status::Completed);
}

TEST(PeriodicExportingMetricReaderTest, CollectReturnsFailedWhenExportFails)
{
    std::vector<mti::MetricBatchHandle> handles;
    handles.push_back(mti::MetricBatchHandle{
        {},
        std::make_shared<const mt::Resource>(),
        mti::InstrumentationScope{.name = "lib", .version = "1.0"},
    });
    FakeMetricProducer producer{std::move(handles)};
    FakeMetricExporter exporter{mti::ExportResult::Failure};
    mts::PeriodicExportingMetricReader reader{producer, exporter, 60'000ms};

    EXPECT_EQ(reader.Collect(100ms), mt::Status::Failed);
}

// ── ForceFlush ────────────────────────────────────────────────────────────────

TEST(PeriodicExportingMetricReaderTest, ForceFlushCallsProducerAndExporter)
{
    FakeMetricProducer producer;
    FakeMetricExporter exporter;
    mts::PeriodicExportingMetricReader reader{producer, exporter, 60'000ms};

    EXPECT_EQ(reader.ForceFlush(100ms), mt::Status::Completed);
    EXPECT_GE(producer.CollectCount(), 1);
    // ForceFlush also delegates to exporter.ForceFlush
    EXPECT_GE(exporter.FlushCount(), 1);
}

TEST(PeriodicExportingMetricReaderTest, ForceFlushPropagatesExporterResult)
{
    FakeMetricProducer producer;
    FakeMetricExporter exporter;
    exporter.SetFlushResult(mt::Status::TimedOut);
    mts::PeriodicExportingMetricReader reader{producer, exporter, 60'000ms};

    EXPECT_EQ(reader.ForceFlush(100ms), mt::Status::TimedOut);
}

// ── Shutdown ──────────────────────────────────────────────────────────────────

TEST(PeriodicExportingMetricReaderTest, ShutdownDelegatesToExporter)
{
    FakeMetricProducer producer;
    FakeMetricExporter exporter;
    mts::PeriodicExportingMetricReader reader{producer, exporter, 60'000ms};

    EXPECT_EQ(reader.Shutdown(100ms), mt::Status::Completed);
    EXPECT_EQ(exporter.ShutdownCount(), 1);
}

TEST(PeriodicExportingMetricReaderTest, ShutdownPropagatesExporterResult)
{
    FakeMetricProducer producer;
    FakeMetricExporter exporter;
    exporter.SetShutdownResult(mt::Status::Failed);
    mts::PeriodicExportingMetricReader reader{producer, exporter, 60'000ms};

    EXPECT_EQ(reader.Shutdown(100ms), mt::Status::Failed);
}

TEST(PeriodicExportingMetricReaderTest, DoubleShutdownReturnsAlreadyShutDown)
{
    FakeMetricProducer producer;
    FakeMetricExporter exporter;
    mts::PeriodicExportingMetricReader reader{producer, exporter, 60'000ms};

    EXPECT_EQ(reader.Shutdown(100ms), mt::Status::Completed);
    EXPECT_EQ(reader.Shutdown(100ms), mt::Status::AlreadyShutDown);
    EXPECT_EQ(exporter.ShutdownCount(), 1);  // called only once
}

// ── Post-shutdown guards ──────────────────────────────────────────────────────

TEST(PeriodicExportingMetricReaderTest, CollectAfterShutdownReturnsAlreadyShutDown)
{
    FakeMetricProducer producer;
    FakeMetricExporter exporter;
    mts::PeriodicExportingMetricReader reader{producer, exporter, 60'000ms};

    (void)reader.Shutdown(100ms);
    EXPECT_EQ(reader.Collect(100ms), mt::Status::AlreadyShutDown);
}

TEST(PeriodicExportingMetricReaderTest, ForceFlushAfterShutdownReturnsAlreadyShutDown)
{
    FakeMetricProducer producer;
    FakeMetricExporter exporter;
    mts::PeriodicExportingMetricReader reader{producer, exporter, 60'000ms};

    (void)reader.Shutdown(100ms);
    EXPECT_EQ(reader.ForceFlush(100ms), mt::Status::AlreadyShutDown);
}

// ── Background thread fires at interval ──────────────────────────────────────

TEST(PeriodicExportingMetricReaderTest, BackgroundThreadFiresAtConfiguredInterval)
{
    FakeMetricProducer producer;
    FakeMetricExporter exporter;
    // Very short interval so the background thread fires quickly in CI.
    mts::PeriodicExportingMetricReader reader{producer, exporter, 5ms};

    // Wait long enough for at least 3 background cycles.
    std::this_thread::sleep_for(50ms);
    (void)reader.Shutdown(500ms);

    EXPECT_GE(producer.CollectCount(), 3);
}

// ── Destructor shuts down cleanly ─────────────────────────────────────────────

TEST(PeriodicExportingMetricReaderTest, DestructorShutdownDoesNotCrash)
{
    FakeMetricProducer producer;
    FakeMetricExporter exporter;
    {
        mts::PeriodicExportingMetricReader reader{producer, exporter, 60'000ms};
        // Destructor runs here — must not crash or block indefinitely.
    }
}

// ── Temporality threading ─────────────────────────────────────────────────────

TEST(PeriodicExportingMetricReaderTest, Collect_DefaultTemporality_PassesCumulativeToProducer)
{
    FakeMetricProducer producer;
    FakeMetricExporter exporter;
    mts::PeriodicExportingMetricReader reader{
        producer, exporter, 60'000ms, mti::AggregationTemporality::Cumulative};

    EXPECT_EQ(reader.Collect(100ms), mt::Status::Completed);
    EXPECT_EQ(producer.LastTemporality(), mti::AggregationTemporality::Cumulative);
}

TEST(PeriodicExportingMetricReaderTest, Collect_WithDeltaTemporality_PassesDeltaToProducer)
{
    FakeMetricProducer producer;
    FakeMetricExporter exporter;
    mts::PeriodicExportingMetricReader reader{
        producer, exporter, 60'000ms, mti::AggregationTemporality::Delta};

    EXPECT_EQ(reader.Collect(100ms), mt::Status::Completed);
    EXPECT_EQ(producer.LastTemporality(), mti::AggregationTemporality::Delta);
}
