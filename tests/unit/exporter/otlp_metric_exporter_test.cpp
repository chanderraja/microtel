// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for OtlpMetricExporter (M12).

#include "exporter/otlp_metric_exporter.hpp"

#include "microtel/internal/exporter.hpp"
#include "microtel/internal/metric_batch.hpp"
#include "microtel/provider.hpp"
#include "microtel/resource.hpp"
#include "microtel/status.hpp"

#include "fakes/fake_diagnostics_sink.hpp"
#include "mocks/mock_metric_encoder.hpp"
#include "mocks/mock_wire_codec.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <vector>

namespace mt = microtel;
namespace mti = microtel::internal;
namespace mtmk = microtel::testing;
namespace mte = microtel::exporter;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static mti::MetricBatchHandle MakeBatchOf(std::size_t metric_count)
{
    return mti::MetricBatchHandle{
        std::vector<mti::MetricRecord>(metric_count),
        std::make_shared<mt::Resource>(),
        mti::InstrumentationScope{.name = "test", .version = "0.1"},
    };
}

static mti::MetricBatchHandle MakeBatch()
{
    return MakeBatchOf(0);
}

static std::uint64_t DropCount(const mtmk::FakeDiagnosticsSink& sink, mt::DropReason reason)
{
    return sink.drop_counters.at(static_cast<std::size_t>(reason));
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

// ---------------------------------------------------------------------------
// Diagnostics — issue #169. This exporter had no sink at all: a metric
// pipeline could drop every batch and GetExporterHealth() showed nothing.
// ---------------------------------------------------------------------------

TEST(OtlpMetricExporterTest, Diagnostics_SuccessfulExport_RecordsBatchSent)
{
    mtmk::MockMetricEncoder encoder;
    mtmk::MockWireCodec codec;
    mtmk::FakeDiagnosticsSink sink;
    codec.result_to_return.success = true;
    mte::OtlpMetricExporter exporter{&encoder, &codec, {}, &sink};

    (void)exporter.Export(MakeBatch());
    ASSERT_EQ(exporter.ForceFlush(kFlushTimeout), mt::Status::Completed);

    EXPECT_EQ(sink.batches_sent, 1U);
    EXPECT_EQ(sink.batches_failed, 0U);
}

TEST(OtlpMetricExporterTest, Diagnostics_FailedExport_RecordsBatchFailedAndMessage)
{
    mtmk::MockMetricEncoder encoder;
    mtmk::MockWireCodec codec;
    mtmk::FakeDiagnosticsSink sink;
    codec.result_to_return = mti::WireResult{
        .success = false,
        .retryable = false,
        .error = mt::Error{.kind = mt::Error::Kind::Network, .message = "HTTP 401"},
    };
    mte::OtlpMetricExporter exporter{&encoder, &codec, {}, &sink};

    (void)exporter.Export(MakeBatch());
    ASSERT_EQ(exporter.ForceFlush(kFlushTimeout), mt::Status::Completed);

    EXPECT_EQ(sink.batches_failed, 1U);
    EXPECT_EQ(sink.last_error_message, "HTTP 401");
}

TEST(OtlpMetricExporterTest, Diagnostics_QueueFull_CountsEveryMetricInTheRejectedBatch)
{
    mtmk::MockMetricEncoder encoder;
    mtmk::MockWireCodec codec;
    mtmk::FakeDiagnosticsSink sink;
    mte::OtlpMetricExporterConfig cfg;
    cfg.max_queue_size = 0;
    mte::OtlpMetricExporter exporter{&encoder, &codec, cfg, &sink};

    EXPECT_EQ(exporter.Export(MakeBatchOf(3)), mti::ExportResult::Dropped);

    EXPECT_EQ(DropCount(sink, mt::DropReason::QueueFull), 3U);
}

TEST(OtlpMetricExporterTest, Diagnostics_ExportAfterShutdown_CountsPostShutdown)
{
    mtmk::MockMetricEncoder encoder;
    mtmk::MockWireCodec codec;
    mtmk::FakeDiagnosticsSink sink;
    mte::OtlpMetricExporter exporter{&encoder, &codec, {}, &sink};

    ASSERT_EQ(exporter.Shutdown(kFlushTimeout), mt::Status::Completed);
    EXPECT_EQ(exporter.Export(MakeBatchOf(2)), mti::ExportResult::AlreadyShutDown);

    EXPECT_EQ(DropCount(sink, mt::DropReason::PostShutdown), 2U);
}

TEST(OtlpMetricExporterTest, Diagnostics_NullSink_IsNotDereferenced)
{
    mtmk::MockMetricEncoder encoder;
    mtmk::MockWireCodec codec;
    codec.result_to_return.success = true;
    mte::OtlpMetricExporter exporter{&encoder, &codec};  // no sink

    (void)exporter.Export(MakeBatch());
    EXPECT_EQ(exporter.ForceFlush(kFlushTimeout), mt::Status::Completed);
}

// ---------------------------------------------------------------------------
// Drain-path exception accounting — issue #224.
//
// `ProcessBatches` runs with the queue lock released, inside a `noexcept`
// worker, so `DrainQueue` catches everything it can throw. The catch was empty
// behind a `NOLINTNEXTLINE(bugprone-empty-catch)` and a "diag hook deferred"
// comment: the batches vanished and `GetExporterHealth()` reported a clean
// pipeline. They are still lost — there is nowhere to put them — but the loss
// is now countable.
//
// `RecordBatchFailed` is the surface, not a `DropReason`: no existing reason
// names "the encoder threw", and adding one is an ICP (interfaces.md §3.5).
// ---------------------------------------------------------------------------

namespace
{

/// An encoder that always throws — the only way to reach `DrainQueue`'s catch
/// from a test, every other collaborator on that path being `noexcept`.
class ThrowingMetricEncoder final : public mti::IMetricEncoder
{
public:
    [[nodiscard]] mti::EncodedPayload Encode(const mti::MetricBatchHandle& /*batch*/) override
    {
        throw std::runtime_error("metric encode blew up");
    }
};

}  // namespace

TEST(OtlpMetricExporterTest, Diagnostics_DrainThrows_RecordsBatchFailed)
{
    ThrowingMetricEncoder encoder;
    mtmk::MockWireCodec codec;
    mtmk::FakeDiagnosticsSink sink;
    mte::OtlpMetricExporter exporter{&encoder, &codec, {}, &sink};

    EXPECT_EQ(exporter.Export(MakeBatchOf(2)), mti::ExportResult::Success);
    ASSERT_EQ(exporter.ForceFlush(kFlushTimeout), mt::Status::Completed);

    EXPECT_EQ(sink.batches_failed, 1U) << "a swallowed drain failure must still be countable";
    EXPECT_EQ(sink.batches_sent, 0U);
    EXPECT_FALSE(sink.last_error_message.empty())
        << "GetExporterHealth() must be able to say why the batch was lost";
    EXPECT_TRUE(sink.last_error_time.has_value());
}

TEST(OtlpMetricExporterTest, Diagnostics_DrainThrows_WithoutSink_StillDrains)
{
    ThrowingMetricEncoder encoder;
    mtmk::MockWireCodec codec;
    mte::OtlpMetricExporter exporter{&encoder, &codec};  // no sink

    (void)exporter.Export(MakeBatch());
    EXPECT_EQ(exporter.ForceFlush(kFlushTimeout), mt::Status::Completed);
}
