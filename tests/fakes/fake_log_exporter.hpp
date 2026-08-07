// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/internal/exporter.hpp"  // ExportResult
#include "microtel/internal/log_batch.hpp"
#include "microtel/internal/log_exporter.hpp"
#include "microtel/status.hpp"

#include <chrono>
#include <utility>
#include <vector>

namespace microtel::testing
{

/// @brief Fake `ILogExporter` that captures every exported batch.
///
/// Tests inspect `exported` — each moved-in `LogBatchHandle` — to assert on
/// record counts, scope grouping, and Resource. Read `exported` only after a
/// `ForceFlush` or `Shutdown` has synchronised with the processor's worker.
class FakeLogExporter : public internal::ILogExporter
{
public:
    std::vector<internal::LogBatchHandle> exported;

    internal::ExportResult export_result = internal::ExportResult::Success;
    microtel::Status force_flush_result = microtel::Status::Completed;
    microtel::Status shutdown_result = microtel::Status::Completed;
    int force_flush_call_count = 0;
    int shutdown_call_count = 0;

    internal::ExportResult Export(internal::LogBatchHandle&& batch) noexcept override
    {
        exported.push_back(std::move(batch));
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
