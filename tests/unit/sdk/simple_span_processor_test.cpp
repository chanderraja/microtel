// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Behavioural tests for SimpleSpanProcessor.
//
// Per spec §8: "Simple (non-batched) processor — for tests/debug." Each
// `OnEnd` synchronously hands a single-span batch to the configured
// exporter; no buffering, no thread, no retry. Lifecycle methods
// (`ForceFlush`, `Shutdown`) are trivial — `ForceFlush` is a no-op
// because there's nothing buffered, and `Shutdown` delegates to the
// exporter.

#include "sdk/simple_span_processor.hpp"

#include "microtel/internal/batch.hpp"
#include "microtel/internal/processor.hpp"
#include "microtel/resource.hpp"
#include "microtel/status.hpp"

#include "fakes/fake_exporter.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <utility>

namespace mt = microtel;

namespace
{

mt::internal::SpanRecord MakeSpanRecord(std::string name)
{
    mt::internal::SpanRecord r;
    r.name = std::move(name);
    return r;
}

class SimpleSpanProcessorTest : public ::testing::Test
{
protected:
    mt::testing::FakeExporter m_exporter;
    std::shared_ptr<const mt::Resource> m_resource = std::make_shared<const mt::Resource>();
    mt::internal::InstrumentationScope m_scope{.name = "test.scope", .version = "1"};
};

TEST_F(SimpleSpanProcessorTest, OnEndForwardsSingleSpanBatchToExporter)
{
    mt::internal::SimpleSpanProcessor proc{&m_exporter, m_resource, m_scope};

    proc.OnEnd(MakeSpanRecord("first"));

    ASSERT_EQ(m_exporter.received_batches.size(), std::size_t{1});
    const auto& batch = m_exporter.received_batches[0];
    ASSERT_EQ(batch.Spans().size(), std::size_t{1});
    EXPECT_EQ(batch.Spans()[0].name, "first");
    EXPECT_EQ(batch.Scope().name, "test.scope");
    EXPECT_EQ(batch.Scope().version, "1");
}

TEST_F(SimpleSpanProcessorTest, EachOnEndProducesSeparateBatch)
{
    mt::internal::SimpleSpanProcessor proc{&m_exporter, m_resource, m_scope};

    proc.OnEnd(MakeSpanRecord("a"));
    proc.OnEnd(MakeSpanRecord("b"));
    proc.OnEnd(MakeSpanRecord("c"));

    ASSERT_EQ(m_exporter.received_batches.size(), std::size_t{3});
    EXPECT_EQ(m_exporter.received_batches[0].Spans()[0].name, "a");
    EXPECT_EQ(m_exporter.received_batches[1].Spans()[0].name, "b");
    EXPECT_EQ(m_exporter.received_batches[2].Spans()[0].name, "c");
}

TEST_F(SimpleSpanProcessorTest, ForceFlushReturnsCompletedWithoutEngagingExporter)
{
    mt::internal::SimpleSpanProcessor proc{&m_exporter, m_resource, m_scope};

    const auto rc = proc.ForceFlush(std::chrono::seconds{5});
    EXPECT_EQ(rc, mt::Status::Completed);
    EXPECT_EQ(m_exporter.force_flush_call_count, 0);
}

TEST_F(SimpleSpanProcessorTest, ShutdownDelegatesToExporter)
{
    mt::internal::SimpleSpanProcessor proc{&m_exporter, m_resource, m_scope};
    m_exporter.shutdown_result = mt::Status::TimedOut;

    const auto rc = proc.Shutdown(std::chrono::seconds{5});
    EXPECT_EQ(rc, mt::Status::TimedOut);
    EXPECT_EQ(m_exporter.shutdown_call_count, 1);
}

TEST_F(SimpleSpanProcessorTest, ShutdownIsIdempotentByDelegating)
{
    mt::internal::SimpleSpanProcessor proc{&m_exporter, m_resource, m_scope};
    m_exporter.shutdown_result = mt::Status::Completed;

    EXPECT_EQ(proc.Shutdown(std::chrono::seconds{1}), mt::Status::Completed);
    EXPECT_EQ(proc.Shutdown(std::chrono::seconds{1}), mt::Status::Completed);
    EXPECT_EQ(m_exporter.shutdown_call_count, 2);
}

}  // namespace
