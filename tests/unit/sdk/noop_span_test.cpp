// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for the noop span path and SpanDeleter.
// Uses SdkTracer + AlwaysOff sampler so StartSpan returns the noop
// singleton without allocating or touching the processor.

#include "microtel/context.hpp"
#include "microtel/sampler.hpp"
#include "microtel/span.hpp"
#include "microtel/trace.hpp"

#include "fakes/fake_span_processor.hpp"
#include "sdk/sdk_tracer.hpp"

#include <gtest/gtest.h>

#include <memory>

namespace mt = microtel;
namespace mti = microtel::internal;
namespace mtfk = microtel::testing;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

struct NoopFixture
{
    std::shared_ptr<const mt::Resource> resource = std::make_shared<const mt::Resource>();
    mt::SamplerHandle sampler = mt::MakeAlwaysOffSampler();
    mtfk::FakeSpanProcessor proc;
    mt::sdk::SdkTracer tracer{
        sampler.Get(), &proc, resource, {.name = "test", .version = ""}, mt::SpanLimitOptions{}};
};

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST(NoopSpanTest, NoopSpan_IsNotSampled)
{
    NoopFixture f;
    const auto handle = f.tracer.StartSpan("op");
    ASSERT_NE(handle, nullptr);
    EXPECT_FALSE(handle->IsSampled());
}

TEST(NoopSpanTest, NoopSpan_GetContext_ReturnsInvalid)
{
    NoopFixture f;
    const auto handle = f.tracer.StartSpan("op");
    ASSERT_NE(handle, nullptr);
    EXPECT_FALSE(handle->GetContext().IsValid());
}

TEST(NoopSpanTest, NoopSpan_SetAttribute_IsNoOp)
{
    NoopFixture f;
    auto handle = f.tracer.StartSpan("op");
    EXPECT_NO_THROW(handle->SetAttribute("k", std::int64_t{1}));
}

TEST(NoopSpanTest, NoopSpan_AddEvent_IsNoOp)
{
    NoopFixture f;
    auto handle = f.tracer.StartSpan("op");
    EXPECT_NO_THROW(handle->AddEvent("evt"));
}

TEST(NoopSpanTest, NoopSpan_AddLink_IsNoOp)
{
    NoopFixture f;
    auto handle = f.tracer.StartSpan("op");
    mt::SpanContext linked;
    EXPECT_NO_THROW(handle->AddLink(linked));
}

TEST(NoopSpanTest, NoopSpan_SetStatus_IsNoOp)
{
    NoopFixture f;
    auto handle = f.tracer.StartSpan("op");
    EXPECT_NO_THROW(handle->SetStatus(mt::StatusCode::Error));
}

TEST(NoopSpanTest, NoopSpan_UpdateName_IsNoOp)
{
    NoopFixture f;
    auto handle = f.tracer.StartSpan("op");
    EXPECT_NO_THROW(handle->UpdateName("new"));
}

TEST(NoopSpanTest, NoopSpan_End_IsNoOp)
{
    NoopFixture f;
    auto handle = f.tracer.StartSpan("op");
    EXPECT_NO_THROW(handle->End());
    EXPECT_NO_THROW(handle->End());
}

TEST(NoopSpanTest, NoopSpan_OnEndNotCalled_WhenDropped)
{
    NoopFixture f;
    {
        auto handle = f.tracer.StartSpan("op");
        handle->End();
    }
    EXPECT_TRUE(f.proc.received_spans.empty());
}

TEST(NoopSpanTest, NoopSpan_Destructor_DoesNotDelete)
{
    // The noop singleton must not be deleted by unique_ptr destructor.
    // ASAN would catch a double-free or delete-of-static-storage.
    NoopFixture f;
    {
        auto h1 = f.tracer.StartSpan("a");
        auto h2 = f.tracer.StartSpan("b");
        // Both handles point at the same static instance; both must
        // safely destruct without crash.
    }
}

TEST(NoopSpanTest, SpanDeleter_NullDeleter_IsNoOp)
{
    mt::internal::SpanDeleter d{};
    EXPECT_NO_THROW(d(nullptr));
}
