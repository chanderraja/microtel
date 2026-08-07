// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for OtlpLogExporter — M14 L4.3 (docs/logs-design.md §3).
// Mirrors the OtlpMetricExporter tests: encode-and-send, lifecycle, queue cap.

#include "exporter/otlp_log_exporter.hpp"

#include "microtel/internal/exporter.hpp"
#include "microtel/internal/log_batch.hpp"
#include "microtel/resource.hpp"
#include "microtel/status.hpp"

#include "mocks/mock_log_encoder.hpp"
#include "mocks/mock_wire_codec.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <vector>

namespace mt = microtel;
namespace mti = microtel::internal;
namespace mtmk = microtel::testing;
namespace mte = microtel::exporter;

namespace
{

mti::LogBatchHandle MakeBatch()
{
    return mti::LogBatchHandle{
        std::vector<mt::LogRecord>{},
        std::make_shared<mt::Resource>(),
        mti::InstrumentationScope{.name = "test", .version = "0.1"},
    };
}

constexpr auto kFlushTimeout = std::chrono::milliseconds(500);

TEST(OtlpLogExporterTest, ExportCallsEncoderAndCodec)
{
    mtmk::MockLogEncoder encoder;
    mtmk::MockWireCodec codec;
    codec.result_to_return.success = true;
    mte::OtlpLogExporter exporter{&encoder, &codec};

    EXPECT_EQ(exporter.Export(MakeBatch()), mti::ExportResult::Success);
    ASSERT_EQ(exporter.ForceFlush(kFlushTimeout), mt::Status::Completed);
    EXPECT_EQ(encoder.encode_call_count.load(), 1);
    EXPECT_EQ(codec.send_call_count.load(), 1);
}

TEST(OtlpLogExporterTest, MultipleExportsAllProcessed)
{
    mtmk::MockLogEncoder encoder;
    mtmk::MockWireCodec codec;
    codec.result_to_return.success = true;
    mte::OtlpLogExporter exporter{&encoder, &codec};

    (void)exporter.Export(MakeBatch());
    (void)exporter.Export(MakeBatch());
    (void)exporter.Export(MakeBatch());

    ASSERT_EQ(exporter.ForceFlush(kFlushTimeout), mt::Status::Completed);
    EXPECT_EQ(encoder.encode_call_count.load(), 3);
}

TEST(OtlpLogExporterTest, ExportAfterShutdownReturnsAlreadyShutDown)
{
    mtmk::MockLogEncoder encoder;
    mtmk::MockWireCodec codec;
    mte::OtlpLogExporter exporter{&encoder, &codec};

    ASSERT_EQ(exporter.Shutdown(kFlushTimeout), mt::Status::Completed);
    EXPECT_EQ(exporter.Export(MakeBatch()), mti::ExportResult::AlreadyShutDown);
}

TEST(OtlpLogExporterTest, ShutdownIsIdempotent)
{
    mtmk::MockLogEncoder encoder;
    mtmk::MockWireCodec codec;
    mte::OtlpLogExporter exporter{&encoder, &codec};

    EXPECT_EQ(exporter.Shutdown(kFlushTimeout), mt::Status::Completed);
    EXPECT_EQ(exporter.Shutdown(kFlushTimeout), mt::Status::AlreadyShutDown);
}

TEST(OtlpLogExporterTest, ShutdownWaitsForPendingBatch)
{
    mtmk::MockLogEncoder encoder;
    mtmk::MockWireCodec codec;
    codec.result_to_return.success = true;
    mte::OtlpLogExporter exporter{&encoder, &codec};

    (void)exporter.Export(MakeBatch());
    ASSERT_EQ(exporter.Shutdown(kFlushTimeout), mt::Status::Completed);
    EXPECT_EQ(encoder.encode_call_count.load(), 1);
}

TEST(OtlpLogExporterTest, ExportQueueFullReturnsDropped)
{
    mtmk::MockLogEncoder encoder;
    mtmk::MockWireCodec codec;
    mte::OtlpLogExporterConfig cfg;
    cfg.max_queue_size = 0;
    mte::OtlpLogExporter exporter{&encoder, &codec, cfg};

    EXPECT_EQ(exporter.Export(MakeBatch()), mti::ExportResult::Dropped);
}

TEST(OtlpLogExporterTest, ForceFlushEmptyQueueReturnsCompleted)
{
    mtmk::MockLogEncoder encoder;
    mtmk::MockWireCodec codec;
    mte::OtlpLogExporter exporter{&encoder, &codec};

    EXPECT_EQ(exporter.ForceFlush(kFlushTimeout), mt::Status::Completed);
}

}  // namespace
