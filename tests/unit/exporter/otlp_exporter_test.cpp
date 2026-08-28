// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for OtlpExporter (M3 close + M5-A retry engine).

#include "exporter/otlp_exporter.hpp"

#include "microtel/internal/batch.hpp"
#include "microtel/internal/exporter.hpp"
#include "microtel/resource.hpp"
#include "microtel/status.hpp"

#include "fakes/fake_diagnostics_sink.hpp"
#include "fakes/fake_steady_clock.hpp"
#include "fakes/fake_wire_codec.hpp"
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

// Zero-delay retry config: retries happen without sleeping, for fast tests.
static mte::RetryPolicyConfig ZeroDelayRetry(std::uint32_t max_attempts = 5)
{
    return mte::RetryPolicyConfig{
        .max_attempts = max_attempts,
        .initial_backoff = std::chrono::milliseconds{0},
        .max_backoff = std::chrono::milliseconds{0},
        .backoff_multiplier = 1.0,
        .jitter_fraction = 0.0,
        .retry_budget = std::chrono::minutes(5),
    };
}

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
    EXPECT_EQ(encoder.encode_call_count.load(), 1);
    EXPECT_EQ(codec.send_call_count.load(), 1);
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
    EXPECT_EQ(encoder.encode_call_count.load(), 3);
    EXPECT_EQ(codec.send_call_count.load(), 3);
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
    EXPECT_EQ(encoder.encode_call_count.load(), 1);
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
// Failure classification — single-attempt scenarios
// ---------------------------------------------------------------------------

TEST(OtlpExporterTest, Export_NonRetryableFailure_SingleAttempt)
{
    mtmk::MockOtlpEncoder encoder;
    mtmk::MockWireCodec codec;
    // default: success=false, retryable=false
    mte::OtlpExporterConfig cfg;
    cfg.retry_policy = ZeroDelayRetry(3);
    mte::OtlpExporter exporter{&encoder, &codec, cfg};

    (void)exporter.Export(MakeBatch());
    ASSERT_EQ(exporter.ForceFlush(kFlushTimeout), mt::Status::Completed);
    EXPECT_EQ(codec.send_call_count.load(), 1);
}

// ---------------------------------------------------------------------------
// M5-A: Retry engine
// ---------------------------------------------------------------------------

TEST(OtlpExporterTest, Retry_RetryableFailure_ExhaustsMaxAttempts)
{
    mtmk::MockOtlpEncoder encoder;
    mtmk::FakeWireCodec codec;
    codec.default_result = mti::WireResult{.success = false, .retryable = true};

    mte::OtlpExporterConfig cfg;
    cfg.retry_policy = ZeroDelayRetry(3);
    mte::OtlpExporter exporter{&encoder, &codec, cfg};

    (void)exporter.Export(MakeBatch());
    ASSERT_EQ(exporter.ForceFlush(kFlushTimeout), mt::Status::Completed);
    // 3 attempts total (1 initial + 2 retries)
    EXPECT_EQ(codec.send_call_count.load(), 3);
    EXPECT_EQ(encoder.encode_call_count.load(), 3);
}

TEST(OtlpExporterTest, Retry_SuccessOnSecondAttempt_StopsRetrying)
{
    mtmk::MockOtlpEncoder encoder;
    mtmk::FakeWireCodec codec;
    codec.scripted_results.push_back(mti::WireResult{.success = false, .retryable = true});
    codec.default_result = mti::WireResult{.success = true};

    mte::OtlpExporterConfig cfg;
    cfg.retry_policy = ZeroDelayRetry(5);
    mte::OtlpExporter exporter{&encoder, &codec, cfg};

    (void)exporter.Export(MakeBatch());
    ASSERT_EQ(exporter.ForceFlush(kFlushTimeout), mt::Status::Completed);
    EXPECT_EQ(codec.send_call_count.load(), 2);
    EXPECT_EQ(encoder.encode_call_count.load(), 2);
}

TEST(OtlpExporterTest, Retry_NonRetryableAfterRetryable_StopsImmediately)
{
    mtmk::MockOtlpEncoder encoder;
    mtmk::FakeWireCodec codec;
    codec.scripted_results.push_back(mti::WireResult{.success = false, .retryable = true});
    codec.default_result = mti::WireResult{.success = false, .retryable = false};

    mte::OtlpExporterConfig cfg;
    cfg.retry_policy = ZeroDelayRetry(5);
    mte::OtlpExporter exporter{&encoder, &codec, cfg};

    (void)exporter.Export(MakeBatch());
    ASSERT_EQ(exporter.ForceFlush(kFlushTimeout), mt::Status::Completed);
    EXPECT_EQ(codec.send_call_count.load(), 2);
}

