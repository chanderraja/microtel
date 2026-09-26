// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// LeafTable: the receiver's bounded cache of resolved leaf Resources
// (docs/leaf-concentrator-design.md §4.5). LRU eviction, re-resolution when a
// leaf's declared Resource changes, and the "second insert is discarded" race
// rule of §3.5.

#include "sdk/leaf_table.hpp"

#include "microtel/resource.hpp"

#include <gtest/gtest.h>

#include <memory>

namespace mt = microtel;
namespace mts = microtel::sdk;

namespace
{

std::shared_ptr<const mt::Resource> NewResource()
{
    return std::make_shared<const mt::Resource>();
}

}  // namespace

TEST(LeafTableTest, FindMissesAnUnknownLeaf)
{
    mts::LeafTable table{4};
    EXPECT_EQ(table.Find("a", 1), nullptr);
    EXPECT_EQ(table.Size(), 0U);
}

TEST(LeafTableTest, FindHitsWithTheSameDeclaredHashOnly)
{
    mts::LeafTable table{4};
    const auto r = NewResource();
    EXPECT_EQ(table.Insert("a", 1, r), r);

    EXPECT_EQ(table.Find("a", 1), r);
    EXPECT_EQ(table.Find("a", 2), nullptr) << "a changed declared Resource is resolved again";
}

TEST(LeafTableTest, InsertWithANewHashReplacesTheEntry)
{
    mts::LeafTable table{4};
    const auto old_r = NewResource();
    const auto new_r = NewResource();
    (void)table.Insert("a", 1, old_r);

    EXPECT_EQ(table.Insert("a", 2, new_r), new_r);
    EXPECT_EQ(table.Find("a", 2), new_r);
    EXPECT_EQ(table.Size(), 1U);
    EXPECT_EQ(table.Evicted(), 0U) << "a replacement is not an eviction";
}

TEST(LeafTableTest, ASecondInsertOfTheSameLeafAndHashIsDiscarded)
{
    mts::LeafTable table{4};
    const auto first = NewResource();
    const auto second = NewResource();
    (void)table.Insert("a", 1, first);

    EXPECT_EQ(table.Insert("a", 1, second), first)
        << "two threads that raced on a new leaf end up sharing the first Resource";
}

TEST(LeafTableTest, EvictsTheLeastRecentlySeenWhenFull)
{
    mts::LeafTable table{2};
    const auto a = NewResource();
    const auto b = NewResource();
    const auto c = NewResource();
    (void)table.Insert("a", 1, a);
    (void)table.Insert("b", 1, b);
    ASSERT_EQ(table.Find("a", 1), a);  // a is now the most recent

    (void)table.Insert("c", 1, c);

    EXPECT_EQ(table.Size(), 2U);
    EXPECT_EQ(table.Evicted(), 1U);
    EXPECT_EQ(table.Find("b", 1), nullptr);
    EXPECT_EQ(table.Find("a", 1), a);
    EXPECT_EQ(table.Find("c", 1), c);
}

TEST(LeafTableTest, AnEvictedResourceStaysAliveForWhoeverHoldsIt)
{
    mts::LeafTable table{1};
    auto held = table.Insert("a", 1, NewResource());
    (void)table.Insert("b", 1, NewResource());

    EXPECT_EQ(table.Find("a", 1), nullptr);
    ASSERT_NE(held, nullptr);
    EXPECT_TRUE(held->Attributes().empty()) << "a queued record keeps its Resource after eviction";
}
