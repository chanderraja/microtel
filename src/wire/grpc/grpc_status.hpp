// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace microtel::wire
{

/// @brief One row of the gRPC status table.
struct GrpcStatusInfo
{
    /// Canonical spec name, e.g. `"UNAUTHENTICATED"`. Never empty.
    std::string_view name;
    /// Whether microtel retries this status, per `docs/error-model.md` §7.2.
    ///
    /// @warning `RESOURCE_EXHAUSTED (8)` is `false` here and that is not the
    ///          whole rule: it becomes retryable when the response carries a
    ///          `google.rpc.RetryInfo`, which `GrpcWireCodec` decides before it
    ///          consults this table. Flipping this bool to `true` would retry
    ///          every overload without the server's delay.
    bool retryable;
};

/// Number of canonical gRPC status codes, i.e. the valid range is `[0, 17)`.
inline constexpr int kGrpcStatusCount = 17;

/// Ceiling on how much of a `grpc-message` reaches the operator. Matches the
/// cap `DiagnosticsCounters` applies to `HealthSnapshot::last_error_message`,
/// so truncating here costs nothing that would have survived anyway — and
/// keeps a 64 KiB trailer from being assembled into a string first.
inline constexpr std::size_t kMaxGrpcMessageChars = 256;

/// @brief Look up a gRPC status code.
///
/// @param code the value parsed from the `grpc-status` trailer.
/// @return the canonical entry, or `nullopt` when @p code falls outside
///         `[0, kGrpcStatusCount)` — a server sending one of those is speaking
///         a protocol this codec cannot classify.
[[nodiscard]] std::optional<GrpcStatusInfo> LookupGrpcStatus(int code) noexcept;

/// @brief Percent-decode a `grpc-message` trailer value.
///
/// `grpc-message` is percent-encoded UTF-8 (`docs/grpc-wire-protocol.md` §4.4).
/// Both hex cases are accepted. An escape that is not `%` followed by two hex
/// digits — including a `%` at the very end — passes through verbatim rather
/// than failing the decode: the field is human-readable text that may
/// legitimately contain a bare `%`, and losing the operator's whole message
/// over a formatting detail is the worse outcome.
///
/// Single-pass: a decoded `%` is not rescanned, so `"%2541"` decodes to
/// `"%41"`, not `"A"`.
///
/// @param in the raw trailer value; bounded upstream by `max_trailer_bytes`.
/// @return the decoded bytes. May contain embedded NULs — it is text, not a
///         C string, and the length is authoritative.
[[nodiscard]] std::string PercentDecode(std::string_view in);

/// @brief Build the operator-visible message for a failed gRPC RPC.
///
/// Shape: `"UNAUTHENTICATED (16): <message>"`, or `"UNAUTHENTICATED (16)"` when
/// there is no message, or `"UNRECOGNIZED (42)"` for a code outside the
/// canonical range. The name alone is not enough — an operator grepping logs
/// needs the number too — and the number alone is not enough to read at a
/// glance, so both are always present.
///
/// @param code the value parsed from the `grpc-status` trailer.
/// @param decoded_message the `grpc-message`, **already** percent-decoded, or
///        empty. Taking it decoded rather than raw keeps the decode to one
///        pass: the codec needs the same text for `WireResult::response_excerpt`
///        and a message containing a literal `"%41"` must not become `"A"`.
/// @return the formatted string, with the message truncated to
///         `kMaxGrpcMessageChars`.
[[nodiscard]] std::string FormatGrpcError(int code, std::string_view decoded_message);

}  // namespace microtel::wire
