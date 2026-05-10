// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for SdkSpan: attribute / event / link limits, status
// transitions, name update, End() calling OnEnd on the processor.

#include "sdk/sdk_span.hpp"

#include "microtel/attribute.hpp"
#include "microtel/context.hpp"
#include "microtel/internal/batch.hpp"
#include "microtel/sdk_builder.hpp"
#include "microtel/trace.hpp"

#include "fakes/fake_span_processor.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <string>

namespace mt = microtel;
namespace mti = microtel::internal;
namespace mtfk = microtel::testing;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static mt::SpanContext MakeValidContext()
{
    mt::TraceId::Bytes tid{};
    tid[0] = 1;
    mt::SpanId::Bytes sid{};
    sid[0] = 2;
    return mt::SpanContext{
        .trace_id = mt::TraceId{tid},
        .span_id = mt::SpanId{sid},
        .trace_flags = {},
        .trace_state = {},
        .remote = false,
    };
}

static std::unique_ptr<mt::sdk::SdkSpan> MakeSpan(
    mtfk::FakeSpanProcessor& proc, const mt::SpanLimitOptions& limits = mt::SpanLimitOptions{})
{
    auto resource = std::make_shared<const mt::Resource>();
    return std::make_unique<mt::sdk::SdkSpan>(
        MakeValidContext(),
        mt::SpanContext{},
        "test-op",
        mt::SpanKind::Internal,
        std::chrono::system_clock::now(),
        &proc,
        resource,
        mti::InstrumentationScope{.name = "test", .version = ""},
        limits);
}

// ---------------------------------------------------------------------------
// Basic properties
// ---------------------------------------------------------------------------

TEST(SdkSpanTest, IsSampled_ReturnsTrue)
{
    mtfk::FakeSpanProcessor proc;
    const auto span = MakeSpan(proc);
    EXPECT_TRUE(span->IsSampled());
}

TEST(SdkSpanTest, GetContext_ReturnsConstructedContext)
{
    mtfk::FakeSpanProcessor proc;
    const auto span = MakeSpan(proc);
    EXPECT_TRUE(span->GetContext().IsValid());
}

TEST(SdkSpanTest, End_CallsOnEnd)
{
    mtfk::FakeSpanProcessor proc;
    auto span = MakeSpan(proc);
    span->End();
    ASSERT_EQ(proc.received_spans.size(), 1U);
    EXPECT_EQ(proc.received_spans[0].name, "test-op");
}

TEST(SdkSpanTest, End_IsIdempotent)
{
    mtfk::FakeSpanProcessor proc;
    auto span = MakeSpan(proc);
    span->End();
    span->End();
    EXPECT_EQ(proc.received_spans.size(), 1U);
}

TEST(SdkSpanTest, Destructor_CallsEnd)
{
    mtfk::FakeSpanProcessor proc;
    {
        auto span = MakeSpan(proc);
        // End not called explicitly.
    }
    EXPECT_EQ(proc.received_spans.size(), 1U);
}

// ---------------------------------------------------------------------------
// Attributes
// ---------------------------------------------------------------------------

TEST(SdkSpanTest, SetAttribute_RecordedInSpan)
{
    mtfk::FakeSpanProcessor proc;
    auto span = MakeSpan(proc);
    span->SetAttribute("key", std::int64_t{42});
    span->End();

    ASSERT_EQ(proc.received_spans.size(), 1U);
    const auto& attrs = proc.received_spans[0].attributes;
    ASSERT_EQ(attrs.size(), 1U);
    EXPECT_EQ(attrs[0].key, "key");
    EXPECT_EQ(std::get<std::int64_t>(attrs[0].value), 42);
}

TEST(SdkSpanTest, SetAttribute_Limit_DropsNewAttributes)
{
    mt::SpanLimitOptions lim;
    lim.attribute_count_limit = 2;
    mtfk::FakeSpanProcessor proc;
    auto span = MakeSpan(proc, lim);

    span->SetAttribute("a", std::int64_t{1});
    span->SetAttribute("b", std::int64_t{2});
    span->SetAttribute("c", std::int64_t{3});  // dropped
    span->End();

    ASSERT_EQ(proc.received_spans.size(), 1U);
    EXPECT_EQ(proc.received_spans[0].attributes.size(), 2U);
}

// ---------------------------------------------------------------------------
// Events
// ---------------------------------------------------------------------------

TEST(SdkSpanTest, AddEvent_RecordedInSpan)
{
    mtfk::FakeSpanProcessor proc;
    auto span = MakeSpan(proc);
    span->AddEvent("my-event");
    span->End();

    ASSERT_EQ(proc.received_spans.size(), 1U);
    ASSERT_EQ(proc.received_spans[0].events.size(), 1U);
    EXPECT_EQ(proc.received_spans[0].events[0].name, "my-event");
}

