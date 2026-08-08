// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/log_record.hpp"
#include "microtel/logger.hpp"

#include <utility>
#include <vector>

namespace microtel::testing
{

/// @brief Fake `microtel::Logger` that captures every emitted `LogRecord`.
///
/// Tests inspect `emitted` to assert on the records produced by adapters and
/// other `Logger` clients. Single-threaded use only (no internal locking).
class FakeLogger : public microtel::Logger
{
public:
    std::vector<microtel::LogRecord> emitted;

    void Emit(microtel::LogRecord record) noexcept override
    {
        emitted.push_back(std::move(record));
    }
};

}  // namespace microtel::testing
