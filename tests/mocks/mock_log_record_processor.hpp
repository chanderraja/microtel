// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/internal/batch.hpp"
#include "microtel/internal/log_record_processor.hpp"
#include "microtel/log_record.hpp"
#include "microtel/status.hpp"

#include <chrono>

namespace microtel::testing
{

/// @brief Dumb mock for `microtel::internal::ILogRecordProcessor`.
///
/// Counts calls and returns configured statuses. No logic — tests that need to
/// inspect the emitted records use `FakeLogRecordProcessor` instead.
class MockLogRecordProcessor : public internal::ILogRecordProcessor
{
public:
    // --- Configurable returns ---
    microtel::Status force_flush_result = microtel::Status::Completed;
    microtel::Status shutdown_result = microtel::Status::Completed;

    // --- Recording ---
    int on_emit_call_count = 0;
    int force_flush_call_count = 0;
    int shutdown_call_count = 0;

    // --- ILogRecordProcessor ---

    void OnEmit(microtel::LogRecord&& /*record*/,
                const internal::InstrumentationScope& /*scope*/) noexcept override
    {
        ++on_emit_call_count;
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
