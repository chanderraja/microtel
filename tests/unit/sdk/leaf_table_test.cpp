// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// LeafTable: the receiver's bounded cache of resolved leaf Resources
// (docs/leaf-concentrator-design.md §4.5). LRU eviction, re-resolution when a
// leaf's declared Resource changes, and the "second insert is discarded" race
// rule of §3.5; the per-leaf settings and boot anchor, and idle-timeout
// eviction.

#include "sdk/leaf_table.hpp"

#include "microtel/resource.hpp"

#include "sdk/leaf_time.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <memory>
#include <optional>

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

namespace
{

using Clock = mts::LeafTable::TimePoint;

Clock At(int seconds)
{
    return Clock{std::chrono::seconds{seconds}};
}

std::shared_ptr<const mts::LeafSettings> NewSettings()
{
    return std::make_shared<const mts::LeafSettings>(
        mts::LeafSettings{.configured = true, .time_mode = std::nullopt, .resource = {}});
}

}  // namespace

TEST(LeafTableTest, SettingsMissUntilAdoptedAndTheFirstAdoptionWins)
{
    mts::LeafTable table{4};
    EXPECT_EQ(table.Settings("a", At(0)), nullptr);
    const auto first = NewSettings();
    const auto second = NewSettings();

    EXPECT_EQ(table.AdoptSettings("a", first, At(0)), first);
    EXPECT_EQ(table.AdoptSettings("a", second, At(0)), first)
        << "two threads that raced on a new leaf share the first answer";
    EXPECT_EQ(table.Settings("a", At(0)), first);
    EXPECT_EQ(table.Size(), 1U);
}

TEST(LeafTableTest, AnEntryHoldsSettingsResourceAndAnchorTogether)
{
    mts::LeafTable table{4};
    const auto settings = NewSettings();
    const auto resource = NewResource();
    (void)table.AdoptSettings("a", settings, At(0));
    (void)table.Insert("a", 1, resource, At(0));
    (void)table.UpdateBootAnchor(
        "a", mts::BootSample{.boot_id = 1, .offset = 5, .at = 0}, 10, At(0));

    EXPECT_EQ(table.Size(), 1U);
    EXPECT_EQ(table.Settings("a", At(0)), settings);
    EXPECT_EQ(table.Find("a", 1, At(0)), resource);
}

TEST(LeafTableTest, AResourceInsertIntoAnEntryWithNoResourceIsKeptWhateverTheHash)
{
    mts::LeafTable table{4};
    (void)table.AdoptSettings("a", NewSettings(), At(0));
    const auto r = NewResource();

    EXPECT_EQ(table.Insert("a", 0, r, At(0)), r)
        << "an entry created for settings has no Resource yet, even though its hash is 0";
}

TEST(LeafTableTest, TheBootAnchorLivesInTheEntryAndGoesWithIt)
{
    mts::LeafTable table{1};
    EXPECT_EQ(table.UpdateBootAnchor("a", {.boot_id = 1, .offset = 100, .at = 0}, 10, At(0)), 100);
    EXPECT_EQ(table.UpdateBootAnchor("a", {.boot_id = 1, .offset = 100, .at = 0}, 10, At(0)), 100);
    EXPECT_EQ(table.UpdateBootAnchor("a", {.boot_id = 1, .offset = 50, .at = 0}, 10, At(0)), 100)
        << "one low sample is not enough";
    (void)table.UpdateBootAnchor("b", {.boot_id = 1, .offset = 1, .at = 0}, 10, At(0));  // evicts a

    EXPECT_EQ(table.UpdateBootAnchor("a", {.boot_id = 1, .offset = 70, .at = 0}, 10, At(0)), 70)
        << "a new entry anchors on its own first sample";
    EXPECT_EQ(table.Evicted(), 2U);
}

TEST(LeafTableTest, IdleEntriesAreEvictedOnInsertOnly)
{
    mts::LeafTable table{8, std::chrono::seconds{60}};
    (void)table.AdoptSettings("a", NewSettings(), At(0));
    (void)table.AdoptSettings("b", NewSettings(), At(30));

    EXPECT_NE(table.Settings("b", At(100)), nullptr) << "a lookup does not evict";
    EXPECT_EQ(table.Size(), 2U);

    (void)table.AdoptSettings("c", NewSettings(), At(100));

    EXPECT_EQ(table.Size(), 2U) << "a (idle 100 s) went; b was seen at 100 s";
    EXPECT_EQ(table.Evicted(), 1U);
    EXPECT_EQ(table.Settings("a", At(100)), nullptr);
}

TEST(LeafTableTest, AnEntryIdleForExactlyTheTimeoutIsKept)
{
    mts::LeafTable table{8, std::chrono::seconds{60}};
    (void)table.AdoptSettings("a", NewSettings(), At(0));
    (void)table.AdoptSettings("b", NewSettings(), At(60));
    EXPECT_EQ(table.Size(), 2U);
    (void)table.AdoptSettings("c", NewSettings(), At(61));
    EXPECT_EQ(table.Size(), 2U);
    EXPECT_EQ(table.Settings("a", At(61)), nullptr);
}

TEST(LeafTableTest, AZeroIdleTimeoutNeverEvictsForIdleness)
{
    mts::LeafTable table{8};
    (void)table.AdoptSettings("a", NewSettings(), At(0));
    (void)table.AdoptSettings("b", NewSettings(), At(1'000'000));
    EXPECT_EQ(table.Size(), 2U);
    EXPECT_EQ(table.Evicted(), 0U);
}

TEST(LeafTableTest, IdleEvictionRunsBeforeTheCapacityCheck)
{
    mts::LeafTable table{2, std::chrono::seconds{60}};
    (void)table.AdoptSettings("a", NewSettings(), At(0));
    (void)table.AdoptSettings("b", NewSettings(), At(50));

    (void)table.AdoptSettings("c", NewSettings(), At(100));

    EXPECT_EQ(table.Evicted(), 1U) << "only the idle a went, so b is not evicted for room";
    EXPECT_NE(table.Settings("b", At(100)), nullptr);
}
