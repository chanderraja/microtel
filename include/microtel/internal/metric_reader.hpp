// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/status.hpp"

#include <chrono>

namespace microtel::internal
{

/// @brief Drives metric collection. The default implementation is
/// `PeriodicExportingMetricReader` (push model, 60s interval per
/// `metrics-design.md` §5): on each interval it pulls a snapshot from its
/// `IMetricProducer` and hands it to its `IMetricExporter`.
///
/// `Collect` forces an immediate synchronous collect + export (the
/// manual / test path). `ForceFlush` / `Shutdown` propagate through to the
/// exporter and reuse the `microtel::Status` taxonomy.
///
/// @threadsafety Thread-safe.
/// @noexcept All methods.
/// @see docs/metrics-design.md §5
class IMetricReader
{
public:
    virtual ~IMetricReader() noexcept = default;

    [[nodiscard]] virtual microtel::Status Collect(std::chrono::milliseconds timeout) noexcept = 0;

    [[nodiscard]] virtual microtel::Status ForceFlush(
        std::chrono::milliseconds timeout) noexcept = 0;

    [[nodiscard]] virtual microtel::Status Shutdown(std::chrono::milliseconds timeout) noexcept = 0;
};

}  // namespace microtel::internal
