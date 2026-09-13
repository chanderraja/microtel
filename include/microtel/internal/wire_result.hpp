// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/error.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>

namespace microtel::internal
{

/// @brief Outcome of one `IWireCodec::Send` call.
///
/// Carries everything the exporter needs to act, with no protocol-specific
/// knowledge required upstream. Retry classification is the codec's
/// responsibility — the exporter respects `retryable` and `retry_after`
/// without reinterpretation.
///
/// `success=true` and `partial_success_rejected > 0` is the partial-success
/// case; the exporter must not retry. (LOCKED — `docs/error-model.md` §6.)
///
/// @see docs/interfaces.md §3.2
struct WireResult
{
    bool success = false;
    bool retryable = false;
    std::optional<std::chrono::milliseconds> retry_after;
    std::uint32_t partial_success_rejected = 0;
    std::optional<Error> error;
    /// Short operator-facing excerpt of the response, for diagnostics. Its own
    /// cap is the excerpt length each codec applies; the whole response it is
    /// taken from is already bounded by `ConnectOptions::max_response_bytes`.
    std::string response_excerpt;
};

}  // namespace microtel::internal
