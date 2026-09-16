// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// v1.1 sugar layer — `microtel::sugar::RecordException` (ICP 0028 §1).
//
// Two calls on the public `Span`, in a fixed order: `SetStatus(Error, what)`
// first — it cannot fail — then the OTel `exception` event carrying
// `exception.type` and `exception.message`.
//
// The type name is deliberately the **mangled** `typeid(e).name()`: ICP 0028
// rejects `abi::__cxa_demangle` (it allocates on an error path and is
// ABI-specific) and offers the (type, message) overload to anyone who wants a
// prettier name. `exception.stacktrace` and `exception.escaped` are omitted —
// microtel captures no stack traces, and escape is not knowable from inside
// the helper.
//
// Setting `Error` status is a deliberate divergence from
// opentelemetry-cpp's `Span::RecordException`, which does not. Spec §18.1
// excludes sugar from conformance testing, so nothing measures this helper
// against the OTel API surface (ICP 0028, "Restated exclusions").

#include "microtel/sugar/exception.hpp"

#include "microtel/trace.hpp"

#include "fakes/fake_span.hpp"

#include <gtest/gtest.h>

#include <stdexcept>
#include <string>
#include <typeinfo>
#include <variant>

namespace mt = microtel::sugar;

namespace
{

TEST(SugarRecordException, StdExceptionSetsErrorStatusWithTheMessage)
{
    microtel::testing::FakeSpan span;
    const std::runtime_error err{"disk on fire"};

    mt::RecordException(span, err);

    ASSERT_EQ(span.statuses.size(), 1U);
    EXPECT_EQ(span.statuses[0].code, microtel::StatusCode::Error);
    EXPECT_EQ(span.statuses[0].description, "disk on fire");
}

TEST(SugarRecordException, StdExceptionAddsTheOtelExceptionEvent)
{
    microtel::testing::FakeSpan span;
    const std::runtime_error err{"disk on fire"};

    mt::RecordException(span, err);

    ASSERT_EQ(span.events.size(), 1U);
    EXPECT_EQ(span.events[0].name, "exception");
    ASSERT_EQ(span.events[0].attributes.size(), 2U);
    EXPECT_EQ(span.events[0].attributes[0].key, "exception.type");
    EXPECT_EQ(span.events[0].attributes[1].key, "exception.message");
    EXPECT_EQ(std::get<std::string>(span.events[0].attributes[1].value), "disk on fire");
}

TEST(SugarRecordException, TypeIsTheMangledDynamicTypeName)
{
    microtel::testing::FakeSpan span;
    const std::runtime_error err{"disk on fire"};

    // The helper takes `const std::exception&`; `typeid` on a polymorphic
    // lvalue resolves to the dynamic type, so the recorded name is
    // runtime_error's, not std::exception's — and it is the mangled form.
    const std::exception& as_base = err;
    mt::RecordException(span, as_base);

    ASSERT_EQ(span.events.size(), 1U);
    ASSERT_EQ(span.events[0].attributes.size(), 2U);
    EXPECT_EQ(std::get<std::string>(span.events[0].attributes[0].value),
              typeid(std::runtime_error).name());
    EXPECT_NE(std::get<std::string>(span.events[0].attributes[0].value),
              typeid(std::exception).name());
}

TEST(SugarRecordException, TypeAndMessageOverloadRecordsExactlyWhatItIsGiven)
{
    microtel::testing::FakeSpan span;

    mt::RecordException(span, "grpc.Status", "UNAVAILABLE: no healthy upstream");

    ASSERT_EQ(span.statuses.size(), 1U);
    EXPECT_EQ(span.statuses[0].code, microtel::StatusCode::Error);
    EXPECT_EQ(span.statuses[0].description, "UNAVAILABLE: no healthy upstream");

    ASSERT_EQ(span.events.size(), 1U);
    EXPECT_EQ(span.events[0].name, "exception");
    ASSERT_EQ(span.events[0].attributes.size(), 2U);
    EXPECT_EQ(span.events[0].attributes[0].key, "exception.type");
    EXPECT_EQ(std::get<std::string>(span.events[0].attributes[0].value), "grpc.Status");
    EXPECT_EQ(span.events[0].attributes[1].key, "exception.message");
    EXPECT_EQ(std::get<std::string>(span.events[0].attributes[1].value),
              "UNAVAILABLE: no healthy upstream");
}

TEST(SugarRecordException, AnEmptyMessageStillProducesBothAttributes)
{
    microtel::testing::FakeSpan span;

    mt::RecordException(span, "io.Timeout", "");

    ASSERT_EQ(span.events.size(), 1U);
    ASSERT_EQ(span.events[0].attributes.size(), 2U);
    EXPECT_EQ(std::get<std::string>(span.events[0].attributes[1].value), "");
    ASSERT_EQ(span.statuses.size(), 1U);
    EXPECT_TRUE(span.statuses[0].description.empty());
}

TEST(SugarRecordException, RecordingTwiceLeavesTwoEvents)
{
    microtel::testing::FakeSpan span;

    mt::RecordException(span, "a.First", "one");
    mt::RecordException(span, "b.Second", "two");

    ASSERT_EQ(span.events.size(), 2U);
    EXPECT_EQ(std::get<std::string>(span.events[0].attributes[0].value), "a.First");
    EXPECT_EQ(std::get<std::string>(span.events[1].attributes[0].value), "b.Second");
    EXPECT_EQ(span.statuses.size(), 2U);
}

}  // namespace
