// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/attribute.hpp"
#include "microtel/internal/clock.hpp"
#include "microtel/leaf_receiver.hpp"
#include "microtel/resource.hpp"

#include "sdk/leaf_time.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace microtel::sdk
{

/// @brief Hash for string-keyed maps looked up by `std::string_view`, so a
///        lookup costs no key copy.
struct TransparentStringHash
{
    using is_transparent = void;
    [[nodiscard]] std::size_t operator()(std::string_view s) const noexcept
    {
        return std::hash<std::string_view>{}(s);
    }
};

/// How many negative settings answers the receiver keeps (issue #343): enough
/// to spare the resolver a repeat question from a handful of unconfigured
/// leaves, small enough that a flood of ids costs little.
inline constexpr std::uint32_t kUnknownLeafCacheSize = 256;
/// How long a negative answer is used before the resolver is asked again, so
/// a leaf configured later is accepted within this time.
inline constexpr std::chrono::seconds kUnknownLeafTtl{60};

/// @brief A leaf's effective configuration: its static `leaves` entry with
///        the resolver's answer merged over it (§4.3). Resolved once per table
///        entry and shared, immutable, by every payload of that leaf.
struct LeafSettings
{
    /// Whether the static table or the resolver knows the leaf; an unknown
    /// leaf is governed by `unknown_leaf` (§4.4).
    bool configured = false;
    /// Unset: the receiver's `default_time_mode` (§5.1).
    std::optional<LeafTimeMode> time_mode;
    /// Layer 3 of §4.4: the configured Resource, file then code then resolver.
    std::vector<KeyValue> resource;
};

/// @brief The leaf receiver's bounded per-leaf state
///        (`docs/leaf-concentrator-design.md` §4.5).
///
/// One entry per leaf id: the leaf's settings (so a resolver is asked once
/// per entry), its resolved Resource with a hash of the Resource the leaf
/// declared (so a changed declaration, a firmware update, is noticed and
/// resolved again), its boot-relative anchor (§5.4), and when it was last
/// seen. Eviction loses only this cached state: the next payload from that
/// leaf resolves its settings and Resource again and re-anchors, and a queued
/// record keeps its Resource alive through its own `shared_ptr`.
///
/// Two bounds, both applied when an entry is inserted, so an idle
/// concentrator costs no timer thread:
/// - `idle_timeout`: entries not seen for longer than it are evicted first;
/// - `max_leaves`: inserting into a full table then evicts the least recently
///   seen.
///
/// @threadsafety Thread-safe. One mutex, held only for the lookup or update
///               itself and never across a call out, so the receiver's
///               "at most one non-leaf lock" rule holds
///               (`docs/threading-model.md` §4 rule 2).
class LeafTable
{
public:
    using TimePoint = internal::TimePointSteady;

    /// @param max_leaves   the most entries the table holds; at least 1.
    /// @param idle_timeout entries not seen for longer than this are evicted
    ///                     on the next insert; zero or negative disables it.
    explicit LeafTable(
        std::uint32_t max_leaves,
        std::chrono::nanoseconds idle_timeout = std::chrono::nanoseconds::zero()) noexcept;

    /// @brief The settings cached for @p id, marking the entry seen at @p now.
    /// @return the settings, or nullptr if there is no entry or it has none.
    /// @throws std::bad_alloc if the lookup key cannot be built.
    [[nodiscard]] std::shared_ptr<const LeafSettings> Settings(std::string_view id, TimePoint now);

    /// @brief Cache @p settings for @p id, inserting an entry if there is none.
    ///
    /// If the entry already has settings — another thread resolved the same
    /// leaf first (§3.5) — these are discarded and the existing ones returned.
    ///
    /// @return the settings now cached for @p id.
    /// @throws std::bad_alloc if the entry cannot be allocated.
    [[nodiscard]] std::shared_ptr<const LeafSettings> AdoptSettings(
        std::string_view id, std::shared_ptr<const LeafSettings> settings, TimePoint now);

    /// @brief The cached Resource for @p id, if it was resolved from a
    ///        declaration with the same @p declared_hash. Marks the entry seen.
    /// @return the Resource, or nullptr on a miss or a changed declaration.
    /// @throws std::bad_alloc if the lookup key cannot be built.
    [[nodiscard]] std::shared_ptr<const Resource> Find(std::string_view id,
                                                       std::uint64_t declared_hash,
                                                       TimePoint now = {});

    /// @brief Cache @p resource for @p id, inserting an entry if there is none.
    ///
    /// If an entry for @p id with the same @p declared_hash is already there —
    /// another thread resolved the same leaf first (§3.5) — this insert is
    /// discarded and the existing Resource is returned, so both threads
    /// enqueue under one Resource. An entry with a different hash is replaced.
    ///
    /// @return the Resource now cached for @p id.
    /// @throws std::bad_alloc if the entry cannot be allocated.
    [[nodiscard]] std::shared_ptr<const Resource> Insert(std::string_view id,
                                                         std::uint64_t declared_hash,
                                                         std::shared_ptr<const Resource> resource,
                                                         TimePoint now = {});

    /// @brief Add a boot-relative sample to @p id's anchor and return the
    ///        anchor `B` (§5.4), inserting an entry if there is none (the leaf
    ///        was evicted after its settings were read).
    /// @throws std::bad_alloc if the entry cannot be allocated.
    [[nodiscard]] std::int64_t UpdateBootAnchor(std::string_view id,
                                                const BootSample& sample,
                                                std::int64_t window,
                                                TimePoint now);

    /// @brief How many leaves the table holds now.
    [[nodiscard]] std::uint64_t Size() const noexcept;

    /// @brief How many entries have been evicted, for room or for idleness, ever.
    [[nodiscard]] std::uint64_t Evicted() const noexcept;

private:
    struct Entry
    {
        std::string id;
        std::shared_ptr<const LeafSettings> settings;
        std::uint64_t declared_hash = 0;
        std::shared_ptr<const Resource> resource;
        BootAnchor anchor;
        TimePoint last_seen{};
    };
    using Recency = std::list<Entry>;  ///< front = most recently seen

    /// The entry for @p id marked seen at @p now, or nullptr. Lock held.
    [[nodiscard]] Entry* Touch(std::string_view id, TimePoint now);
    /// The entry for @p id marked seen, inserted if absent after evicting the
    /// idle entries and, if still full, the least recently seen. Lock held.
    [[nodiscard]] Entry& TouchOrInsert(std::string_view id, TimePoint now);
    /// Evict the least recently seen entry. Lock held; the table is not empty.
    void EvictOldest();
    /// Evict every entry idle for longer than the timeout. Lock held.
    void EvictIdle(TimePoint now);

    std::uint32_t m_max_leaves;
    std::chrono::nanoseconds m_idle_timeout;
    mutable std::mutex m_mu;
    Recency m_recency;
    std::unordered_map<std::string, Recency::iterator, TransparentStringHash, std::equal_to<>>
        m_index;
    std::uint64_t m_evicted = 0;
};

/// @brief The leaf ids whose settings answer was "not configured", for a
///        receiver that rejects unknown leaves (issue #343; design §4.5).
///
/// Kept apart from the `LeafTable`, so a burst of unknown ids can evict only
/// other negative answers, never a leaf the receiver accepts along with its
/// Resource and boot anchor. An answer is used for at most `ttl` from when it
/// was cached, however often the leaf is seen, so a leaf configured later is
/// accepted within that time; when the cache is full, the oldest answer goes.
/// Losing an answer costs only a repeat resolver call.
///
/// @threadsafety Thread-safe. One mutex, held only for the lookup or update
///               itself; never taken together with the `LeafTable`'s.
class UnknownLeafCache
{
public:
    using TimePoint = internal::TimePointSteady;

    /// @param capacity the most answers kept; at least 1.
    /// @param ttl      how long an answer is used after it is cached.
    UnknownLeafCache(std::uint32_t capacity, std::chrono::nanoseconds ttl) noexcept;

    /// @brief Whether @p id has a negative answer no older than the TTL at
    ///        @p now. An expired answer is dropped.
    /// @throws std::bad_alloc if the lookup key cannot be built.
    [[nodiscard]] bool Contains(std::string_view id, TimePoint now);

    /// @brief Cache a negative answer for @p id at @p now, first dropping the
    ///        expired answers and then, if still full, the oldest. If @p id
    ///        already has one (two threads raced on it), that one is kept.
    /// @throws std::bad_alloc if the entry cannot be allocated.
    void Insert(std::string_view id, TimePoint now);

    /// @brief How many answers the cache holds now.
    [[nodiscard]] std::uint64_t Size() const noexcept;

private:
    struct Answer
    {
        std::string id;
        TimePoint at{};
    };
    using Age = std::list<Answer>;  ///< front = oldest

    /// Drop the answer at @p it. Lock held.
    void Erase(Age::iterator it) noexcept;

    std::uint32_t m_capacity;
    std::chrono::nanoseconds m_ttl;
    mutable std::mutex m_mu;
    Age m_age;
    std::unordered_map<std::string, Age::iterator, TransparentStringHash, std::equal_to<>> m_index;
};

}  // namespace microtel::sdk
