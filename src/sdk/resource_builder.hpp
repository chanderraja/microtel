// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/error.hpp"
#include "microtel/expected.hpp"
#include "microtel/internal/resource_detector.hpp"
#include "microtel/resource.hpp"

#include "common/config/config.hpp"

#include <memory>
#include <span>

namespace microtel::sdk
{

/// @brief Compose the `Provider`'s Resource per `microtel-spec.md` §12.7.
///
/// Four layers, merged left to right so that a later one overrides an earlier
/// one key by key (`Resource::Merge`):
///
///   1. **Built-in defaults** — the `unknown_service` placeholder, and only
///      when nothing configured a service name (`service_name_defaulted`).
///   2. **Detectors**, in registration order; a later detector overrides an
///      earlier one.
///   3. **Environment**, and 4. **user-supplied** file or code — both of which
///      the config layer has already collapsed into `cfg` by the time this
///      runs, env first and code last.
///
/// Each detector is called exactly once, on this thread, per the one-shot
/// contract in `docs/interfaces.md` §4.10.
///
/// @param cfg the resolved, validated configuration. Borrowed; read only.
/// @param detectors registration-ordered detectors. Borrowed; each is invoked
///        once. The span's elements must be non-null.
/// @return the composed Resource, or the first detector `ConfigError` when
///         `cfg.resource_detectors_strict` is set. Under the default lenient
///         policy a failing detector is logged at Warn and skipped, and this
///         never returns an error.
[[nodiscard]] Expected<Resource, ConfigError> BuildResource(
    const config::Config& cfg,
    std::span<const std::unique_ptr<internal::IResourceDetector>> detectors);

}  // namespace microtel::sdk
