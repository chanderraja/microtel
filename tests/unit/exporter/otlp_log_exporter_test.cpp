// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for OtlpLogExporter — M14 L4.3 (docs/logs-design.md §3).
// Mirrors the OtlpMetricExporter tests: encode-and-send, lifecycle, queue cap.

#include "exporter/otlp_log_exporter.hpp"

#include "microtel/internal/exporter.hpp"
#include "microtel/internal/log_batch.hpp"
#include "microtel/provider.hpp"
#include "microtel/resource.hpp"
#include "microtel/status.hpp"

#include "fakes/fake_diagnostics_sink.hpp"
#include "mocks/mock_log_encoder.hpp"
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

namespace
{

mti::LogBatchHandle MakeBatchOf(std::size_t record_count)
{
    return mti::LogBatchHandle{
        std::vector<mt::LogRecord>(record_count),
        std::make_shared<mt::Resource>(),
        mti::InstrumentationScope{.name = "test", .version = "0.1"},
    };
}

mti::LogBatchHandle MakeBatch()
{
    return MakeBatchOf(0);
}

std::uint64_t DropCount(const mtmk::FakeDiagnosticsSink& sink, mt::DropReason reason)
{
    return sink.drop_counters.at(static_cast<std::size_t>(reason));
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

// ---------------------------------------------------------------------------
// Diagnostics — issue #169. This exporter had no sink at all: a log pipeline
// could drop every batch it was given and GetExporterHealth() showed nothing.
// ---------------------------------------------------------------------------

TEST(OtlpLogExporterTest, DiagnosticsSuccessfulExportRecordsBatchSent)
{
    mtmk::MockLogEncoder encoder;
    mtmk::MockWireCodec codec;
    mtmk::FakeDiagnosticsSink sink;
    codec.result_to_return.success = true;
    mte::OtlpLogExporter exporter{&encoder, &codec, {}, &sink};

    (void)exporter.Export(MakeBatch());
    ASSERT_EQ(exporter.ForceFlush(kFlushTimeout), mt::Status::Completed);

    EXPECT_EQ(sink.batches_sent, 1U);
    EXPECT_EQ(sink.batches_failed, 0U);
}

TEST(OtlpLogExporterTest, DiagnosticsFailedExportRecordsBatchFailedAndMessage)
{
    mtmk::MockLogEncoder encoder;
    mtmk::MockWireCodec codec;
    mtmk::FakeDiagnosticsSink sink;
    codec.result_to_return = mti::WireResult{
        .success = false,
        .retryable = false,
        .error = mt::Error{.kind = mt::Error::Kind::Network, .message = "HTTP 401"},
    };
    mte::OtlpLogExporter exporter{&encoder, &codec, {}, &sink};

    (void)exporter.Export(MakeBatch());
    ASSERT_EQ(exporter.ForceFlush(kFlushTimeout), mt::Status::Completed);

    EXPECT_EQ(sink.batches_failed, 1U);
    EXPECT_EQ(sink.last_error_message, "HTTP 401");
}

TEST(OtlpLogExporterTest, DiagnosticsQueueFullCountsEveryRecordInTheRejectedBatch)
{
    mtmk::MockLogEncoder encoder;
    mtmk::MockWireCodec codec;
    mtmk::FakeDiagnosticsSink sink;
    mte::OtlpLogExporterConfig cfg;
    cfg.max_queue_size = 0;
    mte::OtlpLogExporter exporter{&encoder, &codec, cfg, &sink};

    EXPECT_EQ(exporter.Export(MakeBatchOf(3)), mti::ExportResult::Dropped);

    EXPECT_EQ(DropCount(sink, mt::DropReason::QueueFull), 3U);
}

TEST(OtlpLogExporterTest, DiagnosticsExportAfterShutdownCountsPostShutdown)
{
    mtmk::MockLogEncoder encoder;
    mtmk::MockWireCodec codec;
    mtmk::FakeDiagnosticsSink sink;
    mte::OtlpLogExporter exporter{&encoder, &codec, {}, &sink};

    ASSERT_EQ(exporter.Shutdown(kFlushTimeout), mt::Status::Completed);
    EXPECT_EQ(exporter.Export(MakeBatchOf(2)), mti::ExportResult::AlreadyShutDown);

    EXPECT_EQ(DropCount(sink, mt::DropReason::PostShutdown), 2U);
}

TEST(OtlpLogExporterTest, DiagnosticsNullSinkIsNotDereferenced)
{
    mtmk::MockLogEncoder encoder;
    mtmk::MockWireCodec codec;
    codec.result_to_return.success = true;
    mte::OtlpLogExporter exporter{&encoder, &codec};  // no sink

    (void)exporter.Export(MakeBatch());
    EXPECT_EQ(exporter.ForceFlush(kFlushTimeout), mt::Status::Completed);
}

// ---------------------------------------------------------------------------
// Drain-path exception accounting — issue #224, mirroring the metrics
// exporter. `DrainQueue`'s catch was empty behind a "diag hook deferred"
// comment, so a batch lost to a throwing encoder left `GetExporterHealth()`
// reporting a clean pipeline. The batch is still lost; the loss is countable.
// ---------------------------------------------------------------------------

/// An encoder that always throws — the only way to reach `DrainQueue`'s catch
/// from a test, every other collaborator on that path being `noexcept`.
class ThrowingLogEncoder final : public mti::ILogEncoder
{
public:
    [[nodiscard]] mti::EncodedPayload Encode(const mti::LogBatchHandle& /*batch*/) override
    {
        throw std::runtime_error("log encode blew up");
    }
};

TEST(OtlpLogExporterTest, DiagnosticsDrainThrowsRecordsBatchFailed)
{
    ThrowingLogEncoder encoder;
    mtmk::MockWireCodec codec;
    mtmk::FakeDiagnosticsSink sink;
    mte::OtlpLogExporter exporter{&encoder, &codec, {}, &sink};

    EXPECT_EQ(exporter.Export(MakeBatchOf(2)), mti::ExportResult::Success);
    ASSERT_EQ(exporter.ForceFlush(kFlushTimeout), mt::Status::Completed);

    EXPECT_EQ(sink.batches_failed, 1U) << "a swallowed drain failure must still be countable";
    EXPECT_EQ(sink.batches_sent, 0U);
    EXPECT_FALSE(sink.last_error_message.empty())
        << "GetExporterHealth() must be able to say why the batch was lost";
    EXPECT_TRUE(sink.last_error_time.has_value());
}

TEST(OtlpLogExporterTest, DiagnosticsDrainThrowsWithoutSinkStillDrains)
{
    ThrowingLogEncoder encoder;
    mtmk::MockWireCodec codec;
    mte::OtlpLogExporter exporter{&encoder, &codec};  // no sink

    (void)exporter.Export(MakeBatch());
    EXPECT_EQ(exporter.ForceFlush(kFlushTimeout), mt::Status::Completed);
}

}  // namespace
