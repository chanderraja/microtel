// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/internal/sampler.hpp"

#include <string_view>

namespace microtel::testing
{

/// @brief Dumb mock for `microtel::internal::ISampler`.
///
/// Returns a configured `SamplingResult` on every `ShouldSample` call.
/// Default is `RecordAndSample` so tests that don't care about sampling
/// behaviour get spans through to the processor.
class MockSampler : public internal::ISampler
{
public:
    // --- Configurable returns ---
    internal::SamplingResult result_to_return{internal::SamplingDecision::RecordAndSample, {}, {}};
    std::string_view description = "MockSampler";

    // --- Recording ---
    mutable int should_sample_call_count = 0;

    // --- ISampler ---

    [[nodiscard]] internal::SamplingResult ShouldSample(
        const internal::SamplingContext& /*ctx*/) const noexcept override
    {
        ++should_sample_call_count;
        return result_to_return;
    }

    [[nodiscard]] std::string_view Description() const noexcept override
    {
        return description;
    }
};

}  // namespace microtel::testing
