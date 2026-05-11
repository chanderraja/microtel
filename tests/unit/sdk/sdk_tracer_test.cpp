// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for SdkTracer: sampling decisions, ID generation, parent
// propagation, and StartAsCurrentSpan stub.

#include "sdk/sdk_tracer.hpp"

#include "microtel/context.hpp"
#include "microtel/sampler.hpp"
#include "microtel/sdk_builder.hpp"
#include "microtel/span.hpp"
#include "microtel/trace.hpp"

#include "fakes/fake_span_processor.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <memory>
#include <set>

namespace mt = microtel;
namespace mtfk = microtel::testing;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

struct TracerFixture
{
    std::shared_ptr<const mt::Resource> resource = std::make_shared<const mt::Resource>();
    mt::SamplerHandle sampler_owner;  // keeps ISampler alive for the tracer
    mtfk::FakeSpanProcessor proc;

    mt::sdk::SdkTracer MakeTracer(mt::SamplerHandle sampler)
    {
        sampler_owner = std::move(sampler);
        return mt::sdk::SdkTracer{sampler_owner.Get(),
                                  &proc,
                                  resource,
                                  {.name = "lib", .version = "1.0"},
                                  mt::SpanLimitOptions{}};
    }
};

// ---------------------------------------------------------------------------
// AlwaysOff → noop path
// ---------------------------------------------------------------------------

TEST(SdkTracerTest, AlwaysOff_SpanIsNotSampled)
{
    TracerFixture f;
    auto t = f.MakeTracer(mt::MakeAlwaysOffSampler());
    const auto h = t.StartSpan("op");
    ASSERT_NE(h, nullptr);
    EXPECT_FALSE(h->IsSampled());
}

TEST(SdkTracerTest, AlwaysOff_OnEndNotCalled)
{
    TracerFixture f;
    auto t = f.MakeTracer(mt::MakeAlwaysOffSampler());
    {
        auto h = t.StartSpan("op");
        h->End();
    }
    EXPECT_TRUE(f.proc.received_spans.empty());
}

TEST(SdkTracerTest, AlwaysOff_HandleNotNull)
{
    TracerFixture f;
    auto t = f.MakeTracer(mt::MakeAlwaysOffSampler());
    const auto h = t.StartSpan("op");
    EXPECT_NE(h, nullptr);
}

// ---------------------------------------------------------------------------
// AlwaysOn → sampled path
// ---------------------------------------------------------------------------

TEST(SdkTracerTest, AlwaysOn_SpanIsSampled)
{
    TracerFixture f;
    auto t = f.MakeTracer(mt::MakeAlwaysOnSampler());
    const auto h = t.StartSpan("op");
    ASSERT_NE(h, nullptr);
    EXPECT_TRUE(h->IsSampled());
}

TEST(SdkTracerTest, AlwaysOn_GetContext_IsValid)
{
    TracerFixture f;
    auto t = f.MakeTracer(mt::MakeAlwaysOnSampler());
    const auto h = t.StartSpan("op");
    ASSERT_NE(h, nullptr);
    EXPECT_TRUE(h->GetContext().IsValid());
}

TEST(SdkTracerTest, AlwaysOn_OnEndCalled_OnEnd)
{
    TracerFixture f;
    auto t = f.MakeTracer(mt::MakeAlwaysOnSampler());
    {
        auto h = t.StartSpan("op");
        h->End();
    }
    ASSERT_EQ(f.proc.received_spans.size(), 1U);
    EXPECT_EQ(f.proc.received_spans[0].name, "op");
}

TEST(SdkTracerTest, AlwaysOn_MultipleSpans_UniqueTraceIds)
{
    TracerFixture f;
    auto t = f.MakeTracer(mt::MakeAlwaysOnSampler());

    // Root spans must have unique TraceIds.
    std::set<mt::TraceId::Bytes> ids;
    constexpr int kN = 8;
    for (int i = 0; i < kN; ++i)
    {
        auto h = t.StartSpan("op");
        ids.insert(h->GetContext().trace_id.AsBytes());
        h->End();
    }
    EXPECT_EQ(static_cast<int>(ids.size()), kN);
}

TEST(SdkTracerTest, AlwaysOn_MultipleSpans_UniqueSpanIds)
{
    TracerFixture f;
    auto t = f.MakeTracer(mt::MakeAlwaysOnSampler());

    std::set<mt::SpanId::Bytes> ids;
    constexpr int kN = 8;
    for (int i = 0; i < kN; ++i)
    {
        auto h = t.StartSpan("op");
        ids.insert(h->GetContext().span_id.AsBytes());
        h->End();
    }
    EXPECT_EQ(static_cast<int>(ids.size()), kN);
}

// ---------------------------------------------------------------------------
// Parent propagation
// ---------------------------------------------------------------------------

