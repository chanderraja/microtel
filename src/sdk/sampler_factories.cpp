// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Sampler factories and the public `microtel::SamplerHandle` out-of-line
// definitions. The destructor must be defined here (not inline in the
// public header) because it requires the full definition of
// `internal::ISampler`, which is intentionally not exposed in
// include/microtel/sampler.hpp.
//
// `AlwaysOn`, `AlwaysOff`, and `TraceIdRatio` are implemented here.
// `ParentBased` lands in M3-A4; it is declared-only until then.

#include "microtel/internal/sampler.hpp"
#include "microtel/sampler.hpp"
#include "microtel/trace.hpp"

#include <algorithm>
#include <cstdint>
#include <format>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace microtel
{

// --- SamplerHandle out-of-line definitions ------------------------------

SamplerHandle::SamplerHandle(std::unique_ptr<internal::ISampler> impl) noexcept
    : m_impl(std::move(impl))
{
}

SamplerHandle::~SamplerHandle() noexcept = default;

internal::ISampler* SamplerHandle::Get() const noexcept
{
    return m_impl.get();
}

std::unique_ptr<internal::ISampler> SamplerHandle::Release() noexcept
{
    return std::move(m_impl);
}

// --- AlwaysOn sampler ---------------------------------------------------

namespace
{

/// @brief Sampler that returns `RecordAndSample` for every span.
///
/// Stateless, allocation-free on the hot path (the `SamplingResult`'s
/// vector + optional members are default-constructed and remain empty,
/// so no heap allocation occurs).
class AlwaysOnSampler final : public internal::ISampler
{
public:
    [[nodiscard]] internal::SamplingResult ShouldSample(
        const internal::SamplingContext& /*ctx*/) const noexcept override
    {
        return internal::SamplingResult{
            .decision = internal::SamplingDecision::RecordAndSample,
            .additional_attributes = {},
            .trace_state = std::nullopt,
        };
    }

    [[nodiscard]] std::string_view Description() const noexcept override
    {
        return "AlwaysOnSampler";
    }
};

}  // namespace

SamplerHandle MakeAlwaysOnSampler()
{
    return SamplerHandle{std::make_unique<AlwaysOnSampler>()};
}

// --- AlwaysOff sampler --------------------------------------------------

namespace
{

/// @brief Sampler that returns `Drop` for every span. Stateless and
/// allocation-free on the hot path.
class AlwaysOffSampler final : public internal::ISampler
{
public:
    [[nodiscard]] internal::SamplingResult ShouldSample(
        const internal::SamplingContext& /*ctx*/) const noexcept override
    {
        return internal::SamplingResult{
            .decision = internal::SamplingDecision::Drop,
            .additional_attributes = {},
            .trace_state = std::nullopt,
        };
    }

    [[nodiscard]] std::string_view Description() const noexcept override
    {
        return "AlwaysOffSampler";
    }
};

}  // namespace

SamplerHandle MakeAlwaysOffSampler()
{
    return SamplerHandle{std::make_unique<AlwaysOffSampler>()};
}

// --- TraceIdRatio sampler -----------------------------------------------

namespace
{

/// @brief Deterministic ratio sampler.
///
/// Reads the lower 8 bytes of the 16-byte trace id as a big-endian
/// `std::uint64_t`. Samples when that value is below
/// `ratio * UINT64_MAX`.
///
/// Boundary handling:
/// - `ratio >= 1.0`: always samples (avoids the UB of casting `2^64` back
///   to `std::uint64_t` after the double-precision multiplication rounds
///   up).
/// - `ratio <= 0.0`: never samples.
/// - `ratio` outside `[0, 1]` is clamped to that range.
///
/// Description string includes the resolved ratio at three decimals,
/// matching the substring assertion in
/// `tests/unit/sdk/trace_id_ratio_sampler_test.cpp`.
class TraceIdRatioSampler final : public internal::ISampler
{
public:
    explicit TraceIdRatioSampler(double ratio)
        : m_ratio(std::clamp(ratio, 0.0, 1.0)),
          m_always_sample(m_ratio >= 1.0),
          m_threshold(ComputeThreshold(m_ratio)),
          m_description(std::format("TraceIdRatioSampler{{{:.3f}}}", m_ratio))
    {
    }

    [[nodiscard]] internal::SamplingResult ShouldSample(
        const internal::SamplingContext& ctx) const noexcept override
    {
        const auto decision = SampleDecision(ctx);
        return internal::SamplingResult{
            .decision = decision,
            .additional_attributes = {},
            .trace_state = std::nullopt,
        };
    }

    [[nodiscard]] std::string_view Description() const noexcept override
    {
        return m_description;
    }

private:
    [[nodiscard]] internal::SamplingDecision SampleDecision(
        const internal::SamplingContext& ctx) const noexcept
    {
        if (m_always_sample)
        {
            return internal::SamplingDecision::RecordAndSample;
        }
        const std::uint64_t low = ReadLowerBE64(ctx.trace_id);
        return (low < m_threshold) ? internal::SamplingDecision::RecordAndSample
                                   : internal::SamplingDecision::Drop;
    }

    static std::uint64_t ReadLowerBE64(const TraceId& tid) noexcept
    {
        const auto& bytes = tid.AsBytes();
        std::uint64_t v = 0;
        for (std::size_t i = 0; i < 8; ++i)
        {
            v = (v << 8) | static_cast<std::uint64_t>(bytes[8 + i]);
        }
        return v;
    }

    static std::uint64_t ComputeThreshold(double ratio) noexcept
    {
        if (ratio >= 1.0)
        {
            return std::numeric_limits<std::uint64_t>::max();
        }
        if (ratio <= 0.0)
        {
            return 0;
        }
        const auto max_d = static_cast<double>(std::numeric_limits<std::uint64_t>::max());
        return static_cast<std::uint64_t>(ratio * max_d);
    }

    double m_ratio;
    bool m_always_sample;
    std::uint64_t m_threshold;
    std::string m_description;
};

}  // namespace

SamplerHandle MakeTraceIdRatioSampler(double ratio)
{
    return SamplerHandle{std::make_unique<TraceIdRatioSampler>(ratio)};
}

}  // namespace microtel
