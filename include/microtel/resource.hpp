// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/attribute.hpp"

#include <algorithm>
#include <utility>
#include <vector>

namespace microtel
{

/// @brief OpenTelemetry Resource — a set of attributes describing the entity
/// producing telemetry (service, host, deployment, etc.).
///
/// The Resource is built once at `SdkBuilder::Build()` time from the explicit
/// configuration, the active `IResourceDetector` set, and the OTel-standard
/// environment variables (`OTEL_SERVICE_NAME`, `OTEL_RESOURCE_ATTRIBUTES`).
/// Conflicts are resolved per `microtel-spec.md` §12.7.
///
/// After `Build()`, the Resource is **immutable** for the lifetime of the
/// `Provider`. Hot reload is not in v1; per-`Provider` re-build is required to
/// change Resource attributes.
///
/// @threadsafety Thread-safe (read-only after construction).
class Resource
{
public:
    Resource() noexcept = default;

    /// @brief Construct from an explicit attribute list.
    ///
    /// Duplicate keys: the last occurrence wins (semantic enforcement is
    /// the SDK's job during merge; the constructor itself is shallow).
    explicit Resource(std::vector<KeyValue> attrs) : m_attributes(std::move(attrs)) {}

    /// @brief Read-only view of the merged attributes.
    [[nodiscard]] const std::vector<KeyValue>& Attributes() const noexcept
    {
        return m_attributes;
    }

    /// @brief Key-level merge of two resources — `overriding` wins.
    ///
    /// This is the primitive the SDK composes the §12.7 precedence chain out
    /// of: `Merge(Merge(detectors, env), user)`. Applied left to right, each
    /// later layer overrides the ones before it, and every key no later layer
    /// names survives.
    ///
    /// Key collisions resolve to the value in `overriding`, whatever the two
    /// value types are; the key keeps the position it held in `base`, so the
    /// resolved Resource is stable between runs. A key repeated *within*
    /// `overriding` resolves to its last occurrence — `Resource`'s constructor
    /// is shallow, and this is where "last one wins" is enforced.
    ///
    /// @param base the lower-precedence layer.
    /// @param overriding the higher-precedence layer.
    /// @return a new Resource; neither argument is modified.
    [[nodiscard]] static Resource Merge(const Resource& base, const Resource& overriding)
    {
        std::vector<KeyValue> merged = base.m_attributes;
        for (const auto& kv : overriding.m_attributes)
        {
            const auto it = std::ranges::find(merged, kv.key, &KeyValue::key);
            if (it == merged.end())
            {
                merged.push_back(kv);
            }
            else
            {
                it->value = kv.value;
            }
        }
        return Resource{std::move(merged)};
    }

private:
    std::vector<KeyValue> m_attributes;
};

}  // namespace microtel
