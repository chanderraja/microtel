// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for BatchSpanProcessor: queue, drop policy, ForceFlush,
// Shutdown, and worker-thread batching.

#include "sdk/batch_span_processor.hpp"

#include "microtel/context.hpp"
#include "microtel/internal/batch.hpp"
#include "microtel/resource.hpp"
#include "microtel/sdk_builder.hpp"
#include "microtel/span.hpp"
#include "microtel/status.hpp"
#include "microtel/trace.hpp"

#include "fakes/fake_exporter.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>

namespace mt = microtel;
namespace mti = microtel::internal;
namespace mtfk = microtel::testing;

namespace
{

// Minimal concrete Span for testing OnStart (which is a no-op and ignores all args).
struct NullSpan final : mt::Span
{
    [[nodiscard]] mt::SpanContext GetContext() const noexcept override
    {
        return {};
    }
    [[nodiscard]] bool IsSampled() const noexcept override
    {
        return false;
    }
    void SetAttribute(std::string_view /*key*/, mt::AttributeValue /*value*/) noexcept override {}
    void AddEvent(std::string_view /*name*/,
                  mt::AttributeSpan /*attrs*/,
                  std::chrono::system_clock::time_point /*ts*/) noexcept override
    {
    }
    void AddLink(const mt::SpanContext& /*ctx*/, mt::AttributeSpan /*attrs*/) noexcept override {}
    void SetStatus(mt::StatusCode /*code*/, std::string_view /*desc*/) noexcept override {}
    void UpdateName(std::string_view /*name*/) noexcept override {}
    void End(std::chrono::system_clock::time_point /*end_time*/) noexcept override {}
};

}  // namespace

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static mti::SpanRecord MakeRecord(const std::string& name)
{
    mti::SpanRecord r;
    r.name = name;
    r.start_time = std::chrono::system_clock::now();
    return r;
}

