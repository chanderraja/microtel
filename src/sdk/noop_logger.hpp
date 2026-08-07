// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/log_record.hpp"
#include "microtel/logger.hpp"

namespace microtel::sdk
{

/// @brief A `Logger` whose `Emit` drops every record.
///
/// Returned by `SdkProvider::GetLogger` when no logs exporter is configured, so
/// callers get a valid, non-null logger that is simply a no-op (ICP 0012,
/// `docs/logs-design.md` §8).
///
/// @threadsafety Thread-safe (stateless).
class NoopLogger final : public microtel::Logger
{
public:
    NoopLogger() noexcept = default;
    ~NoopLogger() noexcept override = default;

    NoopLogger(const NoopLogger&) = delete;
    NoopLogger& operator=(const NoopLogger&) = delete;
    NoopLogger(NoopLogger&&) noexcept = default;
    NoopLogger& operator=(NoopLogger&&) noexcept = default;

    void Emit(LogRecord /*record*/) noexcept override {}
};

}  // namespace microtel::sdk
