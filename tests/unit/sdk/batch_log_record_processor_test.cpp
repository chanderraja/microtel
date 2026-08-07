// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for BatchLogRecordProcessor — M14 L4.2 (docs/logs-design.md §2).
// Queue + worker + batch, ForceFlush / Shutdown, drop policy, per-scope
// grouping into LogBatchHandles, and concurrent OnEmit (TSAN target).

#include "sdk/batch_log_record_processor.hpp"

#include "microtel/internal/batch.hpp"
#include "microtel/log_record.hpp"
#include "microtel/resource.hpp"
#include "microtel/sdk_builder.hpp"
#include "microtel/status.hpp"

#include "fakes/fake_log_exporter.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace mt = microtel;
namespace mts = microtel::sdk;
namespace mti = microtel::internal;
namespace mtfk = microtel::testing;

namespace
{

std::unique_ptr<mts::BatchLogRecordProcessor> MakeBlp(mtfk::FakeLogExporter& exp,
                                                      mt::BatchOptions opts = mt::BatchOptions{})
{
    auto resource = std::make_shared<const mt::Resource>();
    return std::make_unique<mts::BatchLogRecordProcessor>(&exp, std::move(resource), opts);
}

void Emit(mts::BatchLogRecordProcessor& blp, std::string scope_name = "logger")
{
    blp.OnEmit(mt::LogRecord{},
               mti::InstrumentationScope{.name = std::move(scope_name), .version = ""});
}

void EmitN(mts::BatchLogRecordProcessor& blp, int count)
{
    for (int i = 0; i < count; ++i)
    {
        Emit(blp);
    }
}

std::size_t TotalRecords(const mtfk::FakeLogExporter& exp)
{
    std::size_t total = 0;
    for (const auto& handle : exp.exported)
    {
        total += handle.Records().size();
    }
    return total;
}

mt::BatchOptions ManualDrainOpts()
{
    mt::BatchOptions opts;
    opts.schedule_delay = std::chrono::hours(1);  // never fires by timer
    opts.max_export_batch_size = 512;
    return opts;
}

constexpr auto kTimeout = std::chrono::milliseconds(2000);
constexpr auto kShort = std::chrono::milliseconds(500);

TEST(BatchLogRecordProcessorTest, CreateDoesNotCrash)
{
    mtfk::FakeLogExporter exp;
    const auto blp = MakeBlp(exp);
    EXPECT_NE(blp, nullptr);
}

TEST(BatchLogRecordProcessorTest, ShutdownReturnsCompleted)
{
    mtfk::FakeLogExporter exp;
    auto blp = MakeBlp(exp);
    EXPECT_EQ(blp->Shutdown(kShort), mt::Status::Completed);
}

TEST(BatchLogRecordProcessorTest, ShutdownTwiceReturnsAlreadyShutDown)
{
    mtfk::FakeLogExporter exp;
    auto blp = MakeBlp(exp);
    (void)blp->Shutdown(kShort);
    EXPECT_EQ(blp->Shutdown(kShort), mt::Status::AlreadyShutDown);
}

TEST(BatchLogRecordProcessorTest, ForceFlushEmptyReturnsCompleted)
{
    mtfk::FakeLogExporter exp;
    auto blp = MakeBlp(exp);
    EXPECT_EQ(blp->ForceFlush(kShort), mt::Status::Completed);
    (void)blp->Shutdown(kShort);
}

TEST(BatchLogRecordProcessorTest, ForceFlushExportsQueuedRecords)
{
    mtfk::FakeLogExporter exp;
    auto blp = MakeBlp(exp, ManualDrainOpts());

    Emit(*blp);
    Emit(*blp);

    EXPECT_EQ(blp->ForceFlush(kTimeout), mt::Status::Completed);
    EXPECT_EQ(TotalRecords(exp), 2U);

    (void)blp->Shutdown(kShort);
}

TEST(BatchLogRecordProcessorTest, ForceFlushAfterShutdownReturnsAlreadyShutDown)
{
    mtfk::FakeLogExporter exp;
    auto blp = MakeBlp(exp);
    (void)blp->Shutdown(kShort);
    EXPECT_EQ(blp->ForceFlush(kShort), mt::Status::AlreadyShutDown);
}

TEST(BatchLogRecordProcessorTest, ShutdownExportsAllPendingRecords)
{
    mtfk::FakeLogExporter exp;
    auto blp = MakeBlp(exp, ManualDrainOpts());

    constexpr int kN = 5;
    for (int i = 0; i < kN; ++i)
    {
        Emit(*blp);
    }

    (void)blp->Shutdown(kTimeout);
    EXPECT_EQ(TotalRecords(exp), static_cast<std::size_t>(kN));
}

TEST(BatchLogRecordProcessorTest, BatchSizeTriggersExportMidSchedule)
{
    mt::BatchOptions opts = ManualDrainOpts();
    opts.max_export_batch_size = 3;

    mtfk::FakeLogExporter exp;
    auto blp = MakeBlp(exp, opts);

    Emit(*blp);
    Emit(*blp);
    Emit(*blp);  // reaching the batch size wakes the worker

    EXPECT_EQ(blp->ForceFlush(kTimeout), mt::Status::Completed);
    EXPECT_EQ(TotalRecords(exp), 3U);
    (void)blp->Shutdown(kShort);
}

TEST(BatchLogRecordProcessorTest, GroupsRecordsByScope)
{
    mtfk::FakeLogExporter exp;
    auto blp = MakeBlp(exp, ManualDrainOpts());

    Emit(*blp, "scope.a");
    Emit(*blp, "scope.a");
    Emit(*blp, "scope.b");

    EXPECT_EQ(blp->ForceFlush(kTimeout), mt::Status::Completed);

    std::size_t a_records = 0;
    std::size_t b_records = 0;
    for (const auto& handle : exp.exported)
    {
        if (handle.Scope().name == "scope.a")
        {
            a_records += handle.Records().size();
        }
        else if (handle.Scope().name == "scope.b")
        {
            b_records += handle.Records().size();
        }
    }
    EXPECT_EQ(a_records, 2U);
    EXPECT_EQ(b_records, 1U);

    (void)blp->Shutdown(kShort);
}

TEST(BatchLogRecordProcessorTest, DropNewestDropsIncomingWhenQueueFull)
{
    mt::BatchOptions opts = ManualDrainOpts();
    opts.max_queue_size = 2;
    opts.drop_policy = mt::DropPolicy::DropNewest;

    mtfk::FakeLogExporter exp;
    auto blp = MakeBlp(exp, opts);

    Emit(*blp);
    Emit(*blp);
    Emit(*blp);  // dropped — queue full

    (void)blp->Shutdown(kTimeout);
    EXPECT_EQ(TotalRecords(exp), 2U);
}

TEST(BatchLogRecordProcessorTest, DropOldestEvictsOldestWhenQueueFull)
{
    mt::BatchOptions opts = ManualDrainOpts();
    opts.max_queue_size = 2;
    opts.drop_policy = mt::DropPolicy::DropOldest;

    mtfk::FakeLogExporter exp;
    auto blp = MakeBlp(exp, opts);

    Emit(*blp);
    Emit(*blp);
    Emit(*blp);  // evicts the oldest — queue stays at 2

    (void)blp->Shutdown(kTimeout);
    EXPECT_EQ(TotalRecords(exp), 2U);
}

TEST(BatchLogRecordProcessorTest, ConcurrentOnEmitExportsEveryRecord)
{
    mt::BatchOptions opts = ManualDrainOpts();
    opts.max_queue_size = 100000;  // headroom so nothing is dropped

    mtfk::FakeLogExporter exp;
    auto blp = MakeBlp(exp, opts);

    constexpr int kThreads = 4;
    constexpr int kPerThread = 250;
    std::vector<std::thread> threads;
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t)
    {
        threads.emplace_back([&blp] { EmitN(*blp, kPerThread); });
    }
    for (auto& thread : threads)
    {
        thread.join();
    }

    (void)blp->Shutdown(std::chrono::milliseconds(5000));
    EXPECT_EQ(TotalRecords(exp), static_cast<std::size_t>(kThreads * kPerThread));
}

}  // namespace
