// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Every Span method is noexcept (docs/error-model.md §2.2, LOCKED) and every
// one of them grows a std::string or std::vector. An allocation failure inside
// a noexcept frame calls std::terminate -- so a telemetry library could take
// the host process down at exactly the moment the host was already out of
// memory. §2.2 requires dropping the field and returning silently instead.
//
// The only honest way to test that is to actually make allocation fail, so
// these tests replace global operator new with one that throws on demand. That
// is process-wide, hence a dedicated binary: enabling the failure switch while
// gtest itself allocates would abort the run.

#include "microtel/attribute.hpp"
#include "microtel/error.hpp"
#include "microtel/resource.hpp"
#include "microtel/trace.hpp"

#include "mocks/mock_exporter.hpp"
#include "mocks/mock_span_processor.hpp"
#include "sdk/diagnostics_counters.hpp"
#include "sdk/sdk_span.hpp"
#include "sdk/simple_span_processor.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <memory>
#include <new>
#include <string>
#include <utility>

namespace
{

// Armed only around the exact call under test.
std::atomic<bool> g_fail_allocations{false};

struct ScopedAllocFailure
{
    ScopedAllocFailure() noexcept
    {
        g_fail_allocations.store(true, std::memory_order_release);
    }
    ~ScopedAllocFailure() noexcept
    {
        g_fail_allocations.store(false, std::memory_order_release);
    }
    ScopedAllocFailure(const ScopedAllocFailure&) = delete;
    ScopedAllocFailure& operator=(const ScopedAllocFailure&) = delete;
    ScopedAllocFailure(ScopedAllocFailure&&) = delete;
    ScopedAllocFailure& operator=(ScopedAllocFailure&&) = delete;
};

}  // namespace

// Replacing global operator new is legal and is the only way to drive the
// bad_alloc path deterministically.
void* operator new(std::size_t size)
{
    if (g_fail_allocations.load(std::memory_order_acquire))
    {
        throw std::bad_alloc{};
    }
    void* const p = std::malloc(size);  // NOLINT(cppcoreguidelines-no-malloc)
    if (p == nullptr)
    {
        throw std::bad_alloc{};
    }
    return p;
}

void operator delete(void* p) noexcept
{
    std::free(p);  // NOLINT(cppcoreguidelines-no-malloc)
}

void operator delete(void* p, std::size_t /*size*/) noexcept
{
    std::free(p);  // NOLINT(cppcoreguidelines-no-malloc)
}

namespace
{

namespace mt = microtel;
namespace mti = microtel::internal;
namespace mts = microtel::sdk;
namespace mtmk = microtel::testing;

// A long value forces a heap allocation rather than a small-string one.
std::string LongValue()
{
    return std::string(512, 'x');
}

std::unique_ptr<mts::SdkSpan> MakeSpan(mtmk::MockSpanProcessor& processor)
{
    return std::make_unique<mts::SdkSpan>(mt::SpanContext{},
                                          mt::SpanContext{},
                                          "span",
                                          mt::SpanKind::Internal,
                                          std::chrono::system_clock::time_point{},
                                          &processor,
                                          std::make_shared<mt::Resource>(),
                                          mti::InstrumentationScope{.name = "s", .version = "1"},
                                          mt::SpanLimitOptions{});
}

// Each of these would have called std::terminate before the fix. Surviving the
// call *is* the assertion; the process being alive to run EXPECT is the proof.

TEST(NoexceptAllocFailureTest, SetAttributeSurvivesAllocationFailure)
{
    mtmk::MockSpanProcessor processor;
    auto span = MakeSpan(processor);
    const std::string key = LongValue();

    {
        const ScopedAllocFailure fail;
        span->SetAttribute(key, mt::AttributeValue{true});
    }
    SUCCEED();
}

TEST(NoexceptAllocFailureTest, AddEventSurvivesAllocationFailure)
{
    mtmk::MockSpanProcessor processor;
    auto span = MakeSpan(processor);
    const std::string name = LongValue();

    {
        const ScopedAllocFailure fail;
        span->AddEvent(name, {}, std::chrono::system_clock::time_point{});
    }
    SUCCEED();
}

TEST(NoexceptAllocFailureTest, AddLinkSurvivesAllocationFailure)
{
    mtmk::MockSpanProcessor processor;
    auto span = MakeSpan(processor);
    const std::vector<mt::KeyValue> attrs{{.key = "k", .value = mt::AttributeValue{true}}};

    {
        const ScopedAllocFailure fail;
        span->AddLink(mt::SpanContext{}, mt::AttributeSpan{attrs});
    }
    SUCCEED();
}

TEST(NoexceptAllocFailureTest, SetStatusSurvivesAllocationFailure)
{
    mtmk::MockSpanProcessor processor;
    auto span = MakeSpan(processor);
    const std::string description = LongValue();

    {
        const ScopedAllocFailure fail;
        span->SetStatus(mt::StatusCode::Error, description);
    }
    SUCCEED();
}

TEST(NoexceptAllocFailureTest, UpdateNameSurvivesAllocationFailure)
{
    mtmk::MockSpanProcessor processor;
    auto span = MakeSpan(processor);
    const std::string name = LongValue();

    {
        const ScopedAllocFailure fail;
        span->UpdateName(name);
    }
    SUCCEED();
}

// The span stays usable afterwards: a dropped field must not corrupt it.
TEST(NoexceptAllocFailureTest, SpanRemainsUsableAfterADroppedField)
{
    mtmk::MockSpanProcessor processor;
    auto span = MakeSpan(processor);
    // Built before arming: constructing it inside the scope would throw in the
    // test body rather than inside the noexcept method under test.
    const std::string key = LongValue();

    {
        const ScopedAllocFailure fail;
        span->SetAttribute(key, mt::AttributeValue{true});
    }

    span->SetAttribute("recovered", mt::AttributeValue{true});
    span->End();
    EXPECT_EQ(processor.on_end_call_count, 1);
}


// The same defect in two more noexcept frames, found by the same audit.

TEST(NoexceptAllocFailureTest, SimpleSpanProcessorOnEndSurvivesAllocationFailure)
{
    mtmk::MockExporter exporter;
    mti::SimpleSpanProcessor processor{&exporter,
                                       std::make_shared<mt::Resource>(),
                                       mti::InstrumentationScope{.name = "s", .version = "1"}};
    mti::SpanRecord record;

    {
        const ScopedAllocFailure fail;
        processor.OnEnd(std::move(record));
    }
    SUCCEED();
}

TEST(NoexceptAllocFailureTest, DiagnosticsSnapshotSurvivesAllocationFailure)
{
    mts::DiagnosticsCounters counters;
    // A long message so the stored string is heap-allocated and its copy in
    // Snapshot() has to allocate.
    counters.RecordBatchFailed(
        mt::Error{.kind = mt::Error::Kind::Network, .message = std::string(200, 'e')});

    {
        const ScopedAllocFailure fail;
        const auto snap = counters.Snapshot();
        // Counters still readable; only the message is sacrificed.
        EXPECT_EQ(snap.batches_failed, 1U);
    }
    SUCCEED();
}

}  // namespace
