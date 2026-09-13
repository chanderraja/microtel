// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for BatchSpanProcessor: queue, drop policy, ForceFlush,
// Shutdown, and worker-thread batching.

#include "sdk/batch_span_processor.hpp"

#include "microtel/context.hpp"
#include "microtel/internal/batch.hpp"
#include "microtel/provider.hpp"
#include "microtel/resource.hpp"
#include "microtel/sdk_builder.hpp"
#include "microtel/span.hpp"
#include "microtel/status.hpp"
#include "microtel/trace.hpp"

#include "fakes/fake_diagnostics_sink.hpp"
#include "fakes/fake_exporter.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

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
    mtfk::FakeExporter& exp,
    mt::BatchOptions opts = mt::BatchOptions{},
    mtfk::FakeDiagnosticsSink* sink = nullptr,
    std::uint32_t max_record_bytes = mt::MemoryLimitOptions{}.max_record_bytes)
{
    auto resource = std::make_shared<const mt::Resource>();
    return std::make_unique<mt::sdk::BatchSpanProcessor>(
        &exp, std::move(resource), opts, max_record_bytes, sink);
}

static std::uint64_t DropCount(const mtfk::FakeDiagnosticsSink& sink, mt::DropReason reason)
{
    return sink.drop_counters.at(static_cast<std::size_t>(reason));
}

// Ends one span on @p bsp as the tracer named @p scope_name would have.
static void EndSpan(mt::sdk::BatchSpanProcessor& bsp,
                    const std::string& name,
                    const std::string& scope_name = "test",
                    const std::string& scope_version = "")
{
    bsp.OnEnd(MakeRecord(name),
              mti::InstrumentationScope{.name = scope_name, .version = scope_version});
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

    EndSpan(*bsp, "span1");
    EndSpan(*bsp, "span2");

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
        EndSpan(*bsp, "s" + std::to_string(i));
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
    EndSpan(*bsp, "a");
    EndSpan(*bsp, "b");
    EndSpan(*bsp, "c");

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

    EndSpan(*bsp, "keep1");
    EndSpan(*bsp, "keep2");
    EndSpan(*bsp, "dropped");  // dropped

    (void)bsp->Shutdown(std::chrono::milliseconds(2000));

    std::size_t total = 0;
    for (const auto& batch : exp.received_batches)
    {
        total += batch.Spans().size();
    }
    EXPECT_EQ(total, 2U);
}

// ---------------------------------------------------------------------------
// Drop accounting — issue #169. The queue enforced its capacity but nothing
// counted the loss, so GetExporterHealth() could not tell "queue full" from
// "endpoint rejecting us". OnEnd runs on the calling thread here, which is
// this test's thread, so the non-atomic FakeDiagnosticsSink is safe to read.
// ---------------------------------------------------------------------------

TEST(BatchSpanProcessorTest, Diagnostics_DropNewest_CountsQueueFull)
{
    mt::BatchOptions opts;
    opts.max_queue_size = 2;
    opts.max_export_batch_size = 512;
    opts.schedule_delay = std::chrono::hours(1);
    opts.drop_policy = mt::DropPolicy::DropNewest;

    mtfk::FakeExporter exp;
    mtfk::FakeDiagnosticsSink sink;
    auto bsp = MakeBsp(exp, opts, &sink);

    EndSpan(*bsp, "keep1");
    EndSpan(*bsp, "keep2");
    EndSpan(*bsp, "dropped");

    EXPECT_EQ(DropCount(sink, mt::DropReason::QueueFull), 1U);
    EXPECT_EQ(DropCount(sink, mt::DropReason::PostShutdown), 0U);
    (void)bsp->Shutdown(std::chrono::milliseconds(2000));
}

TEST(BatchSpanProcessorTest, Diagnostics_DropOldest_CountsQueueFull)
{
    mt::BatchOptions opts;
    opts.max_queue_size = 2;
    opts.max_export_batch_size = 512;
    opts.schedule_delay = std::chrono::hours(1);
    opts.drop_policy = mt::DropPolicy::DropOldest;

    mtfk::FakeExporter exp;
    mtfk::FakeDiagnosticsSink sink;
    auto bsp = MakeBsp(exp, opts, &sink);

    EndSpan(*bsp, "evicted");
    EndSpan(*bsp, "keep1");
    EndSpan(*bsp, "new3");

    // Evicting the oldest loses exactly one span, the same as refusing the
    // newest — the policy picks which span, not how many.
    EXPECT_EQ(DropCount(sink, mt::DropReason::QueueFull), 1U);
    (void)bsp->Shutdown(std::chrono::milliseconds(2000));
}

