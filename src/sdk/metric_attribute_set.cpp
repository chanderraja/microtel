// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#include "sdk/metric_attribute_set.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

namespace microtel::sdk
{
namespace
{

// Pure arithmetic — genuinely cannot throw.
void HashCombine(std::size_t& seed, std::size_t value) noexcept
{
    constexpr std::size_t kGolden = 0x9e3779b97f4a7c15ULL;
    constexpr std::size_t kShiftLeft = 6;
    constexpr std::size_t kShiftRight = 2;
    seed ^= value + kGolden + (seed << kShiftLeft) + (seed >> kShiftRight);
}

// Hashing/comparison call std::hash / std::string / std::variant operations,
// which are not formally `noexcept`, so these helpers are not marked `noexcept`
// (matching std::string/std::variant comparison semantics). They do not throw
// in practice; the instrument hot-path methods that call them remain noexcept
// and absorb any failure as drop-and-count.

template <typename Vec>
std::size_t HashArray(const Vec& vec)
{
    std::size_t hash = 0;
    for (const auto& elem : vec)
    {
        HashCombine(hash, std::hash<typename Vec::value_type>{}(elem));
    }
    return hash;
}

std::size_t HashValue(const AttributeValue& value)
{
    return std::visit(
        [](const auto& held) -> std::size_t
        {
            using T = std::decay_t<decltype(held)>;
            if constexpr (std::is_same_v<T, std::vector<bool>> ||
                          std::is_same_v<T, std::vector<std::int64_t>> ||
                          std::is_same_v<T, std::vector<double>> ||
                          std::is_same_v<T, std::vector<std::string>>)
            {
                return HashArray(held);
            }
            else
            {
                return std::hash<T>{}(held);
            }
        },
        value);
}

std::size_t HashPair(const KeyValue& pair)
{
    std::size_t hash = std::hash<std::string>{}(pair.key);
    HashCombine(hash, HashValue(pair.value));
    return hash;
}

// Combined commutatively across pairs (an attribute set is unordered), so the
// result is independent of pair order and matches whether computed from a
// sorted owned vector or a borrowed unsorted span.
std::size_t HashPairs(AttributeSpan attrs)
{
    std::size_t total = 0;
    for (const auto& pair : attrs)
    {
        total += HashPair(pair);
    }
    return total;
}

bool PairEqual(const KeyValue& lhs, const KeyValue& rhs)
{
    return lhs.key == rhs.key && lhs.value == rhs.value;
}

}  // namespace

AttributeSet::AttributeSet(AttributeSpan attrs)
    : m_pairs(attrs.begin(), attrs.end()), m_hash(HashPairs(attrs))
{
    std::ranges::sort(m_pairs, {}, &KeyValue::key);
}

std::size_t AttributeSet::HashOf(AttributeSpan attrs)
{
    return HashPairs(attrs);
}

bool AttributeSet::operator==(const AttributeSet& other) const
{
    if (m_hash != other.m_hash || m_pairs.size() != other.m_pairs.size())
    {
        return false;
    }
    // Both vectors are sorted by key, so a positional compare is order-correct.
    return std::ranges::equal(m_pairs, other.m_pairs, PairEqual);
}

bool AttributeSet::MatchesSpan(AttributeSpan attrs) const
{
    if (m_pairs.size() != attrs.size())
    {
        return false;
    }
    // attrs is unordered; match each owned pair against the borrowed span.
    return std::ranges::all_of(m_pairs,
                               [attrs](const KeyValue& mine)
                               {
                                   const auto found = std::ranges::find_if(
                                       attrs,
                                       [&mine](const KeyValue& kv) { return kv.key == mine.key; });
                                   return found != attrs.end() && found->value == mine.value;
                               });
}

const AttributeSet& OverflowAttributeSet()
{
    static const KeyValue kAttr{.key = "otel.metric.overflow", .value = true};
    static const AttributeSet kSet{AttributeSpan{&kAttr, 1}};
    return kSet;
}

}  // namespace microtel::sdk
