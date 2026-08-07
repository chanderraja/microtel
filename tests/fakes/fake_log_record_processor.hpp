// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/internal/batch.hpp"
#include "microtel/internal/log_record_processor.hpp"
#include "microtel/log_record.hpp"
#include "microtel/status.hpp"

#include <chrono>
#include <utility>
#include <vector>

namespace microtel::testing
{

/// @brief Fake `ILogRecordProcessor` that records every emitted record.
///
/// Tests inspect `emitted` — the moved-in `LogRecord` and the scope it was
/// tagged with — to verify `SdkLogger` transformations (observed-time backfill,
/// trace correlation, attribute-limit enforcement).
class FakeLogRecordProcessor : public internal::ILogRecordProcessor
{
public:
    struct Emitted
    {
        microtel::LogRecord record;
        internal::InstrumentationScope scope;
    };

    std::vector<Emitted> emitted;

    microtel::Status force_flush_result = microtel::Status::Completed;
    microtel::Status shutdown_result = microtel::Status::Completed;
    int force_flush_call_count = 0;
    int shutdown_call_count = 0;

    void OnEmit(microtel::LogRecord&& record,
                const internal::InstrumentationScope& scope) noexcept override
    {
        emitted.push_back(Emitted{.record = std::move(record), .scope = scope});
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