TEST(BatchSpanProcessorTest, Diagnostics_OnEndAfterShutdown_CountsPostShutdown)
{
    mtfk::FakeExporter exp;
    mtfk::FakeDiagnosticsSink sink;
    auto bsp = MakeBsp(exp, mt::BatchOptions{}, &sink);

    ASSERT_EQ(bsp->Shutdown(std::chrono::milliseconds(2000)), mt::Status::Completed);
    EndSpan(*bsp, "too-late");

    EXPECT_EQ(DropCount(sink, mt::DropReason::PostShutdown), 1U);
    EXPECT_EQ(DropCount(sink, mt::DropReason::QueueFull), 0U);
}

TEST(BatchSpanProcessorTest, Diagnostics_NullSink_IsNotDereferenced)
{
    mt::BatchOptions opts;
    opts.max_queue_size = 1;
    opts.schedule_delay = std::chrono::hours(1);

    mtfk::FakeExporter exp;
    auto bsp = MakeBsp(exp, opts);  // no sink

    EndSpan(*bsp, "a");
    EndSpan(*bsp, "dropped");
    (void)bsp->Shutdown(std::chrono::milliseconds(2000));
    EndSpan(*bsp, "after-shutdown");
}

// ---------------------------------------------------------------------------
// Record-size limit — spec §13.5, issue #181. `max_record_bytes` was declared
// in `MemoryLimitOptions` and enforced nowhere, and `record_too_large` was one
// of the six counters error-model.md §3 marked *(not yet produced)* for want of
// a detection point. `OnEnd` is that point: the record is measured before it is
// queued, so an oversized one never occupies the queue at all.
//
// The boundary tests take the estimate from `EstimateRecordBytes` itself rather
// than from a hard-coded size — the formula's constants are documented but not
// a contract, and a test that duplicated them would break on every tweak while
// asserting nothing about the limit.
// ---------------------------------------------------------------------------

namespace
{

/// A record with one attribute big enough to dominate the estimate.
mti::SpanRecord MakeFatRecord(const std::string& name)
{
    constexpr std::size_t kAttrValueBytes = 4096;
    auto record = MakeRecord(name);
    record.attributes.push_back(
        mt::KeyValue{.key = "payload", .value = std::string(kAttrValueBytes, 'x')});
    return record;
}

std::size_t TotalExported(const mtfk::FakeExporter& exp)
{
    std::size_t total = 0;
    for (const auto& batch : exp.received_batches)
    {
        total += batch.Spans().size();
    }
    return total;
}

}  // namespace

TEST(BatchSpanProcessorTest, EstimateRecordBytes_GrowsWithEveryOwnedBuffer)
{
    const auto bare = MakeRecord("span");
    const std::size_t bare_bytes = mt::sdk::EstimateRecordBytes(bare);
    EXPECT_GT(bare_bytes, std::string{"span"}.size()) << "fixed overhead is part of the estimate";

    auto with_attr = bare;
    with_attr.attributes.push_back(mt::KeyValue{.key = "k", .value = std::string(100, 'v')});
    const std::size_t attr_bytes = mt::sdk::EstimateRecordBytes(with_attr);
    EXPECT_GE(attr_bytes, bare_bytes + 101U);

    auto with_event = with_attr;
    with_event.events.push_back(
        mti::SpanEvent{.name = std::string(50, 'e'),
                       .timestamp = {},
                       .attributes = {mt::KeyValue{.key = "ek", .value = std::string(60, 'w')}}});
    const std::size_t event_bytes = mt::sdk::EstimateRecordBytes(with_event);
    EXPECT_GE(event_bytes, attr_bytes + 112U);

    auto with_link = with_event;
    with_link.links.push_back(
        mti::SpanLink{.linked_context = {},
                      .attributes = {mt::KeyValue{.key = "lk", .value = std::string(30, 'z')}}});
    EXPECT_GE(mt::sdk::EstimateRecordBytes(with_link), event_bytes + 32U);
}

