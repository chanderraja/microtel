// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for SdkTracer: sampling decisions, ID generation, explicit and
// implicit parent propagation, and StartAsCurrentSpan's ScopedSpan semantics
// (issue #221, ICP 0025 §3).

#include "sdk/sdk_tracer.hpp"

#include "microtel/attribute.hpp"
#include "microtel/baggage.hpp"
#include "microtel/context.hpp"
#include "microtel/provider.hpp"
#include "microtel/sampler.hpp"
#include "microtel/sdk_builder.hpp"
#include "microtel/span.hpp"
#include "microtel/trace.hpp"

#include "fakes/fake_diagnostics_sink.hpp"
#include "fakes/fake_span_processor.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <variant>

namespace mt = microtel;
namespace mtfk = microtel::testing;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

namespace
{

/// A valid, sampled `SpanContext` whose ids are distinguishable by @p seed.
mt::SpanContext MakeParentContext(std::uint8_t seed)
{
    mt::TraceId::Bytes trace_bytes{};
    trace_bytes[0] = seed;
    mt::SpanId::Bytes span_bytes{};
    span_bytes[0] = seed;
    return mt::SpanContext{
        .trace_id = mt::TraceId{trace_bytes},
        .span_id = mt::SpanId{span_bytes},
        .trace_flags = mt::TraceFlags{mt::TraceFlags::kSampled},
        .trace_state = {},
        .remote = false,
    };
}

/// How many drops @p sink counted against @p reason.
std::uint64_t DropCount(const mtfk::FakeDiagnosticsSink& sink, mt::DropReason reason)
{
    return sink.drop_counters.at(static_cast<std::size_t>(reason));
}

}  // namespace

struct TracerFixture
{
    std::shared_ptr<const mt::Resource> resource = std::make_shared<const mt::Resource>();
    mt::SamplerHandle sampler_owner;  // keeps ISampler alive for the tracer
    mtfk::FakeSpanProcessor proc;
    mtfk::FakeDiagnosticsSink diag;

    /// Read by `MakeTracer`; set before calling it to exercise a limit.
    mt::SpanLimitOptions limits;

