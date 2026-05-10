// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for OtlpExporter (M3 close).
// Uses MockOtlpEncoder + MockWireCodec; ForceFlush synchronises the worker.

#include "exporter/otlp_exporter.hpp"

#include "microtel/internal/batch.hpp"
#include "microtel/internal/exporter.hpp"
#include "microtel/resource.hpp"
#include "microtel/status.hpp"

#include "mocks/mock_otlp_encoder.hpp"
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

static mti::BatchHandle MakeBatch()
{
    auto resource = std::make_shared<mt::Resource>();
    mti::InstrumentationScope scope{.name = "test", .version = "0.1"};
    std::vector<mti::SpanRecord> spans;
    spans.push_back(mti::SpanRecord{.name = "s"});
    return mti::BatchHandle{std::move(spans), std::move(resource), std::move(scope)};
}

static constexpr auto kFlushTimeout = std::chrono::milliseconds(500);

// ---------------------------------------------------------------------------
// Basic encode + send
// ---------------------------------------------------------------------------

TEST(OtlpExporterTest, Export_CallsEncoderAndCodec)
{
    mtmk::MockOtlpEncoder encoder;
    mtmk::MockWireCodec codec;
    codec.result_to_return.success = true;
    mte::OtlpExporter exporter{&encoder, &codec};

    const auto result = exporter.Export(MakeBatch());
    EXPECT_EQ(result, mti::ExportResult::Success);

    ASSERT_EQ(exporter.ForceFlush(kFlushTimeout), mt::Status::Completed);
    EXPECT_EQ(encoder.encode_call_count, 1);
    EXPECT_EQ(codec.send_call_count, 1);
}

TEST(OtlpExporterTest, Export_MultipleExports_AllProcessed)
{
    mtmk::MockOtlpEncoder encoder;
    mtmk::MockWireCodec codec;
    codec.result_to_return.success = true;
    mte::OtlpExporter exporter{&encoder, &codec};

    (void)exporter.Export(MakeBatch());
    (void)exporter.Export(MakeBatch());
    (void)exporter.Export(MakeBatch());

    ASSERT_EQ(exporter.ForceFlush(kFlushTimeout), mt::Status::Completed);
    EXPECT_EQ(encoder.encode_call_count, 3);
    EXPECT_EQ(codec.send_call_count, 3);
}

// ---------------------------------------------------------------------------
// Lifecycle: shutdown
// ---------------------------------------------------------------------------

TEST(OtlpExporterTest, Export_AfterShutdown_ReturnsAlreadyShutDown)
{
    mtmk::MockOtlpEncoder encoder;
    mtmk::MockWireCodec codec;
    mte::OtlpExporter exporter{&encoder, &codec};

    ASSERT_EQ(exporter.Shutdown(kFlushTimeout), mt::Status::Completed);
    EXPECT_EQ(exporter.Export(MakeBatch()), mti::ExportResult::AlreadyShutDown);
}

TEST(OtlpExporterTest, Shutdown_Idempotent)
{
    mtmk::MockOtlpEncoder encoder;
    mtmk::MockWireCodec codec;
    mte::OtlpExporter exporter{&encoder, &codec};

    EXPECT_EQ(exporter.Shutdown(kFlushTimeout), mt::Status::Completed);
    EXPECT_EQ(exporter.Shutdown(kFlushTimeout), mt::Status::AlreadyShutDown);
}

TEST(OtlpExporterTest, Shutdown_WaitsForPendingBatch)
{
    mtmk::MockOtlpEncoder encoder;
    mtmk::MockWireCodec codec;
    codec.result_to_return.success = true;
    mte::OtlpExporter exporter{&encoder, &codec};

    (void)exporter.Export(MakeBatch());
    ASSERT_EQ(exporter.Shutdown(kFlushTimeout), mt::Status::Completed);
    EXPECT_EQ(encoder.encode_call_count, 1);
}

// ---------------------------------------------------------------------------
// Queue capacity
// ---------------------------------------------------------------------------

TEST(OtlpExporterTest, Export_QueueFull_ReturnsDropped)
{
    mtmk::MockOtlpEncoder encoder;
    mtmk::MockWireCodec codec;
    mte::OtlpExporterConfig cfg;
    cfg.max_queue_size = 0;
    mte::OtlpExporter exporter{&encoder, &codec, cfg};

    EXPECT_EQ(exporter.Export(MakeBatch()), mti::ExportResult::Dropped);
}

// ---------------------------------------------------------------------------
// ForceFlush
// ---------------------------------------------------------------------------

TEST(OtlpExporterTest, ForceFlush_EmptyQueue_ReturnsCompleted)
{
    mtmk::MockOtlpEncoder encoder;
    mtmk::MockWireCodec codec;
    mte::OtlpExporter exporter{&encoder, &codec};

    EXPECT_EQ(exporter.ForceFlush(kFlushTimeout), mt::Status::Completed);
}

// ---------------------------------------------------------------------------
// Failure classification (M3: single attempt, no retry)
// ---------------------------------------------------------------------------

TEST(OtlpExporterTest, Export_NonRetryableCodecResult_DoesNotRetry)
{
    mtmk::MockOtlpEncoder encoder;
    mtmk::MockWireCodec codec;
    // default: success=false, retryable=false
    mte::OtlpExporter exporter{&encoder, &codec};

    (void)exporter.Export(MakeBatch());
    ASSERT_EQ(exporter.ForceFlush(kFlushTimeout), mt::Status::Completed);
    EXPECT_EQ(codec.send_call_count, 1);
}

TEST(OtlpExporterTest, Export_RetryableCodecResult_NoRetryInM3)
{
    mtmk::MockOtlpEncoder encoder;
    mtmk::MockWireCodec codec;
    codec.result_to_return.retryable = true;
    mte::OtlpExporter exporter{&encoder, &codec};

    (void)exporter.Export(MakeBatch());
    ASSERT_EQ(exporter.ForceFlush(kFlushTimeout), mt::Status::Completed);
    EXPECT_EQ(codec.send_call_count, 1);
}
