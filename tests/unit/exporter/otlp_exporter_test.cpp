// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for OtlpExporter (M3 close + M5-A retry engine).

#include "exporter/otlp_exporter.hpp"

#include "microtel/internal/batch.hpp"
#include "microtel/internal/exporter.hpp"
#include "microtel/provider.hpp"
#include "microtel/resource.hpp"
#include "microtel/status.hpp"

#include "fakes/fake_diagnostics_sink.hpp"
#include "fakes/fake_steady_clock.hpp"
#include "fakes/fake_wire_codec.hpp"
#include "mocks/mock_otlp_encoder.hpp"
#include "mocks/mock_wire_codec.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace mt = microtel;
namespace mti = microtel::internal;
namespace mtmk = microtel::testing;
namespace mte = microtel::exporter;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static mti::BatchHandle MakeBatchOf(std::size_t span_count)
{
    auto resource = std::make_shared<mt::Resource>();
    mti::InstrumentationScope scope{.name = "test", .version = "0.1"};
    std::vector<mti::SpanRecord> spans;
    spans.reserve(span_count);
    for (std::size_t i = 0; i < span_count; ++i)
    {
        spans.push_back(mti::SpanRecord{.name = "s"});
    }
    return mti::BatchHandle{std::move(spans), std::move(resource), std::move(scope)};
}

static mti::BatchHandle MakeBatch()
{
    return MakeBatchOf(1);
}

static std::uint64_t DropCount(const mtmk::FakeDiagnosticsSink& sink, mt::DropReason reason)
{
    return sink.drop_counters.at(static_cast<std::size_t>(reason));
}

