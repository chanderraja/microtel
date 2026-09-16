// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#include "histogram.hpp"

#include <algorithm>
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

    const double target = static_cast<double>(total) * p;
    uint64_t cumulative = 0;

    for (int i = 0; i < kBuckets; ++i)
    {
        const uint64_t in_bucket =
            m_buckets[static_cast<std::size_t>(i)].load(std::memory_order_relaxed);
        if (in_bucket == 0)
        {
            continue;
        }
        if (static_cast<double>(cumulative + in_bucket) <= target)
        {
            cumulative += in_bucket;
            continue;
        }

        // Rank-linear interpolation within the bucket [2^i, 2^(i+1)).
        // A midpoint answer here quantizes every percentile above 128 ns to a
        // multiple of 192 ns (the midpoint of [2^i, 2^(i+1)) is 3 * 2^(i-1)),
        // which is a one-octave, +/-50% resolution. Assuming a uniform spread
        // inside the octave and clamping to the observed extremes is honest to
        // a few percent instead. Issue #261.
        const uint64_t lo = (i == 0) ? 0 : (uint64_t{1} << i);
        const uint64_t hi =
            (i == kBuckets - 1) ? m_max.load(std::memory_order_relaxed) : (uint64_t{1} << (i + 1));
        const double fraction =
            (target - static_cast<double>(cumulative)) / static_cast<double>(in_bucket);
        auto value =
            static_cast<uint64_t>(static_cast<double>(lo) +
                                  fraction * (static_cast<double>(hi) - static_cast<double>(lo)));

        // The extremes are tracked exactly; no percentile can honestly fall
        // outside them.
        const uint64_t observed_min = m_min.load(std::memory_order_relaxed);
        const uint64_t observed_max = m_max.load(std::memory_order_relaxed);
        value = std::max(value, observed_min);
        value = std::min(value, observed_max);
        return value;
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
