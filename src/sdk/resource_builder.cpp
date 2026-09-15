// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0
//
// The spec §12.7 resource composition. Kept out of sdk_builder.cpp so that the
// ordering — the part with actual semantics — is unit-testable without
// standing up a whole Provider.

#include "sdk/resource_builder.hpp"

#include "microtel/attribute.hpp"
#include "microtel/error.hpp"
#include "microtel/expected.hpp"
#include "microtel/internal/resource_detector.hpp"
#include "microtel/log_sink.hpp"
#include "microtel/resource.hpp"

#include "common/config/config.hpp"
#include "common/internal_log.hpp"

#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace microtel::sdk
{

namespace
{

/// @brief Layer 1 — the built-in defaults.
///
/// Only `service.name`, and only when nothing configured one. It lives here
/// rather than alongside the configured attributes because a placeholder must
/// lose to a detector that actually knows the service's name.
[[nodiscard]] Resource DefaultsLayer(const config::Config& cfg)
{
    if (!cfg.service_name_defaulted)
    {
        return Resource{};
    }
    return Resource{{{.key = "service.name", .value = cfg.service_name}}};
}

/// @brief Layers 3 and 4 — the environment and the user, already collapsed.
///
/// `config::OverlayEnv` and `SdkBuilder::Impl::ApplyResourceOverrides` have
/// resolved file, env and code into `cfg` before this runs, in that order, so
/// the two tiers arrive as one layer that sits above every detector.
[[nodiscard]] Resource ConfigLayer(const config::Config& cfg)
{
    std::vector<KeyValue> attrs;
    if (!cfg.service_name_defaulted)
    {
        attrs.push_back({.key = "service.name", .value = cfg.service_name});
    }
    if (!cfg.service_version.empty())
    {
        attrs.push_back({.key = "service.version", .value = cfg.service_version});
    }
    for (const auto& kv : cfg.resource_attrs)
    {
        attrs.push_back(kv);
    }
    return Resource{std::move(attrs)};
}

/// @brief Describe a detector failure for a log line or an error message.
[[nodiscard]] std::string DescribeFailure(const internal::IResourceDetector& detector,
                                          const ConfigError& error)
{
    return "resource detector \"" + std::string{detector.Name()} + "\" failed: " + error.message;
}

}  // namespace

Expected<Resource, ConfigError> BuildResource(
    const config::Config& cfg,
    std::span<const std::unique_ptr<internal::IResourceDetector>> detectors)
{
    Resource detected;
    for (const auto& detector : detectors)
    {
        auto contribution = detector->Detect();
        if (contribution.has_value())
        {
            detected = Resource::Merge(detected, *contribution);
            continue;
        }
        if (cfg.resource_detectors_strict)
        {
            return make_unexpected(
                ConfigError{.kind = contribution.error().kind,
                            .field = contribution.error().field,
                            .message = DescribeFailure(*detector, contribution.error())});
        }
        internal::LogImpl(LogLevel::Warn,
                          DescribeFailure(*detector, contribution.error()) +
                              " - skipping its contribution (set "
                              "sdk.resource_detectors_strict to fail Build instead)");
    }

    return Resource::Merge(Resource::Merge(DefaultsLayer(cfg), detected), ConfigLayer(cfg));
}

}  // namespace microtel::sdk
