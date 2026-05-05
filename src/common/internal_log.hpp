// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/log_sink.hpp"

#include <string_view>

namespace microtel::internal
{

/// @brief Internal log entry point used by every microtel component.
///
/// Routes through the active `microtel::LogSink` if one is installed,
/// otherwise to the default fallback (stderr today; spdlog when M3 wires
/// it under MICROTEL_USE_SPDLOG=ON).
///
/// `noexcept`. The implementation never throws and never propagates an
/// application sink's exception (per `docs/error-model.md` §9.3).
///
/// M2 scope: this is the public-internal hook. Rate-limiting per
/// `(level, reason)` pair lands in M3+ alongside the actual emission
/// sites.
///
/// @threadsafety Thread-safe. May be called from any internal thread.
void LogImpl(microtel::LogLevel level, std::string_view message) noexcept;

}  // namespace microtel::internal
