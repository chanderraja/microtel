// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#include "sdk/leaf_time.hpp"

#include "microtel/internal/batch.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>

namespace microtel::sdk
{
namespace
{

constexpr std::int64_t kMax = std::numeric_limits<std::int64_t>::max();
constexpr std::int64_t kMin = std::numeric_limits<std::int64_t>::min();

[[nodiscard]] std::int64_t ToNs(std::chrono::system_clock::time_point tp) noexcept
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(tp.time_since_epoch()).count();
}

[[nodiscard]] std::chrono::system_clock::time_point FromNs(std::int64_t ns) noexcept
{
    return std::chrono::system_clock::time_point{
        std::chrono::duration_cast<std::chrono::system_clock::duration>(
            std::chrono::nanoseconds{ns})};
}

[[nodiscard]] std::int64_t Corrected(std::int64_t t, const TimeCorrection& c) noexcept
{
    if (c.fixed.has_value())
    {
        return *c.fixed;
    }
    return std::max<std::int64_t>(0, SaturatingAdd(t, c.offset));
}

void Correct(std::chrono::system_clock::time_point& tp, const TimeCorrection& c) noexcept
{
    tp = FromNs(Corrected(ToNs(tp), c));
}

}  // namespace

std::int64_t SaturatingAdd(std::int64_t a, std::int64_t b) noexcept
{
    if (b > 0 && a > kMax - b)
    {
        return kMax;
    }
    if (b < 0 && a < kMin - b)
    {
        return kMin;
    }
    return a + b;
}

std::int64_t SaturatingSub(std::int64_t a, std::int64_t b) noexcept
{
    if (b < 0 && a > kMax + b)
    {
        return kMax;
    }
    if (b > 0 && a < kMin + b)
    {
        return kMin;
    }
    return a - b;
}

TimeCorrection ConcentratorStamped(std::int64_t received,
                                   std::optional<std::int64_t> encode_time) noexcept
{
    if (!encode_time.has_value())
    {
        return TimeCorrection{.offset = 0, .fixed = std::max<std::int64_t>(0, received)};
    }
    return TimeCorrection{.offset = SaturatingSub(received, *encode_time), .fixed = std::nullopt};
}

std::optional<TimeCorrection> SyncRelative(std::int64_t received,
                                           std::int64_t encode_time,
                                           std::int64_t sync_age,
                                           const SyncLimits& limits) noexcept
{
    const std::int64_t skew = SaturatingSub(received, encode_time);
    // |skew| without overflow: the negation of kMin saturates to kMax.
    const std::int64_t abs_skew = skew >= 0 ? skew : SaturatingSub(0, skew);
    const bool fresh = sync_age >= 0 && sync_age <= limits.max_sync_age;
    if (!fresh || abs_skew > limits.max_clock_skew)
    {
        return std::nullopt;
    }
    return TimeCorrection{};
}

void ApplyTimeCorrection(internal::SpanRecord& span, const TimeCorrection& correction) noexcept
{
    Correct(span.start_time, correction);
    Correct(span.end_time, correction);
    for (auto& event : span.events)
    {
        Correct(event.timestamp, correction);
    }
}

std::int64_t BootAnchor::Update(const BootSample& sample, std::int64_t window) noexcept
{
    if (m_boot_id != sample.boot_id)
    {
        m_boot_id = sample.boot_id;
        m_count = 0;
    }
    AgeOut(SaturatingSub(sample.at, window));
    if (m_count == kMaxSamples)
    {
        // Drop the oldest, which is first.
        for (std::size_t i = 1; i < m_count; ++i)
        {
            m_samples.at(i - 1) = m_samples.at(i);
        }
        --m_count;
    }
    m_samples.at(m_count) = Sample{.offset = sample.offset, .at = sample.at};
    ++m_count;
    return Anchor();
}

std::size_t BootAnchor::Size() const noexcept
{
    return m_count;
}

void BootAnchor::AgeOut(std::int64_t cutoff) noexcept
{
    // Keep the samples taken at or after the cutoff, in order.
    std::size_t kept = 0;
    for (std::size_t i = 0; i < m_count; ++i)
    {
        if (m_samples.at(i).at >= cutoff)
        {
            m_samples.at(kept) = m_samples.at(i);
            ++kept;
        }
    }
    m_count = kept;
}

std::int64_t BootAnchor::Anchor() const noexcept
{
    // The smallest and the second-smallest, in one pass; with one sample, the
    // second is that sample (the provisional anchor of §5.4).
    std::int64_t lowest = kMax;
    std::int64_t second = kMax;
    for (std::size_t i = 0; i < m_count; ++i)
    {
        const std::int64_t b = m_samples.at(i).offset;
        if (b < lowest)
        {
            second = lowest;
            lowest = b;
        }
        else if (b < second)
        {
            second = b;
        }
    }
    return m_count < 2 ? lowest : second;
}

}  // namespace microtel::sdk
