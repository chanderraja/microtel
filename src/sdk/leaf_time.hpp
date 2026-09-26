// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/internal/batch.hpp"
#include "microtel/leaf_receiver.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

/// @file
/// The leaf receiver's time correction (`docs/leaf-concentrator-design.md`
/// §5): the arithmetic of the three modes and the boot-relative anchor. Pure;
/// the per-leaf anchor state is kept by `LeafTable`.
///
/// All times are signed 64-bit nanoseconds. Leaf values are untrusted, so all
/// arithmetic saturates instead of overflowing, and a corrected timestamp is
/// clamped to `[0, INT64_MAX]` — the range an OTLP `fixed64` Unix time can
/// hold through a `system_clock::time_point`.

namespace microtel::sdk
{

/// @brief `a + b`, saturated to the `int64_t` range.
[[nodiscard]] std::int64_t SaturatingAdd(std::int64_t a, std::int64_t b) noexcept;

/// @brief `a - b`, saturated to the `int64_t` range.
[[nodiscard]] std::int64_t SaturatingSub(std::int64_t a, std::int64_t b) noexcept;

/// @brief How one ResourceSpans' leaf timestamps become Unix times.
struct TimeCorrection
{
    /// Added to every timestamp: `t' = t + offset` (§5.2–§5.4).
    std::int64_t offset = 0;
    /// Set for a concentrator-stamped payload without an encode time: every
    /// timestamp becomes this value, `R` (§5.2).
    std::optional<std::int64_t> fixed;
};

/// @brief The limits of §5.3's trust test, in nanoseconds.
struct SyncLimits
{
    std::int64_t max_sync_age = 0;
    std::int64_t max_clock_skew = 0;
};

/// @brief §5.2: `t' = t + (R - E)`; with no `E`, every timestamp is `R`.
[[nodiscard]] TimeCorrection ConcentratorStamped(std::int64_t received,
                                                 std::optional<std::int64_t> encode_time) noexcept;

/// @brief §5.3: the timestamps are trusted (`t' = t`) when
///        `0 <= sync_age <= max_sync_age` and `|R - E| <= max_clock_skew`.
/// @return nullopt when they are not, and the caller falls back to
///         `ConcentratorStamped` and counts `time_fallbacks`.
[[nodiscard]] std::optional<TimeCorrection> SyncRelative(std::int64_t received,
                                                         std::int64_t encode_time,
                                                         std::int64_t sync_age,
                                                         const SyncLimits& limits) noexcept;

/// @brief Rewrite a span's start, end and event timestamps (§3.6 step 1).
void ApplyTimeCorrection(internal::SpanRecord& span, const TimeCorrection& correction) noexcept;

/// @brief One boot-relative sample: `b = R - E`, taken at receive time `R`.
struct BootSample
{
    std::int64_t boot_id = 0;
    std::int64_t offset = 0;  ///< `b`
    std::int64_t at = 0;      ///< `R`
};

/// @brief A leaf's boot-relative anchor `B` (§5.4): the second-smallest of the
///        last 16 samples taken within the window, or the only sample while
///        there is one.
///
/// Requiring a second sample at or below the chosen value means a single low
/// outlier — a leaf clock that glitched forward, a stale `received_at`, a
/// concentrator clock step — cannot move `B`. A new `boot_id` discards every
/// sample of the previous boot.
///
/// 16 samples of two 8-byte values: 256 bytes per leaf, kept in the leaf
/// table entry.
///
/// @threadsafety Not thread-safe; `LeafTable` holds its lock around `Update`.
class BootAnchor
{
public:
    /// The most samples the anchor keeps (§5.4).
    static constexpr std::size_t kMaxSamples = 16;

    /// @brief Age out samples taken before `sample.at - window`, add
    ///        @p sample (dropping the oldest when all 16 are in use), and
    ///        return the anchor.
    [[nodiscard]] std::int64_t Update(const BootSample& sample, std::int64_t window) noexcept;

    /// @brief How many samples count now.
    [[nodiscard]] std::size_t Size() const noexcept;

private:
    struct Sample
    {
        std::int64_t offset = 0;
        std::int64_t at = 0;
    };

    void AgeOut(std::int64_t cutoff) noexcept;
    [[nodiscard]] std::int64_t Anchor() const noexcept;

    std::optional<std::int64_t> m_boot_id;
    /// Oldest first; the first `m_count` are in use.
    std::array<Sample, kMaxSamples> m_samples{};
    std::size_t m_count = 0;
};

}  // namespace microtel::sdk
