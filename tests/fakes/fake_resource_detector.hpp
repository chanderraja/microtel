// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/error.hpp"
#include "microtel/expected.hpp"
#include "microtel/internal/resource_detector.hpp"
#include "microtel/resource.hpp"

#include <string_view>

namespace microtel::testing
{

/// @brief Fake `IResourceDetector` for SDK init tests.
///
/// Returns the configured `Resource` on `Detect()`, or a `ConfigError` if
/// `failure` is set. Tests that exercise the strict / lenient detector
/// policy in `error-model.md` §8 use this with a deliberately-failing
/// configuration.
class FakeResourceDetector : public internal::IResourceDetector
{
public:
    microtel::Resource resource_to_return{};
    /// If set, `Detect` returns this error instead of `resource_to_return`.
    std::optional<microtel::ConfigError> failure;
    std::string_view name = "FakeResourceDetector";

    int detect_call_count = 0;

    [[nodiscard]] microtel::Expected<microtel::Resource, microtel::ConfigError> Detect() override
    {
        ++detect_call_count;
        if (failure)
        {
            return microtel::Unexpected<microtel::ConfigError>{*failure};
        }
        return resource_to_return;
    }

    [[nodiscard]] std::string_view Name() const noexcept override
    {
        return name;
    }
};

}  // namespace microtel::testing
