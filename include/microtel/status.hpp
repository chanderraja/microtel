// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>

namespace microtel
{

/// @brief Outcome of a lifecycle operation (`ForceFlush`, `Shutdown`).
///
/// Returned from `Provider::ForceFlush` and `Provider::Shutdown`. The four-way
/// value is intentionally coarse; richer detail is available through
/// `Provider::GetExporterHealth()`.
///
/// @see microtel::Provider::ForceFlush
/// @see microtel::Provider::Shutdown
/// @see docs/error-model.md §2.3
enum class Status : std::uint8_t
{
    /// @brief The operation completed within the timeout.
    Completed = 0,

    /// @brief The timeout elapsed before the operation completed.
    /// Some data may not have been flushed.
    TimedOut = 1,

    /// @brief Idempotent re-call after a prior `Shutdown` returned.
    AlreadyShutDown = 2,

    /// @brief An unrecoverable internal error occurred.
    /// See `Provider::GetExporterHealth()` for diagnostic detail.
    Failed = 3,
};

}  // namespace microtel
