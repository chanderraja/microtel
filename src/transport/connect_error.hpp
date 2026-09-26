// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "microtel/error.hpp"

namespace microtel::transport
{

/// @brief Fold one address's connect failure into the one `TcpConnect` reports.
///
/// A host name can resolve to several addresses, and `TcpConnect` tries each
/// in turn; only one error comes back. A refusal is the least informative
/// outcome — nothing was listening — so it never replaces a failure that got
/// further, such as a peer that accepted and then reset (issue #333).
/// Otherwise the latest failure wins.
///
/// @param previous The errno kept so far; 0 if no address has failed yet.
/// @param current  This address's errno (from `connect` or `SO_ERROR`).
/// @return The errno to keep.
[[nodiscard]] int MergeConnectErrno(int previous, int current) noexcept;

/// @brief The `Error` a TCP connect that failed with @p err reports.
///
/// Always `Error::Kind::Network`: `docs/error-model.md` §7 classifies every
/// connection failure alike (retryable, `connect_failure`), whatever the
/// errno. The errno is what the message and `Error::os_errno` carry:
///
/// - `ECONNREFUSED`, or 0 when no address produced one: "connection refused".
/// - `ECONNRESET` / `EPIPE`: the peer accepted the connection and then closed
///   it, so the message says the peer closed the connection. Reporting it as a
///   refusal would send an operator looking for a receiver that is not
///   listening when one is (issue #333).
/// - Anything else: "connect failed: " and the errno's text.
///
/// @param err The errno `MergeConnectErrno` kept.
[[nodiscard]] microtel::Error ConnectFailureError(int err);

}  // namespace microtel::transport
