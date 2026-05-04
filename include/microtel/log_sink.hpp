// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <functional>
#include <string_view>

namespace microtel
{

/// @brief Severity level for microtel's internal diagnostic logs.
enum class LogLevel : std::uint8_t
{
    Trace = 0,
    Debug = 1,
    Info  = 2,
    Warn  = 3,
    Error = 4,
};

/// @brief Caller-supplied callback for redirecting microtel's internal logs.
///
/// The sink is invoked under no microtel-held lock and may be called from any
/// internal thread (caller, exporter worker, or I/O). Applications are
/// responsible for the sink's thread-safety.
///
/// `msg` is a borrowed view valid for the duration of the call only. If the
/// sink needs to retain it, copy first.
using LogSink = std::function<void(LogLevel lvl, std::string_view msg)>;

/// @brief Install `sink` as the active log sink.
///
/// Thread-safe. Atomically replaces any previously-installed sink.
/// May be called at any point in the process lifetime.
///
/// @param sink callable invoked for each log emission. Ownership is moved in.
///
/// @threadsafety Thread-safe.
/// @noexcept Always succeeds; never throws.
///
/// @see docs/error-model.md §9.3
void SetLogSink(LogSink sink) noexcept;

/// @brief Restore the default sink (spdlog or stderr fallback per build config).
///
/// @threadsafety Thread-safe.
void ResetLogSink() noexcept;

}  // namespace microtel
