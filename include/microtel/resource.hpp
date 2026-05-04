// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/attribute.hpp"

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
    /// Duplicate keys: the last occurrence wins.
    explicit Resource(std::vector<KeyValue> attrs);

    /// @brief Read-only view of the merged attributes.
    [[nodiscard]] const std::vector<KeyValue>& Attributes() const noexcept;

private:
    std::vector<KeyValue> m_attributes;
};

}  // namespace microtel
