// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/resource.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

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

/// @brief The leaf receiver's bounded cache of resolved leaf Resources
///        (`docs/leaf-concentrator-design.md` §4.5).
///
/// One entry per leaf id: the resolved Resource and a hash of the Resource
/// the leaf declared, so a changed declaration (a firmware update) is noticed
/// and resolved again. At most `max_leaves` entries; inserting into a full
/// table evicts the least recently seen. Eviction loses only cached state —
/// the next payload from that leaf resolves its Resource again — and a queued
/// record keeps its Resource alive through its own `shared_ptr`.
///
/// Idle-timeout eviction (`leaf_idle_timeout`) and the boot-relative anchor
/// arrive with the time modes and the rest of the `[concentrator]` config.
///
/// @threadsafety Thread-safe. One mutex, held only for the lookup or insert
///               itself and never across a call out, so the receiver's
///               "at most one non-leaf lock" rule holds
///               (`docs/threading-model.md` §4 rule 2).
class LeafTable
{
public:
    /// @param max_leaves the most entries the table holds; at least 1.
    explicit LeafTable(std::uint32_t max_leaves) noexcept;

    /// @brief The cached Resource for @p id, if it was resolved from a
    ///        declaration with the same @p declared_hash. Marks the entry seen.
    /// @return the Resource, or nullptr on a miss or a changed declaration.
    /// @throws std::bad_alloc if the lookup key cannot be built.
    [[nodiscard]] std::shared_ptr<const Resource> Find(std::string_view id,
                                                       std::uint64_t declared_hash);

    /// @brief Cache @p resource for @p id, evicting the least recently seen
    ///        entry if the table is full.
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
                                                         std::shared_ptr<const Resource> resource);

    /// @brief How many leaves the table holds now.
    [[nodiscard]] std::uint64_t Size() const noexcept;

    /// @brief How many entries have been evicted to make room, ever.
    [[nodiscard]] std::uint64_t Evicted() const noexcept;

private:
    struct Entry
    {
        std::string id;
        std::uint64_t declared_hash = 0;
        std::shared_ptr<const Resource> resource;
    };
    using Recency = std::list<Entry>;  ///< front = most recently seen

    std::uint32_t m_max_leaves;
    mutable std::mutex m_mu;
    Recency m_recency;
    std::unordered_map<std::string, Recency::iterator, TransparentStringHash, std::equal_to<>>
        m_index;
    std::uint64_t m_evicted = 0;
};

}  // namespace microtel::sdk
