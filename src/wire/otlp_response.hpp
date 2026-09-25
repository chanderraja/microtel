// Copyright (c) 2026 The microtel Authors.
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace microtel::wire
{

/// @brief What `ParseRejectedSpans` found in a response body.
enum class PartialSuccessOutcome : std::uint8_t
{
    /// The body parsed and carries no `partial_success` field (an empty body
    /// included): a clean success.
    Absent = 0,
    /// The body parsed and carries `partial_success`; `rejected` holds its
    /// count, which may be 0.
    Parsed = 1,
    /// The body is not a well-formed protobuf message. `rejected` is 0 and
    /// means nothing: the caller classifies the response as malformed
    /// (`docs/error-model.md` §7.1, §7.2).
    Unparseable = 2,
};

/// @brief Result of `ParseRejectedSpans`.
struct PartialSuccessResult
{
    PartialSuccessOutcome outcome = PartialSuccessOutcome::Absent;
    /// Rejected item count, capped to `uint32_t`. 0 unless `outcome` is `Parsed`.
    std::uint32_t rejected = 0;
};

/// @brief Parse the partial-success rejected count from an OTLP Export
///        response body.
///
/// Serves all three signals: `ExportTracePartialSuccess.rejected_spans`,
/// `ExportMetricsPartialSuccess.rejected_data_points` and
/// `ExportLogsPartialSuccess.rejected_log_records` share one wire layout.
/// Used by `HttpWireCodec` (raw body) and `GrpcWireCodec` (message after the
/// 5-byte gRPC prefix). The whole body is walked: any wire-format error —
/// including trailing bytes after a valid `partial_success` — makes the body
/// `Unparseable`, as it would for a protobuf parser. Unknown fields are skipped.
/// A repeated `partial_success` merges, so the last rejected count wins.
[[nodiscard]] PartialSuccessResult ParseRejectedSpans(std::span<const std::byte> body) noexcept;

}  // namespace microtel::wire