    mt::sdk::SdkTracer MakeTracer(mt::SamplerHandle sampler)
    {
        sampler_owner = std::move(sampler);
        return mt::sdk::SdkTracer{
            sampler_owner.Get(), &proc, resource, {.name = "lib", .version = "1.0"}, limits, &diag};
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

// The `Context` handed to `OnStart` carries the *whole* propagated context,
// not just the resolved parent — ICP 0025 §2 and the "Not in this ICP" note
// that named `parent_propagation_ctx` as the site. Baggage is per-context, so
// it comes from the starting thread's current context regardless of how the
// parent was resolved (ICP 0025 §3 contract 6: baggage never parents).

TEST(SdkTracerTest, OnStart_ContextCarriesTheCurrentBaggage)
{
    TracerFixture f;
    auto t = f.MakeTracer(mt::MakeAlwaysOnSampler());

    const mt::ScopedContext scope{
        mt::Context{MakeParentContext(0x6A), mt::Baggage::FromHeader("tenant=t1")}};
    (void)t.StartSpan("child");

    ASSERT_EQ(f.proc.started_contexts.size(), 1U);
    EXPECT_EQ(f.proc.started_contexts[0].baggage.Get("tenant"), std::string_view("t1"));
}

TEST(SdkTracerTest, OnStart_ContextCarriesBaggageEvenWithAnExplicitParent)
{
    TracerFixture f;
    auto t = f.MakeTracer(mt::MakeAlwaysOnSampler());

    const mt::ScopedContext scope{
        mt::Context{mt::SpanContext{}, mt::Baggage::FromHeader("tenant=t2")}};
    (void)t.StartSpan("child",
                      {.kind = mt::SpanKind::Internal,
                       .parent = MakeParentContext(0x6B),
                       .start_time = {},
                       .attributes = {}});

    ASSERT_EQ(f.proc.started_contexts.size(), 1U);
    EXPECT_EQ(f.proc.started_contexts[0].baggage.Get("tenant"), std::string_view("t2"));
}

TEST(SdkTracerTest, OnStart_ContextBaggageIsEmptyWithoutACurrentContext)
{
    TracerFixture f;
    auto t = f.MakeTracer(mt::MakeAlwaysOnSampler());
    (void)t.StartSpan("root");

    ASSERT_EQ(f.proc.started_contexts.size(), 1U);
    EXPECT_TRUE(f.proc.started_contexts[0].baggage.Empty());
}

// ---------------------------------------------------------------------------
// Implicit parent from the current context (#221, ICP 0025 §3 contracts 1-2)
// ---------------------------------------------------------------------------

TEST(SdkTracerTest, StartSpan_UnsetParent_InheritsFromTheCurrentContext)
{
    TracerFixture f;
    auto t = f.MakeTracer(mt::MakeAlwaysOnSampler());

    const mt::SpanContext installed = MakeParentContext(0x5A);
    const mt::ScopedContext scope{mt::Context{installed}};

    auto h = t.StartSpan("child");
    ASSERT_NE(h, nullptr);
    EXPECT_EQ(h->GetContext().trace_id.AsBytes(), installed.trace_id.AsBytes());
}

TEST(SdkTracerTest, StartSpan_UnsetParent_RecordsTheCurrentSpanAsParent)
{
    TracerFixture f;
    auto t = f.MakeTracer(mt::MakeAlwaysOnSampler());

    const mt::SpanContext installed = MakeParentContext(0x5B);
    {
        const mt::ScopedContext scope{mt::Context{installed}};
        auto h = t.StartSpan("child");
        h->End();
    }

    ASSERT_EQ(f.proc.received_spans.size(), 1U);
    EXPECT_EQ(f.proc.received_spans[0].parent_context.span_id.AsBytes(),
              installed.span_id.AsBytes());
}

TEST(SdkTracerTest, StartSpan_ExplicitParent_WinsOverTheCurrentContext)
{
    TracerFixture f;
    auto t = f.MakeTracer(mt::MakeAlwaysOnSampler());

    const mt::SpanContext installed = MakeParentContext(0x11);
    const mt::SpanContext explicit_parent = MakeParentContext(0x22);
    const mt::ScopedContext scope{mt::Context{installed}};

    auto h = t.StartSpan("child",
                         {.kind = mt::SpanKind::Internal,
                          .parent = explicit_parent,
                          .start_time = {},
                          .attributes = {}});
    ASSERT_NE(h, nullptr);
    EXPECT_EQ(h->GetContext().trace_id.AsBytes(), explicit_parent.trace_id.AsBytes());
}

TEST(SdkTracerTest, StartSpan_ExplicitInvalidParent_IsARootDespiteTheCurrentContext)
{
    TracerFixture f;
    auto t = f.MakeTracer(mt::MakeAlwaysOnSampler());

    const mt::SpanContext installed = MakeParentContext(0x33);
    const mt::ScopedContext scope{mt::Context{installed}};

    // Set-but-invalid is how the otel-cpp shim asks for an explicit root.
    auto h = t.StartSpan("root",
                         {.kind = mt::SpanKind::Internal,
                          .parent = mt::SpanContext{},
                          .start_time = {},
                          .attributes = {}});
    ASSERT_NE(h, nullptr);
    EXPECT_NE(h->GetContext().trace_id.AsBytes(), installed.trace_id.AsBytes());
}

TEST(SdkTracerTest, StartSpan_NoCurrentContext_StillStartsARoot)
{
    TracerFixture f;
    auto t = f.MakeTracer(mt::MakeAlwaysOnSampler());
    {
        auto h = t.StartSpan("root");
        h->End();
    }
    ASSERT_EQ(f.proc.received_spans.size(), 1U);
    EXPECT_FALSE(f.proc.received_spans[0].parent_context.IsValid());
}

// ---------------------------------------------------------------------------
// StartAsCurrentSpan — ScopedSpan (#221, ICP 0025 §3)
// ---------------------------------------------------------------------------

static_assert(std::is_nothrow_move_constructible_v<mt::ScopedSpan>,
              "StartAsCurrentSpan returns a ScopedSpan by value from a noexcept method");
static_assert(std::is_nothrow_default_constructible_v<mt::ScopedSpan>);
static_assert(std::is_nothrow_destructible_v<mt::ScopedSpan>);
static_assert(!std::is_copy_constructible_v<mt::ScopedSpan>);
static_assert(!std::is_move_assignable_v<mt::ScopedSpan>);

TEST(SdkTracerTest, StartAsCurrentSpan_ReturnsAUsableSpan)
{
    TracerFixture f;
    auto t = f.MakeTracer(mt::MakeAlwaysOnSampler());
    const auto scoped = t.StartAsCurrentSpan("op");
    ASSERT_NE(scoped.Get(), nullptr);
    EXPECT_TRUE(scoped->IsSampled());
}

TEST(SdkTracerTest, StartAsCurrentSpan_InstallsItselfAsCurrent)
{
    TracerFixture f;
    auto t = f.MakeTracer(mt::MakeAlwaysOnSampler());
    const auto scoped = t.StartAsCurrentSpan("op");
    EXPECT_EQ(mt::CurrentContext().active_span_context.span_id.AsBytes(),
              scoped->GetContext().span_id.AsBytes());
}

TEST(SdkTracerTest, StartAsCurrentSpan_RestoresOnScopeExit)
{
    TracerFixture f;
    auto t = f.MakeTracer(mt::MakeAlwaysOnSampler());
    {
        const auto scoped = t.StartAsCurrentSpan("op");
        ASSERT_TRUE(mt::CurrentContext().active_span_context.IsValid());
    }
    EXPECT_FALSE(mt::CurrentContext().active_span_context.IsValid());
}

TEST(SdkTracerTest, StartAsCurrentSpan_ChildParentsToTheEnclosingScope)
{
    TracerFixture f;
    auto t = f.MakeTracer(mt::MakeAlwaysOnSampler());
    {
        const auto parent = t.StartAsCurrentSpan("parent");
        const auto child = t.StartAsCurrentSpan("child");
        EXPECT_EQ(child->GetContext().trace_id.AsBytes(), parent->GetContext().trace_id.AsBytes());
        EXPECT_NE(child->GetContext().span_id.AsBytes(), parent->GetContext().span_id.AsBytes());
    }
}

TEST(SdkTracerTest, StartAsCurrentSpan_NestedScopesUnwindInReverse)
{
    TracerFixture f;
    auto t = f.MakeTracer(mt::MakeAlwaysOnSampler());
    const auto outer = t.StartAsCurrentSpan("outer");
    const mt::SpanId::Bytes outer_id = outer->GetContext().span_id.AsBytes();
    {
        const auto inner = t.StartAsCurrentSpan("inner");
        ASSERT_NE(mt::CurrentContext().active_span_context.span_id.AsBytes(), outer_id);
    }
    EXPECT_EQ(mt::CurrentContext().active_span_context.span_id.AsBytes(), outer_id);
}

// The span is ended while its scope — and a nested scope — are still live.
// `End()` is a span-lifecycle operation; it does not pop the context slot.
// The ended span therefore stays current until its ScopedSpan is destroyed.
TEST(SdkTracerTest, StartAsCurrentSpan_EndBeforeScopeExit_LeavesTheContextInstalled)
{
    TracerFixture f;
    auto t = f.MakeTracer(mt::MakeAlwaysOnSampler());
    {
        const auto scoped = t.StartAsCurrentSpan("op");
        const mt::SpanId::Bytes id = scoped->GetContext().span_id.AsBytes();
        scoped->End();
        ASSERT_EQ(f.proc.received_spans.size(), 1U);
        EXPECT_EQ(mt::CurrentContext().active_span_context.span_id.AsBytes(), id);

        // A span started after the early End() still parents to it.
        auto late = t.StartSpan("late");
        EXPECT_EQ(late->GetContext().trace_id.AsBytes(), scoped->GetContext().trace_id.AsBytes());
    }
    EXPECT_FALSE(mt::CurrentContext().active_span_context.IsValid());
}

// Out-of-order *end* across nested scopes: the outer span is ended while the
// inner scope is still live. The inner scope still restores the outer span's
// context on destruction — an ended-but-current span, which is legal: OTel
// permits starting a child of an ended span.
TEST(SdkTracerTest, StartAsCurrentSpan_OuterEndedEarly_InnerStillRestoresIt)
{
    TracerFixture f;
    auto t = f.MakeTracer(mt::MakeAlwaysOnSampler());
    const auto outer = t.StartAsCurrentSpan("outer");
    const mt::SpanId::Bytes outer_id = outer->GetContext().span_id.AsBytes();
    {
        const auto inner = t.StartAsCurrentSpan("inner");
        outer->End();
    }
    EXPECT_EQ(mt::CurrentContext().active_span_context.span_id.AsBytes(), outer_id);
}

TEST(SdkTracerTest, StartAsCurrentSpan_MoveTransfersTheScope)
{
    TracerFixture f;
    auto t = f.MakeTracer(mt::MakeAlwaysOnSampler());
    {
        auto original = t.StartAsCurrentSpan("op");
        const mt::SpanId::Bytes id = original->GetContext().span_id.AsBytes();
        {
            const mt::ScopedSpan moved{std::move(original)};
            EXPECT_EQ(mt::CurrentContext().active_span_context.span_id.AsBytes(), id);
        }
        // `moved` restored; the moved-from husk must not restore a second time.
        EXPECT_FALSE(mt::CurrentContext().active_span_context.IsValid());
    }
    EXPECT_FALSE(mt::CurrentContext().active_span_context.IsValid());
}

// ICP 0025 §3 contract 3: the sampler dropped the span, so the handle is the
// no-op singleton — but the computed context is installed anyway, so children
// of an unsampled span stay in the same trace.
TEST(SdkTracerTest, StartAsCurrentSpan_Dropped_StillInstallsTheComputedContext)
{
    TracerFixture f;
    auto t = f.MakeTracer(mt::MakeAlwaysOffSampler());
    const auto scoped = t.StartAsCurrentSpan("op");
    ASSERT_NE(scoped.Get(), nullptr);
    EXPECT_FALSE(scoped->IsSampled());
    EXPECT_TRUE(mt::CurrentContext().active_span_context.IsValid());
    EXPECT_FALSE(mt::CurrentContext().active_span_context.trace_flags.IsSampled());
}

TEST(SdkTracerTest, StartAsCurrentSpan_Dropped_ChildContinuesTheSameTrace)
{
    TracerFixture f;
    auto t = f.MakeTracer(mt::MakeAlwaysOffSampler());
    const auto scoped = t.StartAsCurrentSpan("parent");
    const mt::TraceId::Bytes trace = mt::CurrentContext().active_span_context.trace_id.AsBytes();
    ASSERT_TRUE(mt::CurrentContext().active_span_context.trace_id.IsValid());

    const auto child = t.StartAsCurrentSpan("child");
    EXPECT_EQ(mt::CurrentContext().active_span_context.trace_id.AsBytes(), trace);
}

TEST(SdkTracerTest, StartAsCurrentSpan_Dropped_RestoresOnScopeExit)
{
    TracerFixture f;
    auto t = f.MakeTracer(mt::MakeAlwaysOffSampler());
    {
        const auto scoped = t.StartAsCurrentSpan("op");
        ASSERT_TRUE(mt::CurrentContext().active_span_context.IsValid());
    }
    EXPECT_FALSE(mt::CurrentContext().active_span_context.IsValid());
}

TEST(SdkTracerTest, StartAsCurrentSpan_DoesNotLeakIntoANewThread)
{
    TracerFixture f;
    auto t = f.MakeTracer(mt::MakeAlwaysOnSampler());
    const auto scoped = t.StartAsCurrentSpan("op");

    bool worker_saw_a_span = true;
    std::thread worker([&worker_saw_a_span]
                       { worker_saw_a_span = mt::CurrentContext().active_span_context.IsValid(); });
    worker.join();

    EXPECT_FALSE(worker_saw_a_span);
}

// ---------------------------------------------------------------------------
// StartSpanOptions::attributes — initial attributes (#265)
//
// The field is documented as "initial attributes; copied if span is sampled",
// and the sampled path used to read it only to build the SamplingContext, so
// every caller of the documented field lost them. They now reach the span
// through `SetAttribute`, which is what makes the count limit, the value
// clipping and the drop accounting apply to an initial attribute exactly once
// and exactly as they apply to one set later.
// ---------------------------------------------------------------------------

TEST(SdkTracerTest, StartSpan_InitialAttributes_ReachTheRecord)
{
    TracerFixture f;
    auto t = f.MakeTracer(mt::MakeAlwaysOnSampler());

    const std::array<mt::KeyValue, 2> attrs{
        mt::KeyValue{.key = "http.method", .value = std::string{"POST"}},
        mt::KeyValue{.key = "http.route", .value = std::string{"/checkout"}}};
    {
        auto h = t.StartSpan("http.request",
                             {.kind = mt::SpanKind::Server,
                              .parent = {},
                              .start_time = {},
                              .attributes = mt::AttributeSpan{attrs}});
        h->End();
    }

    ASSERT_EQ(f.proc.received_spans.size(), 1U);
    const auto& recorded = f.proc.received_spans[0].attributes;
    ASSERT_EQ(recorded.size(), 2U);
    EXPECT_EQ(recorded[0].key, "http.method");
    EXPECT_EQ(std::get<std::string>(recorded[0].value), "POST");
    EXPECT_EQ(recorded[1].key, "http.route");
    EXPECT_EQ(std::get<std::string>(recorded[1].value), "/checkout");
}

TEST(SdkTracerTest, StartAsCurrentSpan_InitialAttributes_ReachTheRecord)
{
    TracerFixture f;
    auto t = f.MakeTracer(mt::MakeAlwaysOnSampler());

    const std::array<mt::KeyValue, 1> attrs{
        mt::KeyValue{.key = "rpc.system", .value = std::string{"grpc"}}};
    {
        const auto scoped = t.StartAsCurrentSpan("rpc",
                                                 {.kind = mt::SpanKind::Client,
                                                  .parent = {},
                                                  .start_time = {},
                                                  .attributes = mt::AttributeSpan{attrs}});
        scoped->End();
    }

    ASSERT_EQ(f.proc.received_spans.size(), 1U);
    const auto& recorded = f.proc.received_spans[0].attributes;
    ASSERT_EQ(recorded.size(), 1U);
    EXPECT_EQ(recorded[0].key, "rpc.system");
    EXPECT_EQ(std::get<std::string>(recorded[0].value), "grpc");
}

TEST(SdkTracerTest, StartSpan_InitialAttributes_OverCountLimit_AreDroppedAndCounted)
{
    TracerFixture f;
    f.limits.attribute_count_limit = 2;
    auto t = f.MakeTracer(mt::MakeAlwaysOnSampler());

    const std::array<mt::KeyValue, 3> attrs{mt::KeyValue{.key = "a", .value = std::int64_t{1}},
                                            mt::KeyValue{.key = "b", .value = std::int64_t{2}},
                                            mt::KeyValue{.key = "c", .value = std::int64_t{3}}};
    {
        auto h = t.StartSpan("op",
                             {.kind = mt::SpanKind::Internal,
                              .parent = {},
                              .start_time = {},
                              .attributes = mt::AttributeSpan{attrs}});
        h->End();
    }

    ASSERT_EQ(f.proc.received_spans.size(), 1U);
    const auto& recorded = f.proc.received_spans[0].attributes;
    ASSERT_EQ(recorded.size(), 2U);
    EXPECT_EQ(recorded[0].key, "a");
    EXPECT_EQ(recorded[1].key, "b");
    EXPECT_EQ(DropCount(f.diag, mt::DropReason::SpanAttributeLimit), 1U);
}

// One budget, not two: an initial attribute occupies a slot a later
// `SetAttribute` then cannot have.
TEST(SdkTracerTest, StartSpan_InitialAttributes_ShareTheCountBudgetWithSetAttribute)
{
    TracerFixture f;
    f.limits.attribute_count_limit = 2;
    auto t = f.MakeTracer(mt::MakeAlwaysOnSampler());

    const std::array<mt::KeyValue, 1> attrs{mt::KeyValue{.key = "a", .value = std::int64_t{1}}};
    {
        auto h = t.StartSpan("op",
                             {.kind = mt::SpanKind::Internal,
                              .parent = {},
                              .start_time = {},
                              .attributes = mt::AttributeSpan{attrs}});
        h->SetAttribute("b", std::int64_t{2});
        h->SetAttribute("c", std::int64_t{3});
        h->End();
    }

    ASSERT_EQ(f.proc.received_spans.size(), 1U);
    const auto& recorded = f.proc.received_spans[0].attributes;
    ASSERT_EQ(recorded.size(), 2U);
    EXPECT_EQ(recorded[0].key, "a");
    EXPECT_EQ(recorded[1].key, "b");
    EXPECT_EQ(DropCount(f.diag, mt::DropReason::SpanAttributeLimit), 1U);
}

TEST(SdkTracerTest, StartSpan_InitialAttributes_OverValueLengthLimit_AreTruncatedAndCounted)
{
    TracerFixture f;
    f.limits.attribute_value_length_limit = 4;
    auto t = f.MakeTracer(mt::MakeAlwaysOnSampler());

    const std::array<mt::KeyValue, 1> attrs{
        mt::KeyValue{.key = "k", .value = std::string{"abcdefgh"}}};
    {
        auto h = t.StartSpan("op",
                             {.kind = mt::SpanKind::Internal,
                              .parent = {},
                              .start_time = {},
                              .attributes = mt::AttributeSpan{attrs}});
        h->End();
    }

    ASSERT_EQ(f.proc.received_spans.size(), 1U);
    const auto& recorded = f.proc.received_spans[0].attributes;
    ASSERT_EQ(recorded.size(), 1U);
    EXPECT_EQ(std::get<std::string>(recorded[0].value), "abcd");
    EXPECT_EQ(DropCount(f.diag, mt::DropReason::AttributeValueTruncated), 1U);
}

// The unsampled path returns the no-op handle and must do no work at all —
// including on the initial attributes. The truncation counter is the probe:
// an over-length value that was never clipped is a value that was never
// copied (`docs/memory-model.md` §8.1).
TEST(SdkTracerTest, AlwaysOff_InitialAttributes_AreNeitherRecordedNorProcessed)
{
    TracerFixture f;
    f.limits.attribute_value_length_limit = 4;
    auto t = f.MakeTracer(mt::MakeAlwaysOffSampler());

    const std::array<mt::KeyValue, 1> attrs{
        mt::KeyValue{.key = "k", .value = std::string{"abcdefgh"}}};
    {
        auto h = t.StartSpan("op",
                             {.kind = mt::SpanKind::Internal,
                              .parent = {},
                              .start_time = {},
                              .attributes = mt::AttributeSpan{attrs}});
        h->End();
    }

    EXPECT_TRUE(f.proc.received_spans.empty());
    EXPECT_EQ(DropCount(f.diag, mt::DropReason::AttributeValueTruncated), 0U);
    EXPECT_EQ(DropCount(f.diag, mt::DropReason::SpanAttributeLimit), 0U);
}
