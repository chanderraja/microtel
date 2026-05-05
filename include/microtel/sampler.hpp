// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <memory>
#include <string_view>

namespace microtel
{

/// @brief Forward declaration of the internal `ISampler` interface.
///
/// Defined in `include/microtel/internal/sampler.hpp`. Public consumers do
/// not see the abstract base directly; they construct samplers through the
/// factory functions below and pass the resulting `SamplerHandle` to
/// `SdkBuilder::WithSampler`.
namespace internal
{
class ISampler;
}  // namespace internal

/// @brief Opaque handle to a sampler. Move-only owning wrapper.
///
/// Constructed only via the factory functions in this header. Passed by value
/// (move) to `SdkBuilder::WithSampler`.
class SamplerHandle
{
public:
    SamplerHandle() noexcept = default;
    explicit SamplerHandle(std::unique_ptr<internal::ISampler> impl) noexcept;
    ~SamplerHandle() noexcept;

    SamplerHandle(const SamplerHandle&) = delete;
    SamplerHandle& operator=(const SamplerHandle&) = delete;
    SamplerHandle(SamplerHandle&&) noexcept = default;
    SamplerHandle& operator=(SamplerHandle&&) noexcept = default;

    /// @brief Internal accessor — used by `SdkBuilder` only.
    [[nodiscard]] internal::ISampler* Get() const noexcept;
    [[nodiscard]] std::unique_ptr<internal::ISampler> Release() noexcept;

private:
    std::unique_ptr<internal::ISampler> m_impl;
};

/// @brief Sampler that always samples (`RecordAndSample`).
[[nodiscard]] SamplerHandle MakeAlwaysOnSampler();

/// @brief Sampler that always drops (`Drop`).
[[nodiscard]] SamplerHandle MakeAlwaysOffSampler();

/// @brief TraceId-ratio sampler.
///
/// Samples a fraction of traces based on the trace ID, deterministically.
/// `ratio` is clamped to `[0.0, 1.0]`.
[[nodiscard]] SamplerHandle MakeTraceIdRatioSampler(double ratio);

/// @brief Parent-based sampler.
///
/// If the parent context exists and is sampled, samples; otherwise falls back
/// to `root`. The four standard variants are constructed by passing
/// `MakeAlwaysOnSampler()`, `MakeAlwaysOffSampler()`, `MakeTraceIdRatioSampler(r)`,
/// or another sampler as `root`.
[[nodiscard]] SamplerHandle MakeParentBasedSampler(SamplerHandle root);

}  // namespace microtel