TEST(SdkTracerTest, WithExplicitParent_InheritsTraceId)
{
    TracerFixture f;
    auto t = f.MakeTracer(mt::MakeAlwaysOnSampler());

    mt::TraceId::Bytes parent_tid{};
    parent_tid[0] = 0xAB;
    mt::SpanId::Bytes parent_sid{};
    parent_sid[0] = 0xCD;

    mt::SpanContext parent_ctx{
        .trace_id = mt::TraceId{parent_tid},
        .span_id = mt::SpanId{parent_sid},
        .trace_flags = {},
        .trace_state = {},
        .remote = false,
    };

    const mt::StartSpanOptions opts{
        .kind = mt::SpanKind::Internal, .parent = parent_ctx, .start_time = {}, .attributes = {}};
    auto h = t.StartSpan("child", opts);
    ASSERT_NE(h, nullptr);

    EXPECT_EQ(h->GetContext().trace_id.AsBytes(), parent_tid);
}

TEST(SdkTracerTest, WithExplicitParent_SpanIdIsNew)
{
    TracerFixture f;
    auto t = f.MakeTracer(mt::MakeAlwaysOnSampler());

    mt::TraceId::Bytes parent_tid{};
    parent_tid[0] = 1;
    mt::SpanId::Bytes parent_sid{};
    parent_sid[0] = 2;

    mt::SpanContext parent_ctx{
        .trace_id = mt::TraceId{parent_tid},
        .span_id = mt::SpanId{parent_sid},
        .trace_flags = {},
        .trace_state = {},
        .remote = false,
    };

    const mt::StartSpanOptions opts{
        .kind = mt::SpanKind::Internal, .parent = parent_ctx, .start_time = {}, .attributes = {}};
    auto h = t.StartSpan("child", opts);
    ASSERT_NE(h, nullptr);

    EXPECT_NE(h->GetContext().span_id.AsBytes(), parent_sid);
}

TEST(SdkTracerTest, WithExplicitParent_RecordsParentSpanId)
{
    TracerFixture f;
    auto t = f.MakeTracer(mt::MakeAlwaysOnSampler());

    mt::TraceId::Bytes parent_tid{};
    parent_tid[0] = 1;
    mt::SpanId::Bytes parent_sid{};
    parent_sid[0] = 2;

    mt::SpanContext parent_ctx{
        .trace_id = mt::TraceId{parent_tid},
        .span_id = mt::SpanId{parent_sid},
        .trace_flags = {},
        .trace_state = {},
        .remote = false,
    };

    const mt::StartSpanOptions opts{
        .kind = mt::SpanKind::Internal, .parent = parent_ctx, .start_time = {}, .attributes = {}};
    {
        auto h = t.StartSpan("child", opts);
        h->End();
    }

    ASSERT_EQ(f.proc.received_spans.size(), 1U);
    EXPECT_EQ(f.proc.received_spans[0].parent_context.span_id.AsBytes(), parent_sid);
}

// ---------------------------------------------------------------------------
// StartTime
// ---------------------------------------------------------------------------

TEST(SdkTracerTest, ExplicitStartTime_RecordedInSpan)
{
    TracerFixture f;
    auto t = f.MakeTracer(mt::MakeAlwaysOnSampler());
    const auto ts = std::chrono::system_clock::time_point{std::chrono::seconds{12345}};
    {
        auto h = t.StartSpan(
            "op",
            {.kind = mt::SpanKind::Internal, .parent = {}, .start_time = ts, .attributes = {}});
        h->End();
    }
    ASSERT_EQ(f.proc.received_spans.size(), 1U);
    EXPECT_EQ(f.proc.received_spans[0].start_time, ts);
}

// ---------------------------------------------------------------------------
// OnStart callback
// ---------------------------------------------------------------------------

TEST(SdkTracerTest, OnStart_CalledOnSampledSpan)
{
    TracerFixture f;
    auto t = f.MakeTracer(mt::MakeAlwaysOnSampler());
    (void)t.StartSpan("op");
    EXPECT_EQ(f.proc.on_start_call_count, 1);
}

TEST(SdkTracerTest, OnStart_NotCalled_WhenDropped)
{
    TracerFixture f;
    auto t = f.MakeTracer(mt::MakeAlwaysOffSampler());
    (void)t.StartSpan("op");
    EXPECT_EQ(f.proc.on_start_call_count, 0);
}

// ---------------------------------------------------------------------------
// StartAsCurrentSpan — stub (deferred to v1.1)
// ---------------------------------------------------------------------------

TEST(SdkTracerTest, StartAsCurrentSpan_BehavesLikeStartSpan)
{
    TracerFixture f;
    auto t = f.MakeTracer(mt::MakeAlwaysOnSampler());
    const auto h = t.StartAsCurrentSpan("op");
    ASSERT_NE(h, nullptr);
    EXPECT_TRUE(h->IsSampled());
}