TEST(OtlpExporterTest, Retry_MaxAttemptsOne_NeverRetries)
{
    mtmk::MockOtlpEncoder encoder;
    mtmk::FakeWireCodec codec;
    codec.default_result = mti::WireResult{.success = false, .retryable = true};

    mte::OtlpExporterConfig cfg;
    cfg.retry_policy = ZeroDelayRetry(1);
    mte::OtlpExporter exporter{&encoder, &codec, cfg};

    (void)exporter.Export(MakeBatch());
    ASSERT_EQ(exporter.ForceFlush(kFlushTimeout), mt::Status::Completed);
    EXPECT_EQ(codec.send_call_count.load(), 1);
}

TEST(OtlpExporterTest, Retry_BudgetExhausted_StopsAfterFirstAttempt)
{
    mtmk::MockOtlpEncoder encoder;
    mtmk::FakeWireCodec codec;
    codec.default_result = mti::WireResult{.success = false, .retryable = true};
    mtmk::FakeSteadyClock clock;  // time = epoch

    mte::OtlpExporterConfig cfg;
    cfg.retry_policy = mte::RetryPolicyConfig{
        .max_attempts = 5,
        .initial_backoff = std::chrono::milliseconds{0},
        .max_backoff = std::chrono::milliseconds{0},
        .backoff_multiplier = 1.0,
        .jitter_fraction = 0.0,
        .retry_budget = std::chrono::milliseconds{0},  // budget = 0ms → exhausted immediately
    };
    mte::OtlpExporter exporter{&encoder, &codec, cfg, nullptr, &clock};

    (void)exporter.Export(MakeBatch());
    ASSERT_EQ(exporter.ForceFlush(kFlushTimeout), mt::Status::Completed);
    EXPECT_EQ(codec.send_call_count.load(), 1);
}

TEST(OtlpExporterTest, Retry_SuccessOnFirstAttempt_NeverRetries)
{
    mtmk::MockOtlpEncoder encoder;
    mtmk::FakeWireCodec codec;
    codec.default_result = mti::WireResult{.success = true};

    mte::OtlpExporterConfig cfg;
    cfg.retry_policy = ZeroDelayRetry(5);
    mte::OtlpExporter exporter{&encoder, &codec, cfg};

    (void)exporter.Export(MakeBatch());
    ASSERT_EQ(exporter.ForceFlush(kFlushTimeout), mt::Status::Completed);
    EXPECT_EQ(codec.send_call_count.load(), 1);
}

// ---------------------------------------------------------------------------
// Diagnostics wiring: batches_sent / batches_failed / last_error_message.
//
// Before this, OtlpExporter stored its IDiagnosticsSink* as [[maybe_unused]]
// and never called it, so every counter behind Provider::GetExporterHealth()
// read zero no matter what the exporter did. Reads below happen after
// ForceFlush, which synchronises with the worker through m_mu/m_cv.
// ---------------------------------------------------------------------------

TEST(OtlpExporterTest, Diagnostics_SuccessfulExport_RecordsBatchSent)
{
    mtmk::MockOtlpEncoder encoder;
    mtmk::FakeWireCodec codec;
    mtmk::FakeDiagnosticsSink sink;
    codec.default_result = mti::WireResult{.success = true};

    mte::OtlpExporterConfig cfg;
    cfg.retry_policy = ZeroDelayRetry(3);
    mte::OtlpExporter exporter{&encoder, &codec, cfg, &sink};

    (void)exporter.Export(MakeBatch());
    ASSERT_EQ(exporter.ForceFlush(kFlushTimeout), mt::Status::Completed);

    EXPECT_EQ(sink.batches_sent, 1U);
    EXPECT_EQ(sink.batches_failed, 0U);
}

TEST(OtlpExporterTest, Diagnostics_NonRetryableFailure_RecordsFailureAndMessage)
{
    mtmk::MockOtlpEncoder encoder;
    mtmk::FakeWireCodec codec;
    mtmk::FakeDiagnosticsSink sink;
    codec.default_result = mti::WireResult{
        .success = false,
        .retryable = false,
        .error = mt::Error{.kind = mt::Error::Kind::Network, .message = "connection refused"},
    };

    mte::OtlpExporterConfig cfg;
    cfg.retry_policy = ZeroDelayRetry(3);
    mte::OtlpExporter exporter{&encoder, &codec, cfg, &sink};

    (void)exporter.Export(MakeBatch());
    ASSERT_EQ(exporter.ForceFlush(kFlushTimeout), mt::Status::Completed);

    EXPECT_EQ(sink.batches_failed, 1U);
    EXPECT_EQ(sink.batches_sent, 0U);
    // The message is what an operator actually reads out of GetExporterHealth().
    EXPECT_EQ(sink.last_error_message, "connection refused");
    EXPECT_TRUE(sink.last_error_time.has_value());
}

