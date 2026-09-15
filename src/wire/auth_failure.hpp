// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/error.hpp"
#include "microtel/internal/wire_result.hpp"

#include <string>

namespace microtel::wire
{

/// @brief The `WireResult` an `IAuthProvider` failure produces.
///
/// `docs/interfaces.md` §4.9: the batch is dropped — sending it without the
/// header is worse than not sending it — and the drop is terminal, so the
/// exporter's final-outcome funnel counts it as `non_retryable_failure`
/// (`docs/error-model.md` §3 keeps that counter at the exporter, not here).
///
/// The provider's `Error::Kind` survives and its message is prefixed, so an
/// operator reading `last_error_message` is pointed at the auth stage rather
/// than at a receiver's 401 one hop later (issues #250, #251).
///
/// Shared by both codecs so the classification and the wording cannot drift
/// apart between the HTTP and gRPC paths.
[[nodiscard]] inline internal::WireResult AuthFailure(const microtel::Error& err)
{
    return internal::WireResult{
        .success = false,
        .retryable = false,
        .retry_after = {},
        .partial_success_rejected = 0,
        .error = microtel::Error{.kind = err.kind,
                                 .message = "authorization header unavailable: " + err.message,
                                 .os_errno = err.os_errno},
        .response_excerpt = {},
    };
}

}  // namespace microtel::wire
