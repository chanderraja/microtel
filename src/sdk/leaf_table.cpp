// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#include "sdk/leaf_table.hpp"

#include "microtel/resource.hpp"

#include "sdk/leaf_time.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iterator>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <string_view>
#include <utility>

namespace microtel::sdk
{

LeafTable::LeafTable(std::uint32_t max_leaves, std::chrono::nanoseconds idle_timeout) noexcept
    : m_max_leaves(std::max(max_leaves, 1U)), m_idle_timeout(idle_timeout)
{
}

LeafTable::Entry* LeafTable::Touch(std::string_view id, TimePoint now)
{
    const auto it = m_index.find(id);
    if (it == m_index.end())
    {
        return nullptr;
    }
    m_recency.splice(m_recency.begin(), m_recency, it->second);
    it->second->last_seen = now;
    return &*it->second;
}

void LeafTable::EvictOldest()
{
    m_index.erase(m_recency.back().id);
    m_recency.pop_back();
    ++m_evicted;
}

void LeafTable::EvictIdle(TimePoint now)
{
    if (m_idle_timeout <= std::chrono::nanoseconds::zero())
    {
        return;
    }
    // The list is ordered by last sighting, so the idle entries are at its
    // back.
    while (!m_recency.empty() && now - m_recency.back().last_seen > m_idle_timeout)
    {
        EvictOldest();
    }
}

LeafTable::Entry& LeafTable::TouchOrInsert(std::string_view id, TimePoint now)
{
    if (auto* const entry = Touch(id, now); entry != nullptr)
    {
        return *entry;
    }
    EvictIdle(now);
    if (m_index.size() >= m_max_leaves)
    {
        EvictOldest();
    }
    m_recency.push_front(Entry{.id = std::string{id},
                               .settings = nullptr,
                               .declared_hash = 0,
                               .resource = nullptr,
                               .anchor = {},
                               .last_seen = now});
    try
    {
        m_index.emplace(m_recency.front().id, m_recency.begin());
    }
    catch (const std::bad_alloc&)
    {
        // Keep the list and the index in step: an entry the index cannot
        // find could never be evicted.
        m_recency.pop_front();
        throw;
    }
    return m_recency.front();
}

std::shared_ptr<const LeafSettings> LeafTable::Settings(std::string_view id, TimePoint now)
{
    const std::scoped_lock lock{m_mu};
    const auto* const entry = Touch(id, now);
    return entry == nullptr ? nullptr : entry->settings;
}

std::shared_ptr<const LeafSettings> LeafTable::AdoptSettings(
    std::string_view id, std::shared_ptr<const LeafSettings> settings, TimePoint now)
{
    const std::scoped_lock lock{m_mu};
    Entry& entry = TouchOrInsert(id, now);
    if (entry.settings == nullptr)
    {
        entry.settings = std::move(settings);
    }
    return entry.settings;
}

std::shared_ptr<const Resource> LeafTable::Find(std::string_view id,
                                                std::uint64_t declared_hash,
                                                TimePoint now)
{
    const std::scoped_lock lock{m_mu};
    const auto* const entry = Touch(id, now);
    if (entry == nullptr || entry->declared_hash != declared_hash)
    {
        return nullptr;
    }
    return entry->resource;
}

std::shared_ptr<const Resource> LeafTable::Insert(std::string_view id,
                                                  std::uint64_t declared_hash,
                                                  std::shared_ptr<const Resource> resource,
                                                  TimePoint now)
{
    const std::scoped_lock lock{m_mu};
    Entry& entry = TouchOrInsert(id, now);
    if (entry.resource == nullptr || entry.declared_hash != declared_hash)
    {
        entry.declared_hash = declared_hash;
        entry.resource = std::move(resource);
    }
    return entry.resource;
}

std::int64_t LeafTable::UpdateBootAnchor(std::string_view id,
                                         const BootSample& sample,
                                         std::int64_t window,
                                         TimePoint now)
{
    const std::scoped_lock lock{m_mu};
    return TouchOrInsert(id, now).anchor.Update(sample, window);
}

std::uint64_t LeafTable::Size() const noexcept
{
    const std::scoped_lock lock{m_mu};
    return m_index.size();
}

std::uint64_t LeafTable::Evicted() const noexcept
{
    const std::scoped_lock lock{m_mu};
    return m_evicted;
}

UnknownLeafCache::UnknownLeafCache(std::uint32_t capacity, std::chrono::nanoseconds ttl) noexcept
    : m_capacity(std::max(capacity, 1U)), m_ttl(ttl)
{
}

void UnknownLeafCache::Erase(Age::iterator it) noexcept
{
    m_index.erase(it->id);
    m_age.erase(it);
}

bool UnknownLeafCache::Contains(std::string_view id, TimePoint now)
{
    const std::scoped_lock lock{m_mu};
    const auto it = m_index.find(id);
    if (it == m_index.end())
    {
        return false;
    }
    if (now - it->second->at > m_ttl)
    {
        Erase(it->second);
        return false;
    }
    return true;
}

void UnknownLeafCache::Insert(std::string_view id, TimePoint now)
{
    const std::scoped_lock lock{m_mu};
    if (m_index.contains(id))
    {
        return;
    }
    // Every answer has the same TTL, so the list is in expiry order too.
    while (!m_age.empty() && now - m_age.front().at > m_ttl)
    {
        Erase(m_age.begin());
    }
    if (m_index.size() >= m_capacity)
    {
        Erase(m_age.begin());
    }
    m_age.push_back(Answer{.id = std::string{id}, .at = now});
    try
    {
        m_index.emplace(m_age.back().id, std::prev(m_age.end()));
    }
    catch (const std::bad_alloc&)
    {
        // Keep the list and the index in step, as LeafTable does.
        m_age.pop_back();
        throw;
    }
}

std::uint64_t UnknownLeafCache::Size() const noexcept
{
    const std::scoped_lock lock{m_mu};
    return m_index.size();
}

}  // namespace microtel::sdk