TEST(OtlpExporterTest, Diagnostics_RetriesThenSucceeds_RecordsSentExactlyOnce)
{
    mtmk::MockOtlpEncoder encoder;
    mtmk::FakeWireCodec codec;
    mtmk::FakeDiagnosticsSink sink;
    codec.scripted_results.push_back(mti::WireResult{.success = false, .retryable = true});
    codec.scripted_results.push_back(mti::WireResult{.success = false, .retryable = true});
    codec.default_result = mti::WireResult{.success = true};

    mte::OtlpExporterConfig cfg;
    cfg.retry_policy = ZeroDelayRetry(5);
    mte::OtlpExporter exporter{&encoder, &codec, cfg, &sink};

    (void)exporter.Export(MakeBatch());
    ASSERT_EQ(exporter.ForceFlush(kFlushTimeout), mt::Status::Completed);

    // One batch, one outcome: intermediate retryable failures are attempts,
    // not failed batches. Counting each attempt would make batches_failed
    // meaningless as a batch counter.
    EXPECT_EQ(sink.batches_sent, 1U);
    EXPECT_EQ(sink.batches_failed, 0U);
}

TEST(OtlpExporterTest, Diagnostics_ExhaustedRetries_RecordsFailedExactlyOnce)
{
    mtmk::MockOtlpEncoder encoder;
    mtmk::FakeWireCodec codec;
    mtmk::FakeDiagnosticsSink sink;
    codec.default_result = mti::WireResult{
        .success = false,
        .retryable = true,
        .error = mt::Error{.kind = mt::Error::Kind::Network, .message = "timed out"},
    };

    mte::OtlpExporterConfig cfg;
    cfg.retry_policy = ZeroDelayRetry(3);
    mte::OtlpExporter exporter{&encoder, &codec, cfg, &sink};

    (void)exporter.Export(MakeBatch());
    ASSERT_EQ(exporter.ForceFlush(kFlushTimeout), mt::Status::Completed);

    EXPECT_EQ(codec.send_call_count.load(), 3);  // all attempts made
    EXPECT_EQ(sink.batches_failed, 1U);          // but one failed batch
    EXPECT_EQ(sink.last_error_message, "timed out");
}

TEST(OtlpExporterTest, Diagnostics_FailureWithNoErrorPayload_StillCountsAndNamesTheStage)
{
    mtmk::MockOtlpEncoder encoder;
    mtmk::FakeWireCodec codec;
    mtmk::FakeDiagnosticsSink sink;
    // A codec may report failure without populating `error`; the counter must
    // still move, and the message must not be empty for an operator reading it.
    codec.default_result = mti::WireResult{.success = false, .retryable = false};

    mte::OtlpExporterConfig cfg;
    cfg.retry_policy = ZeroDelayRetry(1);
    mte::OtlpExporter exporter{&encoder, &codec, cfg, &sink};

    (void)exporter.Export(MakeBatch());
    ASSERT_EQ(exporter.ForceFlush(kFlushTimeout), mt::Status::Completed);

    EXPECT_EQ(sink.batches_failed, 1U);
    EXPECT_FALSE(sink.last_error_message.empty());
}

TEST(OtlpExporterTest, Diagnostics_NullSink_IsNotDereferenced)
{
    mtmk::MockOtlpEncoder encoder;
    mtmk::FakeWireCodec codec;
    codec.default_result = mti::WireResult{.success = true};

    mte::OtlpExporterConfig cfg;
    cfg.retry_policy = ZeroDelayRetry(3);
    mte::OtlpExporter exporter{&encoder, &codec, cfg};  // no sink

    (void)exporter.Export(MakeBatch());
    EXPECT_EQ(exporter.ForceFlush(kFlushTimeout), mt::Status::Completed);
}

TEST(OtlpExporterTest, Diagnostics_QueueDepthPublishedOnEnqueueAndDrain)
{
    mtmk::MockOtlpEncoder encoder;
    mtmk::FakeWireCodec codec;
    mtmk::FakeDiagnosticsSink sink;
    codec.default_result = mti::WireResult{.success = true};

    mte::OtlpExporterConfig cfg;
    cfg.retry_policy = ZeroDelayRetry(3);
    mte::OtlpExporter exporter{&encoder, &codec, cfg, &sink};

    (void)exporter.Export(MakeBatch());
    ASSERT_EQ(exporter.ForceFlush(kFlushTimeout), mt::Status::Completed);

    // Depth must actually be published (not merely left at its zero default),
    // and must settle back to 0 after the drain rather than staying at its
    // high-water mark.
    EXPECT_GT(sink.queue_depth_call_count, 0U);
    EXPECT_EQ(sink.queue_depth_now, 0U);
}