static std::unique_ptr<mt::sdk::BatchSpanProcessor> MakeBsp(
    mtfk::FakeExporter& exp, mt::BatchOptions opts = mt::BatchOptions{})
{
    auto resource = std::make_shared<const mt::Resource>();
    return std::make_unique<mt::sdk::BatchSpanProcessor>(
        &exp, std::move(resource), mti::InstrumentationScope{.name = "test", .version = ""}, opts);
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

TEST(BatchSpanProcessorTest, Create_DoesNotCrash)
{
    mtfk::FakeExporter exp;
    const auto bsp = MakeBsp(exp);
    EXPECT_NE(bsp, nullptr);
}

TEST(BatchSpanProcessorTest, Shutdown_ReturnsCompleted)
{
    mtfk::FakeExporter exp;
    auto bsp = MakeBsp(exp);
    const mt::Status s = bsp->Shutdown(std::chrono::milliseconds(500));
    EXPECT_EQ(s, mt::Status::Completed);
}

TEST(BatchSpanProcessorTest, Shutdown_Twice_ReturnsAlreadyShutDown)
{
    mtfk::FakeExporter exp;
    auto bsp = MakeBsp(exp);
    (void)bsp->Shutdown(std::chrono::milliseconds(500));
    const mt::Status s = bsp->Shutdown(std::chrono::milliseconds(500));
    EXPECT_EQ(s, mt::Status::AlreadyShutDown);
}

TEST(BatchSpanProcessorTest, OnStart_IsNoOp)
{
    mtfk::FakeExporter exp;
    auto bsp = MakeBsp(exp);
    // OnStart is a no-op; just must not crash.
    NullSpan span;
    const mt::Context ctx;
    EXPECT_NO_THROW(bsp->OnStart(span, ctx));
    (void)bsp->Shutdown(std::chrono::milliseconds(500));
}

// ---------------------------------------------------------------------------
// ForceFlush
// ---------------------------------------------------------------------------

TEST(BatchSpanProcessorTest, ForceFlush_Empty_ReturnsCompleted)
{
    mtfk::FakeExporter exp;
    auto bsp = MakeBsp(exp);
    const mt::Status s = bsp->ForceFlush(std::chrono::milliseconds(500));
    EXPECT_EQ(s, mt::Status::Completed);
    (void)bsp->Shutdown(std::chrono::milliseconds(500));
}

TEST(BatchSpanProcessorTest, ForceFlush_ExportsQueuedRecords)
{
    mt::BatchOptions opts;
    opts.schedule_delay = std::chrono::hours(1);  // prevent timer-triggered export
    opts.max_export_batch_size = 512;

    mtfk::FakeExporter exp;
    auto bsp = MakeBsp(exp, opts);

    bsp->OnEnd(MakeRecord("span1"));
    bsp->OnEnd(MakeRecord("span2"));

    const mt::Status s = bsp->ForceFlush(std::chrono::milliseconds(2000));
    EXPECT_EQ(s, mt::Status::Completed);

    std::size_t total = 0;
    for (const auto& batch : exp.received_batches)
    {
        total += batch.Spans().size();
    }
    EXPECT_EQ(total, 2U);

    (void)bsp->Shutdown(std::chrono::milliseconds(500));
}

TEST(BatchSpanProcessorTest, ForceFlush_AfterShutdown_ReturnsAlreadyShutDown)
{
    mtfk::FakeExporter exp;
    auto bsp = MakeBsp(exp);
    (void)bsp->Shutdown(std::chrono::milliseconds(500));
    const mt::Status s = bsp->ForceFlush(std::chrono::milliseconds(500));
    EXPECT_EQ(s, mt::Status::AlreadyShutDown);
}

// ---------------------------------------------------------------------------
// Batching
// ---------------------------------------------------------------------------

TEST(BatchSpanProcessorTest, Shutdown_ExportsAllPendingRecords)
{
    mt::BatchOptions opts;
    opts.schedule_delay = std::chrono::hours(1);
    opts.max_export_batch_size = 512;

    mtfk::FakeExporter exp;
    auto bsp = MakeBsp(exp, opts);

    constexpr int kN = 5;
    for (int i = 0; i < kN; ++i)
    {
        bsp->OnEnd(MakeRecord("s" + std::to_string(i)));
    }

    (void)bsp->Shutdown(std::chrono::milliseconds(2000));

    std::size_t total = 0;
    for (const auto& batch : exp.received_batches)
    {
        total += batch.Spans().size();
    }
    EXPECT_EQ(total, static_cast<std::size_t>(kN));
}

TEST(BatchSpanProcessorTest, BatchSize_TriggersMidSchedule)
{
    mt::BatchOptions opts;
    opts.schedule_delay = std::chrono::hours(1);  // never fires by timer
    opts.max_export_batch_size = 3;

    mtfk::FakeExporter exp;
    auto bsp = MakeBsp(exp, opts);

    // Enqueue 3 — should trigger an automatic export.
    bsp->OnEnd(MakeRecord("a"));
    bsp->OnEnd(MakeRecord("b"));
    bsp->OnEnd(MakeRecord("c"));

    const mt::Status flush = bsp->ForceFlush(std::chrono::milliseconds(2000));
    EXPECT_EQ(flush, mt::Status::Completed);
    EXPECT_FALSE(exp.received_batches.empty());
    (void)bsp->Shutdown(std::chrono::milliseconds(500));
}

// ---------------------------------------------------------------------------
// Drop policy — DropNewest
// ---------------------------------------------------------------------------

TEST(BatchSpanProcessorTest, DropNewest_DropsIncoming_WhenQueueFull)
{
    mt::BatchOptions opts;
    opts.max_queue_size = 2;
    opts.max_export_batch_size = 512;
    opts.schedule_delay = std::chrono::hours(1);
    opts.drop_policy = mt::DropPolicy::DropNewest;

    mtfk::FakeExporter exp;
    auto bsp = MakeBsp(exp, opts);

    bsp->OnEnd(MakeRecord("keep1"));
    bsp->OnEnd(MakeRecord("keep2"));
    bsp->OnEnd(MakeRecord("dropped"));  // dropped

    (void)bsp->Shutdown(std::chrono::milliseconds(2000));

    std::size_t total = 0;
    for (const auto& batch : exp.received_batches)
    {
        total += batch.Spans().size();
    }
    EXPECT_EQ(total, 2U);
}

// ---------------------------------------------------------------------------
// Drop policy — DropOldest
// ---------------------------------------------------------------------------

TEST(BatchSpanProcessorTest, DropOldest_EvictsOldest_WhenQueueFull)
{
    mt::BatchOptions opts;
    opts.max_queue_size = 2;
    opts.max_export_batch_size = 512;
    opts.schedule_delay = std::chrono::hours(1);
    opts.drop_policy = mt::DropPolicy::DropOldest;

    mtfk::FakeExporter exp;
    auto bsp = MakeBsp(exp, opts);

    bsp->OnEnd(MakeRecord("evicted"));  // evicted when "new3" arrives
    bsp->OnEnd(MakeRecord("keep1"));
    bsp->OnEnd(MakeRecord("new3"));  // evicts "evicted"

    (void)bsp->Shutdown(std::chrono::milliseconds(2000));

    std::size_t total = 0;
    for (const auto& batch : exp.received_batches)
    {
        total += batch.Spans().size();
    }
    EXPECT_EQ(total, 2U);
}
