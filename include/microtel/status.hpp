// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>

namespace microtel
{

/// @brief Outcome of a lifecycle operation (`ForceFlush`, `Shutdown`) or of one
/// of the four hot-reload setters (`Provider::SetBatchOptions`,
/// `SetMetricInterval`, `SetSamplerRatio`, `SetLogLevel`).
///
/// The value is intentionally coarse; richer detail is available through
/// `Provider::GetExporterHealth()`, and a rejected setter call logs which
/// field and what range at `Warn` through the internal log.
///
/// The last two enumerators are **setters only**: `ForceFlush` and `Shutdown`
/// return one of the first four and never `InvalidArgument` or `Unsupported`
/// (ICP 0026 §2).
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

    /// @brief The argument failed validation. Nothing was changed.
    /// Only returned by the `Set*` setters; never by `ForceFlush`/`Shutdown`.
    InvalidArgument = 4,

    /// @brief The knob does not exist on this provider — no metrics pipeline,
    /// or a sampler with no ratio. Nothing was changed, and this is not an
    /// error condition of the pipeline. Setters only.
    Unsupported = 5,
};

}  // namespace microtel
