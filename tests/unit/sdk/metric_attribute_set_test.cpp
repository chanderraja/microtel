// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Behavioural tests for AttributeSet — the owned, order-insensitive key the
// metric aggregation map is keyed on (M12, docs/metrics-design.md §9).
//
// Contract under test:
//  - Construction from a borrowed AttributeSpan copies + normalises the pairs.
//  - Equality is order-insensitive (attribute sets are unordered).
//  - Equal sets hash equal; HashOf(span) == AttributeSet(span).Hash() so the
//    aggregation map can probe with a borrowed span without materialising a key.
//  - MatchesSpan compares a set against an (unordered) borrowed span.

#include "sdk/metric_attribute_set.hpp"

#include "microtel/attribute.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

namespace mt = microtel;
namespace mts = microtel::sdk;

namespace
{

mt::KeyValue Kv(std::string key, mt::AttributeValue value)
{
    return mt::KeyValue{.key = std::move(key), .value = std::move(value)};
}

}  // namespace

TEST(AttributeSetTest, EmptySetEqualsEmptySpan)
{
    const mts::AttributeSet empty_default;
    const mts::AttributeSet empty_span{mt::AttributeSpan{}};

    EXPECT_TRUE(empty_default == empty_span);
    EXPECT_EQ(empty_default.Hash(), empty_span.Hash());
    EXPECT_TRUE(empty_default.Pairs().empty());
}

TEST(AttributeSetTest, EqualityIsOrderInsensitive)
{
    const std::vector<mt::KeyValue> ab{Kv("a", std::int64_t{1}), Kv("b", std::int64_t{2})};
    const std::vector<mt::KeyValue> ba{Kv("b", std::int64_t{2}), Kv("a", std::int64_t{1})};

    const mts::AttributeSet s_ab{mt::AttributeSpan{ab}};
    const mts::AttributeSet s_ba{mt::AttributeSpan{ba}};

    EXPECT_TRUE(s_ab == s_ba);
    EXPECT_EQ(s_ab.Hash(), s_ba.Hash());
}

TEST(AttributeSetTest, DifferentValuesAreUnequal)
{
    const std::vector<mt::KeyValue> a1{Kv("a", std::int64_t{1})};
    const std::vector<mt::KeyValue> a2{Kv("a", std::int64_t{2})};

    EXPECT_FALSE(mts::AttributeSet{mt::AttributeSpan{a1}} ==
                 mts::AttributeSet{mt::AttributeSpan{a2}});
}

TEST(AttributeSetTest, DifferentKeysAreUnequal)
{
    const std::vector<mt::KeyValue> a{Kv("a", std::int64_t{1})};
    const std::vector<mt::KeyValue> b{Kv("b", std::int64_t{1})};

    EXPECT_FALSE(mts::AttributeSet{mt::AttributeSpan{a}} ==
                 mts::AttributeSet{mt::AttributeSpan{b}});
}

TEST(AttributeSetTest, DifferentSizesAreUnequal)
{
    const std::vector<mt::KeyValue> one{Kv("a", std::int64_t{1})};
    const std::vector<mt::KeyValue> two{Kv("a", std::int64_t{1}), Kv("b", std::int64_t{2})};

    EXPECT_FALSE(mts::AttributeSet{mt::AttributeSpan{one}} ==
                 mts::AttributeSet{mt::AttributeSpan{two}});
}

TEST(AttributeSetTest, HashOfMatchesConstructedHash_RegardlessOfOrder)
{
    const std::vector<mt::KeyValue> ordered{Kv("a", std::int64_t{1}), Kv("b", std::int64_t{2})};
    const std::vector<mt::KeyValue> shuffled{Kv("b", std::int64_t{2}), Kv("a", std::int64_t{1})};

    const mts::AttributeSet s{mt::AttributeSpan{ordered}};

    EXPECT_EQ(mts::AttributeSet::HashOf(mt::AttributeSpan{ordered}), s.Hash());
    EXPECT_EQ(mts::AttributeSet::HashOf(mt::AttributeSpan{shuffled}), s.Hash());
}

TEST(AttributeSetTest, MatchesSpanIsOrderInsensitive)
{
    const std::vector<mt::KeyValue> ordered{Kv("a", std::int64_t{1}), Kv("b", std::int64_t{2})};
    const std::vector<mt::KeyValue> shuffled{Kv("b", std::int64_t{2}), Kv("a", std::int64_t{1})};
    const std::vector<mt::KeyValue> different{Kv("a", std::int64_t{1}), Kv("b", std::int64_t{9})};
    const std::vector<mt::KeyValue> smaller{Kv("a", std::int64_t{1})};

    const mts::AttributeSet s{mt::AttributeSpan{ordered}};

    EXPECT_TRUE(s.MatchesSpan(mt::AttributeSpan{ordered}));
    EXPECT_TRUE(s.MatchesSpan(mt::AttributeSpan{shuffled}));
    EXPECT_FALSE(s.MatchesSpan(mt::AttributeSpan{different}));
    EXPECT_FALSE(s.MatchesSpan(mt::AttributeSpan{smaller}));
}

TEST(AttributeSetTest, AllValueTypesHashAndCompare)
{
    const std::vector<mt::KeyValue> kvs{Kv("b", true),
                                        Kv("i", std::int64_t{42}),
                                        Kv("d", 3.5),
                                        Kv("s", std::string{"hello"}),
                                        Kv("ab", std::vector<bool>{true, false}),
                                        Kv("ai", std::vector<std::int64_t>{1, 2, 3}),
                                        Kv("ad", std::vector<double>{1.0, 2.0}),
                                        Kv("as", std::vector<std::string>{"x", "y"})};

    const mts::AttributeSet s1{mt::AttributeSpan{kvs}};
    const mts::AttributeSet s2{mt::AttributeSpan{kvs}};

    EXPECT_TRUE(s1 == s2);
    EXPECT_EQ(s1.Hash(), s2.Hash());
    EXPECT_EQ(mts::AttributeSet::HashOf(mt::AttributeSpan{kvs}), s1.Hash());
    EXPECT_EQ(s1.Pairs().size(), kvs.size());
}

TEST(AttributeSetTest, ArrayElementOrderIsSignificant)
{
    // Within a single array-valued attribute, element order is part of the value.
    const std::vector<mt::KeyValue> forward{Kv("a", std::vector<std::int64_t>{1, 2})};
    const std::vector<mt::KeyValue> reversed{Kv("a", std::vector<std::int64_t>{2, 1})};

    EXPECT_FALSE(mts::AttributeSet{mt::AttributeSpan{forward}} ==
                 mts::AttributeSet{mt::AttributeSpan{reversed}});
}
