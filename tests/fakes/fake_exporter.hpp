// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/internal/batch.hpp"
#include "microtel/internal/exporter.hpp"
#include "microtel/status.hpp"

#include <chrono>
#include <utility>
#include <vector>

namespace microtel::testing
{

/// @brief Fake `IExporter` that records every received `BatchHandle`.
///
/// Distinct from `MockExporter` (which only counts calls): tests can
/// inspect the actual batches that were handed to the exporter — span
/// records, resource, scope — without engaging the wire stack.
class FakeExporter : public internal::IExporter
{
public:
    std::vector<internal::BatchHandle> received_batches;

    internal::ExportResult export_result = internal::ExportResult::Success;
    microtel::Status force_flush_result = microtel::Status::Completed;
    microtel::Status shutdown_result = microtel::Status::Completed;

    int force_flush_call_count = 0;
    int shutdown_call_count = 0;

    [[nodiscard]] internal::ExportResult Export(internal::BatchHandle&& batch) noexcept override
    {
        received_batches.push_back(std::move(batch));
        return export_result;
    }

    [[nodiscard]] microtel::Status ForceFlush(
        std::chrono::milliseconds /*timeout*/) noexcept override
    {
        ++force_flush_call_count;
        return force_flush_result;
    }

    [[nodiscard]] microtel::Status Shutdown(std::chrono::milliseconds /*timeout*/) noexcept override
    {
        ++shutdown_call_count;
        return shutdown_result;
    }
};

}  // namespace microtel::testing