static std::uint64_t TotalDrops(const mtmk::FakeDiagnosticsSink& sink)
{
    std::uint64_t total = 0;
    for (const auto count : sink.drop_counters)
    {
        total += count;
    }
    return total;
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
// Destructor without Shutdown
//
// CLAUDE.md rule 15: the destructor invokes Shutdown with a small finite
// timeout if not already shut down. Every lifecycle test above calls Shutdown
// explicitly, so nothing exercised the destructor-only path — the one an
// application takes when an exporter simply goes out of scope.
// ---------------------------------------------------------------------------

// Generous by design: the assertion is that teardown *terminates*, not a
// latency budget. The real bound is the destructor's own Shutdown timeout
// (`kDestructorShutdownTimeout`, 5s in `otlp_exporter.cpp`); a sanitizer build
// runs several times slower, and ctest's per-test timeout is the backstop for a
// destructor that wedges rather than merely running late.
static constexpr auto kDestructorTeardownBound = std::chrono::seconds(30);

TEST(OtlpExporterTest, DestroyedWithoutShutdown_DrainsAndJoinsWithinBound)
{
    mtmk::MockOtlpEncoder encoder;
    mtmk::MockWireCodec codec;
    codec.result_to_return.success = true;

    const auto start = std::chrono::steady_clock::now();
    {
        // Declared inside, so encoder and codec outlive the exporter that
        // borrows them — the same ordering the SDK guarantees by member
        // declaration order.
        mte::OtlpExporter exporter{&encoder, &codec};
        (void)exporter.Export(MakeBatch());
        // No Shutdown(), no ForceFlush(). Scope exit is the whole teardown.
    }
    const auto elapsed = std::chrono::steady_clock::now() - start;

    EXPECT_LT(elapsed, kDestructorTeardownBound);
    // Not merely "it returned": the queued batch was drained by the
    // destructor's Shutdown rather than abandoned with the worker thread.
    EXPECT_EQ(encoder.encode_call_count.load(), 1);
    EXPECT_EQ(codec.send_call_count.load(), 1);
}

TEST(OtlpExporterTest, DestroyedImmediatelyAfterConstruction_JoinsWithinBound)
{
    mtmk::MockOtlpEncoder encoder;
    mtmk::MockWireCodec codec;

    const auto start = std::chrono::steady_clock::now();
    {
        // `const` because nothing is called on it — construction and
        // destruction are the whole test, and the destructor runs regardless.
        const mte::OtlpExporter exporter{&encoder, &codec};
    }
    const auto elapsed = std::chrono::steady_clock::now() - start;

    EXPECT_LT(elapsed, kDestructorTeardownBound);
    EXPECT_EQ(encoder.encode_call_count.load(), 0);
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
    // A clean delivery attributes nothing: no drop counter may move.
    EXPECT_EQ(TotalDrops(sink), 0U);
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
    // batches_failed says a batch was lost; the counter says why (issue #169).
    EXPECT_EQ(DropCount(sink, mt::DropReason::NonRetryableFailure), 1U);
    EXPECT_EQ(DropCount(sink, mt::DropReason::RetryBudgetExhausted), 0U);
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
    // The recovery is the visibility this counter exists for: nothing was
    // lost, but the export path is unhealthy. Once per recovered batch, not
    // once per retried attempt.
    EXPECT_EQ(DropCount(sink, mt::DropReason::RetryableFailureRecovered), 1U);
    EXPECT_EQ(DropCount(sink, mt::DropReason::RetryBudgetExhausted), 0U);
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
    // Running out of attempts and running out of budget are the same outcome
    // for an operator — the batch was retried and still lost. One increment
    // either way, on the exit path the loop actually took.
    EXPECT_EQ(DropCount(sink, mt::DropReason::RetryBudgetExhausted), 1U);
    EXPECT_EQ(DropCount(sink, mt::DropReason::NonRetryableFailure), 0U);
}

TEST(OtlpExporterTest, Diagnostics_BudgetExhaustedOnEntry_RecordsExhaustedExactlyOnce)
{
    mtmk::MockOtlpEncoder encoder;
    mtmk::FakeWireCodec codec;
    mtmk::FakeDiagnosticsSink sink;
    codec.default_result = mti::WireResult{.success = false, .retryable = true};
    mtmk::FakeSteadyClock clock;  // never advances: budget of 0ms is spent on entry

    mte::OtlpExporterConfig cfg;
    cfg.retry_policy = mte::RetryPolicyConfig{
        .max_attempts = 5,
        .initial_backoff = std::chrono::milliseconds{0},
        .max_backoff = std::chrono::milliseconds{0},
        .backoff_multiplier = 1.0,
        .jitter_fraction = 0.0,
        .retry_budget = std::chrono::milliseconds{0},
    };
    mte::OtlpExporter exporter{&encoder, &codec, cfg, &sink, &clock};

    (void)exporter.Export(MakeBatch());
    ASSERT_EQ(exporter.ForceFlush(kFlushTimeout), mt::Status::Completed);

    // The budget-on-entry early return takes a different code path from
    // attempt exhaustion, and must still land on exactly one counter.
    EXPECT_EQ(codec.send_call_count.load(), 1);
    EXPECT_EQ(DropCount(sink, mt::DropReason::RetryBudgetExhausted), 1U);
}

TEST(OtlpExporterTest, Diagnostics_PartialSuccessRejection_CountsRejectedRecords)
{
    mtmk::MockOtlpEncoder encoder;
    mtmk::FakeWireCodec codec;
    mtmk::FakeDiagnosticsSink sink;
    // The partial-success shape: the request succeeded, but the collector
    // rejected some of the records in it (error-model.md §6). Both codecs
    // populate this field; until now nothing read it.
    codec.default_result = mti::WireResult{.success = true, .partial_success_rejected = 3};

    mte::OtlpExporterConfig cfg;
    cfg.retry_policy = ZeroDelayRetry(3);
    mte::OtlpExporter exporter{&encoder, &codec, cfg, &sink};

    (void)exporter.Export(MakeBatch());
    ASSERT_EQ(exporter.ForceFlush(kFlushTimeout), mt::Status::Completed);

    // Still a sent batch — partial success is never retried and never a
    // failed batch — but the rejected records must be attributable.
    //
    // The send count is the direct assertion of "never retried": the retry
    // policy allows three attempts and the result carries rejections, so an
    // exporter that treated partial success as a failure would show 3 here.
    // Without it the test proved only that the counters were right, which a
    // retrying implementation could also manage.
    EXPECT_EQ(codec.send_call_count.load(), 1);
    EXPECT_EQ(sink.batches_sent, 1U);
    EXPECT_EQ(sink.batches_failed, 0U);
    EXPECT_EQ(DropCount(sink, mt::DropReason::PartialSuccessRejection), 3U);
}

TEST(OtlpExporterTest, Diagnostics_QueueFull_CountsEveryRecordInTheRejectedBatch)
{
    mtmk::MockOtlpEncoder encoder;
    mtmk::MockWireCodec codec;
    mtmk::FakeDiagnosticsSink sink;
    mte::OtlpExporterConfig cfg;
    cfg.max_queue_size = 0;  // every Export is rejected
    mte::OtlpExporter exporter{&encoder, &codec, cfg, &sink};

    EXPECT_EQ(exporter.Export(MakeBatchOf(4)), mti::ExportResult::Dropped);

    // The unit of loss is the span, not the batch: a rejected 4-span batch
    // loses 4 spans, and that is the number an operator needs.
    EXPECT_EQ(DropCount(sink, mt::DropReason::QueueFull), 4U);
    EXPECT_EQ(DropCount(sink, mt::DropReason::PostShutdown), 0U);
}

TEST(OtlpExporterTest, Diagnostics_ExportAfterShutdown_CountsPostShutdown)
{
    mtmk::MockOtlpEncoder encoder;
    mtmk::MockWireCodec codec;
    mtmk::FakeDiagnosticsSink sink;
    mte::OtlpExporter exporter{&encoder, &codec, {}, &sink};

    ASSERT_EQ(exporter.Shutdown(kFlushTimeout), mt::Status::Completed);
    EXPECT_EQ(exporter.Export(MakeBatchOf(2)), mti::ExportResult::AlreadyShutDown);

    EXPECT_EQ(DropCount(sink, mt::DropReason::PostShutdown), 2U);
    EXPECT_EQ(DropCount(sink, mt::DropReason::QueueFull), 0U);
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
    EXPECT_EQ(DropCount(sink, mt::DropReason::NonRetryableFailure), 1U);
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
