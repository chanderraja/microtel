// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#include "sdk/leaf_table.hpp"

#include "microtel/resource.hpp"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

namespace microtel::sdk
{

LeafTable::LeafTable(std::uint32_t max_leaves) noexcept : m_max_leaves(std::max(max_leaves, 1U)) {}

std::shared_ptr<const Resource> LeafTable::Find(std::string_view id, std::uint64_t declared_hash)
{
    const std::scoped_lock lock{m_mu};
    const auto it = m_index.find(id);
    if (it == m_index.end() || it->second->declared_hash != declared_hash)
    {
        return nullptr;
    }
    m_recency.splice(m_recency.begin(), m_recency, it->second);
    return it->second->resource;
}

std::shared_ptr<const Resource> LeafTable::Insert(std::string_view id,
                                                  std::uint64_t declared_hash,
                                                  std::shared_ptr<const Resource> resource)
{
    const std::scoped_lock lock{m_mu};
    if (const auto it = m_index.find(id); it != m_index.end())
    {
        m_recency.splice(m_recency.begin(), m_recency, it->second);
        if (it->second->declared_hash != declared_hash)
        {
            it->second->declared_hash = declared_hash;
            it->second->resource = std::move(resource);
        }
        return it->second->resource;
    }
    if (m_index.size() >= m_max_leaves)
    {
        m_index.erase(m_recency.back().id);
        m_recency.pop_back();
        ++m_evicted;
    }
    m_recency.push_front(Entry{
        .id = std::string{id}, .declared_hash = declared_hash, .resource = std::move(resource)});
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
    return m_recency.front().resource;
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

}  // namespace microtel::sdk
