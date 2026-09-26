// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#include "common/config/concentrator_config.hpp"

#include "microtel/leaf_receiver.hpp"

#include "common/config/table_merge.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>
#include <system_error>
#include <utility>

namespace microtel::config
{
namespace
{

constexpr std::uint64_t kKiB = 1024;
constexpr std::uint64_t kMiB = kKiB * kKiB;
constexpr std::int64_t kSecondsPerMinute = 60;
constexpr std::int64_t kSecondsPerHour = 60 * kSecondsPerMinute;

/// A value's leading decimal digits and the suffix after them.
struct Number
{
    std::uint64_t value = 0;
    std::string_view suffix;
};

[[nodiscard]] std::optional<Number> SplitNumber(std::string_view text) noexcept
{
    Number n;
    const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), n.value);
    if (ec != std::errc{} || ptr == text.data())
    {
        return std::nullopt;
    }
    n.suffix = text.substr(static_cast<std::size_t>(ptr - text.data()));
    return n;
}

/// The multiplier @p suffix names in @p units, or nullopt.
template <typename T, std::size_t N>
[[nodiscard]] std::optional<T> UnitOf(std::string_view suffix,
                                      const std::array<std::pair<std::string_view, T>, N>& units)
{
    const auto it = std::ranges::find(units, suffix, &std::pair<std::string_view, T>::first);
    return it == units.end() ? std::nullopt : std::optional<T>{it->second};
}

}  // namespace

std::optional<std::uint32_t> ParseByteSize(std::string_view text) noexcept
{
    constexpr std::array<std::pair<std::string_view, std::uint64_t>, 4> kUnits{{
        {"", 1},
        {"B", 1},
        {"KiB", kKiB},
        {"MiB", kMiB},
    }};
    const auto n = SplitNumber(text);
    if (!n.has_value())
    {
        return std::nullopt;
    }
    const auto unit = UnitOf(n->suffix, kUnits);
    constexpr std::uint64_t kMax = std::numeric_limits<std::uint32_t>::max();
    if (!unit.has_value() || n->value > kMax / *unit)
    {
        return std::nullopt;
    }
    return static_cast<std::uint32_t>(n->value * *unit);
}

std::optional<std::chrono::seconds> ParseDuration(std::string_view text) noexcept
{
    constexpr std::array<std::pair<std::string_view, std::int64_t>, 3> kUnits{{
        {"s", 1},
        {"m", kSecondsPerMinute},
        {"h", kSecondsPerHour},
    }};
    const auto n = SplitNumber(text);
    if (!n.has_value())
    {
        return std::nullopt;
    }
    const auto unit = UnitOf(n->suffix, kUnits);
    constexpr auto kMax = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    if (!unit.has_value() || n->value > kMax / static_cast<std::uint64_t>(*unit))
    {
        return std::nullopt;
    }
    return std::chrono::seconds{static_cast<std::int64_t>(n->value) * *unit};
}

std::optional<UnknownLeafPolicy> ParseUnknownLeafPolicy(std::string_view text) noexcept
{
    if (text == "accept")
    {
        return UnknownLeafPolicy::Accept;
    }
    if (text == "reject")
    {
        return UnknownLeafPolicy::Reject;
    }
    return std::nullopt;
}

std::optional<LeafTimeMode> ParseLeafTimeMode(std::string_view text) noexcept
{
    if (text == "concentrator_stamped")
    {
        return LeafTimeMode::ConcentratorStamped;
    }
    if (text == "sync_relative")
    {
        return LeafTimeMode::SyncRelative;
    }
    if (text == "boot_relative")
    {
        return LeafTimeMode::BootRelative;
    }
    return std::nullopt;
}

std::optional<std::optional<LeafTimeMode>> ParseDefaultTimeMode(std::string_view text) noexcept
{
    if (text == "auto")
    {
        return std::optional<LeafTimeMode>{};
    }
    const auto mode = ParseLeafTimeMode(text);
    if (!mode.has_value())
    {
        return std::nullopt;
    }
    return mode;
}

void MergeLeafReceiverOptions(LeafReceiverOptions& base, const LeafReceiverOptions& code)
{
    LeafReceiverOptions merged = code;
    merged.leaf_defaults_resource = std::move(base.leaf_defaults_resource);
    MergeResourceAttrs(merged.leaf_defaults_resource, code.leaf_defaults_resource);
    merged.leaves = std::move(base.leaves);
    for (const auto& [id, leaf] : code.leaves)
    {
        const auto it =
            std::ranges::find(merged.leaves, id, &std::pair<std::string, LeafConfig>::first);
        if (it == merged.leaves.end())
        {
            merged.leaves.emplace_back(id, leaf);
            continue;
        }
        MergeResourceAttrs(it->second.resource, leaf.resource);
        if (leaf.time_mode.has_value())
        {
            it->second.time_mode = leaf.time_mode;
        }
    }
    base = std::move(merged);
}

}  // namespace microtel::config
