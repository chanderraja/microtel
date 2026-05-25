// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#include "histogram.hpp"

#include <bit>
#include <cstdint>

namespace bench
{

namespace
{

constexpr int BucketFor(uint64_t ns) noexcept
{
    if (ns == 0)
    {
        return 0;
    }
    const int bit = 63 - std::countl_zero(ns);
    return bit < Histogram::kBuckets ? bit : Histogram::kBuckets - 1;
}

}  // namespace

void Histogram::Record(uint64_t ns) noexcept
{
    const int bucket = BucketFor(ns);
    m_buckets[static_cast<std::size_t>(bucket)].fetch_add(1, std::memory_order_relaxed);
    m_count.fetch_add(1, std::memory_order_relaxed);

    uint64_t prev_min = m_min.load(std::memory_order_relaxed);
    while (ns < prev_min &&
           !m_min.compare_exchange_weak(prev_min, ns, std::memory_order_relaxed))
    {
    }

    uint64_t prev_max = m_max.load(std::memory_order_relaxed);
    while (ns > prev_max &&
           !m_max.compare_exchange_weak(prev_max, ns, std::memory_order_relaxed))
    {
    }
}

uint64_t Histogram::Percentile(double p) const noexcept
{
    const uint64_t total = m_count.load(std::memory_order_relaxed);
    if (total == 0)
    {
        return 0;
    }

    const auto target = static_cast<uint64_t>(static_cast<double>(total) * p);
    uint64_t cumulative = 0;

    for (int i = 0; i < kBuckets; ++i)
    {
        cumulative += m_buckets[static_cast<std::size_t>(i)].load(std::memory_order_relaxed);
        if (cumulative > target)
        {
            // Return the midpoint of the bucket [2^i, 2^(i+1)).
            const uint64_t lo = (i == 0) ? 0 : (uint64_t{1} << i);
            const uint64_t hi = (i == kBuckets - 1) ? lo : (uint64_t{1} << (i + 1));
            return (lo + hi) / 2;
        }
    }

    return m_max.load(std::memory_order_relaxed);
}

uint64_t Histogram::Count() const noexcept
{
    return m_count.load(std::memory_order_relaxed);
}

uint64_t Histogram::Min() const noexcept
{
    return m_min.load(std::memory_order_relaxed);
}

uint64_t Histogram::Max() const noexcept
{
    return m_max.load(std::memory_order_relaxed);
}

std::array<uint64_t, Histogram::kBuckets> Histogram::Buckets() const noexcept
{
    std::array<uint64_t, kBuckets> snap{};
    for (int i = 0; i < kBuckets; ++i)
    {
        snap[static_cast<std::size_t>(i)] =
            m_buckets[static_cast<std::size_t>(i)].load(std::memory_order_relaxed);
    }
    return snap;
}

}  // namespace bench