// Every `AttributeValue` alternative has its own arm in the estimate, and an
// arm that reported nothing would let an unbounded array through the limit.
// Asserted as a growth relative to the same record without the attribute, so
// the test states the contribution of each type without duplicating the
// per-type constants.
TEST(BatchSpanProcessorTest, EstimateRecordBytes_CountsEveryAttributeValueKind)
{
    const auto bare = MakeRecord("span");
    const std::size_t bare_bytes = mt::sdk::EstimateRecordBytes(bare);

    const auto growth = [&bare, bare_bytes](mt::AttributeValue value)
    {
        auto record = bare;
        record.attributes.push_back(mt::KeyValue{.key = "k", .value = std::move(value)});
        return mt::sdk::EstimateRecordBytes(record) - bare_bytes;
    };

    constexpr std::size_t kKeyBytes = 1;  // "k"
    constexpr std::size_t kElements = 4;

    // Scalars cost a fixed width; the key is charged on top of it in each case.
    const std::size_t scalar = growth(true);
    EXPECT_GT(scalar, kKeyBytes);
    EXPECT_EQ(growth(std::int64_t{42}), scalar);
    EXPECT_EQ(growth(2.5), scalar);

    // Strings and arrays cost what they hold.
    EXPECT_EQ(growth(std::string(64, 's')), kKeyBytes + 64U);
    EXPECT_EQ(growth(std::vector<std::string>{std::string(10, 'a'), std::string(20, 'b')}),
              kKeyBytes + 30U);
    EXPECT_EQ(growth(std::vector<bool>(kElements, true)), kKeyBytes + kElements);
    EXPECT_EQ(growth(std::vector<std::int64_t>(kElements, 7)),
              kKeyBytes + (kElements * sizeof(std::int64_t)));
    EXPECT_EQ(growth(std::vector<double>(kElements, 1.5)),
              kKeyBytes + (kElements * sizeof(double)));
}

TEST(BatchSpanProcessorTest, RecordOverMaxRecordBytes_IsDroppedAndCounted)
{
    mt::BatchOptions opts;
    opts.schedule_delay = std::chrono::hours(1);

    auto record = MakeFatRecord("too-big");
    const std::size_t estimate = mt::sdk::EstimateRecordBytes(record);
    ASSERT_GT(estimate, 1U);

    mtfk::FakeExporter exp;
    mtfk::FakeDiagnosticsSink sink;
    auto bsp = MakeBsp(exp, opts, &sink, static_cast<std::uint32_t>(estimate - 1));

    bsp->OnEnd(std::move(record), mti::InstrumentationScope{.name = "test", .version = ""});

    EXPECT_EQ(DropCount(sink, mt::DropReason::RecordTooLarge), 1U);
    EXPECT_EQ(DropCount(sink, mt::DropReason::QueueFull), 0U);

    // Dropped *before* the queue, not after: the flush has nothing to export.
    EXPECT_EQ(bsp->ForceFlush(std::chrono::milliseconds(2000)), mt::Status::Completed);
    EXPECT_EQ(TotalExported(exp), 0U);
    (void)bsp->Shutdown(std::chrono::milliseconds(2000));
}

TEST(BatchSpanProcessorTest, RecordAtMaxRecordBytes_IsQueued)
{
    mt::BatchOptions opts;
    opts.schedule_delay = std::chrono::hours(1);

    auto record = MakeFatRecord("exactly-at-the-limit");
    const std::size_t estimate = mt::sdk::EstimateRecordBytes(record);

    mtfk::FakeExporter exp;
    mtfk::FakeDiagnosticsSink sink;
    auto bsp = MakeBsp(exp, opts, &sink, static_cast<std::uint32_t>(estimate));

    bsp->OnEnd(std::move(record), mti::InstrumentationScope{.name = "test", .version = ""});

    EXPECT_EQ(DropCount(sink, mt::DropReason::RecordTooLarge), 0U)
        << "the limit is a ceiling the record may reach, not one it may only approach";
    EXPECT_EQ(bsp->ForceFlush(std::chrono::milliseconds(2000)), mt::Status::Completed);
    EXPECT_EQ(TotalExported(exp), 1U);
    (void)bsp->Shutdown(std::chrono::milliseconds(2000));
}

