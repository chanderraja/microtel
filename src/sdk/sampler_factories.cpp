// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// Sampler factories and the public `microtel::SamplerHandle` out-of-line
// definitions. The destructor must be defined here (not inline in the
// public header) because it requires the full definition of
// `internal::ISampler`, which is intentionally not exposed in
// include/microtel/sampler.hpp.
//
// Only `MakeAlwaysOnSampler` is implemented in this M3-A1 chunk. The
// other three factory declarations in include/microtel/sampler.hpp
// (AlwaysOff, TraceIdRatio, ParentBased) land in subsequent M3-A
// chunks; they are declared-only until then. Tests reference only
// MakeAlwaysOn so the link succeeds.

#include "microtel/internal/sampler.hpp"
#include "microtel/sampler.hpp"

#include <memory>
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

}  // namespace microtel
