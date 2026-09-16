// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// v1.1 sugar layer — `microtel::sugar::AttrKey` (ICP 0028 §2).
//
// AttrKey is a call-site binder and nothing else: one borrowed `string_view`,
// rule of zero, `constexpr` construction, and two ways to spend the key —
// `Set()` on a span and `operator()` for the `AttributeSpan` paths. The
// static_asserts below are half the test: a type that allocated, or that
// grew a user-declared special member, would stop being the thing ICP 0028
// §2 locked.
//
// The file spells the consumer-side alias `namespace mt = microtel::sugar;`
// that ICP 0028 §0 documents and deliberately does not declare in any
// microtel header, so core types are written out in full.

#include "microtel/sugar/attr_key.hpp"

#include "microtel/attribute.hpp"

#include "fakes/fake_span.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>
#include <variant>

namespace mt = microtel::sugar;

namespace
{

// Constant-initialised at namespace scope: the intended spelling from the
// ICP's own example. Nothing here runs before main.
constexpr mt::AttrKey kHttpMethod{"http.method"};
constexpr mt::AttrKey kHttpStatus{"http.status_code"};

static_assert(kHttpMethod.Key() == std::string_view{"http.method"},
              "AttrKey::Key must be usable in a constant expression");

// Rule of zero, mechanically. All five special members implicit, no
// allocation, no destructor to run.
static_assert(std::is_trivially_copyable_v<mt::AttrKey>);
static_assert(std::is_trivially_destructible_v<mt::AttrKey>);
static_assert(std::is_nothrow_copy_constructible_v<mt::AttrKey>);
static_assert(!std::is_default_constructible_v<mt::AttrKey>,
              "AttrKey binds a key; a keyless one has no meaning");
static_assert(!std::is_convertible_v<std::string_view, mt::AttrKey>,
              "the constructor is explicit, so a key cannot appear where a value is expected");

TEST(SugarAttrKey, KeyIsTheBorrowedViewVerbatim)
{
    EXPECT_EQ(kHttpMethod.Key(), std::string_view{"http.method"});
    EXPECT_EQ(kHttpMethod.Key().size(), std::string_view{"http.method"}.size());
}

TEST(SugarAttrKey, SetPutsTheBoundKeyOnTheSpan)
{
    microtel::testing::FakeSpan span;

    kHttpMethod.Set(span, std::string{"GET"});

    ASSERT_EQ(span.attributes.size(), 1U);
    EXPECT_EQ(span.attributes[0].key, "http.method");
    EXPECT_EQ(std::get<std::string>(span.attributes[0].value), "GET");
}

TEST(SugarAttrKey, SetCarriesANonStringValueUnchanged)
{
    microtel::testing::FakeSpan span;

    kHttpStatus.Set(span, std::int64_t{503});

    ASSERT_EQ(span.attributes.size(), 1U);
    EXPECT_EQ(span.attributes[0].key, "http.status_code");
    EXPECT_EQ(std::get<std::int64_t>(span.attributes[0].value), 503);
}

TEST(SugarAttrKey, CallOperatorBuildsAKeyValue)
{
    const microtel::KeyValue kv = kHttpStatus(std::int64_t{200});

    EXPECT_EQ(kv.key, "http.status_code");
    EXPECT_EQ(std::get<std::int64_t>(kv.value), 200);
}

TEST(SugarAttrKey, BuiltKeyValuesFlowThroughAnAttributeSpan)
{
    microtel::testing::FakeSpan span;
    const std::array<microtel::KeyValue, 2> attrs{kHttpMethod(std::string{"POST"}),
                                                  kHttpStatus(std::int64_t{201})};

    span.AddEvent("request.done", microtel::AttributeSpan{attrs.data(), attrs.size()}, {});

    ASSERT_EQ(span.events.size(), 1U);
    ASSERT_EQ(span.events[0].attributes.size(), 2U);
    EXPECT_EQ(span.events[0].attributes[0].key, "http.method");
    EXPECT_EQ(std::get<std::string>(span.events[0].attributes[0].value), "POST");
    EXPECT_EQ(span.events[0].attributes[1].key, "http.status_code");
    EXPECT_EQ(std::get<std::int64_t>(span.events[0].attributes[1].value), 201);
}

}  // namespace
