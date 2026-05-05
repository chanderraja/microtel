// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/error.hpp"
#include "microtel/expected.hpp"
#include "microtel/resource.hpp"

#include <string_view>

namespace microtel::internal
{

/// @brief Produces a partial `Resource` at SDK initialisation.
///
/// v1 ships a minimal env-var detector and an explicit-config "detector";
/// full detectors (process, host, k8s, cloud) arrive in v1.1+. The interface
/// is locked in M0 so v1.1 detectors do not break the contract.
///
/// Called at most once per detector instance, during `SdkBuilder::Build()`.
///
/// @threadsafety Not thread-safe (called once on the caller thread).
/// @see docs/interfaces.md §4.10
class IResourceDetector
{
public:
    virtual ~IResourceDetector() noexcept = default;

    [[nodiscard]] virtual microtel::Expected<microtel::Resource, microtel::ConfigError>
    Detect() = 0;

    [[nodiscard]] virtual std::string_view Name() const noexcept = 0;
};

}  // namespace microtel::internal