TEST(SdkSpanTest, AddEvent_Limit_DropsNewEvents)
{
    mt::SpanLimitOptions lim;
    lim.event_count_limit = 1;
    mtfk::FakeSpanProcessor proc;
    auto span = MakeSpan(proc, lim);

    span->AddEvent("e1");
    span->AddEvent("e2");  // dropped
    span->End();

    ASSERT_EQ(proc.received_spans.size(), 1U);
    EXPECT_EQ(proc.received_spans[0].events.size(), 1U);
}

// ---------------------------------------------------------------------------
// Links
// ---------------------------------------------------------------------------

TEST(SdkSpanTest, AddLink_RecordedInSpan)
{
    mtfk::FakeSpanProcessor proc;
    auto span = MakeSpan(proc);
    span->AddLink(MakeValidContext());
    span->End();

    ASSERT_EQ(proc.received_spans.size(), 1U);
    EXPECT_EQ(proc.received_spans[0].links.size(), 1U);
}

TEST(SdkSpanTest, AddLink_Limit_DropsNewLinks)
{
    mt::SpanLimitOptions lim;
    lim.link_count_limit = 1;
    mtfk::FakeSpanProcessor proc;
    auto span = MakeSpan(proc, lim);

    span->AddLink(MakeValidContext());
    span->AddLink(MakeValidContext());  // dropped
    span->End();

    ASSERT_EQ(proc.received_spans.size(), 1U);
    EXPECT_EQ(proc.received_spans[0].links.size(), 1U);
}

// ---------------------------------------------------------------------------
// Status
// ---------------------------------------------------------------------------

TEST(SdkSpanTest, SetStatus_Ok_RecordedInSpan)
{
    mtfk::FakeSpanProcessor proc;
    auto span = MakeSpan(proc);
    span->SetStatus(mt::StatusCode::Ok);
    span->End();

    ASSERT_EQ(proc.received_spans.size(), 1U);
    EXPECT_EQ(proc.received_spans[0].status_code, mt::StatusCode::Ok);
}

TEST(SdkSpanTest, SetStatus_Error_WithDescription)
{
    mtfk::FakeSpanProcessor proc;
    auto span = MakeSpan(proc);
    span->SetStatus(mt::StatusCode::Error, "boom");
    span->End();

    ASSERT_EQ(proc.received_spans.size(), 1U);
    EXPECT_EQ(proc.received_spans[0].status_code, mt::StatusCode::Error);
    EXPECT_EQ(proc.received_spans[0].status_description, "boom");
}

TEST(SdkSpanTest, SetStatus_Ok_DoesNotOverrideOk)
{
    mtfk::FakeSpanProcessor proc;
    auto span = MakeSpan(proc);
    span->SetStatus(mt::StatusCode::Ok);
    span->SetStatus(mt::StatusCode::Error);  // must not override Ok
    span->End();

    ASSERT_EQ(proc.received_spans.size(), 1U);
    EXPECT_EQ(proc.received_spans[0].status_code, mt::StatusCode::Ok);
}

TEST(SdkSpanTest, SetStatus_Error_OverridesUnset)
{
    mtfk::FakeSpanProcessor proc;
    auto span = MakeSpan(proc);
    span->SetStatus(mt::StatusCode::Error, "fail");
    span->End();

    ASSERT_EQ(proc.received_spans.size(), 1U);
    EXPECT_EQ(proc.received_spans[0].status_code, mt::StatusCode::Error);
}

// ---------------------------------------------------------------------------
// UpdateName
// ---------------------------------------------------------------------------

TEST(SdkSpanTest, UpdateName_ChangesSpanName)
{
    mtfk::FakeSpanProcessor proc;
    auto span = MakeSpan(proc);
    span->UpdateName("new-name");
    span->End();

    ASSERT_EQ(proc.received_spans.size(), 1U);
    EXPECT_EQ(proc.received_spans[0].name, "new-name");
}

// ---------------------------------------------------------------------------
// Timing
// ---------------------------------------------------------------------------

TEST(SdkSpanTest, End_WithExplicitTime_RecordedInSpan)
{
    mtfk::FakeSpanProcessor proc;
    auto span = MakeSpan(proc);
    const auto t = std::chrono::system_clock::time_point{std::chrono::seconds{1000}};
    span->End(t);

    ASSERT_EQ(proc.received_spans.size(), 1U);
    EXPECT_EQ(proc.received_spans[0].end_time, t);
}

TEST(SdkSpanTest, End_WithZeroTime_UsesNow)
{
    mtfk::FakeSpanProcessor proc;
    auto span = MakeSpan(proc);
    const auto before = std::chrono::system_clock::now();
    span->End({});
    const auto after = std::chrono::system_clock::now();

    ASSERT_EQ(proc.received_spans.size(), 1U);
    EXPECT_GE(proc.received_spans[0].end_time, before);
    EXPECT_LE(proc.received_spans[0].end_time, after);
}
