// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for OtlpMetricExporter (M12).

#include "exporter/otlp_metric_exporter.hpp"

#include "microtel/internal/exporter.hpp"
#include "microtel/internal/metric_batch.hpp"
#include "microtel/resource.hpp"
#include "microtel/status.hpp"

#include "mocks/mock_metric_encoder.hpp"
#include "mocks/mock_wire_codec.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <vector>

namespace mt = microtel;
namespace mti = microtel::internal;
namespace mtmk = microtel::testing;
namespace mte = microtel::exporter;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static mti::MetricBatchHandle MakeBatch()
{
    return mti::MetricBatchHandle{
        std::vector<mti::MetricRecord>{},
        std::make_shared<mt::Resource>(),
        mti::InstrumentationScope{.name = "test", .version = "0.1"},
    };
}

static constexpr auto kFlushTimeout = std::chrono::milliseconds(500);

// ---------------------------------------------------------------------------
// Basic encode + send
// ---------------------------------------------------------------------------

TEST(OtlpMetricExporterTest, Export_CallsEncoderAndCodec)
{
    mtmk::MockMetricEncoder encoder;
    mtmk::MockWireCodec codec;
    codec.result_to_return.success = true;
    mte::OtlpMetricExporter exporter{&encoder, &codec};

    EXPECT_EQ(exporter.Export(MakeBatch()), mti::ExportResult::Success);
    ASSERT_EQ(exporter.ForceFlush(kFlushTimeout), mt::Status::Completed);
    EXPECT_EQ(encoder.encode_call_count.load(), 1);
    EXPECT_EQ(codec.send_call_count.load(), 1);
}

TEST(OtlpMetricExporterTest, Export_MultipleExports_AllProcessed)
{
    mtmk::MockMetricEncoder encoder;
    mtmk::MockWireCodec codec;
    codec.result_to_return.success = true;
    mte::OtlpMetricExporter exporter{&encoder, &codec};

    (void)exporter.Export(MakeBatch());
    (void)exporter.Export(MakeBatch());
    (void)exporter.Export(MakeBatch());

    ASSERT_EQ(exporter.ForceFlush(kFlushTimeout), mt::Status::Completed);
    EXPECT_EQ(encoder.encode_call_count.load(), 3);
    EXPECT_EQ(codec.send_call_count.load(), 3);
}

// ---------------------------------------------------------------------------
// Lifecycle: shutdown
// ---------------------------------------------------------------------------

TEST(OtlpMetricExporterTest, Export_AfterShutdown_ReturnsAlreadyShutDown)
{
    mtmk::MockMetricEncoder encoder;
    mtmk::MockWireCodec codec;
    mte::OtlpMetricExporter exporter{&encoder, &codec};

    ASSERT_EQ(exporter.Shutdown(kFlushTimeout), mt::Status::Completed);
    EXPECT_EQ(exporter.Export(MakeBatch()), mti::ExportResult::AlreadyShutDown);
}

TEST(OtlpMetricExporterTest, Shutdown_Idempotent)
{
    mtmk::MockMetricEncoder encoder;
    mtmk::MockWireCodec codec;
    mte::OtlpMetricExporter exporter{&encoder, &codec};

    EXPECT_EQ(exporter.Shutdown(kFlushTimeout), mt::Status::Completed);
    EXPECT_EQ(exporter.Shutdown(kFlushTimeout), mt::Status::AlreadyShutDown);
}

TEST(OtlpMetricExporterTest, Shutdown_WaitsForPendingBatch)
{
    mtmk::MockMetricEncoder encoder;
    mtmk::MockWireCodec codec;
    codec.result_to_return.success = true;
    mte::OtlpMetricExporter exporter{&encoder, &codec};

    (void)exporter.Export(MakeBatch());
    ASSERT_EQ(exporter.Shutdown(kFlushTimeout), mt::Status::Completed);
    EXPECT_EQ(encoder.encode_call_count.load(), 1);
}

// ---------------------------------------------------------------------------
// Queue capacity
// ---------------------------------------------------------------------------

TEST(OtlpMetricExporterTest, Export_QueueFull_ReturnsDropped)
{
    mtmk::MockMetricEncoder encoder;
    mtmk::MockWireCodec codec;
    mte::OtlpMetricExporterConfig cfg;
    cfg.max_queue_size = 0;
    mte::OtlpMetricExporter exporter{&encoder, &codec, cfg};

    EXPECT_EQ(exporter.Export(MakeBatch()), mti::ExportResult::Dropped);
}

// ---------------------------------------------------------------------------
// ForceFlush
// ---------------------------------------------------------------------------

TEST(OtlpMetricExporterTest, ForceFlush_EmptyQueue_ReturnsCompleted)
{
    mtmk::MockMetricEncoder encoder;
    mtmk::MockWireCodec codec;
    mte::OtlpMetricExporter exporter{&encoder, &codec};

    EXPECT_EQ(exporter.ForceFlush(kFlushTimeout), mt::Status::Completed);
}
