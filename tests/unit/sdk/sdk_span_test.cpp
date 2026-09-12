// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Unit tests for SdkSpan: attribute / event / link limits, status
// transitions, name update, End() calling OnEnd on the processor.

#include "sdk/sdk_span.hpp"

#include "microtel/attribute.hpp"
#include "microtel/context.hpp"
#include "microtel/internal/batch.hpp"
#include "microtel/provider.hpp"
#include "microtel/sdk_builder.hpp"
#include "microtel/trace.hpp"

#include "fakes/fake_diagnostics_sink.hpp"
#include "fakes/fake_span_processor.hpp"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstddef>
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
    mtfk::FakeSpanProcessor& proc,
    const mt::SpanLimitOptions& limits = mt::SpanLimitOptions{},
    mtfk::FakeDiagnosticsSink* sink = nullptr)
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
        mti::InstrumentationScope{.name = "span.scope", .version = "4.2"},
        limits,
        sink);
}

static std::uint64_t DropCount(const mtfk::FakeDiagnosticsSink& sink, mt::DropReason reason)
{
    return sink.drop_counters.at(static_cast<std::size_t>(reason));
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

// The span's scope is the tracer's, and it must reach the processor — before
// ICP 0023 SdkSpan stored it and never read it (issue #167).
TEST(SdkSpanTest, End_PassesTracerScopeToProcessor)
{
    mtfk::FakeSpanProcessor proc;
    auto span = MakeSpan(proc);
    span->End();
    ASSERT_EQ(proc.received_scopes.size(), 1U);
    EXPECT_EQ(proc.received_scopes[0].name, "span.scope");
    EXPECT_EQ(proc.received_scopes[0].version, "4.2");
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
// Drop accounting — issue #169. The limits above were enforced but invisible:
// a span silently shed attributes, events and links while every one of the
// five record-shaping counters read zero. Spans are single-threaded by
// contract, so the non-atomic FakeDiagnosticsSink needs no synchronisation.
// ---------------------------------------------------------------------------

TEST(SdkSpanTest, Diagnostics_SetAttributeOverLimit_CountsEachDroppedAttribute)
{
    mt::SpanLimitOptions lim;
    lim.attribute_count_limit = 2;
    mtfk::FakeSpanProcessor proc;
    mtfk::FakeDiagnosticsSink sink;
    auto span = MakeSpan(proc, lim, &sink);

    span->SetAttribute("a", std::int64_t{1});
    span->SetAttribute("b", std::int64_t{2});
    span->SetAttribute("c", std::int64_t{3});
    span->SetAttribute("d", std::int64_t{4});
    span->End();

    EXPECT_EQ(DropCount(sink, mt::DropReason::SpanAttributeLimit), 2U);
    EXPECT_EQ(DropCount(sink, mt::DropReason::SpanEventLimit), 0U);
}

TEST(SdkSpanTest, Diagnostics_UnderLimit_CountsNothing)
{
    mtfk::FakeSpanProcessor proc;
    mtfk::FakeDiagnosticsSink sink;
    auto span = MakeSpan(proc, mt::SpanLimitOptions{}, &sink);

    span->SetAttribute("a", std::int64_t{1});
    span->AddEvent("e1");
    span->AddLink(MakeValidContext());
    span->End();

    for (const auto count : sink.drop_counters)
    {
        EXPECT_EQ(count, 0U);
    }
}

TEST(SdkSpanTest, Diagnostics_AddEventOverLimit_CountsEachDroppedEvent)
{
    mt::SpanLimitOptions lim;
    lim.event_count_limit = 1;
    mtfk::FakeSpanProcessor proc;
    mtfk::FakeDiagnosticsSink sink;
    auto span = MakeSpan(proc, lim, &sink);

    span->AddEvent("e1");
    span->AddEvent("e2");
    span->AddEvent("e3");
    span->End();

    EXPECT_EQ(DropCount(sink, mt::DropReason::SpanEventLimit), 2U);
}

TEST(SdkSpanTest, Diagnostics_AddLinkOverLimit_CountsEachDroppedLink)
{
    mt::SpanLimitOptions lim;
    lim.link_count_limit = 1;
    mtfk::FakeSpanProcessor proc;
    mtfk::FakeDiagnosticsSink sink;
    auto span = MakeSpan(proc, lim, &sink);

    span->AddLink(MakeValidContext());
    span->AddLink(MakeValidContext());
    span->End();

    EXPECT_EQ(DropCount(sink, mt::DropReason::SpanLinkLimit), 1U);
}

TEST(SdkSpanTest, Diagnostics_EventAttributesOverLimit_CountsSurplus)
{
    mt::SpanLimitOptions lim;
    lim.event_attribute_count_limit = 1;
    mtfk::FakeSpanProcessor proc;
    mtfk::FakeDiagnosticsSink sink;
    auto span = MakeSpan(proc, lim, &sink);

    const std::array<mt::KeyValue, 3> attrs{
        mt::KeyValue{.key = "a", .value = std::int64_t{1}},
        mt::KeyValue{.key = "b", .value = std::int64_t{2}},
        mt::KeyValue{.key = "c", .value = std::int64_t{3}},
    };
    span->AddEvent("e1", mt::AttributeSpan{attrs.data(), attrs.size()});
    span->End();

    // The event is kept; two of its three attributes are not. A per-event
    // shed is a different loss from a dropped event and has its own counter.
    ASSERT_EQ(proc.received_spans.size(), 1U);
    ASSERT_EQ(proc.received_spans[0].events.size(), 1U);
    EXPECT_EQ(proc.received_spans[0].events[0].attributes.size(), 1U);
    EXPECT_EQ(DropCount(sink, mt::DropReason::EventAttributeLimit), 2U);
    EXPECT_EQ(DropCount(sink, mt::DropReason::SpanEventLimit), 0U);
}

TEST(SdkSpanTest, Diagnostics_LinkAttributesOverLimit_CountsSurplus)
{
    mt::SpanLimitOptions lim;
    lim.link_attribute_count_limit = 1;
    mtfk::FakeSpanProcessor proc;
    mtfk::FakeDiagnosticsSink sink;
    auto span = MakeSpan(proc, lim, &sink);

    const std::array<mt::KeyValue, 3> attrs{
        mt::KeyValue{.key = "a", .value = std::int64_t{1}},
        mt::KeyValue{.key = "b", .value = std::int64_t{2}},
        mt::KeyValue{.key = "c", .value = std::int64_t{3}},
    };
    span->AddLink(MakeValidContext(), mt::AttributeSpan{attrs.data(), attrs.size()});
    span->End();

    ASSERT_EQ(proc.received_spans.size(), 1U);
    ASSERT_EQ(proc.received_spans[0].links.size(), 1U);
    EXPECT_EQ(proc.received_spans[0].links[0].attributes.size(), 1U);
    EXPECT_EQ(DropCount(sink, mt::DropReason::LinkAttributeLimit), 2U);
}

TEST(SdkSpanTest, Diagnostics_NullSink_IsNotDereferenced)
{
    mt::SpanLimitOptions lim;
    lim.attribute_count_limit = 1;
    lim.event_count_limit = 1;
    lim.link_count_limit = 1;
    lim.event_attribute_count_limit = 0;
    lim.link_attribute_count_limit = 0;
    mtfk::FakeSpanProcessor proc;
    auto span = MakeSpan(proc, lim);  // no sink

    const mt::KeyValue kv{.key = "a", .value = std::int64_t{1}};
    span->SetAttribute("a", std::int64_t{1});
    span->SetAttribute("b", std::int64_t{2});
    span->AddEvent("e1", mt::AttributeSpan{&kv, 1});
    span->AddEvent("e2");
    span->AddLink(MakeValidContext(), mt::AttributeSpan{&kv, 1});
    span->AddLink(MakeValidContext());
    span->End();
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