TEST(BatchSpanProcessorTest, RecordTooLarge_WithoutSink_IsStillDropped)
{
    mt::BatchOptions opts;
    opts.schedule_delay = std::chrono::hours(1);

    auto record = MakeFatRecord("no-sink");
    const std::size_t estimate = mt::sdk::EstimateRecordBytes(record);

    mtfk::FakeExporter exp;
    auto bsp = MakeBsp(exp, opts, nullptr, static_cast<std::uint32_t>(estimate - 1));

    bsp->OnEnd(std::move(record), mti::InstrumentationScope{.name = "test", .version = ""});

    EXPECT_EQ(bsp->ForceFlush(std::chrono::milliseconds(2000)), mt::Status::Completed);
    EXPECT_EQ(TotalExported(exp), 0U);
    (void)bsp->Shutdown(std::chrono::milliseconds(2000));
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

    EndSpan(*bsp, "evicted");  // evicted when "new3" arrives
    EndSpan(*bsp, "keep1");
    EndSpan(*bsp, "new3");  // evicts "evicted"

    (void)bsp->Shutdown(std::chrono::milliseconds(2000));

    std::size_t total = 0;
    for (const auto& batch : exp.received_batches)
    {
        total += batch.Spans().size();
    }
    EXPECT_EQ(total, 2U);
}

// ---------------------------------------------------------------------------
// Per-scope grouping (issue #167 / ICP 0023)
// ---------------------------------------------------------------------------

TEST(BatchSpanProcessorTest, Drain_GroupsSpansByScope)
{
    mt::BatchOptions opts;
    opts.schedule_delay = std::chrono::hours(1);
    opts.max_export_batch_size = 512;

    mtfk::FakeExporter exp;
    auto bsp = MakeBsp(exp, opts);

    // Interleaved on purpose: grouping must survive a span of another scope
    // arriving between two spans of the first.
    EndSpan(*bsp, "a1", "scope.a", "1.0");
    EndSpan(*bsp, "b1", "scope.b", "2.0");
    EndSpan(*bsp, "a2", "scope.a", "1.0");

    ASSERT_EQ(bsp->ForceFlush(std::chrono::milliseconds(2000)), mt::Status::Completed);

    ASSERT_EQ(exp.received_batches.size(), std::size_t{2});

    // First-seen order: scope.a was queued first, so its batch goes out first.
    EXPECT_EQ(exp.received_batches[0].Scope().name, "scope.a");
    EXPECT_EQ(exp.received_batches[0].Scope().version, "1.0");
    ASSERT_EQ(exp.received_batches[0].Spans().size(), std::size_t{2});
    EXPECT_EQ(exp.received_batches[0].Spans()[0].name, "a1");
    EXPECT_EQ(exp.received_batches[0].Spans()[1].name, "a2");

    EXPECT_EQ(exp.received_batches[1].Scope().name, "scope.b");
    EXPECT_EQ(exp.received_batches[1].Scope().version, "2.0");
    ASSERT_EQ(exp.received_batches[1].Spans().size(), std::size_t{1});
    EXPECT_EQ(exp.received_batches[1].Spans()[0].name, "b1");

    (void)bsp->Shutdown(std::chrono::milliseconds(500));
}

TEST(BatchSpanProcessorTest, Drain_SeparatesScopesDifferingOnlyInVersion)
{
    mt::BatchOptions opts;
    opts.schedule_delay = std::chrono::hours(1);
    opts.max_export_batch_size = 512;

    mtfk::FakeExporter exp;
    auto bsp = MakeBsp(exp, opts);

    EndSpan(*bsp, "old", "scope.a", "1.0");
    EndSpan(*bsp, "new", "scope.a", "2.0");

    ASSERT_EQ(bsp->ForceFlush(std::chrono::milliseconds(2000)), mt::Status::Completed);

    ASSERT_EQ(exp.received_batches.size(), std::size_t{2});
    EXPECT_EQ(exp.received_batches[0].Scope().version, "1.0");
    EXPECT_EQ(exp.received_batches[1].Scope().version, "2.0");

    (void)bsp->Shutdown(std::chrono::milliseconds(500));
}
