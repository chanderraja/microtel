// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// v1.1 — Resource::Merge, the key-level precedence primitive behind the
// resource composition order in microtel-spec.md §12.7 (detectors first, then
// environment, then user-supplied; later wins).

#include "microtel/resource.hpp"

#include "microtel/attribute.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace
{

/// @brief Look a key up in a resource, or `nullopt` when it is absent.
[[nodiscard]] std::optional<microtel::AttributeValue> Lookup(const microtel::Resource& res,
                                                             const std::string& key)
{
    for (const auto& kv : res.Attributes())
    {
        if (kv.key == key)
        {
            return kv.value;
        }
    }
    return std::nullopt;
}

/// @brief Count the occurrences of `key` — a merge must never duplicate one.
[[nodiscard]] std::size_t CountKey(const microtel::Resource& res, const std::string& key)
{
    std::size_t n = 0;
    for (const auto& kv : res.Attributes())
    {
        if (kv.key == key)
        {
            ++n;
        }
    }
    return n;
}

[[nodiscard]] std::string StringOf(const microtel::AttributeValue& v)
{
    return std::get<std::string>(v);
}

}  // namespace

// ---------------------------------------------------------------------------
// Merge — the precedence table
// ---------------------------------------------------------------------------

TEST(ResourceMergeTest, Merge_DisjointKeys_KeepsBoth)
{
    const microtel::Resource base{{{.key = "a", .value = std::string{"1"}}}};
    const microtel::Resource overriding{{{.key = "b", .value = std::string{"2"}}}};

    const microtel::Resource merged = microtel::Resource::Merge(base, overriding);

    ASSERT_EQ(merged.Attributes().size(), 2U);
    EXPECT_EQ(StringOf(*Lookup(merged, "a")), "1");
    EXPECT_EQ(StringOf(*Lookup(merged, "b")), "2");
}

TEST(ResourceMergeTest, Merge_KeyCollision_LaterWins)
{
    const microtel::Resource base{{{.key = "k", .value = std::string{"from-base"}}}};
    const microtel::Resource overriding{{{.key = "k", .value = std::string{"from-overriding"}}}};

    const microtel::Resource merged = microtel::Resource::Merge(base, overriding);

    EXPECT_EQ(CountKey(merged, "k"), 1U);
    EXPECT_EQ(StringOf(*Lookup(merged, "k")), "from-overriding");
}

TEST(ResourceMergeTest, Merge_KeyCollision_ReplacesValueOfDifferentType)
{
    const microtel::Resource base{{{.key = "k", .value = std::int64_t{7}}}};
    const microtel::Resource overriding{{{.key = "k", .value = std::string{"seven"}}}};

    const microtel::Resource merged = microtel::Resource::Merge(base, overriding);

    ASSERT_EQ(merged.Attributes().size(), 1U);
    EXPECT_EQ(StringOf(*Lookup(merged, "k")), "seven");
}

TEST(ResourceMergeTest, Merge_EmptyBase_YieldsOverriding)
{
    const microtel::Resource base{};
    const microtel::Resource overriding{{{.key = "b", .value = std::string{"2"}}}};

    const microtel::Resource merged = microtel::Resource::Merge(base, overriding);

    ASSERT_EQ(merged.Attributes().size(), 1U);
    EXPECT_EQ(StringOf(*Lookup(merged, "b")), "2");
}

TEST(ResourceMergeTest, Merge_EmptyOverriding_YieldsBase)
{
    const microtel::Resource base{{{.key = "a", .value = std::string{"1"}}}};
    const microtel::Resource overriding{};

    const microtel::Resource merged = microtel::Resource::Merge(base, overriding);

    ASSERT_EQ(merged.Attributes().size(), 1U);
    EXPECT_EQ(StringOf(*Lookup(merged, "a")), "1");
}

TEST(ResourceMergeTest, Merge_BothEmpty_YieldsEmpty)
{
    const microtel::Resource merged = microtel::Resource::Merge({}, {});
    EXPECT_TRUE(merged.Attributes().empty());
}

TEST(ResourceMergeTest, Merge_CollisionKeepsBasePosition)
{
    // Stable ordering: an overridden key keeps the slot it had in `base` rather
    // than moving to the end. Nothing on the wire depends on it, but a stable
    // order makes the resolved Resource diffable between runs.
    const microtel::Resource base{{{.key = "a", .value = std::string{"1"}},
                                   {.key = "k", .value = std::string{"old"}},
                                   {.key = "z", .value = std::string{"26"}}}};
    const microtel::Resource overriding{{{.key = "k", .value = std::string{"new"}}}};

    const microtel::Resource merged = microtel::Resource::Merge(base, overriding);

    ASSERT_EQ(merged.Attributes().size(), 3U);
    EXPECT_EQ(merged.Attributes()[0].key, "a");
    EXPECT_EQ(merged.Attributes()[1].key, "k");
    EXPECT_EQ(StringOf(merged.Attributes()[1].value), "new");
    EXPECT_EQ(merged.Attributes()[2].key, "z");
}

TEST(ResourceMergeTest, Merge_DuplicateKeyWithinOverriding_LastOccurrenceWins)
{
    // `Resource`'s constructor is documented as shallow; the merge is where
    // "last occurrence wins" is actually enforced.
    const microtel::Resource base{};
    const microtel::Resource overriding{{{.key = "k", .value = std::string{"first"}},
                                         {.key = "k", .value = std::string{"second"}}}};

    const microtel::Resource merged = microtel::Resource::Merge(base, overriding);

    EXPECT_EQ(CountKey(merged, "k"), 1U);
    EXPECT_EQ(StringOf(*Lookup(merged, "k")), "second");
}

TEST(ResourceMergeTest, Merge_IsAssociativeLeftToRight_SpecPrecedenceChain)
{
    // The §12.7 chain as the SDK applies it: detectors, then env, then user.
    // Every later layer overrides the ones before it, and each layer's own
    // keys survive where nothing later names them.
    const microtel::Resource detectors{{{.key = "host.name", .value = std::string{"detected"}},
                                        {.key = "service.name", .value = std::string{"detected"}},
                                        {.key = "process.pid", .value = std::int64_t{42}}}};
    const microtel::Resource env{{{.key = "service.name", .value = std::string{"from-env"}},
                                  {.key = "deployment.environment", .value = std::string{"stage"}}}};
    const microtel::Resource user{{{.key = "service.name", .value = std::string{"from-user"}}}};

    const microtel::Resource merged =
        microtel::Resource::Merge(microtel::Resource::Merge(detectors, env), user);

    EXPECT_EQ(StringOf(*Lookup(merged, "service.name")), "from-user");
    EXPECT_EQ(StringOf(*Lookup(merged, "deployment.environment")), "stage");
    EXPECT_EQ(StringOf(*Lookup(merged, "host.name")), "detected");
    EXPECT_EQ(std::get<std::int64_t>(*Lookup(merged, "process.pid")), 42);
    EXPECT_EQ(CountKey(merged, "service.name"), 1U);
}
