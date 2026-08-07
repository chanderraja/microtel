// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for SimpleLogRecordProcessor — M14 L4.2 (docs/logs-design.md §2).
// Synchronous passthrough: each OnEmit exports a single-record batch tagged
// with the call's scope; ForceFlush is a no-op; Shutdown delegates.

#include "sdk/simple_log_record_processor.hpp"

#include "microtel/internal/batch.hpp"
#include "microtel/log_record.hpp"
#include "microtel/resource.hpp"
#include "microtel/status.hpp"

#include "fakes/fake_log_exporter.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <string>
#include <utility>

namespace mt = microtel;
namespace mts = microtel::sdk;
namespace mti = microtel::internal;
namespace mtfk = microtel::testing;

namespace
{

mti::InstrumentationScope Scope(std::string name)
{
    return mti::InstrumentationScope{.name = std::move(name), .version = "1.0"};
}

TEST(SimpleLogRecordProcessorTest, OnEmitExportsSingleRecordBatchTaggedWithScope)
{
    mtfk::FakeLogExporter exp;
    auto resource = std::make_shared<const mt::Resource>();
    mts::SimpleLogRecordProcessor proc{&exp, resource};

    proc.OnEmit(mt::LogRecord{}, Scope("logger.a"));

    ASSERT_EQ(exp.exported.size(), 1U);
    EXPECT_EQ(exp.exported[0].Records().size(), 1U);
    EXPECT_EQ(exp.exported[0].Scope().name, "logger.a");
}

TEST(SimpleLogRecordProcessorTest, EachEmitProducesOneExport)
{
    mtfk::FakeLogExporter exp;
    mts::SimpleLogRecordProcessor proc{&exp, std::make_shared<const mt::Resource>()};

    proc.OnEmit(mt::LogRecord{}, Scope("s"));
    proc.OnEmit(mt::LogRecord{}, Scope("s"));

    EXPECT_EQ(exp.exported.size(), 2U);
}

TEST(SimpleLogRecordProcessorTest, ForceFlushReturnsCompleted)
{
    mtfk::FakeLogExporter exp;
    mts::SimpleLogRecordProcessor proc{&exp, std::make_shared<const mt::Resource>()};

    EXPECT_EQ(proc.ForceFlush(std::chrono::milliseconds(100)), mt::Status::Completed);
}

TEST(SimpleLogRecordProcessorTest, ShutdownDelegatesToExporter)
{
    mtfk::FakeLogExporter exp;
    exp.shutdown_result = mt::Status::TimedOut;
    mts::SimpleLogRecordProcessor proc{&exp, std::make_shared<const mt::Resource>()};

    EXPECT_EQ(proc.Shutdown(std::chrono::milliseconds(100)), mt::Status::TimedOut);
    EXPECT_EQ(exp.shutdown_call_count, 1);
}

}  // namespace
