// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/internal/batch.hpp"
#include "microtel/internal/exporter.hpp"
#include "microtel/status.hpp"

#include <chrono>

namespace microtel::testing
{

/// @brief Dumb mock for `microtel::internal::IExporter`.
///
/// Used by `BatchSpanProcessor` unit tests to assert that batches are
/// handed off correctly without engaging the wire stack.
class MockExporter : public internal::IExporter
{
public:
    // --- Configurable returns ---
    internal::ExportResult export_result = internal::ExportResult::Success;
    microtel::Status force_flush_result = microtel::Status::Completed;
    microtel::Status shutdown_result = microtel::Status::Completed;

    // --- Recording ---
    int export_call_count = 0;
    int force_flush_call_count = 0;
    int shutdown_call_count = 0;

    // --- IExporter ---

    [[nodiscard]] internal::ExportResult Export(internal::BatchHandle&& /*batch*/) noexcept override
    {
        ++export_call_count;
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
