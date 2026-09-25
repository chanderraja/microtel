// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/error.hpp"
#include "microtel/expected.hpp"
#include "microtel/internal/resource_detector.hpp"
#include "microtel/resource.hpp"

#include "common/config/config.hpp"

#include <cstddef>
#include <memory>
#include <span>
#include <string_view>

namespace microtel::sdk
{

/// @brief Most attributes the resolved-Resource log line lists by name. The
/// rest are counted in a trailing "...and N more" marker.
inline constexpr std::size_t kMaxLoggedResourceAttributes = 32;

/// @brief Most characters of one rendered, escaped value the resolved-Resource log line
/// keeps; a longer value is cut (never inside an escape) and ends in "...".
inline constexpr std::size_t kMaxLoggedResourceValueChars = 128;

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
/// On success the composed Resource is logged once at `Info` through
/// `internal::LogImpl` (spec §12.7, "the resolved Resource is logged at
/// init"): keys sorted, keys and string values escaped (backslash, double
/// quote, control characters) so none can split the line, values of
/// secret-looking keys redacted, at most
/// `kMaxLoggedResourceAttributes` pairs and `kMaxLoggedResourceValueChars`
/// characters per value.
///
/// @param cfg the resolved, validated configuration. Borrowed; read only.
/// @param detectors registration-ordered detectors. Borrowed; each is invoked
///        once. The span's elements must be non-null.
/// @param profile_name the profile the provider registers under, named in the
///        log line; empty leaves it out.
/// @return the composed Resource, or the first detector `ConfigError` when
///         `cfg.resource_detectors_strict` is set. Under the default lenient
///         policy a failing detector is logged at Warn and skipped, and this
///         never returns an error.
[[nodiscard]] Expected<Resource, ConfigError> BuildResource(
    const config::Config& cfg,
    std::span<const std::unique_ptr<internal::IResourceDetector>> detectors,
    std::string_view profile_name = {});

}  // namespace microtel::sdk
